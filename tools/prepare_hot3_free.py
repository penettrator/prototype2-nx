"""PERF pass 16: optional lock-free msr regions in a THIRD set of guest functions (mode 5 only), chosen from the
global-mutex contention census (perf_results/pass_16): the job-system spin/counter atomics the workers fight over
(sub_8299D0A8 alone: 6.5 M contended acquisitions, 99 s of waiting in ~5 min). Same mechanism as hot2 (copied from
tools/prepare_hot2_free.py, original description below); flag p2_msr_hot3_free.

PERF pass 4: optional lock-free msr regions in a second set of guest functions (run in mode 5 only).

Pass 2 profile (perf_results/pass_2): the simulation and Main threads wait on the SDK global mutex that mode 5
takes for every lwarx..stwcx region outside the HOT list of prepare_switch_codegen.py. The regions of the
functions below were the ones seen waiting/holding it. Their REX_ENTER_ADDR_LOCK(ea) becomes conditional on the
global flag p2_msr_hot2_free (set at the simulation-loop start from the cvar of the same name, mode 5 only;
default false = exact previous behaviour). Idempotent; refuses unexpected input.
usage: python tools/prepare_hot2_free.py [project dir, default ./project]
"""
import sys
from pathlib import Path

HOT2 = ['sub_8299D0A8', 'sub_82C9D220', 'sub_829AC008', 'sub_8299CC98', 'sub_829AC380',
        'sub_8227B258', 'sub_822E9688', 'sub_821E7890', 'sub_829AC940',
        'sub_821C2208', 'sub_822A4CB8', 'sub_82A6BAA8', 'sub_829AC2E8', 'sub_8299CFE0',
        'sub_8299D3C8']
OLD = '\n\tREX_ENTER_ADDR_LOCK(ea);'
NEW = '\n\t{ extern bool p2_msr_hot3_free; if (!p2_msr_hot3_free) { REX_ENTER_ADDR_LOCK(ea); } } /* P2 hot3 */'

gen = Path(sys.argv[1] if len(sys.argv) > 1 else 'project') / 'generated' / 'default'
found = {}
marked = 0
for f in sorted(gen.glob('prototype2_recomp.*.cpp')):
    src = f.read_text(encoding='utf-8')
    out = src
    for fn in HOT2:
        a = out.find('DEFINE_REX_FUNC(' + fn + ')')
        if a < 0:
            continue
        b = out.find('\nDEFINE_REX_FUNC(', a + 10)
        b = len(out) if b < 0 else b
        body = out[a:b]
        if 'REX_ENTER_HOT_LOCK(ea);' in body:
            sys.exit(f'{fn}: already in the HOT list; refusing')
        n_new = body.count(OLD)
        found[fn] = n_new + body.count('/* P2 hot3 */')
        out = out[:a] + body.replace(OLD, NEW) + out[b:]
        marked += n_new
    if out != src:
        starts = out.count('REX_ENTER_GLOBAL_LOCK();')
        entries = out.count('REX_ENTER_ADDR_LOCK(ea);') + out.count('REX_ENTER_HOT_LOCK(ea);')
        if starts != entries:
            sys.exit(f'{f.name}: {starts} regions but {entries} lock entries; refusing')
        f.write_text(out, encoding='utf-8', newline='\n')
missing = [fn for fn in HOT2 if not found.get(fn)]
if missing:
    sys.exit(f'functions without msr regions or not found: {missing}')
print(f'hot3 regions newly marked: {marked}; regions per function: {found}')
