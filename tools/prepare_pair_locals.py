"""PERF pass 79: register-local copies of the collision pair subtree under job 829AC940 (Main critical path).

Generalisation of tools/prepare_ribbon_locals.py (same liveness / may-dirty / must-valid analyses, any number of
calls and returns; a call is any line with (ctx, base), P2_DIRECT_CALL(, P2_PROMOTED or a sub_XXXXXXXX name, which
must not mention ctx registers). For each function in FUNCS a copy p2_pair_locals_<name> is added after the
original in its file under P2_PAIR_LOCALS; inside the copies, calls to other FUNCS go to their copies. The entry is
sub_8299F1B0's promoted type call (its strong collision-probe override stays in front of it): under P2_PAIR_LOCALS it
calls p2_pair_dispatch_<root> (project/src/switch_pair_dispatch.cpp), which picks copy or original at run time.
Originals untouched; --revert removes every guarded block. --check analyses without writing. Idempotent.

usage: python tools/prepare_pair_locals.py [--revert] [--check]
"""
from hashlib import sha256
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
GEN = ROOT / 'project' / 'generated' / 'default'
# Leaves 8299E988 / 8299ECA8 already keep their registers in locals (P2_LEAF_LOCALS): not copied.
FUNCS = ['sub_829A09C0', 'sub_8299F360', 'sub_829A0328', 'sub_8299FF70', 'sub_8299FE00']
ROOTS = ['sub_829A09C0', 'sub_8299F360']
ENTRY_FILE = 'prototype2_recomp.101.cpp'
GUARD_BEGIN = '#if defined(P2_PAIR_LOCALS) // pair register locals (tools/prepare_pair_locals.py)'
GUARD_END = '#endif // P2_PAIR_LOCALS'
ENTRY_OLD = '\tP2_PROMOTED2(ctr.u32, 0x829A09C0u, P2_DIRECT_CALL(sub_829A09C0), 0x8299F360u, P2_DIRECT_CALL(sub_8299F360));\n'
ENTRY_NEW = (GUARD_BEGIN + '\n'
             '\tP2_PROMOTED2(ctr.u32, 0x829A09C0u, p2_pair_dispatch_sub_829A09C0, 0x8299F360u, p2_pair_dispatch_sub_8299F360);\n'
             '#else\n' + ENTRY_OLD + GUARD_END + '\n')
CALLNAME = re.compile(r'\bsub_[0-9A-F]{8}\b')
DECL = (GUARD_BEGIN + '\n'
        + ''.join(f'extern "C" void p2_pair_locals_{f}(PPCContext& __restrict ctx, uint8_t* base);\n' for f in FUNCS)
        + ''.join(f'extern "C" void p2_pair_dispatch_{f}(PPCContext& __restrict ctx, uint8_t* base);\n' for f in ROOTS)
        + GUARD_END + '\n')

REG = re.compile(r'\bctx\.([rfv]\d+)\b')
LABEL = re.compile(r'^(loc_[0-9A-F]+):\s*$')
GOTO = re.compile(r'^\s*goto (loc_[0-9A-F]+);\s*$')
CGOTO = re.compile(r'^\s*if \(.*\) goto (loc_[0-9A-F]+);\s*$')
ASSIGN = re.compile(r'^\s*ctx\.([rfv]\d+)\.(\w+)(\[[^\]]*\])?\s*=\s*(.*);\s*$')
STORE = re.compile(r'^\s*simde_mm_store_(ps|si128)\(\s*(?:\(simde__m128i\*\))?\s*ctx\.(v\d+)\.(\w+)\s*,\s*(.*)\);\s*$')


def extract(text):
    i = text.index(HEADER)
    j = text.index('\n}\n', i) + 3
    return i, j, text[i:j]


