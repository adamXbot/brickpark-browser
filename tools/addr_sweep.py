#!/usr/bin/env python3
"""One NAME, two ADDRESSES -- and one ADDRESS, two incompatibly sized names.

PORT-P3 §3.3, promoted by PORT-A10. The sibling of `extern_sweep.py`: that one
catches one `extern` STATEMENT carrying several declarators and several
addresses; this one catches the same name declared at two different addresses in
two different FILES, which no single statement reveals.

The class, as PORT-P3 measured it in the running tab:

  * `scrolltick.c` declares `g_view_left` at 0x004b95f4 (the scroll clamp's
    slack, shipped 243200); `coaster3d.c`, `coaster10.c` and `unref3.c` declare
    the same name at 0x008299ac (the coaster's 3D clip rect, shipped zero).
    gen_link emits ONE object, and it picked 0x008299ac -- so the map scroll
    clamps ~475 px short of every edge and the Mechanic's Hut the tutorial tells
    you to click can never be brought on screen.  (P3-1, a blocker.)
  * `fpui2.c` declares `g_popup` at 0x007fdec0; `bighelp.c` and `popup.c`
    declare it at 0x007fdea4. One object is emitted, so every field `fpui2.c`
    stores lands 0x1c below where the other two read it -- and the two element
    pointers the "this is a hut, hire someone" arm compares read ZERO. No
    gardener and no mechanic can be hired in the whole game.  (P3-2, a blocker.)

**Why this needs a tool and not a fix.** The shipped x86 image is unaffected:
on x86 each TU's declaration only says "an object of this type lives
somewhere", and the real linker resolves each spelling to the address the
original .obj recorded, so both sets really are different memory. The portable
build has no original .obj -- `gen_link.py` plans ONE storage object per name
from the source declarations, so the two spellings collapse into one. The bytes
VC6 emits are identical either way, which is why `audit.py`, `relocs.py`,
`match.py` and `verify.py` are all silent and always will be: this is a
PORTABLE-BUILD-ONLY defect that every byte gate the project has is blind to.

Two checks, because the hazard has two directions:

  NAME  one name, several addresses.  gen_link must pick one; every TU that
        spelled the other one is sheared by the difference, silently.
  ADDR  one address, several names whose DECLARED SIZES disagree.  gen_link
        sizes the object by the WIDEST citation, so a narrow spelling reads the
        head of a wider record and is safe -- but two spellings of comparable
        width at one base are two different field layouts over the same bytes,
        which is the `g_popup` shear without the address difference. Rows whose
        only narrow spellings are <= 4 bytes are the project's documented head-
        name convention (`extern int g_copters_path0; /* 0x004c1124 */` is the
        first word of `g_copters_paths`) and are not reported.

Usage:

    python3 portable/tools/addr_sweep.py                    # sweep LEGOLAND/
    python3 portable/tools/addr_sweep.py --baseline FILE    # the ctest gate
    python3 portable/tools/addr_sweep.py --selftest         # shapes, no sources
    python3 portable/tools/addr_sweep.py --names-only       # the cheap half

Exit status is the gate: 0 when every reported row is accepted by the baseline,
1 on any row that is not (or on any row that GREW past its baseline entry). A
baseline row that is no longer reported prints FIXED and still passes, so the
lane that fixes one tightens this file in the same commit instead of fighting
the gate -- which is what lets the baseline hold PORT-P3's 17 names today and
stay green the moment PORT-M15 renames them.

With no baseline it reports and exits 1 on any hit at all, which is the form a
lane wants interactively.

Sources only: no gamedata/, no image, no build products, so it runs in CI on
both toolchains like `extern_sweep` and `bvstruct_sweep`.
"""
import argparse
import collections
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import linkreport as lr  # noqa: E402

SRC = lr.SRC

