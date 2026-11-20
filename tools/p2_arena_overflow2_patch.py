#!/usr/bin/env python3
"""B-001 (Prototype 2, 2026-11-17): overflow hooks of src/native/stream_arena_overflow.cpp in the game's SECOND
stream arena class (vtable[10] sub_82CA2090; run134 crash: heap 148, 128 KiB GPU request). Same mechanism as
tools/p2_arena_overflow_patch.py for the first class; every hook does nothing unless p2_stream_arena_overflow is set.
  sub_82CA1B40 (GPU part alloc), sub_82CA1AA8 (normal part alloc): the null result becomes an overflow block;
  sub_82CA2120 (free): an overflow block is released (the class's own free ignores addresses outside its block);
  sub_82CA21D8 (reset) and sub_82CA23E8 (destroy): overflow blocks still owned by the arena are released.
Line-for-line additions after existing statements; idempotent; reversible (--undo); refuses unexpected input.
  python tools/p2_arena_overflow2_patch.py [--gen project/generated/default] [--undo]
"""
import argparse, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TAG = ' /* P2 B-001 arena2 */'
INCLUDE = '#include "src/native/stream_arena_trace.h"'
EDITS = [
    ('sub_82CA1B40', '\tr30.u64 = ctx.r4.u64;',
     ' const uint32_t p2_ov_size = ctx.r4.u32, p2_ov_align = ctx.r5.u32;'),
    ('sub_82CA1B40', '\tctx.r3.s64 = 0;',
     ' if (P2ArenaOverflowEnabled()) ctx.r3.u64 = P2ArenaOverflowAlloc2(base, r31.u32, p2_ov_size, p2_ov_align, 1);'),
    ('sub_82CA1AA8', '\tctx.r3.s64 = 0;',
     ' if (P2ArenaOverflowEnabled()) ctx.r3.u64 = P2ArenaOverflowAlloc2(base, r31.u32, r30.u32, r28.u32, 0);'),
    ('sub_82CA2120', '\tr30.u64 = ctx.r4.u64;',
     ' if (P2ArenaOverflowEnabled()) P2ArenaOverflowFree(base, ctx.r3.u32, ctx.r4.u32);'),
    ('sub_82CA21D8', '\tr31.u64 = ctx.r3.u64;',
     ' if (P2ArenaOverflowEnabled()) P2ArenaOverflowDestroy(base, ctx.r3.u32);'),
    ('sub_82CA23E8', '\tctx.r12.u64 = ctx.lr;',
     ' if (P2ArenaOverflowEnabled()) P2ArenaOverflowDestroy(base, ctx.r3.u32);'),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', default=os.path.join(ROOT, 'project', 'generated', 'default'))
    ap.add_argument('--undo', action='store_true')
    a = ap.parse_args()
    files = sorted(os.path.join(a.gen, f) for f in os.listdir(a.gen) if re.match(r'prototype2_recomp\.\d+\.cpp$', f))
    texts = {}
    owner = {}
    for func in sorted({e[0] for e in EDITS}):
        hits = [p for p in files if ('\nDEFINE_REX_FUNC(%s) {' % func) in (texts.get(p) or texts.setdefault(p, open(p, encoding='utf-8').read()))]
        if len(hits) != 1:
            sys.exit('ERROR: %s found in %d files' % (func, len(hits)))
        owner[func] = hits[0]
    changed = set()
    done = 0
    for func, anchor, hook in EDITS:
        path = owner[func]
        text = texts[path]
        start = text.find('\nDEFINE_REX_FUNC(%s) {' % func)
        end = text.find('\n}\n', start)
        body = text[start:end]
        patched = anchor + hook + TAG
        if a.undo:
            if patched in body:
                body = body.replace(patched, anchor, 1)
                done += 1
        elif patched not in body:
            lines = body.split('\n')
            idx = [i for i, l in enumerate(lines) if l == anchor]
            if len(idx) != 1:
                sys.exit('ERROR: %s: anchor %r found %d times; refusing' % (func, anchor, len(idx)))
            lines[idx[0]] = patched
            body = '\n'.join(lines)
            done += 1
        else:
            continue
        texts[path] = text[:start] + body + text[end:]
        changed.add(path)
    for path in sorted(set(owner.values())):
        text = texts[path]
        line = INCLUDE + TAG + '\n'
        if a.undo:
            if line in text and TAG.strip() not in text.replace(line, ''):
                texts[path] = text.replace(line, '', 1)
                changed.add(path)
        elif INCLUDE not in text:
            texts[path] = line + text
            changed.add(path)
    for path in changed:
        open(path, 'w', encoding='utf-8', newline='\n').write(texts[path])
    print('p2_arena_overflow2_patch: %d hooks %s, %d files written' % (done, 'removed' if a.undo else 'added', len(changed)))


if __name__ == '__main__':
    main()
