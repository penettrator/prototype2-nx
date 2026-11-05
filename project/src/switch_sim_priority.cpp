// Prototype 2 (Switch): priority of the game's simulation thread.
//
// run49/run52 profiles: the three cores are ~93 % busy (281 % of 300 %) and the thread that runs the game's main
// loop (sub_821A84F8 -> sub_8231E4B8 every frame) is the critical path, yet it got only ~70 % of a core: it ran at
// the same Horizon priority (0x3B) as the ~16 job workers that compete with it. The patched generated code calls
// p2_on_sim_thread_start() once when that loop starts; this raises that one thread above the other guest threads
// (still below the host present/audio threads at 0x2C/0x2B), so the frame's serial work is not time-sliced away.
#include "generated/default/prototype2_pch.h"
#include <chrono>
#include <thread>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <sys/stat.h>
#include <mutex>
#include <atomic>
#if defined(__SWITCH__)
#include <switch.h>
#endif
#include <rex/cvar.h>
#include <rex/logging.h>

extern "C" bool RexSwitchSetCurrentThreadPriorityOk(int priority);  // SDK threading_switch
extern "C" bool RexSwitchPinCurrentThreadToCore(int core);           // SDK threading_switch

REXCVAR_DEFINE_UINT32(p2_sim_thread_priority, 0x3A, "Prototype2",
                      "Horizon priority for the game's main simulation thread (lower = higher priority; guest "
                      "threads run at 0x3B). 0 = leave it at 0x3B")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(p2_sim_thread_core, 0, "Prototype2",
    "Core the game's main simulation thread is pinned to (the host service threads keep off it, "
    "p2_host_core_mask). -1 = not pinned")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// PERF pass 4 (perf_results/pass_2): lwarx..stwcx regions of the functions in tools/prepare_hot2_free.py skip the
// SDK global mutex when this is true. Set once in OnPreSetup (prototype2_app.h), only with p2_msr_lock_mode = 5.
REXCVAR_DEFINE_BOOL(p2_msr_hot2_lockfree, false, "Prototype2",
    "With p2_msr_lock_mode = 5: also run the msr regions of the second hot set (tools/prepare_hot2_free.py) "
    "without the SDK global mutex; restart required")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
bool p2_msr_hot2_free = false;
bool p2_msr_hot2_allowed = false;  // mode 5 and p2_msr_hot2_lockfree, set with p2_msr_hot2_free in OnPreSetup
// pass 8: the main loop starts ~1 s after launch, inside the window of the pass-7 start-up hang (B-004), so the
// set is switched on only p2_msr_hot2_delay_s after it (from the per-frame hook below).
REXCVAR_DEFINE_UINT32(p2_msr_hot2_delay_s, 20, "Prototype2",
    "Seconds after the game's main loop starts before the hot2 msr set runs lock-free (boot keeps the lock)");
static std::chrono::steady_clock::time_point p2_msr_hot2_since{};
REXCVAR_DEFINE_UINT32(p2_msr_hot2_ab_period_s, 0, "Prototype2",
    "Measurement: with p2_msr_hot2_lockfree, alternate lock-free (even periods) and locked (odd periods) every "
    "this many seconds of the system clock (frame_pacing.csv tick_s); 0 = always lock-free");
// Switched once per frame on the simulation thread. Safe at any moment: a region that took the lock releases it
// by its own thread-local flag (REX_LEAVE_GLOBAL_LOCK), whatever this flag says by then.
// PERF pass 16: third lock-free set (tools/prepare_hot3_free.py), from the global-mutex contention census. Armed
// with hot2 (same delay after the main loop starts); in-run A/B with p2_msr_hot3_ab_period_s (odd periods locked).
REXCVAR_DEFINE_BOOL(p2_msr_hot3_lockfree, false, "Prototype2",
    "With p2_msr_lock_mode = 5 and hot2: also run the msr regions of the third set lock-free")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_UINT32(p2_msr_hot3_ab_period_s, 0, "Prototype2",
    "Measurement: alternate hot3 lock-free (even periods) and locked (odd periods) every this many seconds; 0 = off");
bool p2_msr_hot3_free = false;
// PERF pass 17: fourth lock-free set (tools/prepare_hot4_free.py), same arming, own A/B period.
REXCVAR_DEFINE_BOOL(p2_msr_hot4_lockfree, false, "Prototype2",
    "With p2_msr_lock_mode = 5 and hot2: also run the msr regions of the fourth set lock-free")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_UINT32(p2_msr_hot4_ab_period_s, 0, "Prototype2",
    "Measurement: alternate hot4 lock-free (even periods) and locked (odd periods) every this many seconds; 0 = off");
