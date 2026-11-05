"""PERF pass 21: job census hooks in the generated code (flag p2_job_census, default false = previous behaviour).
- sub_82C9D220 (guest job worker loop): times the job call (bctrl at 0x82C9D2F4) and reports its function and
  the event of its countdown latch (job+20 -> latch, latch+4 -> event).
- sub_82238D00 (wait on a countdown latch): times the real wait (bl 0x8227B250) and reports the event and the
  caller's return address (saved LR at r1+88 after its stwu -96).
PERF pass 22: the worker hook passes the job and its latch event at the start, the wait registers its event first
(p2_job_boost). Idempotent; refuses unexpected input.
usage: python tools/prepare_job_census.py [project dir, default ./project]"""
import sys
from pathlib import Path
gen = Path(sys.argv[1] if len(sys.argv) > 1 else 'project') / 'generated' / 'default'
DECL = ('extern bool p2_job_census; extern "C" uint64_t p2_job_now(void); extern "C" uint64_t p2_latch_begin(uint32_t); extern "C" void p2_job_begin(void); extern "C" void p2_job_begin2(uint32_t, uint32_t); '
        'extern "C" void p2_job_end(uint32_t, uint32_t); extern "C" void p2_latch_wait(uint32_t, uint32_t, uint64_t); '
        '/* P2 job census */\n')
EDITS = {
 'prototype2_recomp.171.cpp': ('sub_82C9D220',
   '\tctx.lr = 0x82C9D2F8;\n\tREX_CALL_INDIRECT_FUNC(ctr.u32);\n',
   '\tif (p2_job_census) { const uint32_t p2_l = REX_LOAD_U32(ctx.r1.u32 + 100); p2_job_begin2(ctr.u32, p2_l ? REX_LOAD_U32(p2_l + 4) : 0u); } /* P2 job boost */\n\tctx.lr = 0x82C9D2F8;\n\tREX_CALL_INDIRECT_FUNC(ctr.u32);\n'
   '\tif (p2_job_census) { const uint32_t p2_l = REX_LOAD_U32(ctx.r1.u32 + 100); '
   'p2_job_end(ctr.u32, p2_l ? REX_LOAD_U32(p2_l + 4) : 0u); } /* P2 job census */\n'),
 'prototype2_recomp.133.cpp': ('sub_82238D00',
   '\tctx.lr = 0x82238D44;\n\tP2_DIRECT_CALL(sub_8227B250)(ctx, base);\n',
   '\t{ const uint32_t p2_ev = ctx.r3.u32; const uint64_t p2_t0 = p2_job_census ? p2_latch_begin(p2_ev) : 0; /* P2 job boost */\n'
   '\tctx.lr = 0x82238D44;\n\tP2_DIRECT_CALL(sub_8227B250)(ctx, base);\n'
   '\tif (p2_t0) p2_latch_wait(p2_ev, REX_LOAD_U32(ctx.r1.u32 + 88), p2_t0); } /* P2 job census */\n'),
}
for name, (fn, old, new) in EDITS.items():
    f = gen / name
    src = f.read_text(encoding='utf-8')
    if '/* P2 job census */' in src:
        print(f'{name}: already patched'); continue
    a = src.find('DEFINE_REX_FUNC(' + fn + ')')
    if a < 0: sys.exit(f'{fn} not in {name}')
    b = src.find('\nDEFINE_REX_FUNC(', a + 10)
    body = src[a:b]
    if body.count(old) != 1: sys.exit(f'{fn}: anchor found {body.count(old)} times; refusing')
    out = src[:a] + body.replace(old, new) + src[b:]
    first = out.find('\n') + 1
    out = out[:first] + DECL + out[first:]
    f.write_text(out, encoding='utf-8', newline='\n')
    print(f'{name}: {fn} patched')
