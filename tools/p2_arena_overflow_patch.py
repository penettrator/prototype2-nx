#!/usr/bin/env python3
"""B-001 (Prototype 2): hooks of src/native/stream_arena_overflow.cpp in the game's dual stream arena functions.
Line-for-line edits of the generated code (each hook is appended to an existing statement), idempotent, reversible
(--undo). All hooks do nothing unless p2_stream_arena_overflow is set.
  sub_82CA2008 (GPU part alloc), sub_82CA1F50 (normal part alloc): the null result of a part WITH capacity becomes
      an overflow block from the part's guest heap;
  sub_82CA22E0 (free): an overflow block is released (the arena's own free ignores it: outside its ranges);
  sub_82CA25C8 (destroy): overflow blocks still owned by the arena are released.
  python tools/p2_arena_overflow_patch.py [--gen project/generated/default] [--undo]
"""
import argparse, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TAG = ' /* P2 B-001 */'
EDITS = [
    ('sub_82CA2008', '\tctx.r3.u64 = r28.u64;',
     ' if (P2ArenaOverflowEnabled()) ctx.r3.u64 = P2ArenaOverflowAlloc(base, r31.u32, r30.u32, p2_trace_alignment, 1);'),
    ('sub_82CA1F50', '\tctx.r3.u64 = r26.u64;',
     ' if (P2ArenaOverflowEnabled()) ctx.r3.u64 = P2ArenaOverflowAlloc(base, r31.u32, r30.u32, p2_trace_alignment, 0);'),
    ('sub_82CA22E0', '\tr30.u64 = ctx.r4.u64;',
     ' if (P2ArenaOverflowEnabled()) P2ArenaOverflowFree(base, ctx.r3.u32, ctx.r4.u32);'),
    ('sub_82CA25C8', '\tif (P2ArenaTraceEnabled()) P2ArenaTraceRecord(5,ctx.r3.u32,0,0,0,0,0,0,nullptr); /* P2 arena history */',
     ' if (P2ArenaOverflowEnabled()) P2ArenaOverflowDestroy(base, ctx.r3.u32);'),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', default=os.path.join(ROOT, 'project', 'generated', 'default'))
    ap.add_argument('--undo', action='store_true')
    a = ap.parse_args()
    files = sorted(os.path.join(a.gen, f) for f in os.listdir(a.gen) if re.match(r'prototype2_recomp\.\d+\.cpp$', f))
    done = 0
    for func, anchor, hook in EDITS:
        hit = False
        for path in files:
            text = open(path, encoding='utf-8').read()
            start = text.find('\nDEFINE_REX_FUNC(%s) {' % func)
            if start < 0:
                continue
            end = text.find('\n}\n', start)
            body = text[start:end]
            patched = anchor + hook + TAG
            if a.undo:
                if patched not in body:
                    sys.exit('ERROR: %s: hook not found for undo' % func)
                body = body.replace(patched, anchor, 1)
            elif patched in body:
                hit = True
                break
            else:
                lines = body.split('\n')
                idx = [i for i, l in enumerate(lines) if l == anchor]
                if func in ('sub_82CA2008', 'sub_82CA1F50'):
                    idx = idx[-1:]  # the failure path is the last such statement
                if len(idx) < 1:
                    sys.exit('ERROR: %s: anchor not found: %r' % (func, anchor))
                lines[idx[0]] = patched
                body = '\n'.join(lines)
            open(path, 'w', encoding='utf-8', newline='\n').write(text[:start] + body + text[end:])
            hit = True
            done += 1
            break
        if not hit:
            sys.exit('ERROR: %s not found in %s' % (func, a.gen))
    print('p2_arena_overflow_patch: %d hooks %s' % (done, 'removed' if a.undo else 'added (others already there)'))


if __name__ == '__main__':
    main()
