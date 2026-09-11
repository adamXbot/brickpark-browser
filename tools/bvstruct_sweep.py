#!/usr/bin/env python3
"""The by-value-struct ABI class: the one wasm-ld, linkreport and the callback
type check are all blind to.

A parameter one translation unit spells as a BY-VALUE STRUCT of 4 bytes or fewer
with more than one member, and another spells as a SCALAR, is the same dword on
x86 cdecl and is NOT the same on wasm32: clang passes a by-value aggregate
directly only when it is single-element, so a two-member 2-byte struct goes
INDIRECTLY -- a pointer to a byval temp on the shadow stack. Both spellings
still lower to exactly one `i32` parameter, so

* `wasm-ld` sees the same signature on both sides and warns about nothing;
* `linkreport.py`'s conflict vote sees no conflict;
* `portable/tests/test_callback_types.c` type-checks slots, not parameters;
* nothing traps -- the callee just reads a stack ADDRESS as its argument.

PORT-M10 found it by hand after PORT-B10 reported that LINK "SPACE TOWER RIDE"
never completed however much path was laid: `AddObjectToBuildList(def, bp)`
(popup.c) against `AddObjectToBuildList(int obj, short type)` (sweep1.c) filed
the ride's map tile as (114, 10), so `EventTick_Link` tested a square off the
84x84 map for ever and the ride's `TowerRec` was filed under the wrong cell
(PARK-1 and PARK-3 in one defect). Three instances were live; two more remain
open in the more dangerous direction, where the DEFINITION takes the struct and
the majority of callers hand it an integer (docs/lanes/scope-port-m10.md §1f).

The size window is exactly "multi-member aggregate of 4 bytes or fewer", because
that is the only size that occupies one dword on x86 AND one `i32` on wasm32.
Anything larger changes the wasm arity and wasm-ld already shouts; anything
single-member is passed directly and is safe. `--selftest` proves both halves of
that claim against the real compilers when emcc and node are on PATH.

Two things separate this from PORT-M10's scratch version (`tools/
port_m10_bvstruct_sweep.py`, which this replaces):

* it reads the sources the way the PORTABLE BUILD does, skipping the arms of
  `#ifndef LEGOLAND_PORTABLE` that only VC6 compiles. Without that, M10's three
  FIXES all look like fresh hits -- the fix is a portable arm whose declaration
  sits next to the VC6 one -- and a gate that reports its own repairs is a gate
  nobody keeps;
* it is a GATE: exit 1 on any silent hit that is not in an accepted list
  (`--baseline`, default `portable/tests/bvstruct_accepted.txt`), with the
  reason each accepted row is there.

usage:
  bvstruct_sweep.py [LEGOLAND_DIR] [--baseline FILE] [--all]
  bvstruct_sweep.py --selftest
"""
import os, re, sys, shutil, subprocess, tempfile, collections

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
DEFAULT_SRC = os.path.join(ROOT, 'LEGOLAND')
DEFAULT_BASELINE = os.path.join(HERE, '..', 'tests', 'bvstruct_accepted.txt')

SCALAR_WORDS = {
    'void', 'char', 'short', 'int', 'long', 'float', 'double',
    'signed', 'unsigned', '_Bool', 'const', 'volatile', 'struct', 'union',
    'enum', '__stdcall', '__cdecl', '__declspec', 'dllimport', 'size_t',
    'unsigned int', 'unsigned char', 'unsigned short', 'unsigned long',
}

# an extern function declaration on one line, with a trailing address comment
DECL = re.compile(
    r'^\s*extern\s+(?:__declspec\(dllimport\)\s+)?(?:__stdcall\s+)?'
    r'(?P<ret>[A-Za-z_][A-Za-z0-9_ ]*?[\s*]+)'
    r'(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*\((?P<args>[^;]*)\)\s*;'
    r'.*?/\*\s*(?P<addr>0x[0-9a-fA-F]{6,8})')
