#!/usr/bin/env python3
"""Prototype 2 adaptation of sa2-nx tools/float_loads.py (PERF pass 13). A guest single-precision load
    temp.u32 = REX_LOAD_U32(x);
    F.f64 = double(temp.f32);
becomes
    F.f64 = double(REX_LOAD_F32(x));
REX_LOAD_F32 loads the 4 bytes straight into the vector unit and reverses them there (ldr s + rev32) instead of
ldr w + rev + fmov (the general->vector move costs 5 cycles on the A57). Same bits. REX_FLOAT_LOADS_INTEGER gives
the integer form back. Volatile loads (REX_LOAD_V_U32, the hang guard of prepare_no_volatile.py) and folded constants
(REX_RO_U32) are left alone; so is a load from an address a float was just stored to in the same straight line.

  python tools/p2_float_loads.py --gen <generated/default> [--check]
Idempotent.
"""
import argparse, os, re, sys

STORE = re.compile(r'^\tREX_STORE_U32\((.*), (?:ctx\.)?temp\.u32\);$')
FENCE = re.compile(r'^[^\t]|\(ctx, base[,)]|REX_CALL_INDIRECT_')
LOAD = re.compile(r'^\t((?:ctx\.)?temp)\.u32 = REX_LOAD_U32\((.*)\);$')
USE = re.compile(r'^\t((?:ctx\.|p2_leaf_)?f\d+)\.f64 = double\(((?:ctx\.)?temp)\.f32\);$')
NEW = re.compile(r'^\t((?:ctx\.|p2_leaf_)?f\d+)\.f64 = double\(REX_LOAD_F32\((.*)\)\);$')
MARKER = '// Prototype 2 PERF pass 13: single-precision loads swapped in the vector unit (tools/p2_float_loads.py)'
MACROS = MARKER + '''
#ifndef REX_LOAD_F32
#if defined(__aarch64__) && !defined(REX_FLOAT_LOADS_INTEGER)
#include <arm_neon.h>
[[gnu::always_inline]] static inline float RexFloatFromBigEndian(const void* p) noexcept {
  float f;
  __builtin_memcpy(&f, p, 4);  // ldr s
  const float32x2_t v = vset_lane_f32(f, vdup_n_f32(0.0f), 0);
  return vget_lane_f32(vreinterpret_f32_u8(vrev32_u8(vreinterpret_u8_f32(v))), 0);  // rev32
}
#define REX_LOAD_F32(x) RexFloatFromBigEndian(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x))
#else
#define REX_LOAD_F32(x) std::bit_cast<float>(u32(REX_LOAD_U32(x)))
#endif
#endif
'''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', required=True)
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args()
    files = sorted(f for f in os.listdir(a.gen) if re.match(r'\w+_recomp\.\d+\.cpp$', f))
    pchs = [f for f in os.listdir(a.gen) if f.endswith('_pch.h')]
    if not files or len(pchs) != 1:
        sys.exit('ERROR: p2_float_loads: no *_recomp.N.cpp or no single *_pch.h in %s' % a.gen)
    sites = present = changed = forwarded = 0
    for f in files:
        path = os.path.join(a.gen, f)
        lines = open(path, encoding='utf-8').read().split('\n')
        out = []
        i = 0
        n = 0
        stored = set()
        while i < len(lines):
            line = lines[i]
            if FENCE.search(line):
                stored.clear()
            else:
                st = STORE.match(line)
                if st:
                    stored.add(st.group(1))
            if NEW.match(line):
                present += 1
            m = LOAD.match(line)
            u = USE.match(lines[i + 1]) if m and i + 1 < len(lines) else None
            if m and u and u.group(2) == m.group(1) and m.group(2) in stored:
                forwarded += 1
            elif m and u and u.group(2) == m.group(1):
                out.append('\t%s.f64 = double(REX_LOAD_F32(%s));' % (u.group(1), m.group(2)))
                out.append('\t// (p2_float_loads: load merged into the line above)')  # keeps the line numbers
                n += 1
                i += 2
                continue
            out.append(line)
            i += 1
        if n:
            sites += n
            changed += 1
            if not a.check:
                open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(out))
    pch = os.path.join(a.gen, pchs[0])
    text = open(pch, encoding='utf-8').read()
    if MARKER not in text:
        anchor = '#define REX_MEMORY_SIZE'
        if anchor not in text:
            sys.exit('ERROR: p2_float_loads: no REX_MEMORY_SIZE in %s' % pch)
        if not a.check:
            open(pch, 'w', encoding='utf-8', newline='\n').write(text.replace(anchor, MACROS + anchor, 1))
    print('p2_float_loads%s: %d single-precision loads rewritten in %d files (%d already; %d kept: a float was just '
          'stored to the same address)' % (' (check only)' if a.check else '', sites, changed, present, forwarded))


if __name__ == '__main__':
    main()
