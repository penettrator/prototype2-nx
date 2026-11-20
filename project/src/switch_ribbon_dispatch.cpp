// SPDX-License-Identifier: GPL-3.0-or-later
// PERF pass 77: dispatch for the register-local copy of sub_8275E060 (ribbon/tube vertex generation, job 8275F318).
// tools/prepare_ribbon_locals.py adds the copy p2_ribbon_locals_8275E060 and routes the one direct call site here,
// both only under P2_RIBBON_LOCALS (switch/CMakeLists.txt). The copy is the same function with ctx registers held in
// locals and synchronised before/after each call and at the return (data-flow analyses in the tool), so either
// path gives the same guest state; the choice may change per call.
//
// p2_ribbon_locals (off by default) selects the copy. p2_ribbon_ab_period_s > 0 alternates per system-clock period
// (odd periods: copy, even: original) and both paths are timed, so one ordinary run compares them on the same
// scenes; every 60 s the cumulative calls and time per call of both paths are logged ([p2-ribbon]).
#include "generated/default/prototype2_pch.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <atomic>
#include <cstdint>
#include <utility>

REXCVAR_DEFINE_BOOL(p2_ribbon_locals, false, "Prototype2",
    "PERF77: sub_8275E060 (ribbon geometry) through its register-local copy; restart required")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_UINT32(p2_ribbon_ab_period_s, 0, "Prototype2",
    "PERF77: with p2_ribbon_locals, alternate copy (odd periods) and original (even) every this many seconds and "
    "log the time per call of both; 0 = copy always, untimed")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if defined(P2_RIBBON_LOCALS)
DECLARE_REX_FUNC(sub_8275E060);
extern "C" void p2_ribbon_locals_8275E060(PPCContext& __restrict ctx, uint8_t* base);

namespace {
struct Totals {
  std::atomic<uint64_t> calls{0}, ticks{0};
};
Totals g_totals[2];  // 0 = original, 1 = register-local copy
std::atomic<uint64_t> g_next_log{0};
int8_t g_mode = -1;  // -1 unread, 0 original, 1 copy, 2 A/B
uint64_t g_period_ticks = 0, g_freq = 1;

inline uint64_t Ticks() {
#if defined(__aarch64__)
  uint64_t t;
  asm volatile("mrs %0, cntpct_el0" : "=r"(t));
  return t;
#else
  return 0;
#endif
}

void Init() {
#if defined(__aarch64__)
  asm volatile("mrs %0, cntfrq_el0" : "=r"(g_freq));
  if (!g_freq) g_freq = 1;
#endif
  const bool on = REXCVAR_GET(p2_ribbon_locals);
  const uint32_t period = REXCVAR_GET(p2_ribbon_ab_period_s);
  g_period_ticks = uint64_t(period) * g_freq;
  const int8_t mode = !on ? 0 : (period && g_period_ticks) ? 2 : 1;
  REXLOG_WARN("[p2-ribbon] PERF77 register-local copy={} ab_period_s={}", on, on ? period : 0);
  __atomic_store_n(&g_mode, mode, __ATOMIC_RELEASE);
}

void MaybeLog(uint64_t now) {
  uint64_t next = g_next_log.load(std::memory_order_relaxed);
  if (now < next) return;
  if (!g_next_log.compare_exchange_strong(next, now + 60 * g_freq, std::memory_order_relaxed)) return;
  if (!next) return;  // first call only arms the timer
  auto line = [](const Totals& t, double freq) {
    const uint64_t c = t.calls.load(std::memory_order_relaxed), k = t.ticks.load(std::memory_order_relaxed);
    return std::pair<uint64_t, double>(c, c ? double(k) * 1e9 / freq / double(c) : 0.0);
  };
  const auto o = line(g_totals[0], double(g_freq)), l = line(g_totals[1], double(g_freq));
  REXLOG_WARN("[p2-ribbon] original calls={} ns/call={:.0f} | locals calls={} ns/call={:.0f} | locals/original={:.4f}",
              o.first, o.second, l.first, l.second, o.second > 0 ? l.second / o.second : 0.0);
}
}  // namespace

extern "C" void p2_ribbon_dispatch_8275E060(PPCContext& __restrict ctx, uint8_t* base) {
  int8_t mode = __atomic_load_n(&g_mode, __ATOMIC_ACQUIRE);
  if (mode < 0) {
    static const bool once = (Init(), true);
    (void)once;
    mode = __atomic_load_n(&g_mode, __ATOMIC_ACQUIRE);
  }
  if (mode == 0) {
    P2_DIRECT_CALL(sub_8275E060)(ctx, base);
    return;
  }
  if (mode == 1) {
    p2_ribbon_locals_8275E060(ctx, base);
    return;
  }
  const uint64_t start = Ticks();
  const int path = int((start / g_period_ticks) & 1);
  if (path) {
    p2_ribbon_locals_8275E060(ctx, base);
  } else {
    P2_DIRECT_CALL(sub_8275E060)(ctx, base);
  }
  const uint64_t end = Ticks();
  g_totals[path].ticks.fetch_add(end - start, std::memory_order_relaxed);
  g_totals[path].calls.fetch_add(1, std::memory_order_relaxed);
  MaybeLog(end);
}
#endif  // P2_RIBBON_LOCALS
