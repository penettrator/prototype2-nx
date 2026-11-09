// SPDX-License-Identifier: GPL-3.0-or-later
// Prototype 2 PERF pass 25: the game's voice mixer sub_82CF0958 in native code (profile diag030: 8.1 % of a core,
// on core 3 with the audio thread, where it competes with the job workers the main thread waits for).
//
// Per call (r3 = mixer, r4 = voice, r5 = per-channel source table, r6 = output buffer table, r7 = optional energy
// accumulator): for every channel of the voice, 256 samples are resampled from the channel's ring buffer (16.12
// fixed point, linear interpolation; an integer copy when pitch is 1 and the phase has no fraction), optionally
// enveloped (sub_82CF0818 gives three gain ranges), measured (RMS when voice+2 == 1), low-passed (one pole,
// voice+1036) and mixed into 6 speaker buffers per output set with 6 per-speaker gains.
//
// Same operations in the same order as the generated code: single-precision arithmetic in float (as
// p2_single_floats compiles fmuls/fadds/fsubs), fmadds/fnmsubs as a double fma rounded to float, the comparisons'
// NaN behaviour taken from the branch conditions, the VMX mixing loops non-fused with the FPCR flush mode on and
// the scalar code with it off (as the generated code switches it). The original's 2.5 KB stack buffers are host
// arrays here; the envelope routine gets its struct and results in a guest stack frame at the original offsets.
// Not reproduced: the original's dead stack contents (register save area, scratch spills).
// Unusual shapes (more than 256 history samples or 32 channels) -> the original.
// The first p2_audio_mix_verify calls run both and compare every byte the original writes; a difference keeps the
// original's result and turns the native path off.
#include "generated/default/prototype2_pch.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <arm_neon.h>
#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstring>
#include <vector>

REXCVAR_DEFINE_UINT32(p2_audio_mix_native, 0, "Prototype2",
                      "PERF pass 25: native code for the voice mixer sub_82CF0958 (same results): 0 off, 1 on, "
                      "2 A/B every 30 s (on in odd periods)");
REXCVAR_DEFINE_UINT32(p2_audio_mix_verify, 256, "Prototype2",
                      "First N mixer calls also run the recompiled routine and compare every byte it writes");
REX_EXTERN(__imp__sub_82CF0958);
REX_EXTERN(sub_82CF0818);

namespace {
std::atomic<uint32_t> g_checks{0};
std::atomic<bool> g_failed{false};
std::atomic<uint64_t> g_native_calls{0}, g_original_calls{0};

inline float AsF(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }
inline uint32_t AsU(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
// fmadds / fnmsubs as the generated code computes them.
inline float Fmas(float a, float b, float c) { return float(std::fma(double(a), double(b), double(c))); }
inline float Fnmsubs(float a, float b, float c) { return float(-std::fma(double(a), double(b), -double(c))); }

inline float32x4_t LoadBE4(const uint8_t* p) {
  return vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(p)));
}
inline void StoreBE4(uint8_t* p, float32x4_t v) { vst1q_u8(p, vrev32q_u8(vreinterpretq_u8_f32(v))); }

bool ModeOn() {
  const uint32_t mode = REXCVAR_GET(p2_audio_mix_native);
  if (mode == 2) {
    uint64_t tick, freq;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(tick));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    return freq && ((tick / (freq * 30)) & 1);
  }
  return mode == 1;
}

// Shapes the native code reproduces exactly.
bool Supported(PPCContext& ctx, uint8_t* base) {
  const uint32_t v = ctx.r4.u32;
  return REX_LOAD_U32(v + 1296) <= 256 && REX_LOAD_U32(v + 84) <= 32;
}

