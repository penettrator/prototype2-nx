"""PERF pass 74: opt-in register-local copy of sub_8275E060 (ribbon/tube vertex generation, job 8275F318).

The recompiled function keeps volatile guest registers (r0-r12 and r1, f0-f13, v0-v13, v32-v63) in PPCContext, so
every VMX/FP operation goes through memory that may alias guest stores. The copy keeps every ctx register it uses
in a C++ local and synchronises ctx only where code outside the function can observe it: before/after each of the
four calls and at the return. Which registers are synchronised follows from three data-flow analyses over the
function's goto graph (no ABI assumption):
  * liveness: after a call reload every local that may be read before it is fully overwritten;
  * may-dirty: before a call / return store every local that may have been written since the last sync;
  * must-valid: a stored local must equal the architectural value on every path; registers dirty on one path and
    stale on another are forced into every reload set until no conflict remains.
The tool asserts that every read and every store uses a valid local. The original function is untouched; the copy
(p2_ribbon_locals_8275E060) and the call-site dispatch exist only under P2_RIBBON_LOCALS. --revert removes both
and restores the original bytes. Idempotent.

usage: python tools/prepare_ribbon_locals.py [--revert] [--emit-function OUT]
"""
from hashlib import sha256
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
GEN = ROOT / 'project' / 'generated' / 'default'
FUNC_FILE = GEN / 'prototype2_recomp.26.cpp'
CALL_FILE = GEN / 'prototype2_recomp.181.cpp'
HEADER = 'DEFINE_REX_FUNC(sub_8275E060) {'
ORIGINAL_HASH = None  # filled below after the first check; pinned constant
PINNED_HASH = '__PIN__'
GUARD_BEGIN = '#if defined(P2_RIBBON_LOCALS) // ribbon register locals (tools/prepare_ribbon_locals.py)'
GUARD_END = '#endif // P2_RIBBON_LOCALS'
CALL_OLD = '\tP2_DIRECT_CALL(sub_8275E060)(ctx, base);\n'
CALL_NEW = ('#if defined(P2_RIBBON_LOCALS) // ribbon register locals (tools/prepare_ribbon_locals.py)\n'
            '\tp2_ribbon_dispatch_8275E060(ctx, base);\n'
            '#else\n'
            '\tP2_DIRECT_CALL(sub_8275E060)(ctx, base);\n'
            '#endif // P2_RIBBON_LOCALS\n')
DECL = ('#if defined(P2_RIBBON_LOCALS) // ribbon register locals (tools/prepare_ribbon_locals.py)\n'
        'extern "C" void p2_ribbon_dispatch_8275E060(PPCContext& __restrict ctx, uint8_t* base);\n'
        '#endif // P2_RIBBON_LOCALS\n')

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
        if '(ctx, base);' in code:
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
    if kind.count('call') != 4 or kind.count('ret') != 1:
        raise SystemExit(f'expected 4 calls and 1 return, got {kind.count("call")} / {kind.count("ret")}')
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


def transform(func):
    head, rest = func.split('\n', 1)
    assert head == HEADER
    body = rest[:-3].split('\n') if rest.endswith('\n}\n') else None
    if body is None:
        raise SystemExit('unexpected function end')
    allregs, entry_load, stores, reload_after, forced = analyse(body)
    regs = sorted(allregs, key=regkey)
    out = ['extern "C" void p2_ribbon_locals_8275E060(PPCContext& __restrict ctx, uint8_t* base) {']
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


def main():
    revert = '--revert' in sys.argv
    emit = sys.argv[sys.argv.index('--emit-function') + 1] if '--emit-function' in sys.argv else None
    ftext = FUNC_FILE.read_text(encoding='utf-8')
    ctext = CALL_FILE.read_text(encoding='utf-8')
    applied = GUARD_BEGIN in ftext
    if revert:
        if applied:
            a = ftext.index('\n' + GUARD_BEGIN + '\n')
            b = ftext.index('\n' + GUARD_END + '\n', a) + len(GUARD_END) + 2
            ftext = ftext[:a] + ftext[b:]
            ctext = ctext.replace(CALL_NEW, CALL_OLD).replace(DECL, '')
            FUNC_FILE.write_text(ftext, encoding='utf-8', newline='\n')
            CALL_FILE.write_text(ctext, encoding='utf-8', newline='\n')
        _, _, func = extract(ftext)
        print('reverted' if applied else 'not applied', sha256(func.encode()).hexdigest())
        return
    _, j, func = extract(ftext)
    h = sha256(func.encode()).hexdigest()
    if PINNED_HASH != '__PIN__' and h != PINNED_HASH:
        raise SystemExit(f'sub_8275E060 changed since the equivalence tests: {h}')
    copy, stats = transform(func)
    if emit:
        Path(emit).write_text(func + '\n' + copy, encoding='utf-8', newline='\n')
    print('original', h)
    print('stats', stats)
    if applied:
        print('already applied')
        return
    if ctext.count(CALL_OLD) != 1:
        raise SystemExit('call site of sub_8275E060 not found exactly once')
    ftext = ftext[:j] + '\n' + GUARD_BEGIN + '\n' + copy + GUARD_END + '\n' + ftext[j:]
    first_def = ctext.index('DEFINE_REX_FUNC(')
    ctext = ctext[:first_def] + DECL + ctext[first_def:]
    ctext = ctext.replace(CALL_OLD, CALL_NEW)
    FUNC_FILE.write_text(ftext, encoding='utf-8', newline='\n')
    CALL_FILE.write_text(ctext, encoding='utf-8', newline='\n')
    print('applied')


if __name__ == '__main__':
    main()