MARKER = re.compile(r'^//\s*(?:WIP-)?FUNCTION:\s*LEGOLAND\s+(0x[0-9a-fA-F]{8})')
PORTABLE = 'LEGOLAND_PORTABLE'


def portable_lines(text):
    """The line numbers the PORTABLE build actually compiles.

    A one-symbol preprocessor: `#ifdef LEGOLAND_PORTABLE` / `#ifndef` /
    `#if defined(...)` / `#else` / `#endif`, nested, and every other
    conditional left alone (both arms kept, which is what the old sweep did
    everywhere and is the safe direction for a reporter).

    This is the difference between a gate and a nuisance: every fix of this
    class is `#ifndef LEGOLAND_PORTABLE` <the VC6 spelling> `#else` <the
    spelling the definition reads> `#endif`, so a sweep that reads both arms
    reports all of its own repairs as hits.
    """
    keep, stack = set(), []          # stack of (is_ours, live_now)
    for i, ln in enumerate(text.split('\n'), 1):
        s = ln.strip()
        if s.startswith('#if'):
            m = re.match(r'#\s*if(n?)def\s+(\w+)', s) or \
                re.match(r'#\s*if\s+(?:(!)\s*)?defined\s*\(\s*(\w+)\s*\)', s)
            if m and m.group(2) == PORTABLE:
                stack.append((True, not m.group(1)))   # `n` of ifndef, or `!`
            else:
                stack.append((False, True))
            continue
        if s.startswith('#el') and stack:      # #else / #elif
            ours, live = stack[-1]
            stack[-1] = (ours, not live if ours else True)
            continue
        if s.startswith('#endif'):
            if stack:
                stack.pop()
            continue
        if all(live for _ours, live in stack):
            keep.add(i)
    return keep


def split_args(s):
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def classify(arg):
    """'ptr', 'scalar', 'aggregate:<Type>', 'varargs', 'void', 'unknown'."""
    a = arg.strip()
    if a in ('', 'void'):
        return 'void'
    if a == '...':
        return 'varargs'
    if '*' in a or '[' in a:
        return 'ptr'
    toks = re.findall(r'[A-Za-z_][A-Za-z0-9_]*', a)
    if not toks:
        return 'unknown'
    words = [t for t in toks
             if t not in ('const', 'volatile', 'struct', 'union', 'enum')]
    # drop the parameter NAME (the last identifier) when the type has >= 2 words;
    # a declaration may omit it entirely (`extern void f(BPos);`)
    base = words if len(words) == 1 else words[:-1]
    joined = ' '.join(base)
    if all(w in SCALAR_WORDS for w in base) or joined in SCALAR_WORDS:
        return 'scalar'
    return 'aggregate:' + base[-1]


AGG_DEF = re.compile(r'typedef\s+(struct|union)\s+(?:\w+\s*)?\{(.*?)\}\s*'
                     r'(\w+)\s*;', re.S)


def collect_aggregates(texts):
    """{typedef name -> [(kind, body, file), ...]} over the whole tree.

    Tree-wide on purpose. PORT-M10's sweep looked the type up only in the file
    that mentioned it, so `union BPosW { unsigned short w; BPos b; }` -- a
    2-byte, 2-member union, squarely inside the silent window and the very type
    M10's §1b table measured as INDIRECT -- came out as "size unknown" and was
    filed under the heading that says wasm-ld already warns. It does not, and
    eleven more live instances were hiding under it.

    Every variant is kept, because the recovered sources give each translation
    unit its own view of a record and 261 of the tree's 731 aggregate typedefs
    have more than one body: a file that needs one field declares one field.
    `agg_info` then takes the LARGEST view, so a partial one can never shrink a
    big record into the silent window. Measured: the types that are actually
    passed by value here -- BPos, BPosW, RideTile, ShopTile, CellPos, MapPos
    (2 bytes) and Pos (8) -- agree in every file that spells them out.
    """
    out = collections.defaultdict(list)
    for f in sorted(texts):
        for m in AGG_DEF.finditer(texts[f]):
            out[m.group(3)].append((m.group(1), m.group(2), f))
    return out