void Native(PPCContext& ctx, uint8_t* base) {
  const uint32_t obj = ctx.r3.u32, v = ctx.r4.u32, sources = ctx.r5.u32, outputs = ctx.r6.u32, energy = ctx.r7.u32;
  const uint32_t old_r1 = ctx.r1.u32;
  const uint32_t sp = old_r1 - 2480;
  REX_STORE_U32(sp, old_r1);  // stwu back chain
  ctx.fpscr.disableFlushMode();

  alignas(16) float A[256];
  alignas(16) float B[256];
  const float pitch = AsF(REX_LOAD_U32(v + 8));
  const uint32_t history = REX_LOAD_U32(v + 1296);  // r24
  const uint32_t mask = REX_LOAD_U32(v + 88) - 1u;  // r29 (low 32 bits suffice: indices are 32-bit)
  const float f29 = AsF(REX_LOAD_U32(v + 1300));
  const float f27 = 1.0f - f29;
  // r25: the 16.12 step, fctidz of pitch * 4096 as the generated code converts it.
  const double step_d = double(pitch * 4096.0f);
  const int64_t step =
      std::isnan(step_d) ? int64_t(0x8000000000000000ULL)
      : (step_d > double(LLONG_MAX)) ? LLONG_MAX
                                     : simde_mm_cvttsd_si64(simde_mm_load_sd(&step_d));
  for (uint32_t i = 0; i < history; ++i) A[i] = 0.0f;  // the original's zeroed history (B's is dead)

  // Envelope struct (stack +112..+136): constants, replaced by the voice's per channel; written back at the end.
  uint32_t st[7] = {AsU(48000.0f), 0, AsU(0.0f), AsU(100.0f), 0, AsU(1.0f), AsU(0.0f)};
  const uint32_t remain = 256 - history;  // r23
  const float kPitchOne = 1.0f, kFrac = AsF(0x39800000), kRmsScale = AsF(0x3B800000), kLowpassMin = AsF(0x3727C5AC),
              kAlphaMin = AsF(0x3C23D70A), kWetSlope = AsF(0x3E800000), kEnergyStep = AsF(0x44C00000);

  for (uint32_t c = 0; c < REX_LOAD_U32(v + 84); ++c) {
    for (int w = 0; w < 7; ++w) st[w] = REX_LOAD_U32(v + 1124 + 4 * w);
    const uint32_t src = REX_LOAD_U32(sources + 4 + 24 * c);  // r11
    const float f30 = AsF(REX_LOAD_U32(v + 112 + 4 * c));
    // Resample into A[history .. 255].
    const uint64_t phase0 = REX_LOAD_U64(v + 104);
    if (pitch == kPitchOne && (phase0 & 0xFFF) == 0) {
      const uint32_t idx = uint32_t(phase0 >> 12);
      for (uint32_t i = 0; i < remain; ++i)
        A[history + i] = AsF(REX_LOAD_U32(src + (((idx + i) & mask) << 2)));
    } else {
      uint64_t phase = phase0;
      for (uint32_t i = 0; i < remain; ++i) {
        const float frac = float(double(int64_t(phase & 0xFFF)));
        const uint32_t i0 = uint32_t(phase >> 12) & mask;
        const uint32_t i1 = (i0 + 1) & mask;
        phase += uint64_t(step);
        const float t = frac * kFrac;
        const float s0 = AsF(REX_LOAD_U32(src + (i0 << 2)));
        const float s1 = AsF(REX_LOAD_U32(src + (i1 << 2)));
        A[history + i] = Fmas(s0, kPitchOne - t, s1 * t);
      }
    }
    // Envelope.
    if (st[1] != 0) {
      for (int w = 0; w < 7; ++w) REX_STORE_U32(sp + 112 + 4 * w, st[w]);
      ctx.r1.u64 = sp;
      ctx.r3.u64 = sp + 112;
      ctx.r4.u64 = 256;
      ctx.r5.u64 = sp + 140;
      ctx.r6.u64 = sp + 84;
      ctx.r7.u64 = sp + 92;
      ctx.r8.u64 = sp + 88;
      ctx.lr = 0x82CF0E00;
      sub_82CF0818(ctx, base);
      ctx.fpscr.disableFlushMode();
      for (int w = 0; w < 7; ++w) st[w] = REX_LOAD_U32(sp + 112 + 4 * w);
      const uint32_t n = REX_LOAD_U32(sp + 140), m = REX_LOAD_U32(sp + 84);
      const float g1 = AsF(REX_LOAD_U32(sp + 92)), g3 = AsF(REX_LOAD_U32(sp + 88));
      uint32_t k = 0;
      if (n) {
        for (uint32_t i = 0; i < n && i < 256; ++i) A[i] = A[i] * g1;
        k = n;
      }
      if (k < m) {
        float g2 = AsF(st[5]);
        const float d = AsF(st[6]);
        for (; k < m; ++k) {
          g2 = g2 + d;
          if (k < 256) A[k] = A[k] * g2;
        }
        st[5] = AsU(g2);
      }
      for (; k < 256; ++k) A[k] = A[k] * g3;
      if (n > 256 || m > 256) g_failed.store(true, std::memory_order_relaxed);
    }
    // RMS.
    if (REX_LOAD_U8(v + 2) == 1) {
      float a0 = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
      for (int k = 0; k < 64; ++k) {
        a0 = Fmas(A[4 * k], A[4 * k], a0);
        a1 = Fmas(A[4 * k + 1], A[4 * k + 1], a1);
        a2 = Fmas(A[4 * k + 2], A[4 * k + 2], a2);
        a3 = Fmas(A[4 * k + 3], A[4 * k + 3], a3);
      }
      const float sum = ((a3 + a2) + a1) + a0;
      REX_STORE_U32(v + 1152 + 4 * c, AsU(__builtin_sqrtf(sum * kRmsScale)));
    }
    // Low-pass.
    const float* mixsrc;
    float wet = kPitchOne;
    const float cutoff = AsF(REX_LOAD_U32(v + 1036));
    if (cutoff > kLowpassMin) {
      float a = kPitchOne - cutoff;
      a = a * a;
      bool keep = false;
      if (!(a < kAlphaMin) && !(a > kPitchOne)) keep = true;
      if (!keep) a = !(a > kPitchOne) ? kAlphaMin : kPitchOne;
      const float b = kPitchOne - a;
      float y = AsF(REX_LOAD_U32(v + 1224 + 4 * c));
      for (int k = 0; k < 256; ++k) {
        y = Fmas(A[k], a, b * y);
        B[k] = y;
      }
      REX_STORE_U32(v + 1224 + 4 * c, AsU(y));
      const float c2 = AsF(REX_LOAD_U32(v + 1036));
      wet = Fnmsubs(c2, kWetSlope, kPitchOne);
      bool keepw = false;
      if (!(wet < 0.0f) && !(wet > kPitchOne)) keepw = true;
      if (!keepw) wet = !(wet > kPitchOne) ? 0.0f : kPitchOne;
      mixsrc = B;
    } else {
      REX_STORE_U32(v + 1224 + 4 * c, AsU(A[255]));
      mixsrc = A;
    }
    const float vol = AsF(REX_LOAD_U32(v + 4));
    uint32_t first_set = 0;
    // Output set 0 into the energy accumulator path.
    if (energy) {
      const uint32_t ea = energy & ~0xFu;
      float32x4_t acc = LoadBE4(REX_RAW_ADDR(ea));
      first_set = 1;
      const uint32_t own = REX_LOAD_U32(obj + 16);
      float gain;
      const float* s = mixsrc;
      if (own == 0) {
        gain = (AsF(REX_LOAD_U32(v + 1120)) * vol) * wet;
        s = A;
      } else {
        gain = AsF(REX_LOAD_U32(v + 1116)) * vol;
      }
      const float p640 = AsF(REX_LOAD_U32(v + 640 + 16 * c));
      const float p928 = AsF(REX_LOAD_U32(v + 928));
      for (uint32_t j = 0; j < 6; ++j) {
        const float f10 = AsF(REX_LOAD_U32(v + 1044 + 4 * j)) * f29;
        const float f6 = Fmas(AsF(REX_LOAD_U32(v + 208 + 24 * c + 4 * j)), f27, f10);
        const float g = ((((f6 * p928) * p640) * AsF(REX_LOAD_U32(v + 184 + 4 * j))) * gain) * f30;
        if (g > 0.0f) {
          const uint32_t dst = REX_LOAD_U32(outputs + 4 * j);
          ctx.fpscr.enableFlushMode();
          const float32x4_t gv = vdupq_n_f32(g);
          for (uint32_t k = 0; k < 64; ++k) {
            uint8_t* dp = REX_RAW_ADDR((dst + 16 * k) & ~0xFu);
            const float32x4_t v0 = vmulq_f32(gv, vld1q_f32(s + 4 * k));
            StoreBE4(dp, vaddq_f32(v0, LoadBE4(dp)));
            acc = vaddq_f32(vmulq_f32(v0, v0), acc);
          }
          ctx.fpscr.disableFlushMode();
        }
      }
      REX_STORE_U32(energy + 16, AsU(AsF(REX_LOAD_U32(energy + 16)) + kEnergyStep));
      StoreBE4(REX_RAW_ADDR(ea), acc);
    }
    const uint32_t sets = REX_LOAD_U32(obj + 12);
    const uint32_t own = REX_LOAD_U32(obj + 16);
    for (uint32_t set = first_set; set < sets; ++set) {
      float gain;
      const float* s = mixsrc;
      if (own == set) {
        gain = (AsF(REX_LOAD_U32(v + 1120)) * vol) * wet;
        s = A;
      } else {
        gain = AsF(REX_LOAD_U32(v + 1116)) * vol;
      }
      const float p640 = AsF(REX_LOAD_U32(v + 640 + 16 * c + 4 * set));
      const float p928 = AsF(REX_LOAD_U32(v + 928 + 4 * set));
      for (uint32_t j = 0; j < 6; ++j) {
        const float f8 = AsF(REX_LOAD_U32(v + 1044 + 4 * j)) * f29;
        const float f5 = Fmas(AsF(REX_LOAD_U32(v + 208 + 24 * c + 4 * j)), f27, f8);
        const float g = ((((f5 * AsF(REX_LOAD_U32(v + 184 + 4 * j))) * p928) * p640) * gain) * f30;
        if (g > 0.0f) {
          const uint32_t dst = REX_LOAD_U32(outputs + 4 * (j + 6 * set));
          ctx.fpscr.enableFlushMode();
          const float32x4_t gv = vdupq_n_f32(g);
          for (uint32_t k = 0; k < 64; ++k) {
            uint8_t* dp = REX_RAW_ADDR((dst + 16 * k) & ~0xFu);
            StoreBE4(dp, vaddq_f32(vmulq_f32(vld1q_f32(s + 4 * k), gv), LoadBE4(dp)));
          }
          ctx.fpscr.disableFlushMode();
        }
      }
    }
  }
  for (int w = 0; w < 7; ++w) REX_STORE_U32(v + 1124 + 4 * w, st[w]);
  ctx.r1.u64 = old_r1;
}

