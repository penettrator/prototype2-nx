// SPDX-License-Identifier: GPL-3.0-or-later
// Bounded spare capacity for the game's dual city-streaming arenas.
// The factory body below mirrors sub_82CA29E0; only backing sizes change.
// Original allocator IDs, allocation flags, registration, reset and destruction remain in charge.
#include "generated/default/prototype2_pch.h"
#include "stream_heap_budget.h"
#include <atomic>
#include <algorithm>
#ifndef P2_STREAM_BUDGET_TEST
#include <rex/cvar.h>
#include <rex/logging.h>
REXCVAR_DEFINE_UINT32(p2_stream_heap_margin_kib, 0, "Prototype2",
    "Extra KiB in each dual city-stream heap arena, capped at128; 0=original; restart required")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
#endif
REX_EXTERN(__imp__sub_822F0C20);
REX_EXTERN(__imp__sub_82CA29E0);
REX_EXTERN(sub_8222FE80);
REX_EXTERN(sub_8222F980);
REX_EXTERN(sub_822E94C8);
REX_EXTERN(__imp__sub_822E94C8);
REX_EXTERN(sub_82CA2860);
REX_EXTERN(__imp__sub_82CA2860);

namespace {
struct BudgetScope {
    bool active = false;
    uint32_t heap = 0, margin = 0, normal = 0, other = 0;
    uint32_t normal_actual = 0, other_actual = 0, fallbacks = 0;
};
thread_local BudgetScope stream_budget_scope;
struct Record {
    std::atomic<uint32_t> normal{0}, other{0}, normal_actual{0}, other_actual{0}, fallbacks{0}, object{0};
};
Record stream_budget_records[256];
uint32_t StreamBacking(PPCContext& ctx, uint8_t* base, PPCFunc* allocate,
                       uint32_t original, bool other) {
    const uint32_t enlarged = p2::StreamHeapBudget(original, stream_budget_scope.margin);
    // This is creation-time only. A full snapshot is needed for the original-size retry:
    // game allocators may clobber volatile registers and LR even when returning null.
    const PPCContext checkpoint = ctx;
    ctx.r4.u64 = enlarged;
    allocate(ctx, base);
    uint32_t actual = enlarged;
    if (ctx.r3.u32 == 0 && enlarged != original) {
        ctx = checkpoint;
        allocate(ctx, base);
        actual = original;
        ++stream_budget_scope.fallbacks;
    }
    if (other) stream_budget_scope.other_actual = actual;
    else stream_budget_scope.normal_actual = actual;
    return actual;
}
} // namespace
extern "C" REX_FUNC(sub_822F0C20) {
    // Only the original city shared-slot callback. Other uses retain the original behavior.
    const uint32_t margin = std::min(REXCVAR_GET(p2_stream_heap_margin_kib), 128u);
    if (!margin || ctx.lr != 0x82397C28 || ctx.r3.u32 >= 256) {
        __imp__sub_822F0C20(ctx, base);
        return;
    }
    const BudgetScope previous = stream_budget_scope;
    stream_budget_scope = {true, ctx.r3.u32, margin, ctx.r4.u32, ctx.r5.u32,
                           ctx.r4.u32, ctx.r5.u32, 0};
    __imp__sub_822F0C20(ctx, base);
    const auto report = stream_budget_scope;
    auto& record = stream_budget_records[report.heap];
    record.normal.store(report.normal, std::memory_order_relaxed);
    record.other.store(report.other, std::memory_order_relaxed);
    record.normal_actual.store(report.normal_actual, std::memory_order_relaxed);
    record.other_actual.store(report.other_actual, std::memory_order_relaxed);
    record.fallbacks.store(report.fallbacks, std::memory_order_relaxed);
    record.object.store(ctx.r3.u32, std::memory_order_release);
    stream_budget_scope = previous;
#ifndef P2_STREAM_BUDGET_TEST
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed))
        REXLOG_WARN("[p2-stream-budget] enabled margin_KiB={} heap={} normal={}->{} other={}->{} fallback={} object={:08X}",
            margin, report.heap, report.normal, report.normal_actual, report.other, report.other_actual,
            report.fallbacks, ctx.r3.u32);
