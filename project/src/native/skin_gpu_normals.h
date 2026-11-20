// SPDX-License-Identifier: GPL-3.0-or-later
// Prototype 2 PERF pass 81 (p2_skin_gpu_normals): the packed normal/tangent words of the approx skinning path
// (p2_skinning_fast.h ApproxVertex<true>, StoreBE8 of p10) in plain scalar float, in PPC lane order. This is the
// host reference of shaders/skin_normals.cs.glsl, line for line; tests/skinning_diff.cpp checks it against
// ApproxNormalWords. Not bit-exact by design (GPU sqrt/division), fields within 2 codes.
//
// Input vertex (32 bytes, big-endian words q0[0..3], q1[0..3]): q0 = position xyz + bone indices 0/1 (16-bit
// halves of q0[3]); q1[0] = bone indices 2/3; q1[1..3] = per axis (weight << 20 | tangent << 10 | normal), 10 bits
// each, normal/tangent unsigned n -> n * 2/1023 - 1, weight w -> w / 1023, fourth weight 1 - the others.
// Output: word0 = normal packed 2:10:10:10 (lane 3 in bits 30-31, z 20-29, y 10-19, x 0-9) from biased floats
// (3.0 + v * 511.5 * 2^-22: the code is in the low mantissa bits), word1 = tangent likewise | (q1[1] & 0xC0000000).
#pragma once
#include <bit>
#include <cmath>
#include <cstdint>

namespace p2skin_gpu {

inline constexpr float kScaleW = std::bit_cast<float>(uint32_t(8200 + 14976 * 65536));  // v3: 1/1023
inline constexpr float kBias = 3.0f;                                                   // v8
inline constexpr float kPackScale = std::bit_cast<float>(uint32_t(int32_t(-32768) + int32_t(14591) * 65536));  // v9
inline constexpr float kPackScaleW = std::bit_cast<float>(uint32_t(13440) * 65536u);   // v9 lane 3
inline constexpr float kMin10 = std::bit_cast<float>(0x403FFE01u), kMax10 = std::bit_cast<float>(0x404001FFu);
inline constexpr float kMin2 = std::bit_cast<float>(0x40400000u), kMax2 = std::bit_cast<float>(0x40400003u);

inline uint32_t Pack(const float v[4]) {
  uint32_t word = 0;
  for (int l = 0; l < 4; ++l) {
    const float lo = l == 3 ? kMin2 : kMin10, hi = l == 3 ? kMax2 : kMax10;
    float c = v[l] > hi ? hi : v[l];
    c = v[l] < lo ? lo : c;
    const uint32_t bits = std::bit_cast<uint32_t>(c);
    word |= l == 3 ? (bits & 3u) << 30 : (bits & 1023u) << (10 * l);
  }
  return word;
}

// q0/q1: the vertex's input words (host values of the big-endian words); rows: bone b's row r lane l at
// rows[b * 16 + r * 4 + l] (the palette's floats in memory order). out[0..1] = word0, word1 (host values).
inline void NormalWords(const uint32_t q0[4], const uint32_t q1[4], const float* rows, uint32_t out[2]) {
  const float scale_n = kScaleW + kScaleW;  // v4
  const uint32_t m[4] = {q0[3] >> 16, q0[3] & 0xFFFFu, q1[0] >> 16, q1[0] & 0xFFFFu};
  float n[3], t[3], w[4];
  for (int j = 0; j < 3; ++j) {
    const uint32_t word = q1[j + 1];
    n[j] = float(int32_t(word & 1023u)) * scale_n - 1.0f;
    t[j] = float(int32_t((word >> 10) & 1023u)) * scale_n - 1.0f;
    w[j] = float(int32_t((word >> 20) & 1023u)) * kScaleW;
  }
  w[3] = 1.0f - (((0.0f + w[0]) + w[1]) + w[2]);
  float M[4][4];
  for (int r = 0; r < 4; ++r)
    for (int l = 0; l < 4; ++l)
      M[r][l] = (rows[m[0] * 16 + r * 4 + l] * w[0] + rows[m[1] * 16 + r * 4 + l] * w[1]) +
                (rows[m[2] * 16 + r * 4 + l] * w[2] + rows[m[3] * 16 + r * 4 + l] * w[3]);
  float nrm[4], tan[4];
  for (int l = 0; l < 4; ++l) {
    nrm[l] = (n[0] * M[0][l] + n[1] * M[1][l]) + n[2] * M[2][l];
    tan[l] = (t[0] * M[0][l] + t[1] * M[1][l]) + t[2] * M[2][l];
  }
  const float inv = 1.0f / std::sqrt((nrm[0] * nrm[0] + nrm[1] * nrm[1]) + nrm[2] * nrm[2]);
  float on[4], ot[4];
  for (int l = 0; l < 4; ++l) {
    const float s = l == 3 ? kPackScaleW : kPackScale;
    on[l] = (nrm[l] * inv) * s + kBias;
    ot[l] = (tan[l] * inv) * s + kBias;
  }
  out[0] = Pack(on);
  out[1] = Pack(ot) | (q1[1] & 0xC0000000u);
}

}  // namespace p2skin_gpu