def agg_info(name, aggs, _depth=0):
    """The widest view of `name` the tree offers -- see `collect_aggregates`."""
    best = None
    for variant in aggs.get(name, ()):
        got = _agg_info_one(variant, aggs, _depth)
        if got and (best is None or got[0] > best[0]):
            best = got
    return best


def _agg_info_one(variant, aggs, _depth=0):
    """(size, top-level members, is-single-element) for an aggregate, or None.

    `is-single-element` is clang's own rule, recursively: a struct with exactly
    one member which is itself a scalar or a single-element aggregate is passed
    DIRECTLY on wasm32 and is therefore safe. Anything else of 4 bytes or fewer
    is passed indirectly while a scalar of the same width is passed directly,
    and that is the silent window.
    """
    if _depth > 8:
        return None
    kind, body, _where = variant
    body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
    body = re.sub(r'//[^\n]*', '', body)
    size = members = 0
    single = None
    for decl in body.split(';'):
        decl = decl.strip()
        if not decl:
            continue
        inner = None
        if '*' in decl or '(' in decl:
            w = 4                                   # pointer, function pointer
        elif re.search(r'\b(char|_Bool|BYTE)\b', decl):
            w = 1
        elif re.search(r'\b(short|WORD)\b', decl):
            w = 2
        elif re.search(r'\bdouble\b', decl):
            w = 8
        elif re.search(r'\b(int|long|float|unsigned|signed|DWORD|BOOL)\b', decl):
            w = 4
        else:
            tok = re.findall(r'[A-Za-z_]\w*', decl)
            inner = agg_info(tok[0], aggs, _depth + 1) if tok else None
            if inner is None:
                return None              # a type nothing in the tree defines
            w = inner[0]
        for nm in decl.split(','):
            cnt = 1
            arr = re.search(r'\[\s*(0x[0-9a-fA-F]+|\d+)\s*\]', nm)
            if arr:
                cnt = int(arr.group(1), 0)
            members += 1
            single = (cnt == 1 and (inner is None or inner[2])) \
                if members == 1 else False
            if kind == 'struct':
                size += w * cnt
            else:
                size = max(size, w * cnt)
    return (size, members, bool(single)) if members else None


def sweep(root, portable_only=True):
    """(silent, noisy) rows. `silent` is the class no other gate can see."""
    files = sorted(f for f in os.listdir(root) if f.endswith('.c'))
    text = {f: open(os.path.join(root, f), errors='replace').read()
            for f in files}
    sites = collections.defaultdict(list)
    defs = {}
    for f in files:
        lines = text[f].split('\n')
        live = portable_lines(text[f]) if portable_only else None
        for i, ln in enumerate(lines, 1):
            if live is not None and i not in live:
                continue
            m = DECL.match(ln)
            if m:
                args = [classify(a) for a in split_args(m.group('args'))]
                key = m.group('addr').lower().replace('0x00', '0x')
                sites[key].append((f, i, m.group('name'), args, ln.strip(),
                                   'decl'))
                continue
            mk = MARKER.match(ln)
            if mk:
                j, sig = i, ''
                while j < len(lines) and len(sig) < 400:
                    sig += ' ' + lines[j].strip()
                    if '(' in sig and sig.count('(') <= sig.count(')'):
                        break
                    j += 1
                m2 = re.search(r'(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*'
                               r'\((?P<args>[^)]*)\)', sig)
                if m2:
                    args = [classify(a) for a in split_args(m2.group('args'))]
                    key = mk.group(1).lower().replace('0x00', '0x')
                    defs[key] = (f, i + 1, m2.group('name'), args, sig.strip())
    aggs = collect_aggregates(text)
    silent, noisy = [], []
    for addr, rows in sites.items():
        allrows = list(rows)
        if addr in defs:
            f, i, n, a, raw = defs[addr]
            allrows.append((f, i, n, a, raw, 'def'))
        for pos in range(max(len(r[3]) for r in allrows)):
            kinds = {}
            for r in allrows:
                if pos < len(r[3]):
                    kinds.setdefault(r[3][pos], []).append(r)
            aggkinds = [k for k in kinds if k.startswith('aggregate:')]
            scal = [k for k in kinds if k in ('scalar', 'ptr')]
            if not (aggkinds and scal):
                continue
            for k in aggkinds:
                tname = k.split(':', 1)[1]
                info = agg_info(tname, aggs)
                row = (addr, pos, tname, info, kinds[k],
                       [x for s in scal for x in kinds[s]])
                if info and info[2]:
                    pass              # single-element: passed direct, safe
                elif info and info[0] <= 4:
                    silent.append(row)
                else:
                    noisy.append(row)
    return silent, noisy


