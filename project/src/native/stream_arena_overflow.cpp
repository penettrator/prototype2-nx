// SPDX-License-Identifier: GPL-3.0-or-later
// Prototype 2 (Switch), B-001: overflow blocks for the game's dual stream arenas.
//
// The game's streamed-resource arenas (normal part: sub_82CA1F50, write-combine/GPU part: sub_82CA2008; free
// sub_82CA22E0, destroy sub_82CA25C8) are linear: a free only lowers a count, the cursor goes back to the start only
// when every block of that part is free (WORKLOG #146). In long city sessions one arena runs out (run108: heap 120,
// run110: heap 125, pass 13: 16 KiB request) and the caller (sub_82C02B38 texture load, sub_82A2BF30 ...) copies into
// address 0: the crash. Enlarging the arenas exhausts their fixed parent (#145, never again).
//
// With p2_stream_arena_overflow, requests that do not fit are suballocated from committed extents in the same
// guest heap/view and with the part's protection. This matters for physical A-memory: its 64 KiB page size means
// one guest page per small allocation can waste most of the remaining 512 MiB physical heap. The returned ranges
// remain outside the game's linear arena, so its own counts/cursors stay unchanged. The free hook returns ranges;
// an empty extent is released, and the destroy hook releases any remaining extents.
// Nothing changes while the arenas have room. A total cap bounds a lifetime leak, should the game ever drop a block
// without freeing it; past the cap (or when the guest heap is full) the request fails as before.
#include "stream_arena_trace.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iterator>
#include <map>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(p2_stream_arena_overflow, false, "Prototype2",
    "B-001: serve a request that no longer fits a stream arena from the guest heap the arena lives in (freed "
    "with the block / the arena); false = the original null result (crash in the city)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_UINT32(p2_stream_arena_overflow_cap_mb, 96, "Prototype2",
    "Upper limit of all live overflow blocks, MiB");

namespace {
struct Block {
  uint32_t arena;
  uint32_t slab;
  uint32_t size;
};
struct Slab {
  uint32_t arena;
  uint32_t part;
  uint32_t heap_base;
  uint32_t base;
  uint32_t protect;
  uint32_t size;
  // Free byte ranges within a committed guest page extent. Xbox physical A-memory
  // uses 64 KiB pages, so reserving one page per tiny overflow request wastes RAM.
  std::map<uint32_t, uint32_t> free_ranges;
};
std::mutex g_mutex;
std::unordered_map<uint32_t, Block> g_blocks;  // guest address -> owner
std::unordered_map<uint32_t, Slab> g_slabs;     // page extent base -> suballocator
uint64_t g_live_bytes = 0;
uint64_t g_total_allocs = 0, g_total_frees = 0, g_failures = 0;
std::atomic<int> g_enabled{-1};
std::atomic<uint32_t> g_live_count{0};  // lock-free fast path of the free hook

uint32_t Be32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  __builtin_memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}

uint32_t AllocateFromSlab(Slab& slab, uint32_t size, uint32_t alignment) {
  for (auto it = slab.free_ranges.begin(); it != slab.free_ranges.end(); ++it) {
    const uint32_t range_offset = it->first;
    const uint32_t range_size = it->second;
    const uint64_t absolute = uint64_t(slab.base) + range_offset;
    const uint64_t aligned = (absolute + alignment - 1) & ~(uint64_t(alignment) - 1);
    const uint64_t prefix64 = aligned - absolute;
    if (prefix64 > range_size || size > range_size - prefix64) continue;
    const uint32_t prefix = uint32_t(prefix64);
    const uint32_t suffix_offset = range_offset + prefix + size;
    const uint32_t suffix = range_size - prefix - size;
    slab.free_ranges.erase(it);
    if (prefix) slab.free_ranges.emplace(range_offset, prefix);
    if (suffix) slab.free_ranges.emplace(suffix_offset, suffix);
    return uint32_t(aligned);
  }
  return 0;
}

