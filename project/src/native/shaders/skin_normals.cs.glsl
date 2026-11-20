#version 450
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Prototype 2 PERF pass 81 (p2_skin_gpu_normals): the packed normal/tangent words of the skinning routine's
// approx path, written into the renderer's copy of a skinned vertex range. Line for line the scalar reference
// p2skin_gpu::NormalWords (skin_gpu_normals.h); the CPU wrote the positions (p2skin::SkinRangePositions).
// Everything lives in the stream buffer: the call's input vertices (8 words each in host order - the CPU swapped
// them already, skin_gpu_jobs.h Publish - vertices 0..count-1), the palette rows (host floats, 16 per bone) and the vertex copy (24-byte big-endian vertices;
// words 4 and 5 of each are written here). `precise` keeps every multiply and add separate (no fusing).
layout(local_size_x = 64) in;
layout(std430, binding = 0) buffer Stream { uint w[]; };
layout(push_constant) uniform Patch {
  uint in_word;    // first input word
  uint rows_word;  // first row float
  uint dst_word;   // word 0 of the first vertex copy
  uint count;      // vertices
  uint bones;      // palette rows / 16 (bone indices are clamped below it)
} pc;

uint Swap(uint v) {
  return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
}

uint Pack(vec4 v) {
  uint word = 0u;
  for (int l = 0; l < 4; ++l) {
    const float lo = l == 3 ? uintBitsToFloat(0x40400000u) : uintBitsToFloat(0x403FFE01u);
    const float hi = l == 3 ? uintBitsToFloat(0x40400003u) : uintBitsToFloat(0x404001FFu);
    float c = v[l] > hi ? hi : v[l];
    c = v[l] < lo ? lo : c;
    const uint bits = floatBitsToUint(c);
    word |= l == 3 ? (bits & 3u) << 30 : (bits & 1023u) << (10 * l);
  }
  return word;
}

float Row(uint bone, uint r, uint l) { return uintBitsToFloat(w[pc.rows_word + bone * 16u + r * 4u + l]); }

void main() {
  const uint k = gl_GlobalInvocationID.x;
  if (k >= pc.count) return;
  const float scale_w = uintBitsToFloat(0x3A802008u);  // 1/1023
  const float scale_n = uintBitsToFloat(0x3B002008u);  // scale_w + scale_w
  const uint base = pc.in_word + k * 8u;
  const uint q03 = w[base + 3u];
  uint q1[4];
  for (int q = 0; q < 4; ++q) q1[q] = w[base + 4u + uint(q)];
  const uint last = pc.bones - 1u;
  const uint m[4] = uint[4](min(q03 >> 16, last), min(q03 & 0xFFFFu, last), min(q1[0] >> 16, last),
                            min(q1[0] & 0xFFFFu, last));
  precise float n[3], t[3], wt[4];
  for (int j = 0; j < 3; ++j) {
    const uint word = q1[j + 1];
    n[j] = float(int(word & 1023u)) * scale_n - 1.0;
    t[j] = float(int((word >> 10) & 1023u)) * scale_n - 1.0;
    wt[j] = float(int((word >> 20) & 1023u)) * scale_w;
  }
  wt[3] = 1.0 - (((0.0 + wt[0]) + wt[1]) + wt[2]);
  precise vec4 M[4];
  for (uint r = 0u; r < 4u; ++r)
    for (uint l = 0u; l < 4u; ++l)
      M[r][l] = (Row(m[0], r, l) * wt[0] + Row(m[1], r, l) * wt[1]) +
                (Row(m[2], r, l) * wt[2] + Row(m[3], r, l) * wt[3]);
  precise vec4 nrm, tan;
  for (int l = 0; l < 4; ++l) {
    nrm[l] = (n[0] * M[0][l] + n[1] * M[1][l]) + n[2] * M[2][l];
    tan[l] = (t[0] * M[0][l] + t[1] * M[1][l]) + t[2] * M[2][l];
  }
  precise float inv = 1.0 / sqrt((nrm.x * nrm.x + nrm.y * nrm.y) + nrm.z * nrm.z);
  precise vec4 on, ot;
  for (int l = 0; l < 4; ++l) {
    const float s = l == 3 ? uintBitsToFloat(0x34800000u) : uintBitsToFloat(0x38FE8000u);
    on[l] = (nrm[l] * inv) * s + 3.0;
    ot[l] = (tan[l] * inv) * s + 3.0;
  }
  const uint dst = pc.dst_word + k * 6u;
  w[dst + 4u] = Swap(Pack(on));
  w[dst + 5u] = Swap(Pack(ot) | (q1[1] & 0xC0000000u));
}