def analyse(lines):
    """lines: body lines between the header and the closing brace."""
    n = len(lines)
    labels = {}
    for k, l in enumerate(lines):
        m = LABEL.match(l)
        if m:
            labels[m.group(1)] = k
    kind = []
    succ = []
    reads, writes, kills = [], [], []
    for k, l in enumerate(lines):
        code = '' if l.strip().startswith('//') else l
        if '&ctx.' in code or re.search(r'(\+\+|--)\s*ctx\.|ctx\.[rfv]\d+\.\w+\s*(\+\+|--)', code):
            raise SystemExit(f'unsupported register access at body line {k}: {l!r}')
        mentioned = set(REG.findall(code))
        w, kill = set(), set()
        rd = set(mentioned)
        m = ASSIGN.match(code)
        if m:
            reg, field, index, rhs = m.groups()
            w.add(reg)
            if field in ('u64', 's64', 'f64') and not index and reg not in REG.findall(rhs):
                kill.add(reg)
                if code.count('ctx.' + reg) == 1:
                    rd.discard(reg)
        m = STORE.match(code)
        if m:
            _, reg, _, rhs = m.groups()
            w.add(reg)
            if reg not in REG.findall(rhs):
                kill.add(reg)
                rd.discard(reg)
        # Any other mention that might write is rejected: only reads remain outside these two forms.
        other = REG.sub('', code)
        if re.search(r'simde_mm_store\w*\(\s*(?:\(\w+\*\))?\s*$', other):
            raise SystemExit(f'unclassified store at body line {k}: {l!r}')
        if '(ctx, base)' in code or 'P2_PROMOTED' in code or 'P2_DIRECT_CALL(' in code or CALLNAME.search(code):
            if mentioned:
                raise SystemExit(f'call line mentions registers: {l!r}')
            kind.append('call')
        elif re.match(r'^\s*return;\s*$', code):
            kind.append('ret')
        else:
            kind.append('stmt')
        reads.append(rd)
        writes.append(w)
        kills.append(kill)
        s = []
        mg, mc = GOTO.match(code), CGOTO.match(code)
        if mg:
            s = [labels[mg.group(1)]]
        elif mc:
            s = [labels[mc.group(1)]] + ([k + 1] if k + 1 < n else [])
        elif kind[-1] == 'ret':
            s = []
        elif k + 1 < n:
            s = [k + 1]
        succ.append(s)
    if kind.count('ret') < 1:
        raise SystemExit('no return found')
    pred = [[] for _ in range(n)]
    for k, s in enumerate(succ):
        for t in s:
            pred[t].append(k)
    allregs = set().union(*reads, *writes)

    # Liveness (backward). A call reloads what is live after it, so nothing is live into a call.
    live_in = [set() for _ in range(n)]
    live_out = [set() for _ in range(n)]
    changed = True
    while changed:
        changed = False
        for k in range(n - 1, -1, -1):
            out = set().union(*(live_in[t] for t in succ[k])) if succ[k] else set()
            if kind[k] == 'call':
                inn = set()
            elif kind[k] == 'ret':
                inn = set()
            else:
                inn = reads[k] | (out - kills[k])
            if out != live_out[k] or inn != live_in[k]:
                live_out[k], live_in[k] = out, inn
                changed = True

    forced = set()
    while True:
        reload_after = {k: live_out[k] | forced for k in range(n) if kind[k] == 'call'}
        entry_load = live_in[0] | forced
        # may-dirty (forward, union) and must-valid (forward, intersection).
        dirty_in = [set() for _ in range(n)]
        valid_in = [set(allregs) for _ in range(n)]
        valid_in[0] = set(entry_load)
        changed = True
        while changed:
            changed = False
            for k in range(n):
                if k == 0:
                    d_in, v_in = set(), set(entry_load)
                else:
                    ps = pred[k]
                    d_in = set().union(*(dirty_out(j, dirty_in, writes, kind) for j in ps)) if ps else set()
                    v_in = set(allregs)
                    for j in ps:
                        v_in &= valid_out(j, valid_in, writes, kills, kind, reload_after)
                    if not ps:
                        v_in = set()
                if d_in != dirty_in[k] or v_in != valid_in[k]:
                    dirty_in[k], valid_in[k] = d_in, v_in
                    changed = True
        conflict = set()
        for k in range(n):
            if kind[k] in ('call', 'ret'):
                conflict |= dirty_in[k] - valid_in[k]
        if not conflict - forced:
            break
        forced |= conflict
    reach, todo = {0}, [0]
    while todo:
        for t in succ[todo.pop()]:
            if t not in reach:
                reach.add(t)
                todo.append(t)
    for k in range(n):
        if k not in reach:
            continue
        bad = reads[k] - valid_in[k]
        if bad:
            raise SystemExit(f'read of a stale local at body line {k}: {sorted(bad)} {lines[k]!r}')
        if kind[k] in ('call', 'ret') and dirty_in[k] - valid_in[k]:
            raise SystemExit(f'store of a stale local at body line {k}')
    stores = {k: dirty_in[k] for k in range(n) if kind[k] in ('call', 'ret')}
    return allregs, entry_load, stores, reload_after, forced


def dirty_out(j, dirty_in, writes, kind):
    if kind[j] == 'call':
        return set()
    return dirty_in[j] | writes[j]


def valid_out(j, valid_in, writes, kills, kind, reload_after):
    if kind[j] == 'call':
        return set(reload_after[j])
    # A partial write reads its register (live, hence valid); a full write makes it valid.
    return valid_in[j] | kills[j] | (writes[j] & valid_in[j])


def regkey(r):
    return ('rfv'.index(r[0]), int(r[1:]))


