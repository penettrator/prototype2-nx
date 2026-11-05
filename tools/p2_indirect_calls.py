#!/usr/bin/env python3
"""Prototype 2, PERF pass 13: indirect calls with one usual target called directly (after sa2-nx
tools/indirect_calls.py + indirect_promotions.py; ReXGlue lineage).

Step 1, --hook: the pch's REX_CALL_INDIRECT_FUNC gets P2_ICALL_CENSUS_HOOK(target): with -DP2_ICALL_CENSUS=1 (CMake
option P2_ICALL_CENSUS) every indirect call counts (site, target) in src/switch_icall_census.cpp and the counts are
written to logs/rex/icall_census.csv every 30 s; otherwise the hook is nothing. The site is ctx.lr, which the
generated code sets to the return address on the line before every `bctrl`.

Step 2, --promote <census.csv>: every `bctrl` site whose census has at least --min calls and whose targets in the
census are at most three with the top ones covering --cover of the calls becomes, on the same line,
    P2_PROMOTED2(ctr.u32, 0x82AAAAAA, P2_DIRECT_CALL(sub_82AAAAAA), 0x82BBBBBB, sub_82BBBBBB);
a compare per promoted target and a direct call (the compiler may inline it), else the old indirect call. A target that
has a native hook (tools/prepare_direct_calls.py's protected set) is called by its public name, so the hook still runs.
build/switch-hot/p2_promoted_decls.h declares the targets; the pch includes it. P2_PROMOTIONS_OFF gives the indirect
calls back. Idempotent: promoted lines are recognised and rewritten from the census again.

  python tools/p2_indirect_calls.py --hook
  python tools/p2_indirect_calls.py --promote perf_results/.../icall_census.csv [--min 2000] [--cover 0.9]
"""
import argparse, collections, csv, glob, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, 'project', 'generated', 'default')
HOT = os.path.join(ROOT, 'build', 'switch-hot')
sys.path.insert(0, os.path.join(ROOT, 'tools'))

HOOK_MARK = '// Prototype 2 PERF pass 13: indirect call census hook (tools/p2_indirect_calls.py)'
HOOK_DEF = HOOK_MARK + '''
#if defined(P2_ICALL_CENSUS) && P2_ICALL_CENSUS
extern "C" void p2_icall_census(uint32_t site, uint32_t target);
#define P2_ICALL_CENSUS_HOOK(t) p2_icall_census(uint32_t(ctx.lr), (t))
#else
#define P2_ICALL_CENSUS_HOOK(t) ((void)0)
#endif
'''
PROMO_MARK = '// Prototype 2 PERF pass 13: promoted indirect calls (tools/p2_indirect_calls.py)'
PROMO_DEF = PROMO_MARK + '''
#if defined(P2_PROMOTIONS_OFF)
#define P2_PROMOTED1(x, a, fa) REX_CALL_INDIRECT_FUNC(x)
#define P2_PROMOTED2(x, a, fa, b, fb) REX_CALL_INDIRECT_FUNC(x)
#define P2_PROMOTED3(x, a, fa, b, fb, c, fc) REX_CALL_INDIRECT_FUNC(x)
#else
#define P2_PROMOTED1(x, a, fa) \\
  do { const uint32_t p2_t_ = (uint32_t)(x); \\
       if (p2_t_ == (a)) [[likely]] { fa(ctx, base); } else { REX_CALL_INDIRECT_FUNC(p2_t_); } } while (0)
#define P2_PROMOTED2(x, a, fa, b, fb) \\
  do { const uint32_t p2_t_ = (uint32_t)(x); \\
       if (p2_t_ == (a)) [[likely]] { fa(ctx, base); } else if (p2_t_ == (b)) { fb(ctx, base); } \\
       else { REX_CALL_INDIRECT_FUNC(p2_t_); } } while (0)
#define P2_PROMOTED3(x, a, fa, b, fb, c, fc) \\
  do { const uint32_t p2_t_ = (uint32_t)(x); \\
       if (p2_t_ == (a)) [[likely]] { fa(ctx, base); } else if (p2_t_ == (b)) { fb(ctx, base); } \\
       else if (p2_t_ == (c)) { fc(ctx, base); } else { REX_CALL_INDIRECT_FUNC(p2_t_); } } while (0)
#endif
#if __has_include("p2_promoted_decls.h")
#include "p2_promoted_decls.h"
#endif
'''
LR = re.compile(r'^\tctx\.lr = 0x([0-9A-F]{8});$')
ICALL = re.compile(r'^\t(?:REX_CALL_INDIRECT_FUNC\(ctr\.u32\)|P2_PROMOTED\d\(ctr\.u32, .*\));$')