bool p2_msr_hot4_free = false;
static void p2_msr_hot2_frame() {
  if (!p2_msr_hot2_allowed || p2_msr_hot2_since == std::chrono::steady_clock::time_point{}) return;
  static bool armed = false;
  if (!armed) {
    if (std::chrono::steady_clock::now() - p2_msr_hot2_since <
        std::chrono::seconds(REXCVAR_GET(p2_msr_hot2_delay_s)))
      return;
    armed = true;
    p2_msr_hot2_free = true;
    REXLOG_WARN("[sched] msr hot2 set lock-free {} s after the main loop start", REXCVAR_GET(p2_msr_hot2_delay_s));
  }
  if (REXCVAR_GET(p2_msr_hot3_lockfree)) {
    const uint32_t p3 = REXCVAR_GET(p2_msr_hot3_ab_period_s);
    if (!p3) {
      p2_msr_hot3_free = true;
    } else {
#if defined(__SWITCH__) && defined(__aarch64__)
      uint64_t tick3, freq3;
      __asm__ volatile("mrs %0, cntpct_el0" : "=r"(tick3));
      __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq3));
      const uint64_t s3 = freq3 ? tick3 / freq3 : 0;
#else
      const uint64_t s3 = 0;
#endif
      p2_msr_hot3_free = ((s3 / p3) & 1) == 0;
    }
  }
  if (REXCVAR_GET(p2_msr_hot4_lockfree)) {
    const uint32_t p4 = REXCVAR_GET(p2_msr_hot4_ab_period_s);
    if (!p4) {
      p2_msr_hot4_free = true;
    } else {
#if defined(__SWITCH__) && defined(__aarch64__)
      uint64_t tick4, freq4;
      __asm__ volatile("mrs %0, cntpct_el0" : "=r"(tick4));
      __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq4));
      const uint64_t s4 = freq4 ? tick4 / freq4 : 0;
#else
      const uint64_t s4 = 0;
#endif
      p2_msr_hot4_free = ((s4 / p4) & 1) == 0;
    }
  }
  const uint32_t period = REXCVAR_GET(p2_msr_hot2_ab_period_s);
  if (!period) return;
#if defined(__SWITCH__) && defined(__aarch64__)
  uint64_t tick, freq;
  __asm__ volatile("mrs %0, cntpct_el0" : "=r"(tick));
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
  const uint64_t seconds = freq ? tick / freq : 0;
#else
  const uint64_t seconds = uint64_t(std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
  p2_msr_hot2_free = ((seconds / period) & 1) == 0;
}

// PERF pass 12. (1) Wait histogram (SDK xobject.cpp, p2_wait_histogram): written and cleared every 30 s of the
// system clock to logs/rex/wait_hist.csv. (2) In-run A/B of the guest job workers' core mask: with
// p2_core_ab_period_s > 0, odd periods give the workers p2_guest_core_mask_alt (even periods p2_guest_core_mask);
// the simulation thread and the audio engine are pinned on their own and are not in that set.
extern "C" bool RexWaitHistogramEnabled();
extern "C" void RexWaitHistogramDump(FILE* f, uint64_t tick_s, uint32_t phase);
extern "C" void RexSwitchGuestWorkerForget(void);
extern "C" bool RexSwitchSetCurrentThreadMask(uint32_t mask);
extern "C" void RexSwitchSetCurrentThreadPriority(int priority);
extern "C" uint32_t RexSwitchApplyGuestWorkerMask(uint32_t mask);
REXCVAR_DECLARE(uint32_t, p2_guest_core_mask);
REXCVAR_DEFINE_UINT32(p2_core_ab_period_s, 0, "Prototype2",
    "Measurement: alternate the guest job workers between p2_guest_core_mask (even periods) and "
    "p2_guest_core_mask_alt (odd periods) every this many seconds of the system clock; 0 = off");
extern "C" uint32_t RexSwitchApplyGuestWorkerPriority(int priority);
REXCVAR_DEFINE_UINT32(p2_guest_worker_priority_alt, 0, "Prototype2",
    "PERF pass 20, with p2_core_ab_period_s: guest job worker priority of the odd periods (even periods 0x3B); 0 = unchanged");
