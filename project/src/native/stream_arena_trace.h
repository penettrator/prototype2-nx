// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
// This only records scalar values from the original allocator under its own lock.
// No guest pointer dereferences, allocation replacement, or backing-size changes.
extern "C" bool P2ArenaTraceEnabled();
extern "C" void P2ArenaTraceRecord(uint32_t kind, uint32_t object,
    uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f,
    const uint32_t* failure_state);
// B-001 (stream_arena_overflow.cpp): overflow blocks for a full stream arena part; nothing unless
// p2_stream_arena_overflow. Hooked into the generated arena functions by tools/p2_arena_overflow_patch.py.
extern "C" bool P2ArenaOverflowEnabled();
extern "C" uint32_t P2ArenaOverflowAlloc(uint8_t* base, uint32_t arena, uint32_t size, uint32_t alignment,
                                         uint32_t part);
extern "C" void P2ArenaOverflowFree(uint8_t* base, uint32_t arena, uint32_t address);
extern "C" void P2ArenaOverflowDestroy(uint8_t* base, uint32_t arena);
