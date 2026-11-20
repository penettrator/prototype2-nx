// SPDX-License-Identifier: GPL-3.0-or-later
// PERF pass 79: dispatch for the register-local copies of the collision pair subtree under job 829AC940 (Main's
// critical path). tools/prepare_pair_locals.py adds p2_pair_locals_<fn> for sub_829A09C0, sub_8299F360,
// sub_829A0328, sub_8299FF70, sub_8299FE00 (leaves already use P2_LEAF_LOCALS) and routes sub_8299F1B0's promoted type call
// (roots 829A09C0 / 8299F360) here, all under P2_PAIR_LOCALS (switch/CMakeLists.txt). The copies are the same
// functions with ctx registers held in locals and synchronised at every call and return, so either path leaves the
// same guest state and the choice may change per call.
//
// p2_pair_locals (off by default) selects the copies. p2_pair_ab_period_s > 0 alternates per system-clock period
// (odd periods: copies, even: originals). One call in eight (by the low bits of the counter) is timed, per root and
// path; every 60 s the cumulative sampled calls and time per call are logged ([p2-pair]).
#include "generated/default/prototype2_pch.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <atomic>
#include <cstdint>

REXCVAR_DEFINE_BOOL(p2_pair_locals, false, "Prototype2",
    "PERF79: collision pair subtree of job 829AC940 through its register-local copies; restart required")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_UINT32(p2_pair_ab_period_s, 0, "Prototype2",
    "PERF79: with p2_pair_locals, alternate copies (odd periods) and originals (even) every this many seconds and "
    "log sampled time per call of both; 0 = copies always, untimed")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if defined(P2_PAIR_LOCALS)
DECLARE_REX_FUNC(sub_829A09C0);
DECLARE_REX_FUNC(sub_8299F360);
extern "C" void p2_pair_locals_sub_829A09C0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void p2_pair_locals_sub_8299F360(PPCContext& __restrict ctx, uint8_t* base);

namespace {
struct Totals {
  std::atomic<uint64_t> calls{0}, ticks{0};
};
Totals g_totals[2][2];  // [root][path]: root 0 = 829A09C0, 1 = 8299F360; path 0 = original, 1 = copy
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
  const bool on = REXCVAR_GET(p2_pair_locals);
  const uint32_t period = REXCVAR_GET(p2_pair_ab_period_s);
  g_period_ticks = uint64_t(period) * g_freq;
  const int8_t mode = !on ? 0 : (period && g_period_ticks) ? 2 : 1;
  REXLOG_WARN("[p2-pair] PERF79 register-local copies={} ab_period_s={}", on, on ? period : 0);
  __atomic_store_n(&g_mode, mode, __ATOMIC_RELEASE);
}

inline int8_t Mode() {
  int8_t mode = __atomic_load_n(&g_mode, __ATOMIC_ACQUIRE);
  if (mode < 0) {
    static const bool once = (Init(), true);
    (void)once;
    mode = __atomic_load_n(&g_mode, __ATOMIC_ACQUIRE);
  }
  return mode;
}

void MaybeLog(uint64_t now) {
  uint64_t next = g_next_log.load(std::memory_order_relaxed);
  if (now < next) return;
  if (!g_next_log.compare_exchange_strong(next, now + 60 * g_freq, std::memory_order_relaxed)) return;
  if (!next) return;  // first sample only arms the timer
  double per[2][2];
  uint64_t calls[2][2];
  for (int r = 0; r < 2; ++r)
    for (int p = 0; p < 2; ++p) {
      calls[r][p] = g_totals[r][p].calls.load(std::memory_order_relaxed);
      const uint64_t k = g_totals[r][p].ticks.load(std::memory_order_relaxed);
      per[r][p] = calls[r][p] ? double(k) * 1e9 / double(g_freq) / double(calls[r][p]) : 0.0;
    }
  REXLOG_WARN("[p2-pair] 829A09C0 original {} x {:.0f} ns | locals {} x {:.0f} ns ({:.4f}); 8299F360 original {} x "
              "{:.0f} ns | locals {} x {:.0f} ns ({:.4f}) [sampled 1/8]",
              calls[0][0], per[0][0], calls[0][1], per[0][1], per[0][0] > 0 ? per[0][1] / per[0][0] : 0.0,
              calls[1][0], per[1][0], calls[1][1], per[1][1], per[1][0] > 0 ? per[1][1] / per[1][0] : 0.0);
}

template <int Root>
inline void Dispatch(PPCContext& __restrict ctx, uint8_t* base) {
  const int8_t mode = Mode();
  auto original = [&] {
    if constexpr (Root == 0) P2_DIRECT_CALL(sub_829A09C0)(ctx, base);
    else P2_DIRECT_CALL(sub_8299F360)(ctx, base);
  };
  auto copy = [&] {
    if constexpr (Root == 0) p2_pair_locals_sub_829A09C0(ctx, base);
    else p2_pair_locals_sub_8299F360(ctx, base);
  };
  if (mode == 0) { original(); return; }
  if (mode == 1) { copy(); return; }
  const uint64_t start = Ticks();
  const int path = int((start / g_period_ticks) & 1);
  if (path) copy(); else original();
  if ((start & 7) != 0) return;
  const uint64_t end = Ticks();
  g_totals[Root][path].ticks.fetch_add(end - start, std::memory_order_relaxed);
  g_totals[Root][path].calls.fetch_add(1, std::memory_order_relaxed);
  MaybeLog(end);
}
}  // namespace

extern "C" void p2_pair_dispatch_sub_829A09C0(PPCContext& __restrict ctx, uint8_t* base) { Dispatch<0>(ctx, base); }
extern "C" void p2_pair_dispatch_sub_8299F360(PPCContext& __restrict ctx, uint8_t* base) { Dispatch<1>(ctx, base); }
#endif  // P2_PAIR_LOCALS