REXCVAR_DEFINE_UINT32(p2_guest_core_mask_alt, 0xE, "Prototype2",
    "Guest job worker core mask of the odd periods of p2_core_ab_period_s (0xE = cores 1-3, off the "
    "simulation thread's core 0)");
static uint64_t p2_clock_seconds() {
#if defined(__SWITCH__) && defined(__aarch64__)
  uint64_t tick, freq;
  __asm__ volatile("mrs %0, cntpct_el0" : "=r"(tick));
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
  return freq ? tick / freq : 0;
#else
  return uint64_t(std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}
static void p2_msr_census_dump(FILE* f, uint64_t tick_s);  // below (PERF pass 16)
// PERF pass 18 (perf_results/pass_17 diag): in the run the simulation thread waits ~10 ms a frame for Main, and Main
// is neither running nor waiting ~20 % of the time: preempted on cores 1-2 by the host ring/record threads (0x2C).
// In-run A/B: odd periods give Main p2_main_thread_priority_alt (e.g. 0x2B, above them).
extern "C" bool RexSwitchSetMainThreadPriority(int priority);
REXCVAR_DECLARE(uint32_t, p2_main_thread_priority);
REXCVAR_DEFINE_UINT32(p2_main_prio_ab_period_s, 0, "Prototype2",
    "Measurement: alternate Main XThread priority p2_main_thread_priority / p2_main_thread_priority_alt every this "
    "many seconds of the system clock; 0 = off");
REXCVAR_DEFINE_UINT32(p2_main_thread_priority_alt, 0x2B, "Prototype2",
    "Main XThread priority of the odd periods of p2_main_prio_ab_period_s");
// PERF pass 21 (perf_results/pass_21): census of the guest job system. Main waits 20-35 % of its time on countdown
// latches (sub_82238D00, mostly from sub_82283888 <- sub_821D6620) that the job workers (sub_82C9D220) count down.
// The generated-code hooks (tools/prepare_job_census.py) time every job the workers run and every real latch wait;
// per latch event the jobs that counted it down are matched with the wait. Written every 30 s with the wait
// histogram to logs/rex/job_census.csv:
//   W,tick_s,caller_lr,job_fn,waits,wait_us,max_wait_us,jobs,run_us,span_us,late_starts,start_lag_us
//   F,tick_s,job_fn,jobs,run_us,core0,core1,core2,core3
// late_starts = waits whose first job had not started when the wait began. Off by default (p2_job_census).
REXCVAR_DEFINE_BOOL(p2_job_census_enable, false, "Prototype2",
    "Measurement (PERF pass 21): time the guest job workers' jobs and the waits on their countdown latches; "
    "written to logs/rex/job_census.csv every 30 s")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
bool p2_job_census = false;  // generated-code hooks on: census and/or boost below
// PERF pass 22 (pass_21 census: Main's batches start ~0.4 ms after its wait begins and run at 1-1.5 jobs in parallel,
// e.g. 3.5 jobs of sub_8299E8F8 per wait serialised over 6.3 ms; the workers time-share cores with jobs nobody
// waits for, sub_82A24400 alone 29 % of a core): a worker starting a job whose countdown latch some thread is
// waiting on right now runs it at p2_job_boost_priority (0x3A: above the other workers, still below every host
// thread and Main), back to 0x3B after the job. In-run A/B: with p2_job_boost_ab_period_s, odd periods only.
REXCVAR_DEFINE_UINT32(p2_job_boost_priority, 0, "Prototype2",
    "Horizon priority for a guest job whose latch is being waited on (0x3A; guest workers run at 0x3B); 0 = off")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_UINT32(p2_job_boost_ab_period_s, 0, "Prototype2",
    "Measurement: with p2_job_boost_priority, boost only in odd periods of this many seconds of the system clock; "
    "0 = always");
static bool p2_job_census_rec = false;
static std::atomic<bool> p2_job_boost_now{false};
static uint32_t p2_job_boost_prio = 0;
namespace p2job {
inline uint64_t NowNs() {
#if defined(__aarch64__)
  uint64_t tick, freq;
  __asm__ volatile("mrs %0, cntpct_el0" : "=r"(tick));
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
  return freq ? uint64_t((__uint128_t)tick * 1000000000u / freq) : 0;
#else
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}
struct Latch { uint32_t ev = 0, fn = 0, jobs = 0; uint64_t run = 0, first = 0, last = 0; };
struct Fn { uint32_t fn = 0; uint64_t jobs = 0, run = 0, core[4] = {}; };
struct Wait { uint32_t lr = 0, fn = 0; uint64_t waits = 0, wait = 0, maxw = 0, jobs = 0, run = 0, span = 0, late = 0, lag = 0; };
constexpr uint32_t kLatches = 1024, kFns = 512, kWaits = 512;
std::mutex g_m;
Latch g_latch[kLatches];
Fn g_fn[kFns];
Wait g_wait[kWaits];
uint64_t g_dropped = 0;
thread_local uint64_t t_begin = 0;
thread_local bool t_boosted = false;
constexpr uint32_t kAwaited = 32;
std::atomic<uint32_t> g_awaited[kAwaited];
std::atomic<uint64_t> g_boosts{0}, g_awaited_full{0};
inline bool Awaited(uint32_t ev) {
  for (auto& a : g_awaited) if (a.load(std::memory_order_relaxed) == ev) return true;
  return false;
}
inline uint32_t Slot(uint64_t key, uint32_t n) { return uint32_t((key * 0x9E3779B97F4A7C15ull) >> 40) & (n - 1); }
}  // namespace p2job

extern "C" uint64_t p2_job_now(void) { return p2job::NowNs(); }
extern "C" void p2_job_begin(void) { p2job::t_begin = p2job::NowNs(); }
extern "C" void p2_job_begin2(uint32_t fn, uint32_t ev) {
  using namespace p2job;
  (void)fn;
  t_begin = NowNs();
#if defined(__SWITCH__)
  if (ev && p2_job_boost_now.load(std::memory_order_relaxed) && Awaited(ev) &&
      R_SUCCEEDED(svcSetThreadPriority(CUR_THREAD_HANDLE, p2_job_boost_prio))) {
    t_boosted = true;
    g_boosts.fetch_add(1, std::memory_order_relaxed);
  }
#endif
}
extern "C" uint64_t p2_latch_begin(uint32_t ev) {
  using namespace p2job;
  if (ev && p2_job_boost_now.load(std::memory_order_relaxed)) {
    bool put = false;
    for (auto& a : g_awaited) {
      uint32_t z = 0;
      if (a.compare_exchange_strong(z, ev, std::memory_order_relaxed)) { put = true; break; }
    }
    if (!put) g_awaited_full.fetch_add(1, std::memory_order_relaxed);
  }
  return NowNs();
}
extern "C" void p2_job_end(uint32_t fn, uint32_t ev) {
  using namespace p2job;
#if defined(__SWITCH__)
  if (t_boosted) {
    t_boosted = false;
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x3B);
  }
#endif
  if (!p2_job_census_rec) return;
  const uint64_t now = NowNs(), begin = t_begin ? t_begin : now, run = now - begin;
  uint32_t core = 0;
#if defined(__SWITCH__)
  core = svcGetCurrentProcessorNumber() & 3;
#endif
  std::lock_guard<std::mutex> lock(g_m);
  uint32_t i = Slot(fn, kFns);
  for (uint32_t k = 0; k < 16; ++k, i = (i + 1) & (kFns - 1)) {
    Fn& f = g_fn[i];
    if (f.fn != fn && f.fn != 0) continue;
    f.fn = fn; ++f.jobs; f.run += run; ++f.core[core];
    break;
  }
  if (!ev) return;
  i = Slot(ev, kLatches);
  for (uint32_t k = 0; k < 16; ++k, i = (i + 1) & (kLatches - 1)) {
    Latch& l = g_latch[i];
    if (l.ev != ev && l.ev != 0) continue;
    // A latch event is reused batch after batch; a job that starts > 5 ms after the last one of this event ended
    // begins a new batch whose wait (if any) is still to come.
    if (l.ev != ev || begin > l.last + 5000000u) { l = Latch{}; l.ev = ev; l.fn = fn; l.first = begin; }
    ++l.jobs; l.run += run; if (begin < l.first) l.first = begin; if (now > l.last) l.last = now;
    return;
  }
  ++g_dropped;
}
extern "C" void p2_latch_wait(uint32_t ev, uint32_t lr, uint64_t t0) {
  using namespace p2job;
  const uint64_t now = NowNs(), w = now - t0;
  // Unregister one instance of the event (registered only while boosting; harmless when absent).
  for (auto& a : g_awaited) {
    uint32_t e = ev;
    if (a.compare_exchange_strong(e, 0, std::memory_order_relaxed)) break;
  }
  if (!p2_job_census_rec) return;
  std::lock_guard<std::mutex> lock(g_m);
  Latch got{};
  uint32_t i = Slot(ev, kLatches);
  for (uint32_t k = 0; k < 16; ++k, i = (i + 1) & (kLatches - 1)) {
    if (g_latch[i].ev != ev) continue;
    got = g_latch[i];
    // Keep the slot (open addressing) but mark it consumed: the next job of this event starts a new batch.
    g_latch[i].last = 0; g_latch[i].jobs = 0; g_latch[i].run = 0; g_latch[i].first = UINT64_MAX;
    break;
  }
  const uint64_t key = (uint64_t(lr) << 32) | got.fn;
  i = Slot(key, kWaits);
  for (uint32_t k = 0; k < 16; ++k, i = (i + 1) & (kWaits - 1)) {
    Wait& s = g_wait[i];
    if ((s.lr != lr || s.fn != got.fn) && (s.lr != 0 || s.fn != 0 || s.waits != 0)) continue;
    s.lr = lr; s.fn = got.fn; ++s.waits; s.wait += w; if (w > s.maxw) s.maxw = w;
    if (got.jobs) {
      s.jobs += got.jobs; s.run += got.run; s.span += got.last - got.first;
      if (got.first > t0) { ++s.late; s.lag += got.first - t0; }
    }
    return;
  }
  ++g_dropped;
}
static void p2_job_census_dump(FILE* f, uint64_t tick_s) {
  using namespace p2job;
  std::lock_guard<std::mutex> lock(g_m);
  for (Wait& s : g_wait) {
    if (s.waits)
      std::fprintf(f, "W,%llu,%08X,%08X,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n", (unsigned long long)tick_s, s.lr,
                   s.fn, (unsigned long long)s.waits, (unsigned long long)(s.wait / 1000),
                   (unsigned long long)(s.maxw / 1000), (unsigned long long)s.jobs, (unsigned long long)(s.run / 1000),
                   (unsigned long long)(s.span / 1000), (unsigned long long)s.late, (unsigned long long)(s.lag / 1000));
    const uint32_t lr = s.lr, fn = s.fn;
    s = Wait{}; s.lr = lr; s.fn = fn;  // keep the key: open-addressing chains stay intact
  }
  for (Fn& x : g_fn) {
    if (x.jobs)
      std::fprintf(f, "F,%llu,%08X,%llu,%llu,%llu,%llu,%llu,%llu\n", (unsigned long long)tick_s, x.fn,
                   (unsigned long long)x.jobs, (unsigned long long)(x.run / 1000), (unsigned long long)x.core[0],
                   (unsigned long long)x.core[1], (unsigned long long)x.core[2], (unsigned long long)x.core[3]);
    const uint32_t fn = x.fn;
    x = Fn{}; x.fn = fn;
  }
  if (g_dropped) std::fprintf(f, "D,%llu,%llu\n", (unsigned long long)tick_s, (unsigned long long)g_dropped);
  std::fprintf(f, "B,%llu,%llu,%llu\n", (unsigned long long)tick_s,
               (unsigned long long)g_boosts.exchange(0, std::memory_order_relaxed),
               (unsigned long long)g_awaited_full.exchange(0, std::memory_order_relaxed));
  g_dropped = 0;
}

