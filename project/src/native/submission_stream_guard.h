// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <limits>

namespace p2::native {
// Conservative snapshot of OTHER pending submissions. While the recording key is unchanged,
// those submissions may complete, but cannot become pending or change stream_oldest.
// A new BeginRecording key is required before a newly submitted range can be allocated over.
class SubmissionStreamGuard {
 public:
  void Reset(uint64_t recording_key) {
    key_ = recording_key;
    min_ = std::numeric_limits<uint64_t>::max();
    max_ = 0;
    any_ = false;
    valid_ = true;
  }
  void Observe(uint64_t oldest) {
    if (oldest < min_) min_ = oldest;
    if (oldest > max_) max_ = oldest;
    any_ = true;
  }
  bool CanSkip(uint64_t recording_key, uint64_t end, uint64_t capacity) const {
    // max_ prevents unsigned-underflow cases from being accepted by a min-only summary.
    return valid_ && key_ == recording_key &&
           (!any_ || (end >= max_ && end - min_ <= capacity));
  }
 private:
  uint64_t key_ = 0, min_ = 0, max_ = 0;
  bool any_ = false, valid_ = false;
};
}  // namespace p2::native