# The first address-shaped literal in a statement's comments, which is what
# every scanner in the tree takes (`linkreport.scan_sources`, `gen_link`'s
# DECL_RE, `cdecl._addr_for`). Taking the same one is deliberate: this sweep is
# about where those first-addresses DISAGREE between files. A statement with
# several declarators AND several addresses is a different class and belongs to
# `extern_sweep.py`, which gates it to zero.
_ADDR = re.compile(r'\b0x[0-9a-fA-F]{6,8}\b')
_MARKER = re.compile(r'^// (FUNCTION|WIP-FUNCTION): LEGOLAND (0x[0-9a-fA-F]+)')
# `linkreport.scan_sources`' own definition-line matcher, so a function's
# marker address is credited to exactly the name that scanner credits it to.
_SIG = re.compile(r'^[A-Za-z_][A-Za-z0-9_ *]*?\**\s*\**([A-Za-z_][A-Za-z0-9_]*)\s*\(')

# A renamed definition is NOT a collision. PORT-M7 kept castleobj.c's second
# `Track_Update` under its own symbol with `#define Track_Update
# Track_Update_427b20`; the marker still names the original, so the sweep sees
# two addresses for one name while the C has two distinct symbols. Record the
# redirect so the row can say so.
_DEFINE_RENAME = re.compile(r'^\s*#\s*define\s+(\w+)\s+(\w+)\s*$')

# Names the sweep should not report at all. Nothing is here yet; the baseline
# is the place for a ruling, so that a reader sees the reason.
IGNORE = frozenset()


class Cite:
    """One place a name is tied to an address."""
    __slots__ = ('name', 'addr', 'kind', 'file', 'line', 'how', 'typename',
                 'extent')

    def __init__(self, name, addr, kind, file, line, how,
                 typename='', extent=None):
        self.name, self.addr, self.kind = name, addr, kind
        self.file, self.line, self.how = file, line, how
        self.typename, self.extent = typename, extent

    @property
    def where(self):
        return f'{self.file}:{self.line}'

    def __repr__(self):
        return f'<{self.name} 0x{self.addr:08x} {self.how} {self.where}>'


def _sources(src):
    return sorted(glob.glob(os.path.join(src, '*.c'))) + \
        sorted(glob.glob(os.path.join(src, '*.h')))


def name_citations(src=SRC):
    """name -> [Cite] for every address a source ties a name to.

    Both ways a name acquires an address in this tree:

      * `extern <T> name ...;   /* 0x... */`  -- data and functions alike, read
        a STATEMENT at a time so a declaration wrapped over four lines is one
        citation and not four;
      * `// FUNCTION: LEGOLAND 0x...` over a definition -- the marker the VC6
        gates use, which is the only address a recovered function HAS.

    Also returns the `#define old new` renames, so a row whose name was already
    given a distinct symbol can say which."""
    cites = collections.defaultdict(list)
    renames = {}
    for path in _sources(src):
        base = os.path.basename(path)
        text = open(path, encoding='utf-8', errors='replace').read()
        pending = None
        for i, line in enumerate(text.split('\n')):
            m = _MARKER.match(line)
            if m:
                pending = int(m.group(2), 16)
                continue
            d = _DEFINE_RENAME.match(line)
            if d:
                renames.setdefault(d.group(1), (d.group(2), base, i + 1))
            if line.lstrip().startswith(('/*', '*', '#', '//')) or not line.strip():
                continue
            s = _SIG.match(line)
            if s and not line.startswith(('extern', 'typedef', 'static inline')):
                if pending is not None:
                    cites[s.group(1)].append(
                        Cite(s.group(1), pending, 'fn', base, i + 1, 'marker'))
                    pending = None
        pos = 0
        for start in lr._extern_positions(text):
            if start < pos:
                continue
            stmt, comments, pos = lr._statement_at(text, start)
            addr = None
            for c in comments:
                a = _ADDR.search(c)
                if a:
                    addr = int(a.group(), 16)
                    break
            if addr is None:
                continue
            line = text.count('\n', 0, start) + 1
            for name, kind in lr.extern_decl_names(stmt):
                cites[name].append(
                    Cite(name, addr, kind, base, line, 'extern'))
    return cites, renames


