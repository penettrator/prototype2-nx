// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
namespace p2::native {
// Counts jobs, not draws. Read by the optional low-rate test report.
struct StagingReuseCounters {
  std::atomic<uint64_t> created{0}, reused{0}, returned{0}, idle_bytes{0}, budget_bytes{0};
  std::atomic<bool> report_enabled{false};
};
inline StagingReuseCounters staging_reuse_counters;
// Snapshot on the existing 60-frame cache walk, read by optional 10s worker CSV.
struct StreamLifetimeCounters {
  std::atomic<uint64_t> pending{0}, ready{0}, ready_staging_bytes{0};
  std::atomic<uint64_t> discarded{0}, reaped{0}, image_failures{0};
};
inline StreamLifetimeCounters stream_lifetime_counters;
}
