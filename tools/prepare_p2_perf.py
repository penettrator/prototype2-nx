"""Prototype 2 0.3.0: the post-codegen performance pipeline, in the order the release was built (PERF passes 4-22).
Runs after tools/prepare_switch_codegen.py, prepare_no_volatile.py and prepare_switch_hot.py on project/generated:
  city-heap wrapper routes and stream-arena observation calls (B-001), hot2 lock-free msr set, .rdata constants (needs the image dump), float loads, single floats, inline memcpy/memset,
  indirect-call census hook + promotions (config/icall_census_pass13.csv), mode-5 msr try_lock fast path (pch),
  hot3/hot4 lock-free sets, stream arena overflow hooks, job census/boost hooks, heap NULL report.
Every step is idempotent and each behaviour has its own runtime flag (see config/prototype2.toml).
usage: python tools/prepare_p2_perf.py [--image build/image_dump.bin] [--xex assets/game_root/default.xex]"""
import argparse, os, subprocess, sys
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, 'project', 'generated', 'default')
ap = argparse.ArgumentParser()
ap.add_argument('--image', default=os.path.join(ROOT, 'build', 'image_dump.bin'))
ap.add_argument('--xex', default=os.path.join(ROOT, 'assets', 'game_root', 'default.xex'))
a = ap.parse_args()
py = [sys.executable, '-I']
# Not every step is idempotent (p2_single_floats finds more on a second pass), so the pipeline runs once per
# generated tree: a stamp at the end of the pch marks it; a fresh codegen writes a pch without it.
PCH = os.path.join(GEN, 'prototype2_pch.h')
STAMP = '/* P2 perf pipeline applied (tools/prepare_p2_perf.py) */'
if STAMP in open(PCH, encoding='utf-8').read():
    print('prepare_p2_perf: already applied to this generated tree')
    sys.exit(0)
def run(*args):
    print('==', ' '.join(os.path.basename(x) if x.endswith('.py') else x for x in args), flush=True)
    r = subprocess.run(py + [os.path.join(ROOT, 'tools', args[0])] + list(args[1:]), cwd=ROOT)
    if r.returncode: sys.exit('prepare_p2_perf: %s failed (%d)' % (args[0], r.returncode))
proj = os.path.join(ROOT, 'project')
bk = os.path.join(ROOT, 'build', 'prepare-backup')
# 2026-10-16 (B-001 work): city-heap wrappers kept overridable, stream-arena observation calls (flag-gated).
# (only when prepare_direct_calls still routes them through P2_DIRECT_CALL; with the hooks in src it already does not)
if 'P2_DIRECT_CALL(sub_822F0C20)' in open(os.path.join(GEN, 'prototype2_recomp.4.cpp'), encoding='utf-8').read():
    run('prepare_stream_heap_budget.py', proj, '--backup', os.path.join(bk, 'heap-budget'))
if 'P2 arena history' not in open(os.path.join(GEN, 'prototype2_recomp.110.cpp'), encoding='utf-8').read():
    run('prepare_stream_arena_trace.py', proj, '--backup', os.path.join(bk, 'arena-trace'))
# 2026-10-15 failure probes (log only on the failure branch): resource allocation / missing resource, allocator messages.
PROBE_TAG = '#include "src/native/resource_failure_probe.h"'
if PROBE_TAG not in open(os.path.join(GEN, 'prototype2_recomp.186.cpp'), encoding='utf-8').read():
    run('prepare_resource_failure_probe.py', proj, '--backup', os.path.join(bk, 'resource-probe'))
if PROBE_TAG not in open(os.path.join(GEN, 'prototype2_recomp.154.cpp'), encoding='utf-8').read():
    run('prepare_allocator_failure_probe.py', proj, '--backup', os.path.join(bk, 'allocator-probe'))
run('prepare_hot2_free.py', proj)
# The .rdata constants need the loaded, decrypted image (guest 0x82000000, 0xFD0000 bytes). It is game data and is
# never published: a PC run with PROTOTYPE2_DUMP_IMAGE=build/image_dump.bin writes it. Without it the step is skipped:
# the build stays correct, only the constant loads stay loads (the release NRO was built with it).
IMAGE_SHA256 = 'da88f6287614f432'  # prefix of the dump the release was built from
if os.path.isfile(a.image):
    import hashlib
    h = hashlib.sha256(open(a.image, 'rb').read()).hexdigest()
    if not h.startswith(IMAGE_SHA256):
        print('prepare_p2_perf: warning: %s sha256 %s differs from the release dump (%s...)' % (a.image, h[:16], IMAGE_SHA256), flush=True)
    run('p2_ro_constants.py', '--gen', GEN, '--image', a.image, '--xex', a.xex)
else:
    print('prepare_p2_perf: warning: no image dump at %s, .rdata constant folding skipped' % a.image, flush=True)
run('p2_float_loads.py', '--gen', GEN)
run('p2_single_floats.py', '--gen', GEN)
run('p2_inline_memcalls.py', '--gen', GEN)
run('p2_indirect_calls.py', '--hook')
run('p2_indirect_calls.py', '--promote', os.path.join(ROOT, 'config', 'icall_census_pass13.csv'))
# PERF pass 16: mode 5 (the release's msr lock) tries the SDK mutex first; a contended acquisition goes out of line.
pch_path = os.path.join(GEN, 'prototype2_pch.h')
pch = open(pch_path, encoding='utf-8').read()
OLD5 = ('#define REX_ENTER_ADDR_LOCK(a) do { if (rex_p2_msr_lock == 5) { rex::thread::global_critical_region::mutex().lock(); '
        'rex_p2_held_global_ = true; }')
NEW5 = ('/* PERF pass 16: mode 5 takes the lock with try_lock first; a contended acquisition goes through the out-of-line\n'
        '   rex_p2_msr_slow_ (switch_sim_priority.cpp), which waits for the mutex and counts the wait per call site. */\n'
        'extern "C" void rex_p2_msr_slow_(void);\n'
        '#define REX_ENTER_ADDR_LOCK(a) do { if (rex_p2_msr_lock == 5) { if (!rex::thread::global_critical_region::mutex().'
        'try_lock()) rex_p2_msr_slow_(); rex_p2_held_global_ = true; }')
if 'rex_p2_msr_slow_' not in pch:
    if pch.count(OLD5) != 1:
        sys.exit('prepare_p2_perf: mode-5 REX_ENTER_ADDR_LOCK not found once in the pch')
    open(pch_path, 'w', encoding='utf-8', newline='\n').write(pch.replace(OLD5, NEW5))
    print('== pch: mode-5 try_lock fast path', flush=True)
run('prepare_hot3_free.py', proj)
run('prepare_hot4_free.py', proj)
run('p2_arena_overflow_patch.py', '--gen', GEN)
run('prepare_job_census.py', proj)
run('p2_heap_null_patch.py', proj)
open(PCH, 'a', encoding='utf-8', newline='\n').write('\n' + STAMP + '\n')
print('prepare_p2_perf: done')