// The bytes the original may write outside its stack: voice +1124..+1300, the energy accumulator and every
// output buffer it can reach.
struct Region {
  uint32_t address, size;
};
std::vector<Region> Regions(PPCContext& ctx, uint8_t* base) {
  std::vector<Region> r;
  r.push_back({ctx.r4.u32 + 1124, 1300 - 1124});
  if (ctx.r7.u32) r.push_back({ctx.r7.u32 & ~0xFu, 32});
  const uint32_t sets = std::max<uint32_t>(REX_LOAD_U32(ctx.r3.u32 + 12), 1);
  for (uint32_t i = 0; i < 6 * std::min<uint32_t>(sets, 16); ++i) {
    const uint32_t dst = REX_LOAD_U32(ctx.r6.u32 + 4 * i);
    if (dst) r.push_back({dst & ~0xFu, 1024 + 16});
  }
  return r;
}
void Save(uint8_t* base, const std::vector<Region>& r, std::vector<uint8_t>& out) {
  out.clear();
  for (const auto& x : r) out.insert(out.end(), REX_RAW_ADDR(x.address), REX_RAW_ADDR(x.address) + x.size);
}
void Restore(uint8_t* base, const std::vector<Region>& r, const std::vector<uint8_t>& in) {
  size_t o = 0;
  for (const auto& x : r) {
    std::memcpy(REX_RAW_ADDR(x.address), in.data() + o, x.size);
    o += x.size;
  }
}
}  // namespace

