// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
namespace p2::native {
// Optional test telemetry: one atomic bucket increment per presented interval.
// Percentiles are 1-ms upper buckets; 1000 means 1000 ms or more.
struct FramePacingSample {
  uint64_t count=0, gt50=0, gt100=0, ge1000=0, max_us=0;
  uint32_t p95_ms=0, p99_ms=0;
};
class FramePacingWindow {
 public:
  void Record(uint64_t us) {
    const uint64_t rounded=us/1000 + (us%1000!=0);
    bins_[rounded<1000?rounded:1000].fetch_add(1,std::memory_order_relaxed);
    if(us>50000) gt50_.fetch_add(1,std::memory_order_relaxed);
    if(us>100000) gt100_.fetch_add(1,std::memory_order_relaxed);
    if(us>=1000000) ge1000_.fetch_add(1,std::memory_order_relaxed);
    uint64_t old=max_.load(std::memory_order_relaxed);
    while(us>old && !max_.compare_exchange_weak(old,us,std::memory_order_relaxed)) {}
  }
  FramePacingSample Take() {
    FramePacingSample s;
    std::array<uint64_t,1001> counts;
    for(unsigned i=0;i<counts.size();++i){counts[i]=bins_[i].exchange(0,std::memory_order_relaxed);s.count+=counts[i];}
    uint64_t n=0; bool found95=false;
    for(unsigned i=0;i<counts.size()&&s.count;++i){
      n+=counts[i];
      if(!found95&&n>=(s.count*95+99)/100){s.p95_ms=i;found95=true;}
      if(n>=(s.count*99+99)/100){s.p99_ms=i;break;}
    }
    s.gt50=gt50_.exchange(0,std::memory_order_relaxed);
    s.gt100=gt100_.exchange(0,std::memory_order_relaxed);
    s.ge1000=ge1000_.exchange(0,std::memory_order_relaxed);
    s.max_us=max_.exchange(0,std::memory_order_relaxed);
    return s;
  }
 private:
  std::array<std::atomic<uint64_t>,1001> bins_{};
  std::atomic<uint64_t> max_{0},gt50_{0},gt100_{0},ge1000_{0};
};
}  // namespace p2::native
