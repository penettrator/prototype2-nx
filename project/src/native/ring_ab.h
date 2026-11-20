// SPDX-License-Identifier: GPL-3.0-or-later
// Prototype 2 PERF pass 86: in-run A/B of the pass 75 ring optimizations (p2_native_fast_constant_writes,
// p2_native_lookup_front). With p2_native_ring_ab_period_s > 0, odd periods of the system clock (the same clock
// as logs/rex/frame_pacing.csv tick_s) run the original paths. Updated once per frame by the renderer's Swap;
// both optimizations are stateless between calls, so switching between them at a frame boundary is exact.
#pragma once
#include <atomic>
#include <cstdint>

namespace p2::ringab {
inline std::atomic<bool> g_off{false};
inline void Update(uint32_t period) {
  if (!period) {
    g_off.store(false, std::memory_order_relaxed);
    return;
  }
#if defined(__aarch64__)
  uint64_t tick, freq;
  __asm__ volatile("mrs %0, cntpct_el0" : "=r"(tick));
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
  g_off.store(freq && ((tick / freq / period) & 1), std::memory_order_relaxed);
#endif
}
inline bool Off() { return g_off.load(std::memory_order_relaxed); }

// PERF pass 87: second channel for the never-measured pass 64/65 flags (p2_native_merge_stencil_faces on the
// record thread, p2_skin_scan_neon on the simulation thread). p2_native_set87_ab_period_s > 0: odd periods run the
// original paths. Same clock, updated with the first channel.
inline std::atomic<bool> g_off87{false};
inline void Update87(uint32_t period) {
  if (!period) {
    g_off87.store(false, std::memory_order_relaxed);
    return;
  }
#if defined(__aarch64__)
  uint64_t tick, freq;
  __asm__ volatile("mrs %0, cntpct_el0" : "=r"(tick));
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
  g_off87.store(freq && ((tick / freq / period) & 1), std::memory_order_relaxed);
#endif
}
inline bool Off87() { return g_off87.load(std::memory_order_relaxed); }
}  // namespace p2::ringab