def read_baseline(path):
    """{addr -> reason} for the hits a lane has ruled on."""
    out = {}
    if not os.path.exists(path):
        return out
    for line in open(path, encoding='utf-8', errors='replace'):
        line, _, note = line.partition('#')
        if line.strip():
            out[line.split()[0].lower().replace('0x00', '0x')] = note.strip()
    return out


def show(title, rows):
    print('=' * 78)
    print(title, f'-- {len(rows)} site(s)')
    print('=' * 78)
    for addr, pos, tname, info, aggs, scals in sorted(rows):
        sz = f'{info[0]}B/{info[1]} members' if info else 'size unknown'
        print(f'\n{addr}  arg {pos}  aggregate `{tname}` ({sz})')
        for f, i, _n, _a, raw, kind in aggs:
            print(f'    AGG  {kind:4s} {f}:{i}  {raw[:110]}')
        for f, i, _n, _a, raw, kind in scals:
            print(f'    SCA  {kind:4s} {f}:{i}  {raw[:110]}')


# ---- the selftest ----------------------------------------------------------
#
# Two halves. The first is the classifier's own shapes, positive and negative,
# so a refactor cannot quietly turn the gate into a test that always passes --
# including the portable-arm reader, which is the whole reason a gate is
# possible at all. The second is the GROUND TRUTH: the three-file repro from
# PORT-M10 §1b, compiled by the real emcc and the real clang, which is what
# says the class exists and where its size boundary is. It needs emcc and node,
# and says so when they are missing rather than passing quietly.

CLS_CASES = [
    # (argument text, expected classification)
    ('BPos bp', 'aggregate:BPos'),
    ('BPos', 'aggregate:BPos'),
    ('unsigned short bp', 'scalar'),
    ('short type', 'scalar'),
    ('const char* s', 'ptr'),
    ('int a[4]', 'ptr'),
    ('void', 'void'),
    ('...', 'varargs'),
    ('struct JcRoutePos here', 'aggregate:JcRoutePos'),
    ('unsigned int tile', 'scalar'),
]

ARM_SRC = '''\
extern int Before(int a);                                  /* 0x00400010 */
#ifndef LEGOLAND_PORTABLE
extern int Vc6Only(Tile t);                                /* 0x00400020 */
#else
extern int Vc6Only(unsigned short t);                      /* 0x00400020 */
#endif
#ifdef LEGOLAND_PORTABLE
extern int PortOnly(int x);                                /* 0x00400030 */
#endif
#if defined(SOMETHING_ELSE)
extern int OtherArm(int x);                                /* 0x00400040 */
#else
extern int OtherElse(int x);                               /* 0x00400050 */
#endif
extern int After(int a);                                   /* 0x00400060 */
'''

