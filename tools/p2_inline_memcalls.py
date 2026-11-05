#!/usr/bin/env python3
"""Prototype 2 adaptation of sa2-nx tools/inline_memcalls.py (banjo-nx / sasrt-nx lineage), PERF pass 13.
A call of the game's CRT memcpy (sub_82C2CE00: first instruction `std r3,-8(r1)`, reloads r3 from there at its return)
or memset (sub_82C2CD60: byte fill, r3 untouched) whose size register r5 was set to a constant 1-64 (li r5,N) earlier
in the same straight line, with nothing in between that writes r5, a label or a call, becomes
    REX_MEMCPY_CONST(N, <the call as it was>);      REX_MEMSET_CONST(N, <the call as it was>);
The helpers copy / fill the N bytes inline and run the original call when they must not (overlap that the forward
copy smears, a destination over the r1-8 save slot, hardware-register or physical-view ranges, wrap-around): the
decisions are rex/ppc/memcalls.h (copied from sa2-nx). REX_INLINE_MEMCALLS_OFF (compile time) gives the calls back.

  python tools/p2_inline_memcalls.py --gen <generated/default> [--check]
"""
import argparse, collections, glob, os, re, sys

MEMCPY, MEMSET = '82C2CE00', '82C2CD60'
SET_R5 = re.compile(r'^\t(?:ctx\.|p2_leaf_)?r5\.s64 = (-?\d+);$')
WRITES_R5 = re.compile(r'(?:ctx\.|p2_leaf_|\b)r5\.[su](?:64|32|16|8) =|(?:ctx\.|p2_leaf_|\b)r5 =|\(ctx, base[,)]|'
                       r'REX_CALL_INDIRECT|ppc_trap|REX_PROMOTED')
CALL = re.compile(r'^(\t+)((?:P2_DIRECT_CALL\(sub_(%s|%s)\))|(?:(?:__imp__)?sub_(%s|%s)))\(ctx, base\);$' %
                  (MEMCPY, MEMSET, MEMCPY, MEMSET))
DONE = re.compile(r'REX_MEM(?:CPY|SET)_CONST\((\d+), ')
MARKER = '// Prototype 2 PERF pass 13: memcpy / memset of a constant size inline (tools/p2_inline_memcalls.py)'
MACROS = MARKER + r'''
#ifndef REX_MEMCPY_CONST
#if defined(REX_INLINE_MEMCALLS_OFF)
#define REX_MEMCPY_CONST(n, call) call
#define REX_MEMSET_CONST(n, call) call
#else
#include <rex/ppc/memcalls.h>
template <uint32_t N>
[[gnu::always_inline]] static inline bool RexMemcpyConst(PPCContext& ctx, uint8_t* base) noexcept {
  const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32;
  if (rex::ppc::CopySmears<N>(dst, src)) return false;
  if (!rex::ppc::PlainRange<N>(dst) || !rex::ppc::PlainRange<N>(src)) return false;
  if (rex::ppc::CoversSlot<N>(dst, ctx.r1.u32 - 8u)) return false;
  REX_STORE_U64(ctx.r1.u32 - 8, ctx.r3.u64);  // std r3,-8(r1), the original's first instruction
  rex::ppc::CopyConst<N>(REX_RAW_ADDR(dst), REX_RAW_ADDR(src));
  return true;
}
template <uint32_t N>
[[gnu::always_inline]] static inline bool RexMemsetConst(PPCContext& ctx, uint8_t* base) noexcept {
  const uint32_t dst = ctx.r3.u32;
  if (!rex::ppc::PlainRange<N>(dst)) return false;
  __builtin_memset(REX_RAW_ADDR(dst), int(ctx.r4.u32 & 0xFF), N);
  return true;
}
#define REX_MEMCPY_CONST(n, call) do { if (!RexMemcpyConst<n>(ctx, base)) { call; } } while (0)
#define REX_MEMSET_CONST(n, call) do { if (!RexMemsetConst<n>(ctx, base)) { call; } } while (0)
#endif
#endif
'''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', required=True)
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args()
    counts = collections.Counter()
    sizes = collections.Counter()
    changed = 0
    for path in sorted(glob.glob(os.path.join(a.gen, '*_recomp.*.cpp'))):
        text = open(path, encoding='utf-8').read()
        lines = text.split('\n')
        known = None
        for i, line in enumerate(lines):
            if not line.startswith('\t'):
                known = None
                continue
            if line.startswith('\t//'):
                continue
            m = SET_R5.match(line)
            if m:
                known = int(m.group(1))
                continue
            c = CALL.match(line)
            if c:
                which = 'memcpy' if (c.group(3) or c.group(4)) == MEMCPY else 'memset'
                counts[which + ' calls'] += 1
                if known is not None and 1 <= known <= 64:
                    lines[i] = '%sREX_%s_CONST(%d, %s(ctx, base));' % (c.group(1), which.upper(), known, c.group(2))
                    counts[which] += 1
                    sizes[(which, known)] += 1
                known = None
                continue
            if known is not None and WRITES_R5.search(line):
                known = None
        counts['already'] += len(DONE.findall(text))
        new = '\n'.join(lines)
        if new != text:
            changed += 1
            if not a.check:
                open(path, 'w', encoding='utf-8', newline='\n').write(new)
    pchs = glob.glob(os.path.join(a.gen, '*_pch.h'))
    if len(pchs) != 1:
        sys.exit('ERROR: p2_inline_memcalls: no single *_pch.h')
    text = open(pchs[0], encoding='utf-8').read()
    if MARKER not in text and not a.check:
        anchor = '#define REX_MEMORY_SIZE'
        if anchor not in text:
            sys.exit('ERROR: p2_inline_memcalls: no REX_MEMORY_SIZE')
        open(pchs[0], 'w', encoding='utf-8', newline='\n').write(text.replace(anchor, MACROS + anchor, 1))
    top = ', '.join('%s %d x%d' % (w, n, c) for (w, n), c in sizes.most_common(8))
    print('p2_inline_memcalls%s: %d of %d memcpy and %d of %d memset calls inline (%d files; %d already); %s' % (
        ' (check only)' if a.check else '', counts['memcpy'], counts['memcpy calls'], counts['memset'],
        counts['memset calls'], changed, counts['already'], top))


if __name__ == '__main__':
    main()