static void p2_pass12_frame() {
  const uint64_t seconds = p2_clock_seconds();
  if (const uint32_t mp = REXCVAR_GET(p2_main_prio_ab_period_s)) {
    static int applied = -1;
    const int want = ((seconds / mp) & 1) ? int(REXCVAR_GET(p2_main_thread_priority_alt))
                                           : int(REXCVAR_GET(p2_main_thread_priority));
    if (want != applied) {
#if defined(__SWITCH__)
      const bool ok = RexSwitchSetMainThreadPriority(want);
      if (ok) applied = want;
#endif
    }
  }
  const uint32_t period = REXCVAR_GET(p2_core_ab_period_s);
  uint32_t phase = 0;
  if (period) {
    phase = uint32_t((seconds / period) & 1);
    static uint32_t applied = 2;
    if (phase != applied) {
      applied = phase;
      const uint32_t mask = phase ? REXCVAR_GET(p2_guest_core_mask_alt) : REXCVAR_GET(p2_guest_core_mask);
#if defined(__SWITCH__)
      if (const uint32_t wp = REXCVAR_GET(p2_guest_worker_priority_alt))
        RexSwitchApplyGuestWorkerPriority(phase ? int(wp) : 0x3B);  // PERF pass 20: 0x3B = SwitchPriorityFor default
      const uint32_t n = RexSwitchApplyGuestWorkerMask(mask);
      REXLOG_INFO("[sched] core A/B phase {}: {} guest workers -> mask 0x{:X}", phase, n, mask);
#endif
    }
  }
  p2_job_census_rec = REXCVAR_GET(p2_job_census_enable);
  p2_job_boost_prio = REXCVAR_GET(p2_job_boost_priority);
  if (p2_job_boost_prio) {
    const uint32_t bp = REXCVAR_GET(p2_job_boost_ab_period_s);
    const bool on = !bp || ((seconds / bp) & 1);
    if (on != p2_job_boost_now.load(std::memory_order_relaxed)) {
      // Switching off: clear the awaited set so no stale event boosts later (waits in flight unregister harmlessly).
      if (!on) for (auto& a : p2job::g_awaited) a.store(0, std::memory_order_relaxed);
      p2_job_boost_now.store(on, std::memory_order_relaxed);
    }
  }
  p2_job_census = p2_job_census_rec || p2_job_boost_prio != 0;
  if (!RexWaitHistogramEnabled() && !p2_job_census_rec) return;
  // v2: a thread of its own writes the table every 30 s of the system clock (v1 wrote it here: 0.4 s stalls).
  static bool started = false;
  if (started) return;
  started = true;
  // libnx has no pthread_detach (std::thread::detach throws, run 3 of pass 12): the thread object is leaked
  // on purpose and never joined or destroyed.
  new std::thread([] {
#if defined(__SWITCH__)
    RexSwitchSetCurrentThreadMask(0x6);  // off the simulation thread's core 0
    RexSwitchSetCurrentThreadPriority(0x3C);
#endif
    mkdir("sdmc:/switch/Prototype2/logs", 0777);
    mkdir("sdmc:/switch/Prototype2/logs/rex", 0777);
    uint64_t last_window = p2_clock_seconds() / 30;
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      const uint64_t seconds = p2_clock_seconds();
      if (seconds / 30 == last_window) continue;
      last_window = seconds / 30;
      const uint32_t period = REXCVAR_GET(p2_core_ab_period_s);
      const uint32_t phase = period ? uint32_t((seconds / period) & 1) : 0;
      if (p2_job_census_rec) {
        if (FILE* f = std::fopen("sdmc:/switch/Prototype2/logs/rex/job_census.csv", "ab")) {
          p2_job_census_dump(f, seconds);
          std::fclose(f);
        }
      }
      if (!RexWaitHistogramEnabled()) continue;
      if (FILE* f = std::fopen("sdmc:/switch/Prototype2/logs/rex/wait_hist.csv", "ab")) {
        RexWaitHistogramDump(f, seconds, phase);
        std::fclose(f);
      }
      if (FILE* f = std::fopen("sdmc:/switch/Prototype2/logs/rex/msr_contention.csv", "ab")) {
        p2_msr_census_dump(f, seconds);
        std::fclose(f);
      }
    }
  });
}