extern "C" REX_FUNC(sub_82CF0958) {
  if (!ModeOn() || g_failed.load(std::memory_order_relaxed) || !Supported(ctx, base)) {
    g_original_calls.fetch_add(1, std::memory_order_relaxed);
    __imp__sub_82CF0958(ctx, base);
    return;
  }
  const uint32_t verify = REXCVAR_GET(p2_audio_mix_verify);
  if (g_checks.load(std::memory_order_relaxed) >= verify ||
      g_checks.fetch_add(1, std::memory_order_relaxed) >= verify) {
    g_native_calls.fetch_add(1, std::memory_order_relaxed);
    Native(ctx, base);
    return;
  }
  // Verification: native on a copy of the state, then the original on the real one; compare.
  PPCContext saved = ctx;
  const auto regions = Regions(ctx, base);
  std::vector<uint8_t> before, native_out, original_out;
  Save(base, regions, before);
  Native(ctx, base);
  Save(base, regions, native_out);
  Restore(base, regions, before);
  ctx = saved;
  __imp__sub_82CF0958(ctx, base);
  Save(base, regions, original_out);
  if (native_out != original_out) {
    size_t at = 0;
    while (at < native_out.size() && native_out[at] == original_out[at]) ++at;
    size_t o = 0, which = 0;
    while (which < regions.size() && o + regions[which].size <= at) o += regions[which++].size;
    g_failed.store(true, std::memory_order_relaxed);
    REXLOG_WARN("[p2-audio-mix] native mixer differs from the original at region {} (+{:X} of {:08X}) on check "
                "{}: native off",
                which, at - o, which < regions.size() ? regions[which].address : 0,
                g_checks.load(std::memory_order_relaxed));
  } else if (g_checks.load(std::memory_order_relaxed) == verify) {
    REXLOG_WARN("[p2-audio-mix] {} mixer calls checked against the original: identical", verify);
  }
}
