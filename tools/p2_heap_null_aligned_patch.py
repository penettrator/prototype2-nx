"""B-001 (2026-11-16): log every final NULL of the game's aligned allocation fallback sub_8222F980
(heaps[index]->vtable[10](size, align, flags), then the out-of-memory callback). The user-reported 0.5.0 crash wrote a
128 KiB stream-loader buffer (sub_82C02B38 -> sub_82C02AB0 -> sub_8222F980) to address 0 with no [p2-arena-overflow]
and no [p2-heap-null] line. The hook only reports (project/src/native/stream_arena_overflow.cpp, P2HeapNullAligned);
behaviour unchanged. Idempotent; refuses unexpected input.
usage: python tools/p2_heap_null_aligned_patch.py [project dir, default ./project]"""
import sys
from pathlib import Path
MARK = '/* P2 heap null aligned */'
gen = Path(sys.argv[1] if len(sys.argv) > 1 else 'project') / 'generated' / 'default'
hits = [f for f in sorted(gen.glob('prototype2_recomp.*.cpp'))
        if 'DEFINE_REX_FUNC(sub_8222F980)' in f.read_text(encoding='utf-8')]
if len(hits) != 1: sys.exit(f'sub_8222F980 found in {len(hits)} files; refusing')
f = hits[0]
s = f.read_text(encoding='utf-8')
if MARK in s:
    print('already patched'); sys.exit(0)
if s.count('DEFINE_REX_FUNC(sub_8222F980)') != 1: sys.exit('sub_8222F980 defined more than once; refusing')
a = s.find('DEFINE_REX_FUNC(sub_8222F980)')
b = s.find('\nDEFINE_REX_FUNC(', a + 10)
if b < 0: b = len(s)
body = s[a:b]
edits = [
    # caller: the return address the caller set before bl (ctx.lr on entry)
    ('\t// mflr r12\n\tctx.r12.u64 = ctx.lr;\n',
     '\tuint32_t p2_hna_caller = uint32_t(ctx.lr), p2_hna_obj = 0, p2_hna_fn = 0; ' + MARK + '\n'),
    # heaps[index]
    ('\t// lwzx r3,r10,r11\n\tctx.r3.u64 = REX_LOAD_U32(ctx.r10.u32 + ctx.r11.u32);\n',
     '\tp2_hna_obj = ctx.r3.u32; ' + MARK + '\n'),
    # its vtable[10] function (ctr is reused by the OOM callback below)
    ('\t// mtctr r8\n\tctr.u64 = ctx.r8.u64;\n',
     '\tp2_hna_fn = ctx.r8.u32; ' + MARK + '\n'),
    # common exit: r30 = result after the OOM callback
    ('loc_8222FA14:\n\t// mr r3,r30\n',
     '\tif (!r30.u32) { uint32_t p2_hna_state[10] = {}; if (p2_hna_obj) for (int i = 0; i < 10; ++i) '
     'p2_hna_state[i] = REX_LOAD_U32(p2_hna_obj + 88 + 4 * i); '
     'P2HeapNullAligned(r29.u32, p2_hna_obj, p2_hna_fn, r28.u32, r31.u32, r27.u32, REX_LOAD_U32(0x82E9A4BCu), '
     'p2_hna_caller, p2_hna_state); } ' + MARK + '\n'),
]
for old, add in edits:
    if body.count(old) != 1: sys.exit(f'anchor found {body.count(old)} times; refusing: {old!r}')
    body = body.replace(old, old + add)
s = s[:a] + body + s[b:]
first = s.find('\n') + 1
s = (s[:first] + 'extern "C" void P2HeapNullAligned(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, '
     'uint32_t, const uint32_t*); ' + MARK + '\n' + s[first:])
f.write_text(s, encoding='utf-8', newline='\n')
print(f'sub_8222F980 patched in {f.name}')
