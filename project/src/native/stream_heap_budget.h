// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
namespace p2 {
// Zero arenas must stay zero: flags and the original factory use zero as a mode selector.
// Keep large/global arenas unchanged. At64KiB: at most128KiB extra per dual slot.
constexpr uint32_t StreamHeapBudget(uint32_t bytes, uint32_t margin_kib) {
    if (!bytes || bytes > (8u << 20)) return bytes;
    const uint32_t bounded = margin_kib > 128 ? 128 : margin_kib;
    return bytes + bounded * 1024u;
}
}
extern "C" void P2StreamHeapFailureReport(uint32_t heap);
