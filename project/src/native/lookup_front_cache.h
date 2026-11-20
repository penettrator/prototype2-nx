// SPDX-License-Identifier: GPL-3.0-or-later
// PERF pass 75: a direct-mapped front for std::unordered_map<uint64_t, V> lookups on the ring thread.
//
// Pass 57 samples put the hottest spots of Renderer::LoadShader (shaders_by_location) and Draw (setup_cache) on
// the unordered_map lookup itself: a 64-bit division by the prime bucket count and two dependent node loads.
// The front remembers, per slot, the key and a pointer to the map's value. unordered_map never moves a value
// while it exists (rehash keeps references), so a remembered pointer stays the map's answer until that entry is
// erased or the map cleared; Clear() must be called then. Only successful lookups are remembered: a miss always
// goes to the map. The owner verifies the first answers against map.find().
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace p2::native {

template <typename V, unsigned kBits = 10>
class LookupFrontCache {
 public:
  static constexpr size_t kSize = size_t(1) << kBits;

  V* Find(uint64_t key) const {
    const Entry& e = entries_[Slot(key)];
    return (e.generation == generation_ && e.key == key) ? e.value : nullptr;
  }
  void Store(uint64_t key, V* value) {
    Entry& e = entries_[Slot(key)];
    e.key = key;
    e.generation = generation_;
    e.value = value;
  }
  // Every remembered pointer is forgotten (map cleared or an entry erased).
  void Clear() {
    if (++generation_ == 0) {  // wrapped: really wipe, generation 0 never matches
      entries_.fill(Entry{});
      generation_ = 1;
    }
  }

 private:
  struct Entry {
    uint64_t key = 0;
    uint64_t generation = 0;
    V* value = nullptr;
  };
  static size_t Slot(uint64_t key) { return size_t((key * 0x9E3779B97F4A7C15ull) >> (64 - kBits)); }
  std::array<Entry, kSize> entries_{};
  uint64_t generation_ = 1;
};

}  // namespace p2::native