// PERF pass 16: contention census of the SDK global mutex in mode-5 msr regions (pch REX_ENTER_ADDR_LOCK). Only
// contended acquisitions get here: the wait is timed and charged to the call site (host return address; symbolize
// with the NRO's nm) and to the simulation thread separately. Written with the wait histogram every 30 s
// (logs/rex/msr_contention.csv: tick_s,site,count,total_us,max_us,sim_us), then cleared.
namespace p2msr {
constexpr uint32_t kSites = 2048;
struct Site {
  std::atomic<uint64_t> pc{0};
  std::atomic<uint64_t> count{0}, total_ns{0}, max_ns{0}, sim_ns{0};
};
Site g_sites[kSites];
std::atomic<uint64_t> g_dropped{0};
thread_local bool t_is_sim = false;
inline uint64_t NowNs() {
#if defined(__aarch64__)
  uint64_t tick, freq;
  __asm__ volatile("mrs %0, cntpct_el0" : "=r"(tick));
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
  return freq ? uint64_t((__uint128_t)tick * 1000000000u / freq) : 0;
#else
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}
}  // namespace p2msr

extern "C" [[gnu::noinline]] void rex_p2_msr_slow_(void) {
  using namespace p2msr;
  const uint64_t pc = uint64_t(__builtin_return_address(0));
  const uint64_t t0 = NowNs();
  rex::thread::global_critical_region::mutex().lock();
  const uint64_t ns = NowNs() - t0;
  uint64_t h = pc * 0x9E3779B97F4A7C15ull;
  uint32_t i = uint32_t(h >> 53) & (kSites - 1);
  for (uint32_t k = 0; k < 32; ++k, i = (i + 1) & (kSites - 1)) {
    uint64_t cur = g_sites[i].pc.load(std::memory_order_relaxed);
    if (cur == 0) {
      uint64_t expected = 0;
      if (!g_sites[i].pc.compare_exchange_strong(expected, pc) && expected != pc) continue;
      cur = pc;
    }
    if (cur != pc) continue;
    Site& s = g_sites[i];
    s.count.fetch_add(1, std::memory_order_relaxed);
    s.total_ns.fetch_add(ns, std::memory_order_relaxed);
    if (t_is_sim) s.sim_ns.fetch_add(ns, std::memory_order_relaxed);
    uint64_t m = s.max_ns.load(std::memory_order_relaxed);
    while (ns > m && !s.max_ns.compare_exchange_weak(m, ns, std::memory_order_relaxed)) {}
    return;
  }
  g_dropped.fetch_add(1, std::memory_order_relaxed);
}

