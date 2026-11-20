// SPDX-License-Identifier: GPL-3.0-or-later
// PERF pass 75: shader-constant register writes from the PM4 stream (SET_CONSTANT and friends).
//
// NativeGraphicsSystem::WriteRegisters took ~7 % of the GPU ring thread (pass 57 samples, pass 72 ELF): with
// -fno-strict-aliasing every store into the register file may alias the Reader's uint32_t fields and the
// registers_ pointer, so the loop reloaded and stored them for every word. This version keeps the stream state in
// locals, walks the four constant classes as segments and finds the fetch slot without a division.
// Result is identical to the original: the same final register values (a store happens only when the value
// differs), the same four change flags and the same reader position, including a ring that wraps.
#pragma once

#include <cstdint>
#include <cstring>

namespace p2::native::constant_writes {

constexpr uint32_t kFirst = 0x4000;   // first VS float constant register
constexpr uint32_t kPsFloat = 0x4400;
constexpr uint32_t kFetch = 0x4800;
constexpr uint32_t kBoolLoop = 0x48C0;
constexpr uint32_t kEnd = 0x4928;

struct Changes {
  bool vs_float = false;
  bool ps_float = false;
  bool bool_loop = false;
  uint32_t fetch_slots = 0;
  bool operator==(const Changes& o) const {
    return vs_float == o.vs_float && ps_float == o.ps_float && bool_loop == o.bool_loop &&
           fetch_slots == o.fetch_slots;
  }
};

inline uint32_t LoadBig(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, sizeof(v));
  return __builtin_bswap32(v);
}

// The original loop, word by word through the reader's own Read() logic. Reference for verify and tests.
// mask: ring words - 1 (0 = linear buffer); returns the new position.
inline uint32_t Reference(uint32_t* registers, uint32_t index, uint32_t count, const uint8_t* base,
                          uint32_t mask, uint32_t pos, Changes& out) {
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t r = index + i;
    const uint32_t v = LoadBig(base + size_t(pos) * 4);
    pos = mask ? ((pos + 1) & mask) : (pos + 1);
    if (registers[r] == v) continue;
    registers[r] = v;
    if (r < kPsFloat) {
      out.vs_float = true;
    } else if (r < kFetch) {
      out.ps_float = true;
    } else if (r < kBoolLoop) {
      out.fetch_slots |= 1u << ((r - kFetch) / 6);
    } else {
      out.bool_loop = true;
    }
  }
  return pos;
}

namespace detail {
// Contiguous source words for registers [r, r + n), all within one class (or the fetch class).
inline bool PlainRun(uint32_t* __restrict regs, const uint8_t* __restrict src, uint32_t n) {
  uint32_t changed = 0;
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t v = LoadBig(src + size_t(i) * 4);
    const uint32_t old = regs[i];
    if (old != v) {
      regs[i] = v;
      changed = 1;
    }
  }
  return changed != 0;
}

inline uint32_t FetchRun(uint32_t* __restrict regs, const uint8_t* __restrict src, uint32_t r, uint32_t n) {
  uint32_t slots = 0;
  uint32_t slot = (r - kFetch) / 6, sub = (r - kFetch) % 6;
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t v = LoadBig(src + size_t(i) * 4);
    if (regs[i] != v) {
      regs[i] = v;
      slots |= 1u << slot;
    }
    if (++sub == 6) {
      sub = 0;
      ++slot;
    }
  }
  return slots;
}

// Registers [r, r + n) from contiguous source words.
inline void Contiguous(uint32_t* registers, uint32_t r, uint32_t n, const uint8_t* src, Changes& out) {
  const uint32_t end = r + n;
  static constexpr uint32_t kBounds[5] = {kFirst, kPsFloat, kFetch, kBoolLoop, kEnd};
  for (uint32_t s = 0; s < 4; ++s) {
    // Overlap of [r, end) with this class.
    const uint32_t a = r > kBounds[s] ? r : kBounds[s];
    const uint32_t b = end < kBounds[s + 1] ? end : kBounds[s + 1];
    if (a >= b) continue;
    const uint8_t* p = src + size_t(a - r) * 4;
    switch (s) {
      case 0: if (PlainRun(registers + a, p, b - a)) out.vs_float = true; break;
      case 1: if (PlainRun(registers + a, p, b - a)) out.ps_float = true; break;
      case 2: out.fetch_slots |= FetchRun(registers + a, p, a, b - a); break;
      default: if (PlainRun(registers + a, p, b - a)) out.bool_loop = true; break;
    }
  }
}
}  // namespace detail

// Same contract as Reference(); requires kFirst <= index and index + count <= kEnd (the caller's own check),
// pos <= mask when mask != 0.
inline uint32_t Fast(uint32_t* registers, uint32_t index, uint32_t count, const uint8_t* base, uint32_t mask,
                     uint32_t pos, Changes& out) {
  if (!count) return pos;
  if (!mask) {
    detail::Contiguous(registers, index, count, base + size_t(pos) * 4, out);
    return pos + count;
  }
  const uint32_t ring = mask + 1;
  const uint32_t first = count < ring - pos ? count : ring - pos;
  detail::Contiguous(registers, index, first, base + size_t(pos) * 4, out);
  uint32_t done = first;
  // A packet is shorter than the ring, but stay exact for any count: wrap as often as needed.
  while (done < count) {
    const uint32_t n = count - done < ring ? count - done : ring;
    detail::Contiguous(registers, index + done, n, base, out);
    done += n;
  }
  return (pos + count) & mask;
}

}  // namespace p2::native::constant_writes
