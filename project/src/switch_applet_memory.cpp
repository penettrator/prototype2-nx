// SPDX-License-Identifier: GPL-3.0-or-later
// Prototype 2 (PERF pass 31): GPU memory borrowed from the applet pool.
//
// The game gets the application pool only (3,285 MB on retail firmware), and inside it the 2 GB newlib heap
// is nearly full at peaks (logs/rex/memory_streaming.csv: 3 to 130 MB free). While the game runs, the applet
// pool holds ~400 MB nobody uses (logs/rex/physical_pools.csv, pass 31: 99 of 500 MB). With
// p2_applet_memory_mb > 0 the game starts the helper p2mem (tools/p2mem, lives in the applet pool), receives
// that much of its memory as transfer memory, and the NVK driver takes its GPU backing stores from it instead
// of the heap. The heap keeps everything else (guest memory, stacks, the rest of Mesa).
//
// Only the driver is redirected: libvulkan.a is linked with memalign -> p2_nvk_memalign and
// free -> p2_nvk_free (objcopy --redefine-sym, tools/p2mem/redirect_libvulkan.sh). Both fall through to
// the heap when this is off, when the region is full, or for small requests, so with 0 the build behaves as
// before. The first redirected allocation is checked once (GPU map + uncached attribute on the borrowed
// pages); if the kernel refuses, the region is never used and everything stays in the heap.
#include "generated/default/prototype2_pch.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <malloc.h>
#include <sys/stat.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <map>
#include <mutex>
#if defined(__SWITCH__)
#include <switch.h>
#endif

REXCVAR_DEFINE_UINT32(p2_applet_memory_mb, 0, "Prototype2",
                      "Borrow this many MB from the applet pool (helper p2mem) for NVK GPU memory; 0 = off");

namespace {
constexpr uint64_t kHelperProgramId = 0x010000000050324DULL;
constexpr size_t kMinRedirect = 0x10000;  // 64 KiB: the NVK bind granularity; smaller stays in the heap
constexpr size_t kGranule = 0x1000;

std::mutex g_mutex;
uint8_t* g_base = nullptr;
size_t g_size = 0;
std::atomic<int> g_state{0};  // 0 off, 1 mapped (not checked yet), 2 usable, -1 refused
std::map<size_t, size_t> g_free;  // offset -> length
std::map<size_t, size_t> g_used;  // offset -> length
std::atomic<uint64_t> g_used_bytes{0}, g_peak_bytes{0}, g_fallbacks{0}, g_allocs{0};

bool InRegion(const void* p) {
  return g_base && static_cast<const uint8_t*>(p) >= g_base && static_cast<const uint8_t*>(p) < g_base + g_size;
}

void* AllocLocked(size_t align, size_t size) {
  size = (size + kGranule - 1) & ~(kGranule - 1);
  for (auto it = g_free.begin(); it != g_free.end(); ++it) {
    const uintptr_t start = reinterpret_cast<uintptr_t>(g_base) + it->first;
    const uintptr_t aligned = (start + align - 1) & ~(uintptr_t(align) - 1);
    const size_t pad = aligned - start;
    if (it->second < pad + size) continue;
    const size_t off = it->first, len = it->second;
    g_free.erase(it);
    if (pad) g_free.emplace(off, pad);
    if (len > pad + size) g_free.emplace(off + pad + size, len - pad - size);
    g_used.emplace(off + pad, size);
    const uint64_t now = g_used_bytes.fetch_add(size, std::memory_order_relaxed) + size;
    if (now > g_peak_bytes.load(std::memory_order_relaxed)) g_peak_bytes.store(now, std::memory_order_relaxed);
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    return g_base + off + pad;
  }
  return nullptr;
}

void FreeLocked(void* p) {
  const size_t off = static_cast<uint8_t*>(p) - g_base;
  auto u = g_used.find(off);
  if (u == g_used.end()) return;  // not ours: never pass a borrowed address to the heap
  size_t start = off, len = u->second;
  g_used.erase(u);
  g_used_bytes.fetch_sub(len, std::memory_order_relaxed);
  auto next = g_free.lower_bound(start);
  if (next != g_free.end() && next->first == start + len) {
    len += next->second;
    next = g_free.erase(next);
  }
  if (next != g_free.begin()) {
    auto prev = std::prev(next);
    if (prev->first + prev->second == start) {
      start = prev->first;
      len += prev->second;
      g_free.erase(prev);
    }
  }
  g_free.emplace(start, len);
}

#if defined(__SWITCH__)
// The game ships the helper next to the NRO; Atmosphère's loader only starts SD programs from
// atmosphere/contents/<id>/exefs.nsp, so copy it there when missing or different.
bool InstallHelper() {
  static const char* kSrc = "sdmc:/switch/Prototype2/p2mem.nsp";
  static const char* kDir1 = "sdmc:/atmosphere/contents/010000000050324D";
  static const char* kDst = "sdmc:/atmosphere/contents/010000000050324D/exefs.nsp";
  FILE* in = std::fopen(kSrc, "rb");
  if (!in) {
    REXLOG_WARN("p2 applet memory: {} missing", kSrc);
    return false;
  }
  std::string src, dst;
  char buf[16384];
  for (size_t n; (n = std::fread(buf, 1, sizeof(buf), in)) > 0;) src.append(buf, n);
  std::fclose(in);
  if (FILE* f = std::fopen(kDst, "rb")) {
    for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) dst.append(buf, n);
    std::fclose(f);
  }
  if (dst == src) return true;
  mkdir("sdmc:/atmosphere/contents", 0777);
  mkdir(kDir1, 0777);
  FILE* out = std::fopen(kDst, "wb");
  if (!out) return false;
  const bool ok = std::fwrite(src.data(), 1, src.size(), out) == src.size();
  std::fclose(out);
  REXLOG_INFO("p2 applet memory: helper installed to {} ({} bytes, ok {})", kDst, src.size(), ok);
  return ok;
}