def name_collisions(src=SRC, cites=None, renames=None):
    """[(name, {addr: [Cite]}, rename_or_None)] for every name tied to more
    than one address, sorted by name."""
    if cites is None:
        cites, renames = name_citations(src)
    renames = renames or {}
    out = []
    for name in sorted(cites):
        if name in IGNORE:
            continue
        by = collections.defaultdict(list)
        for c in cites[name]:
            by[c.addr].append(c)
        if len(by) < 2:
            continue
        out.append((name, dict(by), renames.get(name)))
    return out


# ---------------------------------------------------------------- the ADDR half
# `cdecl.py` is the only thing in the tree that knows `sizeof` of the game's own
# typedefs, so the size half needs its parse. It is the expensive half (it
# parses every TU), which is why it is separable: `--names-only` and gen_link's
# manifest use the cheap half alone.
HEAD_NAME_BYTES = 4


def addr_collisions(src=SRC, tus=None):
    """[(addr, {name: size}, [Cite])] for every address that carries two or
    more names whose declared sizes disagree, head-name views excluded.

    `size` is the WIDEST extent any file gives that name, because that is the
    one gen_link would size the object by."""
    import cdecl
    if tus is None:
        tus, headers = cdecl.parse_tree(src)
        tus = list(tus) + list(headers.values())
    by = collections.defaultdict(list)
    for tu in tus:
        for obj in tu.objects:
            by[obj.addr].append(
                Cite(obj.name, obj.addr, 'data', tu.file, obj.line, 'extern',
                     obj.typename, obj.extent))
    out = []
    for addr in sorted(by):
        widest = {}
        for c in by[addr]:
            if c.extent is None:
                continue
            widest[c.name] = max(widest.get(c.name, 0), c.extent)
        if len(widest) < 2 or len(set(widest.values())) < 2:
            continue
        mx = max(widest.values())
        # Every narrower spelling <= 4 bytes is the head-name convention: a
        # scalar naming the first word of a wider record, which reads correctly
        # whichever object gen_link emits.
        if all(e <= HEAD_NAME_BYTES for n, e in widest.items() if e < mx):
            continue
        out.append((addr, widest, sorted(by[addr],
                                        key=lambda c: (c.file, c.line))))
    return out


