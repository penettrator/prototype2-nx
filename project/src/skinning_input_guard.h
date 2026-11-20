// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
inline bool P2SkinningFinitePositions(const uint8_t* input, uint32_t count) {
  if (count < 2) return false;
  for (uint32_t i=0;i<count;++i) {
    uint32_t xyz[3]; std::memcpy(xyz,input+size_t(i)*32,12);
    for (uint32_t word : xyz)
      if ((__builtin_bswap32(word)&0x7F800000u)==0x7F800000u) return false;
  }
  return true;
}

#if defined(__aarch64__) && defined(__AARCH64EL__)
#include <arm_neon.h>
#endif

namespace p2skin {

struct SkinInputScan {
  bool finite;
  uint32_t max_index;
};

inline SkinInputScan ScanInputScalar(const uint8_t* v, uint32_t n) {
  uint32_t max_index = 0;
  bool finite = true;
  for (uint32_t i = 0; i < n; ++i, v += 32) {
    uint32_t xyz[3];
    std::memcpy(xyz, v, 12);
    finite &= ((__builtin_bswap32(xyz[0]) & 0x7F800000u) != 0x7F800000u) &
              ((__builtin_bswap32(xyz[1]) & 0x7F800000u) != 0x7F800000u) &
              ((__builtin_bswap32(xyz[2]) & 0x7F800000u) != 0x7F800000u);
    uint16_t idx[4];
    std::memcpy(idx, v + 12, 8);
    const uint32_t a = std::max(__builtin_bswap16(idx[0]), __builtin_bswap16(idx[1]));
    const uint32_t b = std::max(__builtin_bswap16(idx[2]), __builtin_bswap16(idx[3]));
    max_index = std::max(max_index, std::max(a, b));
  }
  return {finite, max_index};
}

#if defined(__aarch64__) && defined(__AARCH64EL__)
inline SkinInputScan ScanInputNeon(const uint8_t* v, uint32_t n) {
  // IEEE-754 exponent mask in the byte order of the guest data. The fourth
  // word is bone-index data and must not participate in the finite check.
  const uint32x4_t exponent = vdupq_n_u32(__builtin_bswap32(0x7F800000u));
  uint32x4_t non_finite = vdupq_n_u32(0);
  uint16x4_t max_indices = vdup_n_u16(0);
  for (uint32_t i = 0; i < n; ++i, v += 32) {
    const auto position = vreinterpretq_u32_u8(vld1q_u8(v));
    non_finite = vorrq_u32(non_finite, vceqq_u32(vandq_u32(position, exponent), exponent));
    const auto indices = vreinterpret_u16_u8(vrev16_u8(vld1_u8(v + 12)));
    max_indices = vmax_u16(max_indices, indices);
  }
  non_finite = vsetq_lane_u32(0, non_finite, 3);
  return {vmaxvq_u32(non_finite) == 0, vmaxv_u16(max_indices)};
}
#endif

}  // namespace p2skin