void FreeSlabRange(Slab& slab, uint32_t address, uint32_t size) {
  uint32_t offset = address - slab.base;
  auto next = slab.free_ranges.lower_bound(offset);
  if (next != slab.free_ranges.begin()) {
    auto previous = std::prev(next);
    if (previous->first + previous->second == offset) {
      offset = previous->first;
      size += previous->second;
      slab.free_ranges.erase(previous);
    }
  }
  next = slab.free_ranges.lower_bound(offset);
  if (next != slab.free_ranges.end() && offset + size == next->first) {
    size += next->second;
    slab.free_ranges.erase(next);
  }
  slab.free_ranges.emplace(offset, size);
}
}  // namespace

extern "C" bool P2ArenaOverflowEnabled() {
  int e = g_enabled.load(std::memory_order_relaxed);
  if (e < 0) {
    e = REXCVAR_GET(p2_stream_arena_overflow) ? 1 : 0;
    g_enabled.store(e, std::memory_order_relaxed);
  }
  return e != 0;
}

// part: 0 = normal (+88 base, +92 capacity), 1 = GPU / write-combine (+100 base, +104 capacity).
extern "C" uint32_t P2ArenaOverflowAlloc(uint8_t* base, uint32_t arena, uint32_t size, uint32_t alignment,
                                         uint32_t part) {
  if (!P2ArenaOverflowEnabled() || !arena || !size) return 0;
  const uint32_t part_base = Be32(base, arena + (part ? 100 : 88));
  const uint32_t part_capacity = Be32(base, arena + (part ? 104 : 92));
  if (!part_base || !part_capacity) return 0;  // a part without capacity: the caller has its own fallback
  auto* memory = rex::system::kernel_memory();
  if (!memory) return 0;
  rex::memory::BaseHeap* heap = memory->LookupHeap(part_base);
  if (!heap) return 0;
  uint32_t protect = rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite;
  uint32_t queried = 0;
  if (heap->QueryProtect(part_base, &queried) && queried) protect = queried;
  const uint32_t page = heap->page_size();
  uint32_t align = 1;
  while (align < alignment && align < 0x1000000u) align <<= 1;
  if (!page || (page & (page - 1))) return 0;
  const uint64_t extent_need = uint64_t(size) + align - 1;
  const uint64_t rounded64 = std::max<uint64_t>(page, (extent_need + page - 1) & ~(uint64_t(page) - 1));
  if (rounded64 > UINT32_MAX) return 0;
  const uint32_t extent = uint32_t(rounded64);
  std::lock_guard<std::mutex> lock(g_mutex);
  const uint32_t block_align = page;
  for (auto& entry : g_slabs) {
    auto& slab = entry.second;
    if (slab.arena != arena || slab.part != part || slab.heap_base != heap->heap_base() ||
        slab.protect != protect)
      continue;
    const uint32_t address = AllocateFromSlab(slab, size, align);
    if (address) {
      g_blocks[address] = Block{arena, entry.first, size};
      g_live_count.store(uint32_t(g_blocks.size()), std::memory_order_release);
      ++g_total_allocs;
      if (g_total_allocs <= 16 || (g_total_allocs & 255) == 0)
        REXLOG_WARN("[p2-arena-overflow] #{}: arena {:08X} part {} request {} align {} -> {:08X}; live {} blocks {} KiB",
                    g_total_allocs, arena, part, size, alignment, address, g_blocks.size(), g_live_bytes >> 10);
      return address;
    }
  }

  if (g_live_bytes + extent > uint64_t(REXCVAR_GET(p2_stream_arena_overflow_cap_mb)) << 20) {
    if (g_failures++ < 8)
      REXLOG_ERROR("[p2-arena-overflow] cap reached: arena {:08X} part {} request {} live {} KiB", arena, part,
                   size, g_live_bytes >> 10);
    return 0;
  }
  uint32_t address = 0;
  if (!heap->AllocRange(heap->heap_base(), heap->heap_base() + heap->heap_size() - 1, extent, block_align,
                        rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit, protect,
                        true, &address) ||
      !address) {
    if (g_failures++ < 8)
      REXLOG_ERROR("[p2-arena-overflow] guest heap full: arena {:08X} part {} request {} (heap {:08X}) live {} KiB",
                   arena, part, size, heap->heap_base(), g_live_bytes >> 10);
    return 0;
  }
  Slab slab{arena, part, heap->heap_base(), address, protect, extent, {}};
  slab.free_ranges.emplace(0, extent);
  const uint32_t allocated = AllocateFromSlab(slab, size, align);
  if (!allocated) {
    heap->Release(address);
    if (g_failures++ < 8)
      REXLOG_ERROR("[p2-arena-overflow] internal slab allocation failed: arena {:08X} part {} request {}",
                   arena, part, size);
    return 0;
  }
  g_slabs.emplace(address, std::move(slab));
  g_blocks[allocated] = Block{arena, address, size};
  g_live_count.store(uint32_t(g_blocks.size()), std::memory_order_release);
  g_live_bytes += extent;
  ++g_total_allocs;
  if (g_total_allocs <= 16 || (g_total_allocs & 255) == 0)
    REXLOG_WARN("[p2-arena-overflow] #{}: arena {:08X} part {} request {} align {} -> {:08X}; live {} blocks {} KiB",
                g_total_allocs, arena, part, size, alignment, allocated, g_blocks.size(), g_live_bytes >> 10);
  return allocated;
}

