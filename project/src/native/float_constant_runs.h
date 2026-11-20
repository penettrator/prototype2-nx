// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace p2::native {

// Visit contiguous enabled register runs in ascending bitmap/register order.
// Each bitmap word covers 64 float registers; a guest float register is 4 uint32_t words.
template <typename Callback>
void ForEachFloatConstantRun(const uint64_t* bitmap, Callback&& callback) {
  for (uint32_t word = 0; word < 4; ++word) {
    uint64_t bits = bitmap[word];
    while (bits) {
      const uint32_t first = std::countr_zero(bits);
      const uint64_t shifted = bits >> first;
      const uint32_t count = std::countr_one(shifted);
      callback(word * 64 + first, count);
      const uint64_t run = count == 64 ? UINT64_MAX : ((uint64_t(1) << count) - 1);
      bits &= ~(run << first);
    }
  }
}

inline size_t CopyFloatConstantRuns(uint8_t* out, const uint32_t* register_words,
                                    const uint64_t* bitmap) {
  size_t written = 0;
  ForEachFloatConstantRun(bitmap, [&](uint32_t first, uint32_t count) {
    if (count < 4) {
      for (uint32_t i = 0; i < count; ++i) {
        std::memcpy(out + written, register_words + size_t(first + i) * 4,
                    4 * sizeof(uint32_t));
        written += 4 * sizeof(uint32_t);
      }
    } else {
      const size_t bytes = size_t(count) * 4 * sizeof(uint32_t);
      std::memcpy(out + written, register_words + size_t(first) * 4, bytes);
      written += bytes;
    }
  });
  return written;
}

}  // namespace p2::native