HIT_SRC = {
    'hit_call.c': '''\
typedef struct { unsigned char x, y; } BPos;
extern int Target(void* d, BPos bp);                       /* 0x00450b90 */
''',
    'hit_def.c': '''\
// FUNCTION: LEGOLAND 0x00450b90
int Target(int obj, short type) { return obj + type; }
''',
    'safe_one.c': '''\
typedef struct { int v; } OneElem;
extern int Single(OneElem o);                              /* 0x00450c10 */
''',
    'safe_one_def.c': '''\
// FUNCTION: LEGOLAND 0x00450c10
int Single(int v) { return v; }
''',
    'noisy_big.c': '''\
typedef struct { int x, y; } Pos;
extern void Wide(void* q, Pos t);                          /* 0x004120a0 */
''',
    'noisy_big_def.c': '''\
// FUNCTION: LEGOLAND 0x004120a0
void Wide(void* q, int tx, int ty) { (void)q; (void)tx; (void)ty; }
''',
    'fixed_arm.c': '''\
typedef struct { unsigned char x, y; } BPos;
#ifndef LEGOLAND_PORTABLE
extern void Fixed(void* o, BPos sq);                       /* 0x00450c00 */
#else
extern void Fixed(void* o, unsigned short sq);             /* 0x00450c00 */
#endif
''',
    'fixed_arm_def.c': '''\
// FUNCTION: LEGOLAND 0x00450c00
void Fixed(void* o, unsigned short sq) { (void)o; (void)sq; }
''',
}

# PORT-M10 §1b's reproduction, as the selftest's positive control: the real
# popup.c declaration against the real sweep1.c body. Native clang sees the
# tile; emcc sees a shadow-stack pointer.
ABI_CALL = '''\
typedef struct { unsigned char x, y; } BPos;
extern int Record(void* d, BPos bp);
int Caller(void) { BPos bp; bp.x = 63; bp.y = 35; return Record((void*)0, bp); }
'''
ABI_DEF = '''\
extern unsigned short g_slot;
int Record(int obj, short type) { (void)obj; g_slot = (unsigned short)type;
                                 return 0; }
'''
ABI_MAIN = '''\
#include <stdio.h>
extern int Caller(void);
unsigned short g_slot;
int main(void) { Caller(); printf("slot=0x%04x\\n", g_slot); return 0; }
'''
# the size boundary: one member is direct (safe), two members are indirect
ABI_SIZES = '''\
#include <stdio.h>
typedef struct { unsigned char a; } One;
typedef struct { unsigned char a, b; } Two;
extern int TakeOne(One v);
extern int TakeTwo(Two v);
int main(void) { One o; Two t; o.a = 0x41; t.a = 0x41; t.b = 0x42;
                 printf("one=0x%02x two=0x%04x\\n", TakeOne(o), TakeTwo(t));
                 return 0; }
'''
ABI_SIZES_DEF = '''\
int TakeOne(unsigned char v) { return v; }
int TakeTwo(unsigned short v) { return v; }
'''
# And the UNION, which is most of this tree's instances: `BPosW`, `RideTile`,
# `ShopTile` are all `union { unsigned short w; BPos b; }`. PORT-M10's sweep
# could not size them (a nested aggregate) and filed them as "wasm-ld already
# warns"; it does not, and this control is what says so.
ABI_UNION_CALL = '''\
typedef struct BPos { unsigned char x, y; } BPos;
typedef union BPosW { unsigned short w; BPos b; } BPosW;
extern int Record(BPosW k);
int Caller(void) { BPosW k; k.b.x = 63; k.b.y = 35; return Record(k); }
'''
ABI_UNION_DEF = '''\
extern unsigned short g_slot;
int Record(unsigned short k) { g_slot = k; return 0; }
'''


