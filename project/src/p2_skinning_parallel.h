// SPDX-License-Identifier: GPL-3.0-or-later
// Prototype 2 (WORKLOG #92): big skinning calls split with one host helper thread.
// run63: p2skin::Skin ran on the game's render/simulation thread (23 % of its 69 %, ~11 ms per frame) while the
// cores 1-2 had idle time. The owner thread publishes the call, both threads take 32-vertex chunks from one
// range, the owner from the front, the helper from the back (the owner never waits for the helper to start:
// if it is late, the owner does everything); the owner returns only once the helper has left the call.
// Same per-vertex operations. Bounds: min/max keep the LATER vertex on ties (+0/-0), so the sequential result is
// reproduced exactly: the owner's part is [0, m) folded in order, the helper's part [m, n) is folded chunk by
// chunk from the back keeping the higher-index chunk on ties, then min(owner, helper) keeps the helper's on ties
// (tests/skinning_diff.cpp checks the same shape). The helper runs with the owner's FPCR (flush mode).
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

extern "C" void RexSwitchSetCurrentThreadPriority(int priority);  // SDK threading_switch
extern "C" bool RexSwitchSetCurrentThreadMask(uint32_t mask);

namespace p2skinpar {

constexpr uint32_t kChunk = 32;

struct Task {
  void (*fn)(void* user, uint32_t k0, uint32_t k1, simde__m128& lo, simde__m128& hi) = nullptr;
  void* user = nullptr;
  uint32_t n = 0;
  uint64_t fpcr = 0;
  simde__m128 init_lo, init_hi;
  std::atomic<uint64_t> ends{0};  // front chunk << 32 | back chunk (exclusive), both claim with CAS
  std::atomic<uint32_t> helper_vertices{0};
  simde__m128 helper_lo, helper_hi;
  bool helper_folded = false;
  // PERF pass 7 (several helpers): when set, every thread claims from the front and stores each chunk's
  // bounds here; the owner folds them in chunk order afterwards (same tie rule: the later chunk wins).
  simde__m128* part_lo = nullptr;
  simde__m128* part_hi = nullptr;
};

inline std::atomic<Task*> g_task{nullptr};
inline std::atomic<int> g_active{0};
inline std::atomic<int> g_sleeping{0};  // helpers in wait_for
inline std::atomic<bool> g_started{false};
inline std::mutex g_mutex;
inline std::condition_variable g_cv;
inline std::atomic<uint32_t> g_spin_us{150};
inline std::atomic<uint64_t> g_calls{0}, g_split{0}, g_vertices{0}, g_helper_vertices{0};
inline std::atomic<uint32_t> g_helpers{0};       // helper threads started
inline std::atomic<uint32_t> g_multi_period{0};  // 0: several helpers whenever started; else in-run A/B period (s)
inline std::atomic<uint64_t> g_multi_calls{0};
// PERF pass 14: in-run A/B of the first helper's priority (switch_skinning.cpp); -1 = keep the start value.
inline std::atomic<int> g_priority_want{-1};
constexpr uint32_t kMaxPartChunks = 512;

// Claims a chunk index from the front (owner) or the back (helper); -1 when none is left.
inline int64_t Claim(Task* t, bool back) {
  uint64_t e = t->ends.load(std::memory_order_relaxed);
  for (;;) {
    const uint32_t front = uint32_t(e >> 32), end = uint32_t(e);
    if (front >= end) return -1;
    const uint64_t next = back ? (uint64_t(front) << 32 | (end - 1)) : (uint64_t(front + 1) << 32 | end);
    if (t->ends.compare_exchange_weak(e, next, std::memory_order_relaxed)) return back ? end - 1 : front;
  }
}

inline uint64_t ReadFpcr() { uint64_t v; asm volatile("mrs %0, fpcr" : "=r"(v)); return v; }
inline void WriteFpcr(uint64_t v) { asm volatile("msr fpcr, %0" : : "r"(v)); }

inline void HelperWork(Task* t) {
  const uint64_t saved = ReadFpcr();
  if (saved != t->fpcr) WriteFpcr(t->fpcr);
  if (t->part_lo) {
    for (int64_t c; (c = Claim(t, false)) >= 0;) {
      const uint32_t k0 = uint32_t(c) * kChunk, k1 = std::min(t->n, k0 + kChunk);
      simde__m128 clo = t->init_lo, chi = t->init_hi;
      t->fn(t->user, k0, k1, clo, chi);
      t->part_lo[c] = clo;
      t->part_hi[c] = chi;
      t->helper_vertices.fetch_add(k1 - k0, std::memory_order_relaxed);
    }
    if (saved != t->fpcr) WriteFpcr(saved);
    return;
  }
  simde__m128 lo = t->init_lo, hi = t->init_hi;
  uint32_t done = 0;
  for (int64_t c; (c = Claim(t, true)) >= 0;) {
    const uint32_t k0 = uint32_t(c) * kChunk, k1 = std::min(t->n, k0 + kChunk);
    simde__m128 clo = t->init_lo, chi = t->init_hi;
    t->fn(t->user, k0, k1, clo, chi);
    lo = simde_mm_min_ps(clo, lo);  // ties keep `lo`: the higher-index chunks, processed first
    hi = simde_mm_max_ps(chi, hi);
    done += k1 - k0;
  }
  if (done) {
    t->helper_lo = lo;
    t->helper_hi = hi;
    t->helper_folded = true;
    t->helper_vertices.store(done, std::memory_order_relaxed);
  }
  if (saved != t->fpcr) WriteFpcr(saved);
}

inline void HelperLoop(int priority, uint32_t mask, uint32_t index) {
  RexSwitchSetCurrentThreadPriority(priority);
  // Falls back to cores 1-2 when the process may not use the requested cores (stock hbl: no core 3).
  if (mask && !RexSwitchSetCurrentThreadMask(mask)) RexSwitchSetCurrentThreadMask(0x6);
  auto last_work = std::chrono::steady_clock::now();
  int current_priority = priority;
  for (;;) {
    if (index == 0) {
      const int want = g_priority_want.load(std::memory_order_relaxed);
      if (want > 0 && want != current_priority) {
        RexSwitchSetCurrentThreadPriority(want);
        current_priority = want;
      }
    }
    Task* t = g_task.load(std::memory_order_acquire);
    // Only the first helper takes single-helper calls (their back-claim fold assumes one helper).
    if (t && !t->part_lo && index != 0) {
      while (g_task.load(std::memory_order_acquire) == t) asm volatile("yield");
      continue;
    }
    if (t) {
      g_active.fetch_add(1, std::memory_order_seq_cst);
      if (g_task.load(std::memory_order_seq_cst) == t) {
        HelperWork(t);
        last_work = std::chrono::steady_clock::now();
      }
      g_active.fetch_sub(1, std::memory_order_release);
      // Do not take the same finished call again.
      while (g_task.load(std::memory_order_acquire) == t) asm volatile("yield");
      continue;
    }
    // run66: reading the clock every poll was over half of the helper's time; check it every 256 polls.
    static thread_local uint32_t polls = 0;
    if ((++polls & 255) != 0) {
      asm volatile("yield");
      continue;
    }
    if (std::chrono::steady_clock::now() - last_work <
        std::chrono::microseconds(g_spin_us.load(std::memory_order_relaxed))) {
      continue;
    }
    std::unique_lock<std::mutex> lock(g_mutex);
    g_sleeping.fetch_add(1, std::memory_order_seq_cst);
    g_cv.wait_for(lock, std::chrono::milliseconds(2),
                  [] { return g_task.load(std::memory_order_seq_cst) != nullptr; });
    g_sleeping.fetch_sub(1, std::memory_order_relaxed);
    last_work = std::chrono::steady_clock::now();
  }
}

inline void Start(int priority, uint32_t mask, uint32_t spin_us) {
  g_spin_us.store(spin_us, std::memory_order_relaxed);
  bool expected = false;
  if (!g_started.compare_exchange_strong(expected, true)) return;
  // Never joined and never detached: libnx's pthread_detach fails (run64: std::thread::detach threw
  // std::system_error -> std::terminate on the first big skinning call). Leaked on purpose.
  g_helpers.fetch_add(1, std::memory_order_release);
  new std::thread([priority, mask] { HelperLoop(priority, mask, 0); });
}

// PERF pass 7: one more helper (once), used only by calls that take the several-helpers path.
inline void StartExtra(int priority, uint32_t mask) {
  static std::atomic<bool> started{false};
  bool expected = false;
  if (!g_started.load(std::memory_order_relaxed) || !started.compare_exchange_strong(expected, true)) return;
  new std::thread([priority, mask] { HelperLoop(priority, mask, 1); });
  g_helpers.fetch_add(1, std::memory_order_release);
}

inline bool MultiPhase() {
  if (g_helpers.load(std::memory_order_acquire) < 2) return false;
  const uint32_t period = g_multi_period.load(std::memory_order_relaxed);
  if (!period) return true;
  uint64_t tick, freq;
  asm volatile("mrs %0, cntpct_el0" : "=r"(tick));
  asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
  return freq && ((tick / freq / period) & 1) == 0;
}

// The runner given to p2skin::Skin.
struct Parallel {
  uint32_t min_vertices;
  template <class F>
  void operator()(uint32_t n, F&& range, simde__m128& v32, simde__m128& v33) const {
    g_calls.fetch_add(1, std::memory_order_relaxed);
    g_vertices.fetch_add(n, std::memory_order_relaxed);
    if (n < min_vertices || !g_started.load(std::memory_order_relaxed)) {
      range(0u, n, v32, v33);
      return;
    }
    const uint32_t chunks = (n + kChunk - 1) / kChunk;
    if (chunks <= kMaxPartChunks && MultiPhase()) {
      Multi(n, range, v32, v33, chunks);
      return;
    }
    Task t;
    t.fn = [](void* user, uint32_t k0, uint32_t k1, simde__m128& lo, simde__m128& hi) {
      (*static_cast<F*>(user))(k0, k1, lo, hi);
    };
    t.user = const_cast<void*>(static_cast<const void*>(&range));
    t.n = n;
    t.fpcr = ReadFpcr();
    t.init_lo = v32;
    t.init_hi = v33;
    t.ends.store((n + kChunk - 1) / kChunk, std::memory_order_relaxed);
    g_task.store(&t, std::memory_order_seq_cst);
    if (g_sleeping.load(std::memory_order_seq_cst)) {
      std::lock_guard<std::mutex> lock(g_mutex);
      g_cv.notify_all();
    }
    for (int64_t c; (c = Claim(&t, false)) >= 0;) {
      const uint32_t k0 = uint32_t(c) * kChunk;
      range(k0, std::min(n, k0 + kChunk), v32, v33);
    }
    g_task.store(nullptr, std::memory_order_seq_cst);
    while (g_active.load(std::memory_order_seq_cst) != 0) asm volatile("yield");
    std::atomic_thread_fence(std::memory_order_acquire);
    if (t.helper_folded) {
      v32 = simde_mm_min_ps(v32, t.helper_lo);  // ties keep the helper's: its vertices come later
      v33 = simde_mm_max_ps(v33, t.helper_hi);
      g_split.fetch_add(1, std::memory_order_relaxed);
      g_helper_vertices.fetch_add(t.helper_vertices.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
  }

  // Several helpers: chunks from one front counter, bounds per chunk, folded here in chunk order. Chunk 0
  // starts from the caller's bounds like the sequential fold; each later chunk is min(acc, chunk) = the later
  // one on ties, as each vertex is inside a chunk (and as the single-helper merge above).
  template <class F>
  void Multi(uint32_t n, F& range, simde__m128& v32, simde__m128& v33, uint32_t chunks) const {
    alignas(16) simde__m128 plo[kMaxPartChunks], phi[kMaxPartChunks];
    Task t;
    t.fn = [](void* user, uint32_t k0, uint32_t k1, simde__m128& lo, simde__m128& hi) {
      (*static_cast<F*>(user))(k0, k1, lo, hi);
    };
    t.user = const_cast<void*>(static_cast<const void*>(&range));
    t.n = n;
    t.fpcr = ReadFpcr();
    t.init_lo = v32;
    t.init_hi = v33;
    t.part_lo = plo;
    t.part_hi = phi;
    t.ends.store(chunks, std::memory_order_relaxed);
    g_task.store(&t, std::memory_order_seq_cst);
    if (g_sleeping.load(std::memory_order_seq_cst)) {
      std::lock_guard<std::mutex> lock(g_mutex);
      g_cv.notify_all();
    }
    for (int64_t c; (c = Claim(&t, false)) >= 0;) {
      const uint32_t k0 = uint32_t(c) * kChunk;
      simde__m128 lo = v32, hi = v33;
      range(k0, std::min(n, k0 + kChunk), lo, hi);
      plo[c] = lo;
      phi[c] = hi;
    }
    g_task.store(nullptr, std::memory_order_seq_cst);
    while (g_active.load(std::memory_order_seq_cst) != 0) asm volatile("yield");
    std::atomic_thread_fence(std::memory_order_acquire);
    simde__m128 lo = plo[0], hi = phi[0];
    for (uint32_t c = 1; c < chunks; ++c) {
      lo = simde_mm_min_ps(lo, plo[c]);
      hi = simde_mm_max_ps(hi, phi[c]);
    }
    v32 = lo;
    v33 = hi;
    g_multi_calls.fetch_add(1, std::memory_order_relaxed);
    g_helper_vertices.fetch_add(t.helper_vertices.load(std::memory_order_relaxed), std::memory_order_relaxed);
  }
};

}  // namespace p2skinpar
