// SPDX-License-Identifier: GPL-3.0-or-later
// Prototype 2 (Switch), B-001: overflow blocks for the game's dual stream arenas.
//
// The game's streamed-resource arenas (normal part: sub_82CA1F50, write-combine/GPU part: sub_82CA2008; free
// sub_82CA22E0, destroy sub_82CA25C8) are linear: a free only lowers a count, the cursor goes back to the start only
// when every block of that part is free (WORKLOG #146). In long city sessions one arena runs out (run108: heap 120,
// run110: heap 125, pass 13: 16 KiB request) and the caller (sub_82C02B38 texture load, sub_82A2BF30 ...) copies into
// address 0: the crash. Enlarging the arenas exhausts their fixed parent (#145, never again).
//
// With p2_stream_arena_overflow, a request that does not fit into a part that HAS a capacity is served from the
// guest heap the part itself lives in (same view: virtual, or physical 0xA0000000 / 0xC0000000 / 0xE0000000), with
// the part's protection, page-aligned (covers every alignment the game asks for, up to the page size). The block
// is outside the arena's ranges, so the arena's own free ignores it (range check) and its counts and cursors stay
// exactly as the game keeps them; the free hook releases the block, and the destroy hook releases what is left.
// Nothing changes while the arenas have room. A total cap bounds a lifetime leak, should the game ever drop a block
// without freeing it; past the cap (or when the guest heap is full) the request fails as before.
#include "stream_arena_trace.h"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>
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
  uint32_t size;
};
std::mutex g_mutex;
std::unordered_map<uint32_t, Block> g_blocks;  // guest address -> owner
uint64_t g_live_bytes = 0;
uint64_t g_total_allocs = 0, g_total_frees = 0, g_failures = 0;
std::atomic<int> g_enabled{-1};
std::atomic<uint32_t> g_live_count{0};  // lock-free fast path of the free hook

uint32_t Be32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  __builtin_memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
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
  uint32_t align = page;
  while (align < alignment && align < 0x1000000u) align <<= 1;
  const uint32_t rounded = (size + page - 1) & ~(page - 1);
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_live_bytes + rounded > uint64_t(REXCVAR_GET(p2_stream_arena_overflow_cap_mb)) << 20) {
    if (g_failures++ < 8)
      REXLOG_ERROR("[p2-arena-overflow] cap reached: arena {:08X} part {} request {} live {} KiB", arena, part,
                   size, g_live_bytes >> 10);
    return 0;
  }
  uint32_t address = 0;
  if (!heap->AllocRange(heap->heap_base(), heap->heap_base() + heap->heap_size() - 1, rounded, align,
                        rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit, protect,
                        true, &address) ||
      !address) {
    if (g_failures++ < 8)
      REXLOG_ERROR("[p2-arena-overflow] guest heap full: arena {:08X} part {} request {} (heap {:08X}) live {} KiB",
                   arena, part, size, heap->heap_base(), g_live_bytes >> 10);
    return 0;
  }
  g_blocks[address] = Block{arena, rounded};
  g_live_count.store(uint32_t(g_blocks.size()), std::memory_order_release);
  g_live_bytes += rounded;
  ++g_total_allocs;
  if (g_total_allocs <= 16 || (g_total_allocs & 255) == 0)
    REXLOG_WARN("[p2-arena-overflow] #{}: arena {:08X} part {} request {} align {} -> {:08X}; live {} blocks {} KiB",
                g_total_allocs, arena, part, size, alignment, address, g_blocks.size(), g_live_bytes >> 10);
  return address;
}

extern "C" void P2ArenaOverflowFree(uint8_t* /*base*/, uint32_t /*arena*/, uint32_t address) {
  if (!address || g_live_count.load(std::memory_order_acquire) == 0) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_blocks.find(address);
  if (it == g_blocks.end()) return;  // ours, whichever arena object the game passes
  auto* memory = rex::system::kernel_memory();
  if (auto* heap = memory ? memory->LookupHeap(address) : nullptr) heap->Release(address);
  g_live_bytes -= it->second.size;
  g_blocks.erase(it);
  g_live_count.store(uint32_t(g_blocks.size()), std::memory_order_release);
  ++g_total_frees;
}

extern "C" void P2ArenaOverflowDestroy(uint8_t* /*base*/, uint32_t arena) {
  if (!arena || g_live_count.load(std::memory_order_acquire) == 0) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  auto* memory = rex::system::kernel_memory();
  uint32_t released = 0;
  for (auto it = g_blocks.begin(); it != g_blocks.end();) {
    if (it->second.arena != arena) {
      ++it;
      continue;
    }
    if (auto* heap = memory ? memory->LookupHeap(it->first) : nullptr) heap->Release(it->first);
    g_live_bytes -= it->second.size;
    it = g_blocks.erase(it);
    ++released;
  }
  g_live_count.store(uint32_t(g_blocks.size()), std::memory_order_release);
  if (released)
    REXLOG_WARN("[p2-arena-overflow] arena {:08X} destroyed with {} overflow blocks still live (released)", arena,
                released);
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