Service g_service;
TransferMemory g_tmem;
#endif
}  // namespace

// Called from OnPreSetup, before the graphics system creates the Vulkan device.
void prototype2_InitAppletMemory() {
#if defined(__SWITCH__)
  const uint32_t mb = REXCVAR_GET(p2_applet_memory_mb);
  if (mb == 0) return;
  if (!InstallHelper()) return;
  Result rc = pmshellInitialize();
  if (R_FAILED(rc)) {
    REXLOG_WARN("p2 applet memory: pm:shell unavailable (0x{:X})", rc);
    return;
  }
  const NcmProgramLocation location = {kHelperProgramId, NcmStorageId_None, {}};
  u64 pid = 0;
  rc = pmshellLaunchProgram(0, &location, &pid);
  pmshellExit();
  REXLOG_INFO("p2 applet memory: helper launch 0x{:X} pid {}", rc, pid);
  rc = MAKERESULT(Module_Libnx, LibnxError_NotFound);
  for (int i = 0; i < 60 && R_FAILED(rc); ++i) {
    rc = smGetService(&g_service, "p2:mem");
    if (R_FAILED(rc)) svcSleepThread(50000000);
  }
  if (R_FAILED(rc)) {
    REXLOG_WARN("p2 applet memory: service p2:mem not reachable (0x{:X})", rc);
    return;
  }
  const u64 want = uint64_t(mb) << 20;
  u64 granted = 0;
  Handle handle = INVALID_HANDLE;
  rc = serviceDispatchInOut(&g_service, 0, want, granted, .out_handle_attrs = {SfOutHandleAttr_HipcCopy},
                            .out_handles = &handle);
  if (R_FAILED(rc) || granted == 0 || handle == INVALID_HANDLE) {
    REXLOG_WARN("p2 applet memory: helper refused {} MB (0x{:X})", mb, rc);
    serviceClose(&g_service);
    return;
  }
  // The helper keeps no access (Perm_None), so the map must ask for none as well: the kernel then maps the
  // pages read-write for us (MemType_TransferMemIsolated). Asking for Perm_Rw fails with 0xFA01 (invalid state).
  tmemLoadRemote(&g_tmem, handle, granted, Perm_None);
  rc = tmemMap(&g_tmem);
  if (R_FAILED(rc)) {
    REXLOG_WARN("p2 applet memory: map of {} MB failed (0x{:X})", granted >> 20, rc);
    tmemClose(&g_tmem);
    serviceClose(&g_service);
    return;
  }
  // The session stays open for the life of the game: closing it would end the helper and with it the pages.
  std::lock_guard<std::mutex> lock(g_mutex);
  g_base = static_cast<uint8_t*>(g_tmem.map_addr);
  g_size = granted;
  g_free.emplace(0, granted);
  g_state.store(1, std::memory_order_release);
  REXLOG_INFO("p2 applet memory: {} MB of the applet pool mapped at {}", granted >> 20, (void*)g_base);
#endif
}

