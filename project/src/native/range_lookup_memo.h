// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "dynamic_buffer_stamp.h"
#include <array>

namespace p2::native {
// An accelerator for the full-key table, never for content validation. Every
// write/clear of that table must also update/clear this direct-mapped memo.
class RangeLookupMemo {
  struct Entry {
    BufferRangeKey key{};
    uint64_t slot = 0, frame = 0, submission = 0, position = UINT64_MAX;
  };
  std::array<Entry, 64> entries_{};
 public:
  bool Find(uint64_t slot, const BufferRangeKey& key, uint64_t frame,
            uint64_t submission, uint64_t head, uint64_t capacity, uint64_t& position) const {
    const auto& e = entries_[slot & 63];
    if (e.position == UINT64_MAX || e.slot != slot || !(e.key == key) ||
        e.frame != frame || e.submission != submission || head - e.position >= capacity / 2)
      return false;
    position = e.position;
    return true;
  }
  void Store(uint64_t slot, const BufferRangeKey& key, uint64_t frame,
             uint64_t submission, uint64_t position) {
    entries_[slot & 63] = {key, slot, frame, submission, position};
  }
  void Clear() { for (auto& e : entries_) e.position = UINT64_MAX; }
};
}  // namespace p2::native