extern "C" void P2ArenaOverflowFree(uint8_t* /*base*/, uint32_t /*arena*/, uint32_t address) {
  if (!address || g_live_count.load(std::memory_order_acquire) == 0) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_blocks.find(address);
  if (it == g_blocks.end()) return;  // ours, whichever arena object the game passes
  auto* memory = rex::system::kernel_memory();
  const uint32_t slab_base = it->second.slab;
  auto slab_it = g_slabs.find(slab_base);
  if (slab_it != g_slabs.end()) {
    auto& slab = slab_it->second;
    FreeSlabRange(slab, address, it->second.size);
    if (slab.free_ranges.size() == 1 && slab.free_ranges.begin()->first == 0 &&
        slab.free_ranges.begin()->second == slab.size) {
      if (auto* heap = memory ? memory->LookupHeap(slab_base) : nullptr) heap->Release(slab_base);
      g_live_bytes -= slab.size;
      g_slabs.erase(slab_it);
    }
  }
  g_blocks.erase(it);
  g_live_count.store(uint32_t(g_blocks.size()), std::memory_order_release);
  ++g_total_frees;
}

extern "C" void P2ArenaOverflowDestroy(uint8_t* /*base*/, uint32_t arena) {
  if (!arena || g_live_count.load(std::memory_order_acquire) == 0) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  auto* memory = rex::system::kernel_memory();
  uint32_t released_extents = 0;
  for (auto it = g_slabs.begin(); it != g_slabs.end();) {
    if (it->second.arena != arena) {
      ++it;
      continue;
    }
    if (auto* heap = memory ? memory->LookupHeap(it->first) : nullptr) heap->Release(it->first);
    g_live_bytes -= it->second.size;
    it = g_slabs.erase(it);
    ++released_extents;
  }
  uint32_t released_allocations = 0;
  for (auto it = g_blocks.begin(); it != g_blocks.end();) {
    if (it->second.arena == arena) {
      it = g_blocks.erase(it);
      ++released_allocations;
    } else {
      ++it;
    }
  }
  g_live_count.store(uint32_t(g_blocks.size()), std::memory_order_release);
  if (released_extents)
    REXLOG_WARN("[p2-arena-overflow] arena {:08X} destroyed with {} allocations in {} extents (released)", arena,
                released_allocations, released_extents);
}

// B-001 (2026-11-04, perf_results/semaphore-ab/logs1): the crash came from a NULL of the game's allocator front end
// sub_8222FE80 (heaps[index]->vtable[6](size)), not from the stream arenas hooked above. Report every such NULL:
// heap index, size, the heap's alloc function and the caller (saved LR of sub_8222FE80's caller). Report only.
extern "C" void P2HeapNull(uint32_t index, uint32_t size, uint32_t alloc_fn, uint32_t caller) {
  static std::atomic<uint32_t> count{0};
  const uint32_t n = count.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 64 || (n & (n - 1)) == 0)
    REXLOG_ERROR("[p2-heap-null] #{}: heap {} size {} alloc {:08X} caller {:08X}", n, index, size, alloc_fn, caller);
}
