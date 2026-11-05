// Prototype 2 (Switch), PERF pass 15: native sub_8222CDB8 (the game's shader-constant upload into its command buffer).
//
// Profile (perf_results/pass_14/analysis.md): 2.7 % of the simulation thread. The guest copies every dirty group of
// constants (a bit per 64-byte group in a 64-bit mask, runs of set bits = one SET_CONSTANT packet) with lvx/stvx: the
// recompiled code byte-reverses each 16 bytes on the load and again on the store, through ctx.v60-v63 in memory.
// Both sides are guest (big-endian) memory, so the copy is a plain 16-byte copy. Packet framing exactly as the guest:
// type-2 pad words 0x80000000 so the data starts 16-byte aligned, header ((dwords - 1) << 16) | index, write pointer
// at object+48, buffer end at object+52. When a packet would not fit (the guest calls sub_82279408 to flush) or the
// mask is 0, the original runs for the whole call. ctx.r4/r5 and ctx.v60-v63 end as in the original.
//   p2_const_native (default false), p2_const_native_verify N: the first N handled calls also run the original on the
//   same input and compare the written bytes, the pointer and the registers (a mismatch logs and keeps the original's
//   result; native stays off after one). p2_const_native_ab_period_s: odd periods of the system clock run the original.
#include "generated/default/prototype2_pch.h"
#include <atomic>
#include <cstring>
#include <vector>
#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(p2_const_native, false, "Prototype2",
    "Native shader-constant upload (sub_8222CDB8); false = the recompiled original");
REXCVAR_DEFINE_UINT32(p2_const_native_verify, 256, "Prototype2",
    "First calls of the native constant upload also run the original and compare");
REXCVAR_DEFINE_UINT32(p2_const_native_ab_period_s, 0, "Prototype2",
    "Measurement: odd periods of this many seconds of the system clock run the original; 0 = off");

REX_EXTERN(__imp__sub_8222CDB8);

namespace {
std::atomic<uint32_t> g_checks{0};
std::atomic<bool> g_failed{false};

inline uint32_t Ld32(uint8_t* base, uint32_t a) { return REX_LOAD_U32(a); }
inline void St32(uint8_t* base, uint32_t a, uint32_t v) { REX_STORE_U32(a, v); }

struct Plan {
  uint32_t end_ptr = 0;  // final write pointer
  bool ok = false;
};

// Walks the runs exactly like the guest; returns false when the guest would take its flush path.
bool Simulate(uint64_t mask, uint32_t w, uint32_t buffer_end, Plan& plan) {
  if (!mask) return false;
  while (mask) {
    const uint32_t z = uint32_t(__builtin_clzll(mask));
    mask <<= z;
    const uint64_t inv = ~mask;
    const uint32_t run = inv ? uint32_t(__builtin_clzll(inv)) : 64u;
    const uint32_t dwords = (run << 4);
    if (!(uint32_t((dwords + 3u) * 4u + w) < buffer_end)) return false;
    const uint32_t m = w & 15u;
    w += 4u * (m == 4u ? 1u : m == 0u ? 2u : m == 12u ? 3u : 0u);
    w += 4u + run * 64u;
    mask = run >= 64 ? 0 : (mask << run);
  }
  plan.end_ptr = w;
  plan.ok = true;
  return true;
}

void Native(PPCContext& ctx, uint8_t* base) {
  const uint32_t obj = ctx.r3.u32;
  uint64_t mask = ctx.r4.u64;
  uint32_t idx = ctx.r5.u32;
  uint32_t src = ctx.r6.u32 - 4u;  // r31
  uint32_t w = Ld32(base, obj + 48);
  uint32_t last_src = 0;
  bool copied = false;
  while (mask) {
    const uint32_t z = uint32_t(__builtin_clzll(mask));
    mask <<= z;
    src += z << 6;
    const uint64_t inv = ~mask;
    const uint32_t run = inv ? uint32_t(__builtin_clzll(inv)) : 64u;
    const uint32_t dwords = run << 4;
    const uint32_t index = (z << 4) + idx;
    const uint32_t m = w & 15u;
    const uint32_t pads = m == 4u ? 1u : m == 0u ? 2u : m == 12u ? 3u : 0u;
    for (uint32_t p = 0; p < pads; ++p) {
      w += 4;
      St32(base, w, 0x80000000u);
    }
    w += 4;
    St32(base, w, ((dwords - 1u) << 16) | index);
    for (uint32_t k = 0; k < run; ++k) {
      for (uint32_t off = 4; off <= 52; off += 16)
        std::memcpy(REX_RAW_ADDR((w + off) & ~15u), REX_RAW_ADDR((src + off) & ~15u), 16);
      last_src = src;
      copied = true;
      src += 64;
      w += 64;
    }
    idx = dwords + index;
    mask = run >= 64 ? 0 : (mask << run);
  }
  St32(base, obj + 48, w);
  ctx.r4.u64 = w;
  ctx.r5.u64 = idx;
  if (copied) {
    // As the original leaves them: the last group's four vectors, byte-reversed into the register image.
    PPCVRegister* regs[4] = {&ctx.v63, &ctx.v62, &ctx.v61, &ctx.v60};
    for (int i = 0; i < 4; ++i) {
      const uint8_t* s = REX_RAW_ADDR((last_src + 4u + 16u * uint32_t(i)) & ~15u);
      for (int b = 0; b < 16; ++b) regs[i]->u8[b] = s[15 - b];
    }
  }
}

bool PhaseOriginal() {
  const uint32_t period = REXCVAR_GET(p2_const_native_ab_period_s);
  if (!period) return false;
#if defined(__aarch64__)
  uint64_t tick, freq;
  __asm__ volatile("mrs %0, cntpct_el0" : "=r"(tick));
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
  return freq && ((tick / freq / period) & 1);
#else
  return false;
#endif
}
}  // namespace

