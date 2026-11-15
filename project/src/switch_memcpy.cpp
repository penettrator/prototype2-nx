// SPDX-License-Identifier: GPL-3.0-or-later
// Prototype 2 PERF pass 48: the game's CRT memcpy (sub_82C2CE00: r3 = destination, r4 = source, r5 = byte count,
// returns r3 = destination; a leaf that also stores r3 at r1-8) in native code. Profile pass 48: 2.3 % of a core
// across the game's threads. For ranges that do not overlap (also checked in the 512 MB physical space, which the
// virtual mirrors alias) any copy order gives the same bytes, so host memcpy is exact; overlapping or wrapping
// ranges and zero lengths run the original. The first p2_memcpy_verify native-eligible calls run the original
// instead and check that it left the destination equal to the source (a plain copy); a difference turns the native
// path off for the session.
#include "generated/default/prototype2_pch.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <atomic>
#include <cstring>

REXCVAR_DEFINE_UINT32(p2_memcpy_native, 0, "Prototype2",
                      "PERF pass 48: native code for the game's memcpy sub_82C2CE00 (same bytes): 0 off, 1 on");
REXCVAR_DEFINE_UINT32(p2_memcpy_verify, 512, "Prototype2",
                      "First N native-eligible memcpy calls run the original and check the copy");
REX_EXTERN(__imp__sub_82C2CE00);

namespace {
std::atomic<uint32_t> g_checks{0};
std::atomic<bool> g_failed{false};
inline bool Overlap(uint64_t a, uint64_t b, uint64_t n) { return a < b + n && b < a + n; }
}  // namespace

extern "C" REX_FUNC(sub_82C2CE00) {
  const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, n = ctx.r5.u32;
  if (!REXCVAR_GET(p2_memcpy_native) || g_failed.load(std::memory_order_relaxed) || n == 0 ||
      uint64_t(dst) + n > 0x100000000ull || uint64_t(src) + n > 0x100000000ull || Overlap(dst, src, n) ||
      Overlap(dst & 0x1FFFFFFFu, src & 0x1FFFFFFFu, n)) {
    __imp__sub_82C2CE00(ctx, base);
    return;
  }
  const uint32_t verify = REXCVAR_GET(p2_memcpy_verify);
  if (g_checks.load(std::memory_order_relaxed) < verify &&
      g_checks.fetch_add(1, std::memory_order_relaxed) < verify) {
    __imp__sub_82C2CE00(ctx, base);
    if (std::memcmp(REX_RAW_ADDR(dst), REX_RAW_ADDR(src), n) != 0 || ctx.r3.u32 != dst) {
      g_failed.store(true, std::memory_order_relaxed);
      REXLOG_WARN("[p2-memcpy] original memcpy {:08X} <- {:08X} ({} bytes) is not a plain copy: native off", dst,
                  src, n);
    } else if (g_checks.load(std::memory_order_relaxed) == verify) {
      REXLOG_WARN("[p2-memcpy] {} memcpy calls checked: plain copies, native on", verify);
    }
    return;
  }
  REX_STORE_U64(ctx.r1.u32 - 8, ctx.r3.u64);  // std r3,-8(r1)
  std::memcpy(REX_RAW_ADDR(dst), REX_RAW_ADDR(src), n);
}