def transform(func, name):
    head, rest = func.split('\n', 1)
    assert head == f'DEFINE_REX_FUNC({name}) {{'
    body = rest[:-3].split('\n') if rest.endswith('\n}\n') else None
    if body is None:
        raise SystemExit('unexpected function end')
    allregs, entry_load, stores, reload_after, forced = analyse(body)
    regs = sorted(allregs, key=regkey)
    out = [f'extern "C" void p2_pair_locals_{name}(PPCContext& __restrict ctx, uint8_t* base) {{']
    decl = []
    for r in regs:
        t = 'PPCVRegister' if r[0] == 'v' else 'PPCRegister'
        decl.append(f'\t{t} p2l_{r};')
    loads = [f'\tp2l_{r} = ctx.{r};' for r in sorted(entry_load, key=regkey)]
    inserted = False
    for k, l in enumerate(body):
        if not inserted and 'REX_FUNC_PROLOGUE' in l:
            out.append(l)
            out += decl + loads
            inserted = True
            continue
        code = l if l.strip().startswith('//') else REG.sub(lambda m: 'p2l_' + m.group(1), l)
        if k in stores:
            out += [f'\tctx.{r} = p2l_{r};' for r in sorted(stores[k], key=regkey)]
        out.append(code)
        if k in reload_after:
            out += [f'\tp2l_{r} = ctx.{r};' for r in sorted(reload_after[k], key=regkey)]
    if not inserted:
        raise SystemExit('REX_FUNC_PROLOGUE not found')
    out.append('}')
    stats = {
        'registers': len(regs), 'entry_loads': len(entry_load), 'forced': sorted(forced, key=regkey),
        'stores': {k: len(v) for k, v in stores.items()}, 'reloads': {k: len(v) for k, v in reload_after.items()},
    }
    return '\n'.join(out) + '\n', stats




def extract(text, name):
    h = f'DEFINE_REX_FUNC({name}) {{'
    i = text.index(h)
    j = text.index('\n}\n', i) + 3
    return i, j, text[i:j]


def redirect(copy):
    """Calls between members of the subtree go to the copies (comment lines keep the original text)."""
    out = []
    for l in copy.split('\n'):
        if not l.strip().startswith('//'):
            for f in FUNCS:
                l = l.replace(f'P2_DIRECT_CALL({f})(ctx, base)', f'p2_pair_locals_{f}(ctx, base)')
                l = re.sub(rf'^(\s*){f}\(ctx, base\);', rf'\1p2_pair_locals_{f}(ctx, base);', l)
        out.append(l)
    return '\n'.join(out)


def files_of():
    where = {}
    for p in sorted(GEN.glob('prototype2_recomp.*.cpp')):
        t = p.read_text(encoding='utf-8')
        for f in FUNCS:
            if f'DEFINE_REX_FUNC({f}) {{' in t:
                where.setdefault(p, []).append(f)
    found = sorted(sum(where.values(), []))
    if found != sorted(FUNCS):
        raise SystemExit(f'functions not found exactly: {found}')
    return where


def strip_blocks(t):
    t = t.replace(ENTRY_NEW, ENTRY_OLD)
    if t.startswith(DECL):
        t = t[len(DECL):]
    marker = '\n' + GUARD_BEGIN + '\n'
    while marker in t:
        a = t.index(marker)
        b = t.index('\n' + GUARD_END + '\n', a) + len(GUARD_END) + 2
        t = t[:a] + t[b:]
    return t


def main():
    revert = '--revert' in sys.argv
    check = '--check' in sys.argv
    where = files_of()
    entry = GEN / ENTRY_FILE
    for p in sorted(set(where) | {entry}):
        t = p.read_text(encoding='utf-8')
        applied = GUARD_BEGIN in t
        if revert:
            if applied:
                t = strip_blocks(t)
                if GUARD_BEGIN in t:
                    raise SystemExit(f'revert incomplete in {p.name}')
                p.write_text(t, encoding='utf-8', newline='\n')
            print('reverted' if applied else 'not applied', p.name, sha256(t.encode()).hexdigest()[:16])
            continue
        if applied:
            print('already applied', p.name)
            continue
        original = t
        for f in where.get(p, []):
            _, j, func = extract(t, f)
            if 'P2 leaf declarations begin' in func:
                raise SystemExit(f'{f} already has leaf locals; do not copy it')
            copy, stats = transform(func, f)
            copy = redirect(copy)
            print(f, 'regs', stats['registers'], 'entry_loads', stats['entry_loads'], 'forced', len(stats['forced']),
                  'stores', sum(stats['stores'].values()), 'reloads', sum(stats['reloads'].values()),
                  'calls', len(stats['reloads']), 'returns', len(stats['stores']) - len(stats['reloads']))
            t = t[:j] + '\n' + GUARD_BEGIN + '\n' + copy + GUARD_END + '\n' + t[j:]
        if p == entry:
            if t.count(ENTRY_OLD) != 1:
                raise SystemExit('entry call not found exactly once')
            t = t.replace(ENTRY_OLD, ENTRY_NEW)
        t = DECL + t
        if strip_blocks(t) != original:
            raise SystemExit(f'revert would not restore {p.name}')
        if not check:
            p.write_text(t, encoding='utf-8', newline='\n')
            print('applied', p.name)


if __name__ == '__main__':
    main()
