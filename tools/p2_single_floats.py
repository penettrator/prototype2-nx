#!/usr/bin/env python3
"""Prototype 2 adaptation of sa2-nx / banjo-nx / sasrt-nx tools/single_floats.py (PERF pass 13), with a simpler,
straight-line analysis. The generator writes fadds / fsubs / fmuls / fdivs / fsqrts / frsp in double and rounds:
    X.f64 = double(float(A.f64 + B.f64));
When A and B are exactly representable as singles, float arithmetic gives the same bits (double has more than
2*24+2 bits, so double rounding never differs for + - * / sqrt in any rounding mode, denormals, overflow, NaN; sa2-nx
sdk/tests/single_floats_test.cpp). Such statements become REX_FADDS(A.f64, B.f64) ... and frsp of a proven single a
copy (REX_FRSP_SINGLE). The compiler then keeps chains in single registers without fcvt pairs.

"Proven single" here: along one straight line of code (forgotten at every label, function boundary, brace / control
statement and call), a float register holds a single after a single-precision load (double(temp.f32) /
double(REX_LOAD_F32(..))), after any statement double(float(...)) (single ops, fused singles, frsp), after fmr / fneg
/ fabs / fnabs of a single and after fsel of two singles. Any other write to the register forgets it.
REX_SINGLE_FLOATS_OFF gives the double forms back. Line-for-line (PGO line numbers unchanged).

  python tools/p2_single_floats.py --gen <generated/default> [--check]
Idempotent.
"""
import argparse, glob, os, re, sys

F = r'(?:ctx\.|p2_leaf_)?f\d+'
LOAD_USE = re.compile(r'^\t(' + F + r')\.f64 = double\((?:(?:ctx\.)?temp\.f32|REX_LOAD_F32\(.*\))\);$')
BINARY = re.compile(r'^\t(' + F + r')\.f64 = double\(float\((' + F + r')\.f64 ([-+*/]) (' + F + r')\.f64\)\);$')
SQRT = re.compile(r'^\t(' + F + r')\.f64 = double\(float\(sqrt\((' + F + r')\.f64\)\)\);$')
FRSP = re.compile(r'^\t(' + F + r')\.f64 = double\(float\((' + F + r')\.f64\)\);$')
ANY_SINGLE = re.compile(r'^\t(' + F + r')\.f64 = double\(float\(.*\)\);$')
NEWFORM = re.compile(r'^\t(' + F + r')\.f64 = REX_F(?:ADDS|SUBS|MULS|DIVS|SQRTS|RSP_SINGLE)\(')
FMR = re.compile(r'^\t(' + F + r')\.f64 = (' + F + r')\.f64;$')
SIGN = re.compile(r'^\t(' + F + r')\.u64 = (' + F + r')\.u64 (?:\^ 0x8000000000000000|& ~0x8000000000000000|'
                  r'\| 0x8000000000000000);$')