def run_abi_repro(fails, log):
    """The ground truth, when emcc and node are on PATH: the same two C files
    compiled by clang and by emcc, and the values they produce."""
    emcc, node = shutil.which('emcc'), shutil.which('node')
    cc = shutil.which('clang') or shutil.which('cc')
    if not (emcc and node and cc):
        missing = [n for n, p in (('emcc', emcc), ('node', node), ('clang', cc))
                   if not p]
        log.append(f'SKIP abi repro: {", ".join(missing)} not on PATH '
                   f'(the classifier half of the selftest still ran)')
        return
    with tempfile.TemporaryDirectory(prefix='bvstruct-') as d:
        w = lambda n, t: (open(os.path.join(d, n), 'w').write(t),
                          os.path.join(d, n))[1]
        call, defn, main = (w('call.c', ABI_CALL), w('def.c', ABI_DEF),
                            w('main.c', ABI_MAIN))
        szmain, szdef = w('sz.c', ABI_SIZES), w('szdef.c', ABI_SIZES_DEF)

        def out_of(cmd, runner=None):
            r = subprocess.run(cmd, cwd=d, capture_output=True, text=True)
            if r.returncode:
                return f'BUILD FAILED: {r.stderr.strip()[:200]}'
            r = subprocess.run(runner, cwd=d, capture_output=True, text=True)
            return (r.stdout + r.stderr).strip()

        nat = out_of([cc, '-O2', '-o', 'nat', call, defn, main],
                     [os.path.join(d, 'nat')])
        wasm = out_of([emcc, '-O2', '-o', 'w.js', call, defn, main],
                      [node, os.path.join(d, 'w.js')])
        log.append(f'  abi repro, the M10 defect: native {nat!r} vs '
                   f'wasm32 {wasm!r}')
        if nat != 'slot=0x233f':
            fails.append(f'abi repro: native clang should pack (63,35) as '
                         f'0x233f, got {nat!r}')
        if wasm == 'slot=0x233f':
            fails.append('abi repro: emcc passed a two-member by-value struct '
                         'DIRECTLY -- the class this sweep exists for may be '
                         'gone in this toolchain; re-read the sweep before '
                         'trusting it')
        elif 'FAILED' in wasm:
            fails.append(f'abi repro: {wasm}')
        ucall, udef = w('ucall.c', ABI_UNION_CALL), w('udef.c', ABI_UNION_DEF)
        unat = out_of([cc, '-O2', '-o', 'unat', ucall, udef, main],
                      [os.path.join(d, 'unat')])
        uwasm = out_of([emcc, '-O2', '-o', 'uw.js', ucall, udef, main],
                       [node, os.path.join(d, 'uw.js')])
        log.append(f'  abi repro, the 2-byte UNION (BPosW): native {unat!r} vs '
                   f'wasm32 {uwasm!r}')
        if unat != 'slot=0x233f':
            fails.append(f'abi repro: native clang should pass a 2-byte union '
                         f'as one dword, got {unat!r}')
        if uwasm == 'slot=0x233f':
            fails.append('abi repro: emcc passed a 2-byte UNION directly -- the '
                         '13 BPosW/RideTile/ShopTile rows this sweep reports '
                         'would then be false positives; re-measure before '
                         'trusting them')
        natsz = out_of([cc, '-O2', '-o', 'natsz', szmain, szdef],
                       [os.path.join(d, 'natsz')])
        wasmsz = out_of([emcc, '-O2', '-o', 'wsz.js', szmain, szdef],
                        [node, os.path.join(d, 'wsz.js')])
        log.append(f'  abi repro, the size boundary: native {natsz!r} vs '
                   f'wasm32 {wasmsz!r}')
        if natsz != 'one=0x41 two=0x4241':
            fails.append(f'abi repro: native size boundary {natsz!r}')
        if wasmsz == 'one=0x41 two=0x4241':
            fails.append('abi repro: emcc agreed with x86 on BOTH sizes -- the '
                         'silent window has moved')
        elif 'one=0x41' not in wasmsz:
            fails.append(f'abi repro: a SINGLE-member struct did not arrive by '
                         f'value on wasm32 either ({wasmsz!r}): the window is '
                         f'wider than this sweep reports')