static void p2_msr_census_dump(FILE* f, uint64_t tick_s) {
  using namespace p2msr;
  for (auto& s : g_sites) {
    const uint64_t pc = s.pc.load(std::memory_order_relaxed);
    if (!pc) continue;
    const uint64_t n = s.count.exchange(0, std::memory_order_relaxed);
    const uint64_t t = s.total_ns.exchange(0, std::memory_order_relaxed);
    const uint64_t m = s.max_ns.exchange(0, std::memory_order_relaxed);
    const uint64_t sim = s.sim_ns.exchange(0, std::memory_order_relaxed);
    if (n) std::fprintf(f, "%llu,%llx,%llu,%llu,%llu,%llu\n", (unsigned long long)tick_s, (unsigned long long)pc,
                        (unsigned long long)n, (unsigned long long)(t / 1000), (unsigned long long)(m / 1000),
                        (unsigned long long)(sim / 1000));
  }
  const uint64_t d = g_dropped.exchange(0, std::memory_order_relaxed);
  if (d) std::fprintf(f, "%llu,0,%llu,0,0,0\n", (unsigned long long)tick_s, (unsigned long long)d);
}

extern "C" void p2_on_sim_thread_start() {
  static thread_local bool done = false;
  if (done) return;
  done = true;
  p2msr::t_is_sim = true;
#if defined(__SWITCH__)
  RexSwitchGuestWorkerForget();  // pinned below, not one of the job workers
#endif
  if (p2_msr_hot2_allowed) p2_msr_hot2_since = std::chrono::steady_clock::now();
  const int32_t core = REXCVAR_GET(p2_sim_thread_core);
  if (core >= 0 && core <= 2) {
    const bool pinned = RexSwitchPinCurrentThreadToCore(core);
    REXLOG_INFO("[sched] simulation thread pinned to core {}: {}", core, pinned ? "yes" : "REFUSED");
  }
  const uint32_t priority = REXCVAR_GET(p2_sim_thread_priority);
  if (!priority) {
    REXLOG_INFO("[sched] simulation thread priority unchanged (0x3B)");
    return;
  }
  const bool ok = RexSwitchSetCurrentThreadPriorityOk(int(priority));
  REXLOG_INFO("[sched] simulation thread priority 0x{:02X}: {}", priority, ok ? "set" : "REFUSED");
}

