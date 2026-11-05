"""B-001 (2026-11-04): log every NULL from the game's allocator front end sub_8222FE80(heap index, size)
(heaps[index]->vtable[6](size)). The 2026-11-04 crash came from sub_82C0A6C0, whose 200-byte object from
sub_8222FF20 -> sub_8222FE80 was NULL; the stream-arena overflow hooks did not see it. The hook only reports
(project/src/native/stream_arena_overflow.cpp, P2HeapNull); behaviour unchanged. Idempotent; refuses unexpected input.
usage: python tools/p2_heap_null_patch.py [project dir, default ./project]"""
import sys
from pathlib import Path
gen = Path(sys.argv[1] if len(sys.argv) > 1 else 'project') / 'generated' / 'default'
f = gen / 'prototype2_recomp.137.cpp'
s = f.read_text(encoding='utf-8')
if '/* P2 heap null */' in s:
    print('already patched'); sys.exit(0)
a = s.find('DEFINE_REX_FUNC(sub_8222FE80)')
if a < 0: sys.exit('sub_8222FE80 not found')
b = s.find('\nDEFINE_REX_FUNC(', a + 10)
body = s[a:b]
old = '\t// mr r31,r3\n\tr31.u64 = ctx.r3.u64;\n'
if body.count(old) != 1: sys.exit(f'anchor found {body.count(old)} times; refusing')
new = old + ('\tif (!ctx.r3.u32) P2HeapNull(r30.u32, r29.u32, ctr.u32, REX_LOAD_U32(ctx.r1.u32 + 104)); '
             '/* P2 heap null */\n')
s = s[:a] + body.replace(old, new) + s[b:]
first = s.find('\n') + 1
s = s[:first] + 'extern "C" void P2HeapNull(uint32_t, uint32_t, uint32_t, uint32_t); /* P2 heap null */\n' + s[first:]
f.write_text(s, encoding='utf-8', newline='\n')
print('sub_8222FE80 patched')
