// Prototype 2 (Switch), PERF pass 13: census of the guest's indirect calls (tools/p2_indirect_calls.py).
// Only a build with -DP2_ICALL_CENSUS=1 on the generated code calls p2_icall_census (from REX_CALL_INDIRECT_FUNC):
// one count per (call site = return address, target). A thread of its own writes the whole table every 30 s to
// logs/rex/icall_census.csv (rewritten: site,target,count, cumulative since start).
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <sys/stat.h>

extern "C" bool RexSwitchSetCurrentThreadMask(uint32_t mask);
extern "C" void RexSwitchSetCurrentThreadPriority(int priority);

namespace {
constexpr uint32_t kSlots = 1u << 17;
std::atomic<uint64_t> g_keys[kSlots];
std::atomic<uint64_t> g_counts[kSlots];
std::atomic<uint64_t> g_dropped{0};
std::atomic<bool> g_started{false};

void Writer() {
#if defined(__SWITCH__)
  RexSwitchSetCurrentThreadMask(0x6);
  RexSwitchSetCurrentThreadPriority(0x3C);
#endif
  mkdir("sdmc:/switch/Prototype2/logs", 0777);
  mkdir("sdmc:/switch/Prototype2/logs/rex", 0777);
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(30));
    if (FILE* f = std::fopen("sdmc:/switch/Prototype2/logs/rex/icall_census.csv", "wb")) {
      std::fprintf(f, "site,target,count\n");
      for (uint32_t i = 0; i < kSlots; ++i) {
        const uint64_t k = g_keys[i].load(std::memory_order_relaxed);
        if (!k) continue;
        std::fprintf(f, "%08X,%08X,%llu\n", uint32_t(k >> 32), uint32_t(k),
                     (unsigned long long)g_counts[i].load(std::memory_order_relaxed));
      }
      std::fprintf(f, "00000000,00000000,%llu\n", (unsigned long long)g_dropped.load());
      std::fclose(f);
    }
  }
}
}  // namespace

extern "C" void p2_icall_census(uint32_t site, uint32_t target) {
  if (!g_started.load(std::memory_order_relaxed) && !g_started.exchange(true)) {
    new std::thread(Writer);  // never joined or destroyed (libnx has no pthread_detach)
  }
  const uint64_t key = (uint64_t(site) << 32) | target;
  if (!key) return;
  uint64_t h = key * 0x9E3779B97F4A7C15ull;
  uint32_t i = uint32_t(h >> 47);
  for (uint32_t probe = 0; probe < 64; ++probe, i = (i + 1) & (kSlots - 1)) {
    uint64_t k = g_keys[i].load(std::memory_order_relaxed);
    if (k == key) {
      g_counts[i].fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (k == 0) {
      uint64_t expected = 0;
      if (g_keys[i].compare_exchange_strong(expected, key) || expected == key) {
        g_counts[i].fetch_add(1, std::memory_order_relaxed);
        return;
      }
    }
  }
  g_dropped.fetch_add(1, std::memory_order_relaxed);
}
