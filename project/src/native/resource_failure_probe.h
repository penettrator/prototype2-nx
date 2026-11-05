// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
extern "C" void P2ResourceAllocationFailed(uint32_t bytes, uint32_t flags,
    uint32_t metadata, uint32_t caller, uintptr_t context);
extern "C" void P2ResourceMissingBeforeLock(uint32_t object, uint32_t descriptor,
    uintptr_t context);
extern "C" void P2AllocatorFailureMessage(uint32_t format, const uint32_t* args,
    unsigned count, uint32_t caller, uintptr_t context);
