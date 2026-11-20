#!/usr/bin/env python3
"""B-014 (Prototype 2, 2026-11-18): fallback block of src/native/stream_arena_overflow.cpp for a new second-class
stream arena. sub_82CA2970 ignores a NULL from sub_821CE038 (heaps[index]->vtable[8]) and builds an arena at address 0
(crash at 0x003AFFB0 after 97 min, user report). Hooks:
  sub_82CA2970: a NULL block is reported and, with p2_arena2_block_fallback, replaced by a guest physical block;
  sub_822BD600 (heap free by index): a fallback block is released and never reaches the game's heap.
Line-for-line additions after existing statements; idempotent; reversible (--undo); refuses unexpected input.
  python tools/p2_arena2_block_patch.py [--gen project/generated/default] [--undo]
"""
import argparse, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TAG = ' /* P2 B-014 arena2 block */'
INCLUDE = '#include "src/native/stream_arena_trace.h"'
EDITS = [
    ('sub_82CA2970', '	r28.u64 = ctx.r3.u64;',
     ' if (!r28.u32) r28.u64 = P2Arena2BlockFallback(base, r31.u32, r30.u32);'),
    ('sub_822BD600', '	REX_FUNC_PROLOGUE();',
     ' if (P2Arena2BlockFree(base, ctx.r3.u32, ctx.r4.u32)) return;'),
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
    print('p2_arena2_block_patch: %d hooks %s, %d files written' % (done, 'removed' if a.undo else 'added', len(changed)))


if __name__ == '__main__':
    main()
