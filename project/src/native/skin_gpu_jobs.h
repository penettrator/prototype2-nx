// SPDX-License-Identifier: GPL-3.0-or-later
// Prototype 2 PERF pass 81 (p2_skin_gpu_normals, off by default): skinning calls whose normal/tangent words the
// renderer patches on the GPU (shaders/skin_normals.cs.glsl).
//
// The skinning side (switch_skinning.cpp) runs p2skin::Skin(..., positions_only) - positions, bounds and the
// call's last vertex in full - and publishes a Job: the input words and palette rows the missing words come from,
// and three vertices' position words to recognise its output later. The renderer, when it copies a vertex range
// into its stream: jobs that lie inside the range and whose positions are still in guest memory get a GPU patch of
// the copy; jobs that only partly overlap it get their words written into guest memory on the CPU first (the copy
// then carries them), so every copy of skinned data has its normals whatever the range layout.
// Jobs stay until a newer call writes the same address or they are 600 frames old.
// PERF pass 84 (run 175 profile: Publish was 11 % of the render/sim thread, mostly malloc and its global lock):
// jobs come from a pool and keep their vectors' capacity; the table's map nodes come from a free list. No
// allocation per call once the pool is warm. Semantics unchanged.
// PERF pass 85 (p2_skin_gpu_lazy_input, off by default): ~63 % of the jobs go stale unused (run 175), yet every
// Publish copied and swapped its whole input on the render/sim thread. Lazy jobs keep only the guest address of
// the input plus three input vertices to recognise it; the renderer reads it from guest memory when it patches.
// A job whose sampled input words changed since Publish is stale like one whose positions changed.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <utility>
#include <mutex>
#include <vector>
#include "skin_gpu_normals.h"