def pch_path():
    p = os.path.join(GEN, 'prototype2_pch.h')
    return p, open(p, encoding='utf-8').read()


def hook():
    p, s = pch_path()
    if HOOK_MARK in s:
        print('p2_indirect_calls: census hook already in the pch')
        return
    a = '    uint32_t rex_indirect_target_ = (uint32_t)(x);                                         \\\n'
    if s.count(a) != 1 or '#define REX_CALL_INDIRECT_FUNC(x)' not in s:
        sys.exit('ERROR: p2_indirect_calls: REX_CALL_INDIRECT_FUNC changed in the pch')
    s = s.replace(a, a + '    P2_ICALL_CENSUS_HOOK(rex_indirect_target_);                                            \\\n', 1)
    s = s.replace('#define REX_CALL_INDIRECT_FUNC(x)', HOOK_DEF + '#define REX_CALL_INDIRECT_FUNC(x)', 1)
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('p2_indirect_calls: census hook added to the pch')


def promote(census, minimum, cover):
    from prepare_direct_calls import protected_targets
    protected, _ = protected_targets()
    defined = set()
    for f in glob.glob(os.path.join(GEN, 'prototype2_recomp.*.cpp')):
        defined.update(re.findall(r'^DEFINE_REX_FUNC\((sub_[0-9A-F]{8})\)', open(f, encoding='utf-8').read(), re.M))
    sites = collections.defaultdict(collections.Counter)
    for row in csv.reader(open(census, encoding='utf-8')):
        if len(row) != 3 or not row[0].startswith(('8', '0x8')):
            continue
        sites[int(row[0], 16)][int(row[1], 16)] += int(row[2])
    total_calls = sum(sum(c.values()) for c in sites.values())
    chosen = {}
    for site, c in sites.items():
        n = sum(c.values())
        if n < minimum:
            continue
        top = [(t, k) for t, k in c.most_common(3) if k >= 0.1 * n and 'sub_%08X' % t in defined]
        if not top or sum(k for _, k in top) < cover * n:
            continue
        chosen[site] = [t for t, _ in top]
    p, s = pch_path()
    if PROMO_MARK not in s:
        s += '\n' + PROMO_DEF
        open(p, 'w', encoding='utf-8', newline='\n').write(s)
    targets = set()
    promoted = calls = 0
    for f in sorted(glob.glob(os.path.join(GEN, 'prototype2_recomp.*.cpp'))):
        lines = open(f, encoding='utf-8').read().split('\n')
        dirty = False
        for i in range(1, len(lines)):
            if not ICALL.match(lines[i]):
                continue
            m = LR.match(lines[i - 1])
            if not m:
                continue
            site = int(m.group(1), 16)
            if site in chosen:
                args = []
                for t in chosen[site]:
                    name = 'sub_%08X' % t
                    targets.add(name)
                    args.append('0x%08Xu, %s' % (t, name if name in protected else 'P2_DIRECT_CALL(%s)' % name))
                new = '\tP2_PROMOTED%d(ctr.u32, %s);' % (len(args), ', '.join(args))
                promoted += 1
                calls += sum(sites[site][t] for t in chosen[site])
            else:
                new = '\tREX_CALL_INDIRECT_FUNC(ctr.u32);'
            if new != lines[i]:
                lines[i] = new
                dirty = True
        if dirty:
            open(f, 'w', encoding='utf-8', newline='\n').write('\n'.join(lines))
    os.makedirs(HOT, exist_ok=True)
    open(os.path.join(HOT, 'p2_promoted_decls.h'), 'w', encoding='utf-8', newline='\n').write(
        '// Generated by tools/p2_indirect_calls.py: targets of promoted indirect calls.\n#pragma once\n' +
        ''.join('DECLARE_REX_FUNC(%s);\n' % t for t in sorted(targets)))
    print('p2_indirect_calls: census %d sites, %d calls; %d sites promoted (%d targets), covering %d calls (%.1f %%)' %
          (len(sites), total_calls, promoted, len(targets), calls, 100.0 * calls / max(total_calls, 1)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--hook', action='store_true')
    ap.add_argument('--promote')
    ap.add_argument('--min', type=int, default=2000)
    ap.add_argument('--cover', type=float, default=0.9)
    a = ap.parse_args()
    if a.hook:
        hook()
    if a.promote:
        promote(a.promote, a.min, a.cover)


if __name__ == '__main__':
    main()