#endif
}
extern "C" void P2StreamHeapFailureReport(uint32_t heap) {
#ifndef P2_STREAM_BUDGET_TEST
    if (heap >= 256) return;
    auto& r = stream_budget_records[heap];
    const uint32_t object = r.object.load(std::memory_order_acquire);
    if (object) REXLOG_ERROR("[p2-stream-budget] failing heap={} object={:08X} normal={}->{} other={}->{} fallback={}",
        heap, object, r.normal.load(), r.normal_actual.load(), r.other.load(), r.other_actual.load(), r.fallbacks.load());
#endif
}

extern "C" REX_FUNC(sub_82CA29E0) {
	REX_FUNC_PROLOGUE();
    if (!stream_budget_scope.active || ctx.lr != 0x822F0C54) {
        __imp__sub_82CA29E0(ctx, base);
        return;
    }
	PPCXERRegister xer{};
	PPCCRRegister cr6{};
	PPCRegister r25{};
	PPCRegister r26{};
	PPCRegister r27{};
	PPCRegister r28{};
	PPCRegister r29{};
	PPCRegister r30{};
	PPCRegister r31{};
	uint32_t ea{};
	// mflr r12
	ctx.r12.u64 = ctx.lr;
	// bl 0x82c2ccdc
	ctx.lr = 0x82CA29E8;
	// stwu r1,-144(r1)
	ea = -144 + ctx.r1.u32;
	REX_STORE_U32(ea, ctx.r1.u32);
	ctx.r1.u32 = ea;
	// mr r31,r4
	r31.u64 = ctx.r4.u64;
	// mr r25,r3
	r25.u64 = ctx.r3.u64;
	// mr r4,r5
	ctx.r4.u64 = ctx.r5.u64;
	// mr r3,r31
	ctx.r3.u64 = r31.u64;
	// mr r29,r5
	r29.u64 = ctx.r5.u64;
	// mr r30,r6
	r30.u64 = ctx.r6.u64;
	// mr r27,r7
	r27.u64 = ctx.r7.u64;
	// bl 0x8222fe80
	ctx.lr = 0x82CA2A0C;
	r29.u64 = StreamBacking(ctx, base, sub_8222FE80, r29.u32, false);
	// mr r26,r3
	r26.u64 = ctx.r3.u64;
	// li r28,0
	r28.s64 = 0;
	// cmplwi cr6,r30,0
	cr6.compare<uint32_t>(r30.u32, 0, xer);
	// beq cr6,0x82ca2a38
	if (cr6.eq) goto loc_82CA2A38;
	// li r7,0
	ctx.r7.s64 = 0;
	// li r6,2
	ctx.r6.s64 = 2;
	// li r5,16
	ctx.r5.s64 = 16;
	// mr r4,r30
	ctx.r4.u64 = r30.u64;
	// mr r3,r31
	ctx.r3.u64 = r31.u64;
	// bl 0x8222f980
	ctx.lr = 0x82CA2A34;
	r30.u64 = StreamBacking(ctx, base, sub_8222F980, r30.u32, true);
	// mr r28,r3
	r28.u64 = ctx.r3.u64;
loc_82CA2A38:
	// li r4,128
	ctx.r4.s64 = 128;
	// mr r3,r25
	ctx.r3.u64 = r25.u64;
	// bl 0x822e94c8
	ctx.lr = 0x82CA2A44;
	P2_DIRECT_CALL(sub_822E94C8)(ctx, base);
	// cmplwi cr6,r3,0
	cr6.compare<uint32_t>(ctx.r3.u32, 0, xer);
	// beq cr6,0x82ca2a70
	if (cr6.eq) goto loc_82CA2A70;
	// mr r9,r27
	ctx.r9.u64 = r27.u64;
	// mr r8,r30
	ctx.r8.u64 = r30.u64;
	// mr r7,r28
	ctx.r7.u64 = r28.u64;
	// mr r6,r29
	ctx.r6.u64 = r29.u64;
	// mr r5,r26
	ctx.r5.u64 = r26.u64;
	// mr r4,r31
	ctx.r4.u64 = r31.u64;
	// bl 0x82ca2860
	ctx.lr = 0x82CA2A68;
	P2_DIRECT_CALL(sub_82CA2860)(ctx, base);
	// addi r1,r1,144
	ctx.r1.s64 = ctx.r1.s64 + 144;
	// b 0x82c2cd2c
	return;
loc_82CA2A70:
	// li r3,0
	ctx.r3.s64 = 0;
	// addi r1,r1,144
	ctx.r1.s64 = ctx.r1.s64 + 144;
	// b 0x82c2cd2c
	return;
}