// Guest D3D flip scheduling (sub_82C746D8, hook inserted by tools/prepare_switch_codegen.py). At each swap the
// game's D3D picks the vblank its flip happens at: last flip + present interval, or the next vblank when the
// swap arrives "late" in the current one. Queued flips make the next frame wait for that vblank, so 35-60 ms of
// work became 50/67/83 ms frames. The real display is paced by the host presenter (Vulkan FIFO), so the guest
// flip can happen at once: target = the current vblank count, which takes D3D's immediate-flip path.
REXCVAR_DEFINE_BOOL(p2_immediate_flip, true, "Prototype2",
    "Flip the guest's frontbuffer at the swap instead of at a later vblank (the screen is paced by the host "
    "presenter). false = the game's own present interval / late-swap wait")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_UINT32(p2_min_present_interval, 2, "Prototype2",
    "With p2_immediate_flip: a frame faster than this many vblanks still waits for that vblank (2 = at most "
    "30 fps, 1 = 60); only slower frames flip at once instead of at the next vblank")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

extern "C" void p2_quality_frame();  // switch_quality.cpp
extern "C" void p2_timeline_frame();  // switch_timeline.cpp

// WORKLOG #91: D3D's wait for ring-buffer space (sub_822A11A0 loops on sub_82C73C08: 32 db16cyc pauses, a GPU
// hang check, returns 1 = keep waiting) spun the simulation thread (run62: 15.6 % of a core, on its own core 0).
// A real sleep instead lets the job workers (guest threads, cores 0-2, lower priority) run the skinning jobs on
// core 0 meanwhile (nanosleep -> svcSleepThread); a yield would only yield to equal priority.
REXCVAR_DEFINE_UINT32(p2_ring_wait_sleep_us, 50, "Prototype2",
                      "Sleep per step of the game's wait for GPU ring space, in microseconds (0 = spin as on "
                      "the Xbox 360)");
