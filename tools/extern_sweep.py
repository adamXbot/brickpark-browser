#!/usr/bin/env python3
"""One `extern` statement, two addresses: the class no byte gate can see.

PORT-M6 §1f. `pathmisc2.c` declared

    extern void *g_route_open, *g_route_closed;   /* 0x00668fc0, 0x00668fc4 */

Two objects, two addresses, ONE trailing comment -- and every scanner in the
tree (`gen_link.py`'s `DECL_RE`, `linkreport.scan_sources`, `cdecl.py`) takes
the FIRST address in the statement and gives it to every declarator in it. So
both names were emitted at 0x00668fc0: in the portable build `g_route_open` WAS
`g_route_closed`, and the gardener and mechanic work-order tails were their own
heads. PORT-M6 found three such statements and split them.

**Why this is a tool and not a one-off.** The VC6 byte gates cannot see it. The
image is unchanged, `audit.py` is unchanged, `relocs.py` is unchanged,
`verify.py` is unchanged -- the C compiles to the same bytes either way,
because on x86 the declaration only has to say "a pointer lives somewhere" and
the linker supplies the address. It is a PORTABLE-BUILD-ONLY defect, invisible
to every gate the project has, and it silently aliases two live globals. The
only way to keep it closed is to sweep for the shape, which is what this does.

PORT-M6 wrote it as a scratch script, PORT-M8 rewrote it as a scratch script to
re-run the gate (§6, still 0), and it is promoted here so the ctest can hold
the line instead of a lane remembering to look.

    python3 portable/tools/extern_sweep.py            # sweep LEGOLAND/
    python3 portable/tools/extern_sweep.py --selftest # the shapes, no sources
    python3 portable/tools/extern_sweep.py --quiet     # exit status only

Exit status is 0 when the class is closed and 1 on any hit, so it wires
straight into ctest. It needs no game assets and no build -- only the sources --
which is what lets it run in CI next to the asset-free tests.
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

# Every address-shaped literal in the statement's comments. Six to eight hex
# digits: the image is 0x00400000..0x00836000, so a `/* +0x14 */` field offset
# (two digits) is not one of these and neither is a `0xff` mask.
_ADDR = re.compile(r'\b0x00[0-9a-fA-F]{6}\b')


def multi_address_externs(src=SRC):
    """[(file, line, n_names, names, addrs, statement)] -- every `extern` DATA
    statement that declares more than one name and whose comments name more
    than one address.

    Both halves matter. Two names and one address is the ordinary case
    (`extern int a, b;  /* 0x00400100 */` really is one object the sources
    split badly, and gen_link's interior aliasing handles it). One name and two
    addresses is a prose comment. Two of each is the defect: the scanners pair
    them up wrongly and nothing downstream can tell.

    A FUNCTION declaration is excluded: several legitimately list the addresses
    of their callers or of a sibling in the comment, and a function has no
    address to be emitted AT -- `gen_link.py` resolves it by name."""
    out = []
    for path in sorted(glob.glob(os.path.join(src, '*.c'))) + \
            sorted(glob.glob(os.path.join(src, '*.h'))):
        base = os.path.basename(path)
        text = open(path, encoding='utf-8', errors='replace').read()
        pos = 0
        for start in lr._extern_positions(text):
            if start < pos:
                continue
            stmt, comments, pos = lr._statement_at(text, start)
            names = lr.extern_decl_names(stmt)
            data = [n for n, kind in names if kind == 'data']
            if len(data) < 2:
                continue
            addrs = []
            for c in comments:
                for a in _ADDR.findall(c):
                    if int(a, 16) not in addrs:
                        addrs.append(int(a, 16))
            if len(addrs) < 2:
                continue
            out.append((base, text.count('\n', 0, start) + 1, len(data), data,
                        addrs, ' '.join(stmt.split())))
    return out


SELFTEST_POSITIVE = [
    # PORT-M6's own two spellings, which are the two the sources used.
    'extern void *g_route_open, *g_route_closed;   /* 0x00668fc0, 0x00668fc4 */',
    'extern void *g_a, *g_b;   /* 0x00668fc0 .. 0x00668fc4 */',
    # the addresses on CONTINUATION lines, and three declarators. PORT-M4's
    # finding #1: a statement may be formatted over several lines and the
    # scanners read to the `;`, so a comment anywhere inside it counts.
    'extern int g_x,      /* 0x00500100 */\n'
    '           g_y,      /* 0x00500104 */\n'
    '           g_z;      /* 0x00500108 */',
    # an array pair, which is the shape the work-order tails had
    'extern char* g_head[4], * g_tail[4];  /* 0x00668f00, 0x00668f10 */',
]
SELFTEST_NEGATIVE = [
    # one address, two names: the ordinary split-object case, not this class
    'extern int g_a, g_b;   /* 0x00400100 */',
    # two addresses, one name: a prose comment about a neighbour
    'extern int g_only;   /* 0x00400100, was 0x00400200 before the reframe */',
    # a FUNCTION whose comment lists its own and a sibling's address
    'extern void DoThing(int, int);  /* 0x00401000, see 0x00402000 */',
    # field offsets are not addresses
    'extern int g_a, g_b;   /* +0x14, +0x18 */',
    # the word "extern" in prose above a real declaration
    '/* the extern below, at 0x00400100, 0x00400200 */\nextern int g_c;',
]


def selftest():
    import tempfile
    fails = []
    d = tempfile.mkdtemp()
    for i, text in enumerate(SELFTEST_POSITIVE + SELFTEST_NEGATIVE):
        want = i < len(SELFTEST_POSITIVE)
        sub = os.path.join(d, f'case{i}')
        os.makedirs(sub, exist_ok=True)
        open(os.path.join(sub, 'case.c'), 'w').write(text + '\n')
        hits = multi_address_externs(sub)
        if bool(hits) != want:
            fails.append(f'{"missed" if want else "false positive on"}: '
                         f'{text.splitlines()[0]!r} -> {hits}')
    # and the fields the report needs, on M6's real statement
    sub = os.path.join(d, 'real')
    os.makedirs(sub, exist_ok=True)
    open(os.path.join(sub, 'pathmisc2.c'), 'w').write(SELFTEST_POSITIVE[0] + '\n')
    hits = multi_address_externs(sub)
    if len(hits) != 1:
        fails.append(f'M6 statement: {len(hits)} hits, want 1')
    else:
        _f, _ln, n, names, addrs, _s = hits[0]
        if (n, names, addrs) != (2, ['g_route_open', 'g_route_closed'],
                                 [0x00668fc0, 0x00668fc4]):
            fails.append(f'M6 statement read as {n} {names} '
                         f'{[hex(a) for a in addrs]}')
    for f in fails:
        print(f'FAIL {f}')
    print(f'extern_sweep selftest: {len(fails)} failure(s)')
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--selftest', action='store_true',
                    help='the shapes, positive and negative; no sources needed')
    ap.add_argument('--src', default=SRC, help='the directory to sweep')
    ap.add_argument('--quiet', action='store_true',
                    help='exit status only, for the ctest')
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    hits = multi_address_externs(args.src)
    if not hits:
        if not args.quiet:
            print(f'0 multi-address extern statement(s) in {args.src} -- '
                  f'the class is closed.')
        return 0
    print(f'{len(hits)} multi-address extern statement(s) -- each declares '
          f'several objects and every scanner in the tree will give them ALL '
          f'the first address:\n')
    for base, line, n, names, addrs, stmt in hits:
        print(f'{base}:{line}  {n} names, {len(addrs)} addresses')
        print(f'    names: {", ".join(names)}')
        print(f'    addrs: {", ".join(f"0x{a:08x}" for a in addrs)}')
        print(f'    {stmt}')
        print('    FIX: one statement per object, each with its own address '
              'comment. No bytes move -- VC6 emits the same code either way, '
              'which is why no byte gate can see this.\n')
    return 1


if __name__ == '__main__':
    sys.exit(main())