def selftest():
    fails, log = [], []
    for arg, want in CLS_CASES:
        got = classify(arg)
        if got != want:
            fails.append(f'classify({arg!r}) = {got!r}, want {want!r}')
    live = portable_lines(ARM_SRC)
    # 3 is the VC6 arm of a fixed site (skipped), 5 its portable arm, 8 a
    # portable-only declaration, and 11/13 the two arms of a conditional this
    # reader does not know -- both kept, which is the safe direction for a
    # reporter and the behaviour the old sweep had everywhere.
    arm_want = {1: True, 3: False, 5: True, 8: True, 11: True, 13: True,
                15: True}
    for ln, want in arm_want.items():
        if (ln in live) != want:
            fails.append(f'portable_lines: line {ln} '
                         f'{"missing" if want else "present"} -- '
                         f'{ARM_SRC.split(chr(10))[ln - 1].strip()[:60]!r}')
    with tempfile.TemporaryDirectory(prefix='bvstruct-src-') as d:
        for n, t in HIT_SRC.items():
            open(os.path.join(d, n), 'w').write(t)
        silent, noisy = sweep(d)
        got = {r[0] for r in silent}
        if '0x450b90' not in got:
            fails.append('the positive control (a 2-byte BPos against a '
                         '`short` definition) was NOT reported')
        if '0x450c10' in got:
            fails.append('a SINGLE-element struct was reported: it is passed '
                         'directly on wasm32 and is safe')
        if '0x4120a0' in got:
            fails.append('an 8-byte Pos was reported as silent: wasm-ld '
                         'already warns about those (the arity differs)')
        if '0x4120a0' not in {r[0] for r in noisy}:
            fails.append('an 8-byte Pos was not reported as noisy either')
        if '0x450c00' in got:
            fails.append('a FIXED site (the scalar in the LEGOLAND_PORTABLE '
                         'arm) was reported: portable_lines is not being '
                         'honoured, and every repair of this class would read '
                         'as a fresh hit')
        silent_all, _ = sweep(d, portable_only=False)
        if '0x450c00' not in {r[0] for r in silent_all}:
            fails.append('--all should still see the VC6 arm of a fixed site')
    run_abi_repro(fails, log)
    for line in log:
        print(line)
    for f in fails:
        print('FAIL ' + f)
    print(f'bvstruct_sweep selftest: {len(fails)} failure(s), '
          f'{len(CLS_CASES) + len(arm_want) + 6} source check(s) and the '
          f'{"SKIPPED " if any(l.startswith("SKIP") for l in log) else ""}'
          f'emcc-vs-clang controls')
    return 1 if fails else 0


def main():
    args = sys.argv[1:]
    if '--selftest' in args:
        sys.exit(selftest())
    baseline = DEFAULT_BASELINE
    if '--baseline' in args:
        i = args.index('--baseline')
        baseline = args[i + 1]
        del args[i:i + 2]
    portable_only = '--all' not in args
    args = [a for a in args if not a.startswith('--')]
    root = args[0] if args else DEFAULT_SRC
    silent, noisy = sweep(root, portable_only=portable_only)
    show('SILENT on wasm32 (multi-member struct <= 4 bytes: indirect vs '
         'direct, same i32 arity, NO wasm-ld warning)', silent)
    show('NOISY (wasm-ld already warns: the arity differs) or size unknown',
         noisy)
    accepted = read_baseline(baseline)
    new = sorted({r[0] for r in silent} - set(accepted))
    print()
    for addr in sorted({r[0] for r in silent} & set(accepted)):
        print(f'  accepted {addr}: {accepted[addr]}')
    for addr in new:
        print(f'FIX {addr}: a by-value struct of 4 bytes or fewer against a '
              f'scalar. On wasm32 one side passes a POINTER and nothing warns. '
              f'Declare the parameter the way the DEFINITION reads it in a '
              f'LEGOLAND_PORTABLE arm and pack at the call site '
              f'(docs/lanes/scope-port-m10.md §1d), or add the address to '
              f'{os.path.basename(baseline)} with a reason.')
    print(f'bvstruct sweep: {len(new)} unaccepted silent site(s), '
          f'{len(silent)} silent, {len(noisy)} noisy')
    sys.exit(1 if new else 0)


if __name__ == '__main__':
    main()