FSEL = re.compile(r'^\t(' + F + r')\.f64 = (' + F + r')\.f64 >= 0\.0 \? (' + F + r')\.f64 : (' + F + r')\.f64;$')
WRITE = re.compile(r'(?<![\w.])(' + F + r')(?!\d)(?:\.\w+)?\s*(?:[-+*/|&^%]|<<|>>)?=(?!=)')
ADDR = re.compile(r'&(' + F + r')(?!\d)')
CALL = re.compile(r'\(ctx, base[,)]|REX_CALL_INDIRECT_|ppc_trap\(|REX_MEM(?:CPY|SET)_CONST')
STRUCTURED = re.compile(r'^\t+(?:switch|for|while|do|else|case|default|try|catch)\b|[{}]\s*$|^\t*\}|^\t\t')
MACRO = {'+': 'REX_FADDS', '-': 'REX_FSUBS', '*': 'REX_FMULS', '/': 'REX_FDIVS'}
MARKER = '// Prototype 2 PERF pass 13: single-precision arithmetic in float where its inputs are singles (tools/p2_single_floats.py)'
MACROS = MARKER + '''
#ifndef REX_FADDS
#if defined(REX_SINGLE_FLOATS_OFF)
#define REX_FADDS(a, b) double(float((a) + (b)))
#define REX_FSUBS(a, b) double(float((a) - (b)))
#define REX_FMULS(a, b) double(float((a) * (b)))
#define REX_FDIVS(a, b) double(float((a) / (b)))
#define REX_FSQRTS(a) double(float(sqrt(a)))
#define REX_FRSP_SINGLE(a) double(float(a))
#else
#define REX_FADDS(a, b) double(float(a) + float(b))
#define REX_FSUBS(a, b) double(float(a) - float(b))
#define REX_FMULS(a, b) double(float(a) * float(b))
#define REX_FDIVS(a, b) double(float(a) / float(b))
#define REX_FSQRTS(a) double(__builtin_sqrtf(float(a)))
#define REX_FRSP_SINGLE(a) (a)
#endif
#endif
'''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', required=True)
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args()
    stats = dict(binary=0, sqrt=0, frsp=0, seen=0, already=0, files=0)
    for path in sorted(glob.glob(os.path.join(a.gen, '*_recomp.*.cpp'))):
        lines = open(path, encoding='utf-8').read().split('\n')
        single = set()
        dirty = False
        for i, line in enumerate(lines):
            if not line.startswith('\t') or STRUCTURED.search(line):
                single.clear()
                continue
            if line.startswith('\t//'):
                continue
            if NEWFORM.match(line):
                stats['already'] += 1
                single.add(NEWFORM.match(line).group(1))
                continue
            m = BINARY.match(line)
            if m:
                stats['seen'] += 1
                if m.group(2) in single and m.group(4) in single:
                    lines[i] = '\t%s.f64 = %s(%s.f64, %s.f64);' % (m.group(1), MACRO[m.group(3)], m.group(2), m.group(4))
                    stats['binary'] += 1
                    dirty = True
                single.add(m.group(1))
                continue
            m = SQRT.match(line)
            if m:
                stats['seen'] += 1
                if m.group(2) in single:
                    lines[i] = '\t%s.f64 = REX_FSQRTS(%s.f64);' % (m.group(1), m.group(2))
                    stats['sqrt'] += 1
                    dirty = True
                single.add(m.group(1))
                continue
            m = FRSP.match(line)
            if m:
                stats['seen'] += 1
                if m.group(2) in single:
                    lines[i] = '\t%s.f64 = REX_FRSP_SINGLE(%s.f64);' % (m.group(1), m.group(2))
                    stats['frsp'] += 1
                    dirty = True
                single.add(m.group(1))
                continue
            m = LOAD_USE.match(line) or ANY_SINGLE.match(line)
            if m:
                single.add(m.group(1))
                continue
            m = FMR.match(line) or SIGN.match(line)
            if m:
                if m.group(2) in single:
                    single.add(m.group(1))
                else:
                    single.discard(m.group(1))
                continue
            m = FSEL.match(line)
            if m:
                if m.group(3) in single and m.group(4) in single:
                    single.add(m.group(1))
                else:
                    single.discard(m.group(1))
                continue
            if CALL.search(line):
                single.clear()
                continue
            for w in WRITE.finditer(line):
                single.discard(w.group(1))
            for w in ADDR.finditer(line):
                single.discard(w.group(1))
        if dirty:
            stats['files'] += 1
            if not a.check:
                open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(lines))
    pchs = glob.glob(os.path.join(a.gen, '*_pch.h'))
    text = open(pchs[0], encoding='utf-8').read()
    if MARKER not in text and not a.check:
        anchor = '#define REX_MEMORY_SIZE'
        open(pchs[0], 'w', encoding='utf-8', newline='\n').write(text.replace(anchor, MACROS + anchor, 1))
    print('p2_single_floats%s: of %d single-precision statements %d binary, %d sqrt, %d frsp in float (%d files; %d '
          'already)' % (' (check only)' if a.check else '', stats['seen'], stats['binary'], stats['sqrt'],
                        stats['frsp'], stats['files'], stats['already']))


if __name__ == '__main__':
    main()