#if defined(__SWITCH__)
// First redirected request: prove the kernel lets the GPU map these pages and lets NVK make them uncached,
// as nouveau_horizon_memory_nvmap_create does. Called with g_mutex held.
static bool CheckRegionLocked() {
  void* p = AllocLocked(0x10000, 0x10000);
  if (!p) return false;
  NvMap map;
  Result rc = nvMapCreate(&map, p, 0x10000, 0x10000, NvKind_Pitch, false);
  Result attr = 0;
  if (R_SUCCEEDED(rc)) {
    attr = svcSetMemoryAttribute(p, 0x10000, 8, 8);
    if (R_SUCCEEDED(attr)) svcSetMemoryAttribute(p, 0x10000, 8, 0);
    nvMapClose(&map);
  }
  FreeLocked(p);
  g_allocs.store(0, std::memory_order_relaxed);
  g_peak_bytes.store(0, std::memory_order_relaxed);
  const bool ok = R_SUCCEEDED(rc) && R_SUCCEEDED(attr);
  REXLOG_INFO("p2 applet memory: GPU map check nvmap 0x{:X} attribute 0x{:X} -> {}", rc, attr,
              ok ? "in use" : "not usable, heap only");
  return ok;
}
#endif

extern "C" void* p2_nvk_memalign(size_t align, size_t size) {
  const int state = g_state.load(std::memory_order_acquire);
  if (state > 0 && size >= kMinRedirect && align >= kGranule && align <= 0x200000 && (align & (align - 1)) == 0) {
    std::lock_guard<std::mutex> lock(g_mutex);
#if defined(__SWITCH__)
    if (g_state.load(std::memory_order_relaxed) == 1) g_state.store(CheckRegionLocked() ? 2 : -1);
#endif
    if (g_state.load(std::memory_order_relaxed) == 2) {
      if (void* p = AllocLocked(align, size)) return p;
      g_fallbacks.fetch_add(1, std::memory_order_relaxed);
    }
  }
  return memalign(align, size);
}

extern "C" void p2_nvk_free(void* p) {
  if (p && InRegion(p)) {
    std::lock_guard<std::mutex> lock(g_mutex);
    FreeLocked(p);
    return;
  }
  free(p);
}

// For logs/rex/physical_pools.csv.
extern "C" void p2_applet_memory_stats(uint64_t* size, uint64_t* used, uint64_t* peak, uint64_t* fallbacks,
                                       int* state) {
  *size = g_size;
  *used = g_used_bytes.load(std::memory_order_relaxed);
  *peak = g_peak_bytes.load(std::memory_order_relaxed);
  *fallbacks = g_fallbacks.load(std::memory_order_relaxed);
  *state = g_state.load(std::memory_order_relaxed);
}