# ------------------------------------------------------------------- baseline
def parse_baseline(path):
    """Returns ({name: set(addr)}, {addr: {name: size}}, [bad lines]).

    Format, one row per line, `#` starts a comment and a row's reason is the
    comment after it:

        NAME g_popup 0x007fdea4,0x007fdec0   # reason
        ADDR 0x0066809c g_lock=124,g_ddsd=108   # reason
    """
    names, addrs, bad = {}, {}, []
    if not os.path.exists(path):
        return names, addrs, [f'no baseline at {path}']
    for lineno, raw in enumerate(open(path, encoding='utf-8'), 1):
        line = raw.split('#', 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        if parts[0] == 'NAME' and len(parts) == 3:
            try:
                names[parts[1]] = {int(a, 16) for a in parts[2].split(',')}
            except ValueError:
                bad.append(f'{path}:{lineno}: bad address list: {line}')
        elif parts[0] == 'ADDR' and len(parts) == 3:
            try:
                addrs[int(parts[1], 16)] = {
                    kv.split('=')[0]: int(kv.split('=')[1])
                    for kv in parts[2].split(',')}
            except (ValueError, IndexError):
                bad.append(f'{path}:{lineno}: bad name=size list: {line}')
        else:
            bad.append(f'{path}:{lineno}: not a NAME or ADDR row: {line}')
    return names, addrs, bad


def check(name_rows, addr_rows, baseline):
    """(failures, notes) against a baseline file."""
    b_names, b_addrs, bad = parse_baseline(baseline)
    fails = list(bad)
    notes = []
    seen_names, seen_addrs = set(), set()
    for name, by, rename in name_rows:
        seen_names.add(name)
        got = set(by)
        want = b_names.get(name)
        if want is None:
            fails.append(
                f'NAME `{name}` is declared at '
                + ', '.join(f'0x{a:08x}' for a in sorted(got))
                + ' and is NOT in the baseline')
        elif got - want:
            fails.append(
                f'NAME `{name}` GREW: new address(es) '
                + ', '.join(f'0x{a:08x}' for a in sorted(got - want))
                + f' (baseline: {", ".join(f"0x{a:08x}" for a in sorted(want))})')
        elif want - got:
            notes.append(
                f'NARROWED NAME `{name}`: the baseline lists '
                + ', '.join(f'0x{a:08x}' for a in sorted(want))
                + ', the sources now say '
                + ', '.join(f'0x{a:08x}' for a in sorted(got)))
    for addr, widest, _cites in addr_rows:
        seen_addrs.add(addr)
        want = b_addrs.get(addr)
        if want is None:
            fails.append(
                f'ADDR 0x{addr:08x} carries '
                + ', '.join(f'{n}={e}' for n, e in sorted(widest.items()))
                + ' and is NOT in the baseline')
        else:
            new = {n: e for n, e in widest.items()
                   if n not in want or want[n] != e}
            if new:
                fails.append(
                    f'ADDR 0x{addr:08x} CHANGED: '
                    + ', '.join(f'{n}={e}' for n, e in sorted(new.items()))
                    + ' (baseline: '
                    + ', '.join(f'{n}={e}' for n, e in sorted(want.items()))
                    + ')')
    for name in sorted(set(b_names) - seen_names):
        notes.append(f'FIXED NAME `{name}`: one address now -- drop the row')
    for addr in sorted(set(b_addrs) - seen_addrs):
        notes.append(f'FIXED ADDR 0x{addr:08x}: no size disagreement now -- '
                     f'drop the row')
    return fails, notes


# ------------------------------------------------------------------- reporting
def report_name(name, by, rename):
    lines = [f'NAME `{name}` -- {len(by)} addresses: '
             + ', '.join(f'0x{a:08x}' for a in sorted(by))]
    for addr in sorted(by):
        for c in sorted(by[addr], key=lambda c: (c.file, c.line)):
            lines.append(f'    0x{addr:08x}  {c.where:24s} {c.how} ({c.kind})')
    if rename:
        lines.append(f'    NOTE {rename[1]}:{rename[2]} has '
                     f'`#define {name} {rename[0]}`, so the C already has two '
                     f'distinct symbols; the marker address is the only thing '
                     f'that still collides.')
    else:
        lines.append('    FIX: one of the two spellings is a different object '
                     'and needs a different NAME (the portable build plans one '
                     'storage object per name; the original had two, one per '
                     '.obj). No bytes move, so no byte gate can see either '
                     'state.')
    return lines


def report_addr(addr, widest, cites):
    lines = [f'ADDR 0x{addr:08x} -- '
             + ', '.join(f'{n}={e}' for n, e in
                         sorted(widest.items(), key=lambda kv: -kv[1]))]
    for c in cites:
        lines.append(f'    {c.where:24s} {c.typename} {c.name}'
                     f'  declared {c.extent}')
    lines.append('    FIX: say which spelling is the OBJECT and make the '
                 'others views of it (a `#define` onto its field, as gpu.c '
                 'does for g_ddsd), or accept the row in the baseline with a '
                 'reason.')
    return lines


# ------------------------------------------------------------------- selftest
SELFTEST_FILES = {
    # the positive control for NAME: one name, two files, two addresses -- the
    # exact shape of P3-1.
    'scrolltick.c': 'extern int g_view_left;   /* 0x004b95f4 */\n',
    'coaster3d.c': 'extern int g_view_left;   /* 0x008299ac */\n',
    # ... and the negative: the SAME address from four files is the ordinary
    # case and must not be reported.
    'a.c': 'extern int g_agreed;   /* 0x00400100 */\n',
    'b.c': 'extern int g_agreed;   /* 0x00400100 */\n',
    # a function defined at two addresses under two markers (the Track_Update
    # shape), and the rename that mitigates it
    'castleobj.c': '#define Track_Update Track_Update_427b20\n'
                   '// FUNCTION: LEGOLAND 0x00427b20\n'
                   'void Track_Update(int a)\n{\n}\n',
    'coaster.c': '// FUNCTION: LEGOLAND 0x004275d0\n'
                 'void Track_Update(int a, int b, int c)\n{\n}\n',
    # a declaration wrapped over several lines is ONE citation, not several
    'wrapped.c': 'extern int\n    g_wrapped;   /* 0x00400200 */\n',
    'wrapped2.c': 'extern int g_wrapped;   /* 0x00400200 */\n',
    # an address in prose above a declaration must not become a citation of it
    'prose.c': '/* the value at 0x00400300 is the one below */\n'
               'extern int g_agreed2;   /* 0x00400400 */\n',
    'prose2.c': 'extern int g_agreed2;   /* 0x00400400 */\n',
}

# The ADDR half needs types, so it gets its own fixture: one record and one
# alternative spelling of comparable width at the same base (reported), plus a
# head-name scalar (not reported).
SELFTEST_ADDR = {
    'gpu.c': 'typedef struct Wide { int a[31]; } Wide;\n'
             'extern Wide g_lock;        /* 0x0066809c */\n',
    'blitmisc.c': 'typedef struct Narrow { int a[27]; } Narrow;\n'
                  'extern Narrow g_ddsd;      /* 0x0066809c */\n',
    'heads.c': 'typedef struct Big { int a[20]; } Big;\n'
               'extern Big g_paths;        /* 0x004c1124 */\n'
               'extern int g_path0;        /* 0x004c1124 */\n',
}


def selftest():
    import tempfile
    fails = []
    d = tempfile.mkdtemp()
    names_dir = os.path.join(d, 'names')
    os.makedirs(names_dir, exist_ok=True)
    for fn, text in SELFTEST_FILES.items():
        open(os.path.join(names_dir, fn), 'w').write(text)
    rows = {name: (by, rn) for name, by, rn in name_collisions(names_dir)}

    def chk(what, got, want):
        if got != want:
            fails.append(f'{what}: got {got!r}, want {want!r}')

    chk('the NAME positive control is reported', 'g_view_left' in rows, True)
    if 'g_view_left' in rows:
        by, _rn = rows['g_view_left']
        chk('g_view_left addresses', sorted(by), [0x004b95f4, 0x008299ac])
        chk('g_view_left cites both files',
            sorted(c.file for a in by for c in by[a]),
            ['coaster3d.c', 'scrolltick.c'])
    chk('one address from two files is NOT a collision',
        'g_agreed' in rows, False)
    chk('the two-marker function is reported', 'Track_Update' in rows, True)
    if 'Track_Update' in rows:
        by, rn = rows['Track_Update']
        chk('Track_Update addresses', sorted(by), [0x004275d0, 0x00427b20])
        chk('the #define rename is recorded',
            rn[0] if rn else None, 'Track_Update_427b20')
        chk('both citations are markers',
            sorted({c.how for a in by for c in by[a]}), ['marker'])
    chk('a wrapped declaration is one citation', 'g_wrapped' in rows, False)
    chk('an address in prose is not a citation', 'g_agreed2' in rows, False)

    # the ADDR half
    addr_dir = os.path.join(d, 'addrs')
    os.makedirs(addr_dir, exist_ok=True)
    for fn, text in SELFTEST_ADDR.items():
        open(os.path.join(addr_dir, fn), 'w').write(text)
    arows = {a: w for a, w, _c in addr_collisions(addr_dir)}
    chk('the ADDR positive control is reported', 0x0066809c in arows, True)
    if 0x0066809c in arows:
        chk('both widths are read',
            sorted(arows[0x0066809c].items()), [('g_ddsd', 108), ('g_lock', 124)])
    chk('a <=4-byte head name is NOT a collision', 0x004c1124 in arows, False)

    # the baseline reader, and the three verdicts it has to give
    bl = os.path.join(d, 'baseline.txt')
    open(bl, 'w').write(
        '# fixture\n'
        'NAME g_view_left 0x004b95f4,0x008299ac   # accepted\n'
        'NAME g_gone 0x00400500,0x00400600   # already fixed\n'
        'ADDR 0x0066809c g_lock=124,g_ddsd=108   # accepted\n')
    nrows = name_collisions(names_dir)
    arows_full = addr_collisions(addr_dir)
    f, notes = check(nrows, arows_full, bl)
    chk('an accepted NAME row does not fail',
        any('g_view_left' in x for x in f), False)
    chk('an unaccepted NAME row fails',
        any('Track_Update' in x and 'NOT in the baseline' in x for x in f), True)
    chk('a baseline row that is gone prints FIXED',
        any('FIXED NAME `g_gone`' in x for x in notes), True)
    chk('an accepted ADDR row does not fail',
        any('0x0066809c' in x for x in f), False)
    # and a GROWN row must fail
    open(bl, 'w').write('NAME g_view_left 0x004b95f4   # half the truth\n'
                        'ADDR 0x0066809c g_lock=124,g_ddsd=108   # accepted\n')
    f2, _n2 = check(nrows, arows_full, bl)
    chk('a NAME row with a new address fails',
        any('GREW' in x and 'g_view_left' in x for x in f2), True)
    # a missing baseline is a failure, not a pass
    f3, _n3 = check(nrows, arows_full, os.path.join(d, 'nope.txt'))
    chk('a missing baseline fails', any('no baseline' in x for x in f3), True)

    for x in fails:
        print(f'FAIL {x}')
    print(f'addr_sweep selftest: {len(fails)} failure(s)')
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--selftest', action='store_true',
                    help='the shapes, positive and negative; no sources needed')
    ap.add_argument('--src', default=SRC, help='the directory to sweep')
    ap.add_argument('--baseline',
                    help='the accepted rows; a row not here fails the gate')
    ap.add_argument('--names-only', action='store_true',
                    help='the NAME half only (no cdecl parse)')
    ap.add_argument('--quiet', action='store_true',
                    help='the verdict line and the failures, nothing else')
    args = ap.parse_args()
    if args.selftest:
        return selftest()

    name_rows = name_collisions(args.src)
    addr_rows = [] if args.names_only else addr_collisions(args.src)

    if not args.quiet:
        if name_rows or addr_rows:
            print(f'{len(name_rows)} name(s) declared at more than one address, '
                  f'{len(addr_rows)} address(es) carrying names of '
                  f'incompatible declared size:\n')
        for name, by, rename in name_rows:
            print('\n'.join(report_name(name, by, rename)))
            print()
        for addr, widest, cites in addr_rows:
            print('\n'.join(report_addr(addr, widest, cites)))
            print()

    if not args.baseline:
        if not name_rows and not addr_rows:
            print('addr_sweep: 0 address collision(s) -- the class is closed.')
            return 0
        return 1

    fails, notes = check(name_rows, addr_rows, args.baseline)
    for n in notes:
        print(n)
    for f in fails:
        print(f'FAIL {f}')
    print(f'addr_sweep gate: {len(fails)} failure(s) over '
          f'{len(name_rows)} name row(s) and {len(addr_rows)} address row(s)')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