extern "C" REX_FUNC(sub_8222CDB8) {
  static const bool enabled = REXCVAR_GET(p2_const_native);
  if (!enabled || g_failed.load(std::memory_order_relaxed) || PhaseOriginal()) {
    __imp__sub_8222CDB8(ctx, base);
    return;
  }
  Plan plan;
  if (!Simulate(ctx.r4.u64, REX_LOAD_U32(ctx.r3.u32 + 48), REX_LOAD_U32(ctx.r3.u32 + 52), plan)) {
    __imp__sub_8222CDB8(ctx, base);
    return;
  }
  const uint32_t verify = REXCVAR_GET(p2_const_native_verify);
  if (g_checks.load(std::memory_order_relaxed) >= verify ||
      g_checks.fetch_add(1, std::memory_order_relaxed) >= verify) {
    Native(ctx, base);
    return;
  }
  // Verify: the written range is [w0 + 4, end) plus object+48 and the source-side registers.
  const uint32_t obj = ctx.r3.u32;
  const uint32_t w0 = REX_LOAD_U32(obj + 48);
  const uint32_t lo = w0 & ~15u, hi = (plan.end_ptr + 64u) & ~15u;
  std::vector<uint8_t> before(hi - lo), mine(hi - lo);
  std::memcpy(before.data(), REX_RAW_ADDR(lo), hi - lo);
  PPCContext saved = ctx;
  Native(ctx, base);
  PPCContext native_ctx = ctx;
  const uint32_t native_end = REX_LOAD_U32(obj + 48);
  std::memcpy(mine.data(), REX_RAW_ADDR(lo), hi - lo);
  std::memcpy(REX_RAW_ADDR(lo), before.data(), hi - lo);
  REX_STORE_U32(obj + 48, w0);
  ctx = saved;
  __imp__sub_8222CDB8(ctx, base);
  const bool same_mem = std::memcmp(mine.data(), REX_RAW_ADDR(lo), hi - lo) == 0;
  const bool same_end = native_end == REX_LOAD_U32(obj + 48);
  const bool same_regs = native_ctx.r4.u32 == ctx.r4.u32 && native_ctx.r5.u32 == ctx.r5.u32 &&
                         std::memcmp(native_ctx.v60.u8, ctx.v60.u8, 16) == 0 &&
                         std::memcmp(native_ctx.v61.u8, ctx.v61.u8, 16) == 0 &&
                         std::memcmp(native_ctx.v62.u8, ctx.v62.u8, 16) == 0 &&
                         std::memcmp(native_ctx.v63.u8, ctx.v63.u8, 16) == 0;
  const uint32_t n = g_checks.load(std::memory_order_relaxed);
  if (!same_mem || !same_end || !same_regs) {
    g_failed.store(true, std::memory_order_relaxed);
    REXLOG_ERROR("[p2-const] verify MISMATCH (mem {} end {} regs {}): mask {:016X} idx {} obj {:08X} w {:08X} -> "
                 "native {:08X} original {:08X}; native constant upload off",
                 same_mem, same_end, same_regs, saved.r4.u64, saved.r5.u32, obj, w0, native_end,
                 REX_LOAD_U32(obj + 48));
  } else if (n <= 4 || n == verify) {
    REXLOG_WARN("[p2-const] verify {} OK ({} bytes)", n, plan.end_ptr - w0);
  }
}