REX_EXTERN(__imp__sub_82C73C08);
// PERF pass 3: in-run A/B of the step above. Phase = (system tick seconds / period) % 2, the same clock as
// logs/rex/frame_pacing.csv tick_s, so 10 s report windows can be assigned to a phase afterwards. Odd phases
// sleep p2_ring_wait_sleep_alt_us. Only the sleep length changes; the wait loop and its exit are the game's.
REXCVAR_DEFINE_UINT32(p2_ring_wait_ab_period_s, 0, "Prototype2",
                      "Measurement: alternate the ring-space wait step between p2_ring_wait_sleep_us (even "
                      "periods) and p2_ring_wait_sleep_alt_us (odd periods) every this many seconds; 0 = off");
REXCVAR_DEFINE_UINT32(p2_ring_wait_sleep_alt_us, 20, "Prototype2",
                      "Alternative ring-space wait step (us) used in odd periods of p2_ring_wait_ab_period_s");
static uint32_t p2_ring_wait_step_us() {
  const uint32_t period = REXCVAR_GET(p2_ring_wait_ab_period_s);
  if (!period) return REXCVAR_GET(p2_ring_wait_sleep_us);
#if defined(__SWITCH__) && defined(__aarch64__)
  uint64_t tick, freq;
  __asm__ volatile("mrs %0, cntpct_el0" : "=r"(tick));
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
  const uint64_t seconds = freq ? tick / freq : 0;
#else
  const uint64_t seconds = uint64_t(std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
  return ((seconds / period) & 1) ? REXCVAR_GET(p2_ring_wait_sleep_alt_us) : REXCVAR_GET(p2_ring_wait_sleep_us);
}
extern "C" REX_FUNC(sub_82C73C08) {
  __imp__sub_82C73C08(ctx, base);
  const uint32_t us = p2_ring_wait_step_us();
  if (ctx.r3.u32 == 1 && us) std::this_thread::sleep_for(std::chrono::microseconds(us));
}

extern "C" uint32_t p2_flip_target(uint32_t target, uint32_t now, uint32_t interval, uint32_t threshold,
                                   uint32_t last_flip) {
  static uint32_t swaps = 0, queued = 0, capped = 0, last_interval = 0xFFFFFFFFu, last_threshold = 0xFFFFFFFFu;
  ++swaps;
  if (target != now) ++queued;
  if (interval != last_interval || threshold != last_threshold) {
    last_interval = interval;
    last_threshold = threshold;
    REXLOG_INFO("[flip] present interval {} late threshold {}% (target {} now {} last {})", interval,
                threshold, target, now, last_flip);
  }
  uint32_t result = target;
  if (REXCVAR_GET(p2_immediate_flip)) {
    const uint32_t cap = last_flip + std::max<uint32_t>(interval, REXCVAR_GET(p2_min_present_interval));
    if (int32_t(cap - now) > 0) {
      result = cap;  // faster than the cap: flip at that vblank (frame rate limit, even pacing)
      ++capped;
    } else {
      result = now;  // slower: at once, not rounded up to the next vblank
    }
  }
  if (swaps % 600 == 0) {
    REXLOG_INFO("[flip] last 600 swaps: {} the game queued for a later vblank, {} held by the frame cap{}",
                queued, capped, REXCVAR_GET(p2_immediate_flip) ? "" : " (immediate flip off)");
    queued = 0;
    capped = 0;
  }
  p2_quality_frame();
  p2_timeline_frame();
  p2_msr_hot2_frame();
  p2_pass12_frame();
  return result;
}
