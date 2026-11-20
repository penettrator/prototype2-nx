// SPDX-License-Identifier: GPL-3.0-or-later
// Diagnostic-only timing of all seven original calls in guest job 829AC940.
#include <rex/cvar.h>
#include <rex/logging.h>
#include <atomic>
#include <chrono>
#include <cstdint>

REXCVAR_DEFINE_BOOL(p2_job_child_timing, false, "Prototype2",
    "Diagnostic build: cumulative wall time of child calls in job829AC940; restart required")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if defined(P2_JOB_CHILD_TIMING)
namespace {
struct Bucket { std::atomic<uint64_t> calls{0}, ns{0}, max_ns{0}; } buckets[7];
constexpr uint32_t children[7]={0x82C9D568,0x8299C838,0x82980DA0,0x8299F1B0,0x8299CFE0,0x8299CC58,0x82234188};
std::atomic<uint64_t> events{0};
uint64_t NowNs() {
#if defined(__aarch64__)
  uint64_t tick,freq;
  asm volatile("mrs %0, cntpct_el0":"=r"(tick));
  asm volatile("mrs %0, cntfrq_el0":"=r"(freq));
  return freq?uint64_t((__uint128_t)tick*1000000000u/freq):0;
#else
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}
}
extern "C" uint64_t P2JobChildBegin() {
  static const bool enabled=REXCVAR_GET(p2_job_child_timing);
  return enabled?NowNs():0;
}
extern "C" void P2JobChildEnd(uint32_t child,uint64_t begin) {
  if(!begin||child>=7)return;
  const uint64_t ns=NowNs()-begin;
  auto& b=buckets[child];
  b.ns.fetch_add(ns,std::memory_order_relaxed);
  b.calls.fetch_add(1,std::memory_order_relaxed);
  auto old=b.max_ns.load(std::memory_order_relaxed);
  while(old<ns&&!b.max_ns.compare_exchange_weak(old,ns,std::memory_order_relaxed)){}
  if((events.fetch_add(1,std::memory_order_relaxed)&16383)!=16383)return;
  // Cumulative approximate concurrent snapshot; no global barrier or SD writes
  // per event. Wall time includes scheduling delays, not just CPU execution.
  for(uint32_t i=0;i<7;++i)
    REXLOG_INFO("[p2-job-child] parent=829AC940 child={:08X} calls={} ns={} max_ns={}",
      children[i],buckets[i].calls.load(std::memory_order_relaxed),
      buckets[i].ns.load(std::memory_order_relaxed),buckets[i].max_ns.load(std::memory_order_relaxed));
}
#endif
