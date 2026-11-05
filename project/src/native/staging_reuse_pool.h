// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace p2::native {
// Callers own synchronization and may return only GPU-complete allocations.
// Capacity is usable buffer size; bytes includes Vulkan allocation alignment.
template <typename Block> class StagingReusePool {
 public:
  bool Take(uint64_t needed, Block& result) {
    size_t best = blocks_.size();
    for (size_t i = 0; i < blocks_.size(); ++i) {
      if (blocks_[i].capacity >= needed &&
          (best == blocks_.size() || blocks_[i].capacity < blocks_[best].capacity)) best = i;
    }
    if (best == blocks_.size()) return false;
    result = std::move(blocks_[best]);
    bytes_ -= result.bytes;
    if (best + 1 != blocks_.size()) blocks_[best] = std::move(blocks_.back());
    blocks_.pop_back();
    return true;
  }
  bool Put(const Block& block, uint64_t budget) {
    if (!budget || !block.capacity || !block.bytes || blocks_.size() >= 64 ||
        block.bytes > budget || bytes_ > budget - block.bytes) return false;
    blocks_.push_back(block);
    bytes_ += block.bytes;
    return true;
  }
  template <typename Destroy> void Clear(Destroy destroy) {
    for (auto& block : blocks_) destroy(block);
    blocks_.clear();
    bytes_ = 0;
  }
  uint64_t bytes() const { return bytes_; }
  size_t size() const { return blocks_.size(); }
 private:
  std::vector<Block> blocks_;
  uint64_t bytes_ = 0;
};

// Power-of-two buckets avoid allocating a different buffer for each mip chain.
// Large one-off uploads keep their exact size and are never retained.
inline uint64_t StagingBucketSize(uint64_t needed, uint64_t budget) {
  constexpr uint64_t max_bucket = 4 * 1024 * 1024;
  if (!budget || !needed || needed > max_bucket || needed > budget) return needed;
  uint64_t capacity = 64 * 1024;
  while (capacity < needed) capacity *= 2;
  return capacity <= budget ? capacity : needed;
}
}  // namespace p2::native