namespace p2::skingpu {

// Set by the renderer once its pipeline exists and p2_skin_gpu_normals is on; cleared for good on a mismatch.
inline std::atomic<bool> g_ready{false};
inline std::atomic<uint64_t> g_published{0}, g_patched{0}, g_cpu_filled{0}, g_stale{0}, g_verified{0}, g_pruned{0};
// In-run A/B (p2_skin_gpu_ab_period_s, set by the renderer): odd periods of the system clock skin on the CPU in
// full (jobs already published are still patched).
inline std::atomic<uint32_t> g_ab_period{0};
inline std::atomic<bool> g_lazy_input{false};  // set by the renderer from p2_skin_gpu_lazy_input
inline bool OddPeriod(uint32_t period) {
  if (!period) return false;
#if defined(__aarch64__)
  uint64_t tick, freq;
  __asm__ volatile("mrs %0, cntpct_el0" : "=r"(tick));
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
  return freq && ((tick / freq / period) & 1);
#else
  return false;
#endif
}
// In-run A/B of pass 85 (p2_skin_gpu_lazy_ab_period_s): odd periods publish copying jobs.
inline std::atomic<uint32_t> g_lazy_ab_period{0};
inline bool AbOff() {
  const uint32_t period = g_ab_period.load(std::memory_order_relaxed);
  if (!period) return false;
#if defined(__aarch64__)
  uint64_t tick, freq;
  __asm__ volatile("mrs %0, cntpct_el0" : "=r"(tick));
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
  return freq && ((tick / freq / period) & 1);
#else
  return false;
#endif
}

struct Job {
  std::atomic<uint32_t> refs{0};  // JobPtr references; at 0 the job goes back to the pool
  uint32_t phys = 0;   // physical address of vertex 0
  uint32_t n = 0;      // vertices of the call; words of 0..n-2 are missing in guest memory
  uint32_t frame = 0;  // skinlog frame of the call
  std::vector<uint32_t> input;  // 8 host-order words per vertex 0..n-2 (q0[0..3], q1[0..3]); empty when lazy
  uint32_t in = 0;              // guest (virtual) address of the input vertex 0
  bool lazy = false;
  uint32_t in_check[3][8] = {}; // lazy: host-order input words of the check vertices
  std::vector<float> rows;      // 16 floats per bone 0..max
  uint32_t check_k[3] = {};
  uint32_t check[3][4] = {};    // host-order position words (out + 24k, 16 bytes)
  uint32_t Bytes() const { return 24u * (n - 1); }  // the vertices whose words are missing
};

// Free jobs (vectors keep their capacity). Lock order: table mutex, then pool mutex.
struct JobPool {
  std::mutex mutex;
  std::vector<Job*> free;
};
inline JobPool g_pool;
inline constexpr size_t kPoolKeep = 1024;
inline Job* AcquireJob() {
  {
    std::lock_guard<std::mutex> lock(g_pool.mutex);
    if (!g_pool.free.empty()) {
      Job* job = g_pool.free.back();
      g_pool.free.pop_back();
      return job;
    }
  }
  return new Job;
}
inline void ReleaseJob(Job* job) {
  if (job->refs.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
  {
    std::lock_guard<std::mutex> lock(g_pool.mutex);
    if (g_pool.free.size() < kPoolKeep) {
      g_pool.free.push_back(job);
      return;
    }
  }
  delete job;
}
// Shared reference to a job (the renderer keeps the covered ones past the table lock, as shared_ptr did).
class JobPtr {
 public:
  JobPtr() = default;
  explicit JobPtr(Job* job) : job_(job) { if (job_) job_->refs.fetch_add(1, std::memory_order_relaxed); }
  JobPtr(const JobPtr& o) : JobPtr(o.job_) {}
  JobPtr(JobPtr&& o) noexcept : job_(o.job_) { o.job_ = nullptr; }
  JobPtr& operator=(JobPtr o) noexcept { std::swap(job_, o.job_); return *this; }
  ~JobPtr() { if (job_) ReleaseJob(job_); }
  const Job& operator*() const { return *job_; }
  const Job* operator->() const { return job_; }
  explicit operator bool() const { return job_ != nullptr; }
 private:
  Job* job_ = nullptr;
};

// Map nodes from a free list; every use of the table's map is under its mutex, so the list needs no lock.
template <size_t Size>
inline void*& NodeFreeList() {
  static void* head = nullptr;
  return head;
}
template <class T>
struct TableAlloc {
  using value_type = T;
  TableAlloc() = default;
  template <class U>
  TableAlloc(const TableAlloc<U>&) {}
  T* allocate(size_t n) {
    static_assert(sizeof(T) >= sizeof(void*));
    void*& head = NodeFreeList<sizeof(T)>();
    if (n == 1 && head) {
      void* p = head;
      head = *static_cast<void**>(p);
      return static_cast<T*>(p);
    }
    return static_cast<T*>(::operator new(n * sizeof(T)));
  }
  void deallocate(T* p, size_t n) {
    if (n != 1) {
      ::operator delete(p);
      return;
    }
    void*& head = NodeFreeList<sizeof(T)>();
    *reinterpret_cast<void**>(p) = head;
    head = p;
  }
  template <class U>
  bool operator==(const TableAlloc<U>&) const { return true; }
};

inline uint32_t Physical(uint32_t guest) {
  // 0xE0000000+ views sit 4 KB above their physical pages (skin_write_log.h MarkPages).
  return ((guest & 0x1FFFFFFFu) + (guest >= 0xE0000000u ? 0x1000u : 0u)) & 0x1FFFFFFFu;
}
inline uint32_t BE32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return __builtin_bswap32(v); }

struct Table {
  std::mutex mutex;
  std::map<uint32_t, JobPtr, std::less<uint32_t>, TableAlloc<std::pair<const uint32_t, JobPtr>>> jobs;  // by phys
  uint32_t pruned_frame = ~0u;
};
inline Table g_table;

// Whether a call can take the positions-only path: aligned input (the kernel's 16-byte loads then read exactly
// the vertex), 4-byte aligned output (the GPU writes words), n >= 2.
inline bool Eligible(uint32_t out, uint32_t in, uint32_t n) {
  return g_ready.load(std::memory_order_relaxed) && n >= 2 && (in & 15u) == 0 && (out & 3u) == 0 && !AbOff();
}

// After a positions-only Skin (or a cache replay, whose stored words are stale) wrote [out, out + 24n).
inline void Publish(const uint8_t* base, uint32_t out, uint32_t in, uint32_t n, uint32_t palette, uint32_t max_index,
                    uint32_t frame) {
  JobPtr ref(AcquireJob());
  Job* job = const_cast<Job*>(ref.operator->());
  job->phys = Physical(out);
  job->n = n;
  job->frame = frame;
  job->in = in;
  job->lazy = g_lazy_input.load(std::memory_order_relaxed) &&
              !OddPeriod(g_lazy_ab_period.load(std::memory_order_relaxed));
  if (job->lazy) {
    job->input.clear();
  } else {
    job->input.resize(size_t(n - 1) * 8);
    for (uint32_t k = 0; k + 1 < n; ++k)
      for (uint32_t q = 0; q < 8; ++q) job->input[k * 8 + q] = BE32(base + uint32_t(in + 32 * k + 4 * q));
  }
  job->rows.resize(size_t(max_index + 1) * 16);
  for (uint32_t b = 0; b <= max_index; ++b) {
    const uint32_t mb = __builtin_rotateleft32(b, 6) + palette;
    for (uint32_t r = 0; r < 4; ++r) {
      const uint8_t* row = base + (uint32_t(mb + 16 * r) & ~15u);
      for (uint32_t l = 0; l < 4; ++l) {
        const uint32_t v = BE32(row + 4 * l);
        std::memcpy(&job->rows[b * 16 + r * 4 + l], &v, 4);
      }
    }
  }
  const uint32_t ks[3] = {0, (n - 2) / 2, n - 2};
  for (int i = 0; i < 3; ++i) {
    job->check_k[i] = ks[i];
    for (int q = 0; q < 4; ++q) job->check[i][q] = BE32(base + uint32_t(out + 24 * ks[i] + 4 * q));
    if (job->lazy)
      for (int q = 0; q < 8; ++q) job->in_check[i][q] = BE32(base + uint32_t(in + 32 * ks[i] + 4 * q));
  }
  g_published.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(g_table.mutex);
  g_table.jobs[job->phys] = std::move(ref);
  // Run 172: pruning on every call once the table was full walked 8192 entries per call under the lock the
  // renderer also takes. Now at most once per frame; per-frame dynamic buffers give new addresses every frame,
  // so beyond 600-frame-old entries the table is cut to the last 30 frames when it grows past 12288.
  if (g_table.jobs.size() > 8192 && g_table.pruned_frame != frame) {
    g_table.pruned_frame = frame;
    const uint32_t keep = g_table.jobs.size() > 12288 ? 30 : 600;
    for (auto it = g_table.jobs.begin(); it != g_table.jobs.end();) {
      if (frame - it->second->frame > keep) {
        it = g_table.jobs.erase(it);
        g_pruned.fetch_add(1, std::memory_order_relaxed);
      } else {
        ++it;
      }
    }
  }
}

// The job's positions are still what it wrote (`guest` = guest memory at the job's address) and, for a lazy job,
// its sampled input words are unchanged (`in_guest` = guest memory at job.in).
inline bool Current(const Job& job, const uint8_t* guest, const uint8_t* in_guest) {
  for (int i = 0; i < 3; ++i)
    for (int q = 0; q < 4; ++q)
      if (BE32(guest + 24 * job.check_k[i] + 4 * q) != job.check[i][q]) return false;
  if (job.lazy) {
    if (!in_guest) return false;
    for (int i = 0; i < 3; ++i)
      for (int q = 0; q < 8; ++q)
        if (BE32(in_guest + 32 * job.check_k[i] + 4 * q) != job.in_check[i][q]) return false;
  }
  return true;
}

// The job's input words, host order, 8 per vertex 0..n-2 (`in_guest` = guest memory at job.in for a lazy job).
inline void InputHost(const Job& job, const uint8_t* in_guest, uint32_t* dst) {
  const size_t words = size_t(job.n - 1) * 8;
  if (!job.lazy) {
    std::memcpy(dst, job.input.data(), words * 4);
    return;
  }
  for (size_t i = 0; i < words; ++i) {
    uint32_t v;
    std::memcpy(&v, in_guest + 4 * i, 4);
    dst[i] = __builtin_bswap32(v);
  }
}
inline const uint32_t* InputWords(const Job& job, const uint8_t* in_guest, std::vector<uint32_t>& scratch) {
  if (!job.lazy) return job.input.data();
  scratch.resize(size_t(job.n - 1) * 8);
  InputHost(job, in_guest, scratch.data());
  return scratch.data();
}

// The missing words of every vertex of the job, on the CPU (big-endian, into `dst` = the job's vertex 0).
inline void FillCpu(const Job& job, uint8_t* dst, const uint8_t* in_guest) {
  thread_local std::vector<uint32_t> scratch;
  const uint32_t* input = InputWords(job, in_guest, scratch);
  for (uint32_t k = 0; k + 1 < job.n; ++k) {
    uint32_t words[2];
    p2skin_gpu::NormalWords(&input[k * 8], &input[k * 8 + 4], job.rows.data(), words);
    for (int i = 0; i < 2; ++i) {
      const uint32_t be = __builtin_bswap32(words[i]);
      std::memcpy(dst + 24 * k + 16 + 4 * i, &be, 4);
    }
  }
}

// Renderer, before copying [phys, phys + bytes) of guest memory (`translate(phys)` gives a guest pointer):
// jobs partly overlapping the range are written into guest memory now (and dropped), stale jobs are dropped,
// and the jobs inside it are returned for a GPU patch of the copy.
template <class Translate, class TranslateVirtual>
inline void Prepare(uint32_t phys, uint32_t bytes, Translate&& translate, TranslateVirtual&& translate_virtual,
                    std::vector<JobPtr>& inside) {
  inside.clear();
  const uint64_t end = uint64_t(phys) + bytes;
  std::lock_guard<std::mutex> lock(g_table.mutex);
  if (g_table.jobs.empty()) return;
  auto it = g_table.jobs.lower_bound(phys);
  if (it != g_table.jobs.begin()) {
    auto prev = std::prev(it);
    if (uint64_t(prev->first) + prev->second->Bytes() > phys) it = prev;  // starts before, reaches in
  }
  while (it != g_table.jobs.end() && it->first < end) {
    const Job& job = *it->second;
    uint8_t* guest = translate(job.phys);
    const uint8_t* in_guest = job.lazy ? translate_virtual(job.in) : nullptr;
    if (!guest || !Current(job, guest, in_guest)) {
      g_stale.fetch_add(1, std::memory_order_relaxed);
      it = g_table.jobs.erase(it);
      continue;
    }
    if (job.phys >= phys && uint64_t(job.phys) + job.Bytes() <= end) {
      inside.push_back(it->second);
      ++it;
      continue;
    }
    FillCpu(job, guest, in_guest);
    g_cpu_filled.fetch_add(1, std::memory_order_relaxed);
    it = g_table.jobs.erase(it);
  }
}

// Everything still pending goes back to guest memory (feature turned off after a mismatch).
template <class Translate, class TranslateVirtual>
inline void FlushAllCpu(Translate&& translate, TranslateVirtual&& translate_virtual) {
  std::lock_guard<std::mutex> lock(g_table.mutex);
  for (auto& [phys, job] : g_table.jobs) {
    uint8_t* guest = translate(phys);
    const uint8_t* in_guest = job->lazy ? translate_virtual(job->in) : nullptr;
    if (guest && Current(*job, guest, in_guest)) FillCpu(*job, guest, in_guest);
  }
  g_table.jobs.clear();
}

}  // namespace p2::skingpu
