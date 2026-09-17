#!/usr/bin/env python3
"""One address, two prototypes -- one of them variadic. The wasm32 class that
every signature gate in the tree is structurally blind to.

PORT-M20 found it in the Spider Ride. `LEGOLAND/mechrides.c` declared
0x0049e573 -- `sprintf`, spelled `Format(char*, const char*, ...)` in six other
files -- as

    extern int sprintf_w(char* dst, const char* fmt, int v);   /* 0x0049e573 */

and called it as `sprintf_w(&g_spider_pathname[6], "%02d", b->seat)`. On x86
cdecl the two spellings are the SAME instruction stream: the seat number is one
pushed dword either way, and `sprintf` reads it out of the stack frame with
`va_arg`. On wasm32 they are not the same thing at all. clang lowers `...` by
writing the variable arguments into a buffer and passing ITS ADDRESS as one
extra parameter, so libc's `int sprintf(char*, const char*, ...)` really is

    (i32, i32, i32) -> i32            <- the third i32 is a va_list POINTER

and a fixed three-parameter declaration of the same function is *also*

    (i32, i32, i32) -> i32            <- the third i32 is the seat NUMBER

Both wasm signatures are identical, so **wasm-ld warns about nothing**,
`linkreport.py`'s signature vote sees no conflict, `name_trap.py` has no
`call_indirect` to explain and the module validates. `sprintf` then read the
seat number as the address of a va_list: every `"%02d"` came out `00`, every
rider asked for path `SPIDER00` and the Spider Ride's riders never got seated.

And the byte gates cannot help either, which is the other half of why this
needs a tool: VC6 emits the same code for both spellings, so `audit.py`,
`relocs.py`, `match.py` and `verify.py` are silent and always will be. Like
`extern_sweep.py` (one statement, two addresses), `addr_sweep.py` (one name, two
addresses) and `bvstruct_sweep.py` (a 4-byte aggregate passed by value), this is
a PORTABLE-BUILD-ONLY defect with no other witness in the project.

## What the sweep checks

Every file-scope prototype and every `extern` declaration in `LEGOLAND/*.c` and
`*.h`, grouped by the ADDRESS its comment cites (by name when it cites none),
against the shape the group's own truth says it has -- a definition in the tree
if there is one, else libc's prototype from `gen_link.CRT_PROTOS`, else the
majority of the declarations. A group is clean when every live declaration of it
agrees on BOTH

  * variadic or not, and
  * the number of FIXED parameters before the `...`.

Four ways to disagree, and the severity differs because the generator bridges
one of them and cannot bridge the others:

  VA-SLOT    a non-variadic declaration with MORE fixed parameters than the
             variadic one. Argument `nfix+1` lands in the slot the callee reads
             as its va_list pointer. This is M20's defect, and it is silent in
             every other gate.
  VA-COUNT   variadic, but the `...` starts at a different parameter. The buffer
             is built at the wrong offset: the same defect one argument along.
  VA-UNSPEC  `extern void Foo();` -- no prototype at all, so the call site emits
             whatever it was handed and no varargs buffer exists.
  VA-EMPTY   a non-variadic declaration with EXACTLY the variadic's fixed count.
             For a CRT target `gen_link.crt_alias` bridges this correctly (it
             declares libc's real variadic prototype, so clang builds the empty
             buffer -- PORT-A4 §1's `DebugPrint` -> `printf`). For a GAME target
             it becomes a cast forwarder and traps at the `call_indirect`. It is
             still a false claim about the callee's ABI, so it is reported.

The gate is the absence of all four: there is no baseline file, because every
row has a fix and PORT-A11 made all of them.

## The fix, always

The VC6 text is load-bearing -- the declaration's parameter types are the
caller's codegen lever (docs/HANDOFF.md section 3) -- so the repair is a guard
arm in the declaring file, never an edit of the original spelling:

    #ifndef LEGOLAND_PORTABLE
    extern int sprintf_w(char* dst, const char* fmt, int v);  /* 0x0049e573 */
    #else
    /* wasm32: the callee is variadic, so the third dword is a va_list
     * pointer, not the value. PORT-A11 / PORT-M20. */
    extern int sprintf_w(char* dst, const char* fmt, ...);    /* 0x0049e573 */
    #endif

which is why this sweep is preprocessor-aware for `LEGOLAND_PORTABLE`: it reads
the arm the PORTABLE build compiles and leaves the VC6 arm alone. A sweep that
read both would report every one of its own repairs.

Usage:

    python3 tools/variadic_sweep.py                  # sweep LEGOLAND/
    python3 tools/variadic_sweep.py --census         # every group
    python3 tools/variadic_sweep.py --markdown       # manifest section
    python3 tools/variadic_sweep.py --selftest       # shapes, no sources
    python3 tools/variadic_sweep.py --quiet           # status only

Exit status is the gate: 0 when every group agrees, 1 on any conflict. Sources
only -- no gamedata/, no image, no build products -- so it runs in CI on both
toolchains next to the other asset-free sweeps. `gen_link.py` calls
`conflicts()` before it generates anything and REFUSES to close the link while
one is open.
"""
import argparse
import bisect
import collections
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import linkreport as lr            # noqa: E402
import bvstruct_sweep as bvs       # noqa: E402

SRC = lr.SRC

# Six to eight hex digits in an image-shaped range: a `/* +0x14 */` field offset
# and a `0xff` mask are not addresses. Same literal shape extern_sweep.py uses.
_ADDR = re.compile(r'\b0x00[0-9a-fA-F]{6}\b')

# ---- the shapes -------------------------------------------------------------
# A shape is what a declaration CLAIMS about the callee's wasm signature:
#   ('...', n)    variadic with n fixed parameters -> n+1 wasm parameters, the
#                 last one a pointer to the varargs buffer
#   ('fixed', n)  n parameters, none of them a va_list pointer
#   ('()', -1)    no prototype: the call site emits whatever it is handed
VARIADIC, FIXED, UNSPEC = '...', 'fixed', '()'


def shape_text(shape):
    kind, n = shape
    if kind == UNSPEC:
        return '()'
    if kind == VARIADIC:
        return f'({n} fixed, ...)' if n else '(...)'
    return f'({n} fixed)'


def wasm_arity(shape):
    """How many wasm parameters the shape claims. The equality of these two
    numbers across a VA-SLOT row is exactly why no link-time gate can see it."""
    kind, n = shape
    if kind == UNSPEC:
        return None
    return n + 1 if kind == VARIADIC else n


class Decl:
    """One prototype of one function, as the sources spell it."""

    __slots__ = ('file', 'line', 'name', 'addrs', 'shape', 'defn', 'text')

    def __init__(self, file, line, name, addrs, shape, defn, text):
        self.file, self.line, self.name = file, line, name
        self.addrs, self.shape, self.defn, self.text = addrs, shape, defn, text

    @property
    def where(self):
        return f'{self.file}:{self.line}'

    def __repr__(self):
        return f'<{self.where} {self.name}{shape_text(self.shape)}>'


# ---- the scanner ------------------------------------------------------------
# Two passes, because the tree spells a prototype two ways and both have carried
# a defect of this class:
#
#   A. every `extern` statement, at any brace depth, read with linkreport.py's
#      own statement reader -- the same text `lr.scan_sources` and gen_link see.
#   B. every statement at FILE SCOPE, which is how `audiomisc.c` spells
#      `int sprintf(char*, const char*, ...);` (no `extern`) and how
#      `sysstubs.c` spells the DEFINITION `void DebugPrintf(const char*, ...) {}`.
#      The definition matters most: it is the group's truth.

def _params_of(piece, name):
    """The parameter list of declarator `piece` as a list of token lists."""
    idxs = [k for k, t in enumerate(piece)
            if t == name and k + 1 < len(piece) and piece[k + 1] == '(']
    if not idxs:
        return None
    depth, j, ptoks = 0, idxs[-1] + 1, []
    while j < len(piece):
        t = piece[j]
        if t == '(':
            depth += 1
            if depth == 1:
                j += 1
                continue
        elif t == ')':
            depth -= 1
            if depth == 0:
                break
        ptoks.append(t)
        j += 1
    params, cur, d = [], [], 0
    for t in ptoks + [',']:
        if t in '([':
            d += 1
        elif t in ')]':
            d -= 1
        if t == ',' and d == 0:
            params.append(cur)
            cur = []
            continue
        cur.append(t)
    return [p for p in params if p]


def _shape_of_params(params):
    variadic = bool(params) and params[-1] == ['.', '.', '.']
    fixed = [p for p in params if p != ['.', '.', '.']]
    if len(fixed) == 1 and fixed[0] == ['void']:
        fixed = []
    if not params:
        return (UNSPEC, -1)
    return (VARIADIC if variadic else FIXED, len(fixed))


def _declarators(stmt):
    """[(name, piece)] for every function declarator in one statement."""
    toks = lr._TOKENS.findall(lr._strip_braces(stmt))
    pieces, piece, depth = [], [], 0
    for t in toks + [',']:
        if t in '([':
            depth += 1
        elif t in ')]':
            depth -= 1
        if t in (',', ';') and depth == 0:
            pieces.append(piece)
            piece = []
            continue
        piece.append(t)
    out = []
    for p in pieces:
        hit = lr._declarator_name(p)
        if hit and hit[1] == 'fn':
            out.append((hit[0], p))
    return out


def _addrs_in(comments):
    out = []
    for c in comments:
        for a in _ADDR.findall(c):
            if int(a, 16) not in out:
                out.append(int(a, 16))
    return out


def _file_scope_statements(text):
    """(start, code, comments, is_definition) for every statement at brace
    depth 0. Preprocessor directives are skipped whole (a `#define` body is not
    a declaration and can be unbalanced); a definition's BODY is skipped, so a
    prototype inside a function is pass A's business, not this one's.

    `comments` holds only what is INSIDE the statement or trails its `;` -- the
    same rule `linkreport._statement_at` uses, and not an accident: the prose
    block above a declaration routinely names a DIFFERENT address (the function
    it is about, a sibling, a caller). Attributing those cost this sweep two
    phantom groups before the rule went in -- `wsprintfA` filed under
    `LoadLmsModel`'s 0x00420640 because schoolcar7.c's CODEGEN note sits above
    it."""
    out = []
    i, n = 0, len(text)
    start, code, comments = None, [], []
    paren = 0
    line_start = True

    def reset():
        return None, [], []

    def kept(cs):
        return [c for p, c in cs if start is not None and p >= start]

    while i < n:
        c = text[i]
        if c == '\n':
            line_start = True
            code.append(' ')
            i += 1
            continue
        if line_start and c == '#':
            while i < n:
                j = text.find('\n', i)
                if j < 0:
                    i = n
                    break
                if text[j - 1:j] == '\\':
                    i = j + 1
                    continue
                i = j + 1
                break
            continue
        if c in ' \t\r':
            code.append(' ')
            i += 1
            continue
        if text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            comments.append((i, text[i:j]))
            i = j
            continue
        if text.startswith('//', i):
            j = text.find('\n', i)
            j = n if j < 0 else j
            comments.append((i, text[i:j]))
            i = j
            continue
        line_start = False
        if c in '"\'':
            q, j = c, i + 1
            while j < n and text[j] != q:
                j += 2 if text[j] == '\\' else 1
            code.append('""')
            i = j + 1
            continue
        if start is None:
            start = i
        if c == '(':
            paren += 1
        elif c == ')':
            paren -= 1
        elif c == ';' and paren <= 0:
            code.append(';')
            i += 1
            while i < n and text[i] in ' \t':
                i += 1
            if text.startswith('/*', i):
                j = text.find('*/', i + 2)
                j = n if j < 0 else j + 2
                comments.append((i, text[i:j]))
                i = j
            elif text.startswith('//', i):
                j = text.find('\n', i)
                j = n if j < 0 else j
                comments.append((i, text[i:j]))
                i = j
            out.append((start, ''.join(code), kept(comments), False))
            start, code, comments = reset()
            paren = 0
            continue
        elif c == '{' and paren <= 0:
            # A body (or a `struct { ... }` / `= { ... }`): emit what came
            # before it, then skip the braces. Emitting a struct definition is
            # harmless -- it has no function declarator -- and skipping the body
            # is what keeps a local prototype out of the file-scope pass.
            out.append((start, ''.join(code) + ';', kept(comments), True))
            depth = 0
            while i < n:
                if text.startswith('/*', i):
                    j = text.find('*/', i + 2)
                    i = n if j < 0 else j + 2
                    continue
                if text.startswith('//', i):
                    j = text.find('\n', i)
                    i = n if j < 0 else j + 1
                    continue
                ch = text[i]
                if ch in '"\'':
                    q, j = ch, i + 1
                    while j < n and text[j] != q:
                        j += 2 if text[j] == '\\' else 1
                    i = j + 1
                    continue
                if ch == '{':
                    depth += 1
                elif ch == '}':
                    depth -= 1
                    if depth == 0:
                        i += 1
                        break
                i += 1
            start, code, comments = reset()
            paren = 0
            line_start = True
            continue
        code.append(c)
        i += 1
    return out


_DEFINE = re.compile(r'^\s*#\s*define\s+([A-Za-z_]\w*)\s+([A-Za-z_]\w*)\s*$')
_UNDEF = re.compile(r'^\s*#\s*undef\s+([A-Za-z_]\w*)\s*$')
_MARKER = re.compile(r'^// (FUNCTION|WIP-FUNCTION): LEGOLAND (0x[0-9a-fA-F]+)',
                     re.M)


def renames(text, live=None):
    """A line-ordered list of (line, {name: replacement}) for the identifier-to-
    identifier `#define`s in force, so a declarator can be read under the name
    the PORTABLE build really compiles it as.

    PORT-M3's repair pattern needs this. `sweep1.c` keeps the matched VC6 body
    and gives the portable build the honest prototype by renaming the first:

        #ifdef LEGOLAND_PORTABLE
        #define DBPrintf DBPrintf_vc6_body     <- line 18
        ...
        void DBPrintf(void) { }                <- line 325, really the _vc6_body
        #ifdef LEGOLAND_PORTABLE
        #undef DBPrintf
        void DBPrintf(const char* fmt, ...) { (void)fmt; }   <- the real one
        #endif

    A sweep that missed the rename read `(void)` as DBPrintf's definition and
    reported all 34 honest declarations as conflicts -- a phantom, and the loudest
    possible one."""
    events, active = [], {}
    for i, ln in enumerate(text.split('\n'), 1):
        if live is not None and i not in live:
            continue
        m = _DEFINE.match(ln)
        if m and m.group(1) != m.group(2):
            active = dict(active)
            active[m.group(1)] = m.group(2)
            events.append((i, active))
            continue
        m = _UNDEF.match(ln)
        if m and m.group(1) in active:
            active = dict(active)
            del active[m.group(1)]
            events.append((i, active))
    return events


def rename_at(events, line):
    out = {}
    for i, mapping in events:
        if i > line:
            break
        out = mapping
    return out


def scan_decls(src=SRC, live_only=True):
    """Every prototype of every function in `src`, as [Decl].

    `live_only` keeps the lines the PORTABLE build compiles: an
    `#ifndef LEGOLAND_PORTABLE` arm is the VC6 spelling and is not this gate's
    business (it is how every fix of this class is written)."""
    out = []
    paths = sorted(glob.glob(os.path.join(src, '*.c'))) + \
        sorted(glob.glob(os.path.join(src, '*.h')))
    for path in paths:
        base = os.path.basename(path)
        text = open(path, encoding='utf-8', errors='replace').read()
        live = bvs.portable_lines(text) if live_only else None
        ren = renames(text, live)
        seen = set()
        # one bisect instead of `text.count('\n', 0, start)` per statement: that
        # was quadratic in the file and most of the sweep's 25 s in CI
        nl = [m.start() for m in re.finditer('\n', text)]

        def add(start, stmt, comments, defn, at=None):
            line = bisect.bisect_left(nl, start) + 1
            if live is not None and line not in live:
                return
            for name, piece in _declarators(stmt):
                params = _params_of(piece, name)
                if params is None:
                    continue
                key = (line, name)
                if key in seen:
                    return
                seen.add(key)
                eff = rename_at(ren, line).get(name, name)
                # A body the portable build compiles under a DIFFERENT name
                # (PORT-M3's `_vc6_body`) is not the definition of the marker's
                # address any more -- the wrapper that kept the name is. Letting
                # it claim the marker made sweep1.c's empty `DBPrintf(void)` the
                # callee of all 34 honest declarations.
                addrs = _addrs_in(comments) or \
                    ([at] if at is not None and eff == name else [])
                out.append(Decl(base, line, eff, addrs,
                                _shape_of_params(params), defn,
                                ' '.join(stmt.split())))

        # pass A: `extern` statements at any depth, linkreport's own reader
        pos = 0
        for start in lr._extern_positions(text):
            if start < pos:
                continue
            stmt, comments, pos = lr._statement_at(text, start)
            add(start, stmt, comments, False)
        # pass B: file scope, which is where a non-`extern` prototype and every
        # DEFINITION lives. A definition's address is its `// FUNCTION:` marker,
        # taken from the nearest one above it and CONSUMED, so the next body
        # cannot inherit it. (`linkreport.scan_sources` answers this question by
        # regex on the next signature-shaped line, which on schoolcar7.c:417
        # picks up the `__declspec(dllimport) ... wsprintfA` prototype above
        # `LoadLmsModel` and files 0x00420640 under the import's name. Reading
        # the declarator is what this module already does, so it does not need
        # to inherit that.)
        markers = [(bisect.bisect_left(nl, m.start()) + 1, int(m.group(2), 16))
                   for m in _MARKER.finditer(text)]
        mi = 0
        for start, stmt, comments, defn in _file_scope_statements(text):
            at = None
            if defn:
                line = bisect.bisect_left(nl, start) + 1
                while mi < len(markers) and markers[mi][0] < line:
                    at = markers[mi][1]
                    mi += 1
            add(start, stmt, comments, defn, at)
    return out


# ---- grouping ---------------------------------------------------------------

def group_decls(decls):
    """key -> [Decl], where the key is ('addr', a) when anything cites one and
    ('name', n) when nothing does.

    A declaration with no address comment joins its name's address when the tree
    cites exactly ONE for that name -- which is the ordinary case, and the reason
    `loadmap.c`'s `Format` and `profiles.c`'s `DBPrintf` are grouped with the rest
    instead of sitting alone. Two addresses for one function name is
    `addr_sweep.py`'s class, not this one's, and such a name is left in a group of
    its own rather than merging two functions that have nothing to do with each
    other."""
    name_addr = collections.defaultdict(set)
    for d in decls:
        if d.addrs:
            name_addr[d.name].add(d.addrs[0])
    groups = collections.defaultdict(list)
    for d in decls:
        if d.addrs:
            key = ('addr', d.addrs[0])
        else:
            a = name_addr.get(d.name)
            key = ('addr', next(iter(a))) if a and len(a) == 1 else ('name', d.name)
        groups[key].append(d)
    return groups


def _crt_variadic_rows():
    """name -> (fixed count) for every variadic row of gen_link's CRT_PROTOS.

    Imported lazily: gen_link imports this module, so a module-level import
    here would be a cycle."""
    import gen_link as gl
    return {n: len(p[1]) for n, p in gl.CRT_PROTOS.items() if p[2]}


def truth_of(decls, crt_variadic):
    """(shape, why) the group's declarations have to agree with.

    In order: a DEFINITION in the tree, then libc's prototype for a CRT name in
    the group, then the majority of the declarations. The order is the point --
    PORT-A4 §1's lesson was that the alias's own spelling is never the authority
    when somebody else owns the body."""
    defs = [d for d in decls if d.defn]
    if defs:
        return defs[0].shape, f'the definition in {defs[0].where}'
    for d in decls:
        if d.name in crt_variadic:
            return (VARIADIC, crt_variadic[d.name]), \
                f"libc's prototype for `{d.name}` (gen_link.CRT_PROTOS)"
    votes = collections.Counter(d.shape for d in decls)
    shape, n = votes.most_common(1)[0]
    return shape, f'{n} of {len(decls)} declarations'


def classify(decl, truth, crt_variadic, group_names):
    """Why `decl` is wrong, as (code, one-line consequence)."""
    tkind, tfix = truth
    kind, nfix = decl.shape
    if kind == UNSPEC:
        return 'VA-UNSPEC', ('no prototype at all: the call site emits what it '
                             'is handed and builds no varargs buffer')
    if kind == VARIADIC:
        if tkind != VARIADIC:
            return 'VA-COUNT', ('declared variadic where the callee is not: '
                                'the buffer pointer is passed as a real '
                                'parameter')
        return 'VA-COUNT', (f'the `...` starts at parameter {nfix + 1}, the '
                            f'callee builds its buffer after {tfix}: every '
                            f'variable argument is one slot out')
    if tkind != VARIADIC:
        return 'VA-COUNT', f'{nfix} parameters where the callee has {tfix}'
    if nfix > tfix:
        extra = ', '.join(f'argument {i + 1}' for i in range(tfix, nfix))
        return 'VA-SLOT', (f'{extra} lands in the slot the callee reads as its '
                           f'va_list POINTER -- identical wasm signature '
                           f'{wasm_arity(decl.shape)} x i32, so no link gate '
                           f'can see it (PORT-M20)')
    crt = sorted(n for n in group_names if n in crt_variadic)
    if nfix == tfix and crt:
        return 'VA-EMPTY', (f'gen_link.crt_alias bridges this one (it declares '
                            f"libc's `{crt[0]}` and clang builds the empty "
                            f'buffer), but the declaration still denies the '
                            f'callee is variadic')
    return 'VA-EMPTY', ('the callee is variadic and this spelling is not: a '
                        'cast forwarder, which traps at the `call_indirect`')


Group = collections.namedtuple(
    'Group', 'key addr names decls truth why conflicts')


def census(src=SRC):
    """[Group] for every group whose truth is variadic or that disagrees about
    being variadic -- the population this gate is about, conflicts first.

    `conflicts` is [(Decl, code, consequence)]; a group with none is clean."""
    crt_variadic = _crt_variadic_rows()
    decls = scan_decls(src)
    out = []
    for key, rs in group_decls(decls).items():
        names = sorted({d.name for d in rs})
        truth, why = truth_of(rs, crt_variadic)
        touches_variadic = truth[0] == VARIADIC or \
            any(d.shape[0] == VARIADIC for d in rs) or \
            any(n in crt_variadic for n in names)
        if not touches_variadic:
            continue
        bad = []
        for d in rs:
            if d.shape != truth:
                bad.append((d,) + classify(d, truth, crt_variadic, names))
        out.append(Group(key, key[1] if key[0] == 'addr' else None, names, rs,
                         truth, why, bad))
    out.sort(key=lambda g: (not g.conflicts, str(g.key)))
    return out


def conflicts(src=SRC):
    return [g for g in census(src) if g.conflicts]


# ---- output -----------------------------------------------------------------

def _group_title(g):
    return f'0x{g.addr:08x}' if g.addr is not None else f'`{g.key[1]}` (no address cited)'


def report(groups, out=sys.stdout):
    """The refusal text: file:line and BOTH spellings for every conflict."""
    bad = [g for g in groups if g.conflicts]
    if not bad:
        return
    n = sum(len(g.conflicts) for g in bad)
    print(f'{n} variadic declaration conflict(s) in {len(bad)} function(s):\n',
          file=out)
    for g in bad:
        ok = [d for d in g.decls if d.shape == g.truth]
        print(f'{_group_title(g)}  {"/".join(g.names)}', file=out)
        print(f'    the callee is {shape_text(g.truth)} -- {g.why}', file=out)
        if ok:
            print(f'    agreed in {len(ok)} declaration(s), e.g. '
                  f'{ok[0].where}: {ok[0].text}', file=out)
        for d, code, why in g.conflicts:
            print(f'  {d.where}  {code}', file=out)
            print(f'      declared: {d.text}', file=out)
            print(f'      callee:   {d.name}{shape_text(g.truth)} '
                  f'({g.why})', file=out)
            print(f'      {why}', file=out)
        print('    FIX: keep the VC6 spelling and add the portable one in a '
              'guard arm --', file=out)
        print('         `#ifndef LEGOLAND_PORTABLE` <as shipped> `#else` '
              '<variadic> `#endif`', file=out)
        print('         then re-gate the file with tools/audit.py and '
              'tools/relocs.py. No bytes move:', file=out)
        print('         VC6 pushes the same dwords either way, which is why no '
              'byte gate can see this.\n', file=out)


def census_text(groups, out=sys.stdout):
    for g in groups:
        mark = 'CONFLICT' if g.conflicts else 'ok      '
        print(f'{mark} {_group_title(g):12s} {shape_text(g.truth):14s} '
              f'{len(g.decls):3d} decl(s)  {", ".join(g.names)}', file=out)


def markdown(groups):
    """The `## Variadic declarations` section of gen/manifest.md."""
    bad = [g for g in groups if g.conflicts]
    md = ['', '## Variadic declarations (tools/variadic_sweep.py)', '',
          'A variadic callee takes ONE extra wasm parameter -- the pointer to '
          'the buffer clang', 'writes the variable arguments into -- so a fixed '
          '`(char*, const char*, int)`',
          'declaration of `sprintf` has the SAME wasm signature as the real '
          '`(char*, const',
          'char*, ...)` and passes its third argument where the callee reads a '
          'va_list.',
          'wasm-ld, linkreport and name_trap are all blind to it; so are '
          'audit.py, relocs.py',
          'and verify.py, because VC6 emits the same code either way. PORT-M20 '
          'lost the Spider',
          "Ride's riders to it; PORT-A11 made it a gate.", '',
          f'- functions whose prototype is variadic somewhere in the tree: '
          f'{len(groups)}',
          f'- declarations of them: {sum(len(g.decls) for g in groups)}',
          f'- **conflicts: {sum(len(g.conflicts) for g in bad)}** in '
          f'{len(bad)} function(s)', '',
          '| address | names | callee | declarations | conflicts |',
          '| --- | --- | --- | --- | --- |']
    for g in groups:
        addr = f'`0x{g.addr:08x}`' if g.addr is not None else '(none cited)'
        md.append(f'| {addr} | {", ".join(f"`{n}`" for n in g.names)} | '
                  f'`{shape_text(g.truth)}` | {len(g.decls)} | '
                  f'{len(g.conflicts) or "-"} |')
    if bad:
        md += ['', 'The open rows, with both spellings:', '', '```']
        for g in bad:
            for d, code, why in g.conflicts:
                md.append(f'{d.where} {code} {d.text}')
                md.append(f'    callee is {d.name}{shape_text(g.truth)} '
                          f'-- {why}')
        md += ['```']
    else:
        md += ['', 'No conflicts: every declaration of every variadic function '
               'in the tree agrees', 'with the callee about where the `...` '
               'starts.']
    return md


# ---- selftest ---------------------------------------------------------------
# Positive and negative controls for the classifier AND for the two scanners,
# because a sweep whose reader quietly stops finding declarations is a gate that
# always passes.

_SELFTEST_CASES = [
    # (name, source, expected conflict codes)
    ('M20: the Spider Ride seat, exactly as mechrides.c had it',
     'extern int Format(char* dst, const char* fmt, ...);   /* 0x0049e573 */\n'
     'extern int sprintf_w(char* dst, const char* fmt, int v); /* 0x0049e573 */\n',
     ['VA-SLOT']),
    ('the same defect with no `extern` (audiomisc.c spells it this way)',
     'int sprintf(char*, const char*, ...);            /* 0x0049e573 */\n'
     'int sprintf_x(char*, const char*, int);          /* 0x0049e573 */\n',
     ['VA-SLOT']),
    ('a DEFINITION is the truth, even when every declaration disagrees',
     '// FUNCTION: LEGOLAND 0x0047f870\n'
     'void Log(const char* fmt, ...) {}\n'
     'extern void Log2(const char* fmt, char* name);  /* 0x0047f870 */\n',
     ['VA-SLOT']),
    ('the reverse direction: one variadic spelling, the rest fixed',
     'extern void Plain(int a, int b);      /* 0x00401000 */\n'
     'extern void Plain2(int a, ...);       /* 0x00401000 */\n'
     'extern void Plain3(int a, int b);     /* 0x00401000 */\n',
     ['VA-COUNT']),
    ('a different fixed count before the `...`',
     'extern int Fmt(char* d, const char* f, ...);   /* 0x0049e573 */\n'
     'extern int Fmt2(char* d, ...);                 /* 0x0049e573 */\n',
     ['VA-COUNT']),
    ('no prototype at all',
     'extern int Fmt(char* d, const char* f, ...);   /* 0x0049e573 */\n'
     'extern int Fmt3();                             /* 0x0049e573 */\n',
     ['VA-UNSPEC']),
    ('A4 §1: the CRT alias gen_link bridges is reported, not hidden',
     'extern int printf(const char* fmt, ...);  /* 0x0049e5c5 */\n'
     'extern void DebugPrint(const char* msg);  /* 0x0049e5c5 */\n',
     ['VA-EMPTY']),
    # ---- negatives: every one of these used to be, or could be, a phantom --
    ('agreement is not a conflict',
     'extern int Format(char* d, const char* f, ...);  /* 0x0049e573 */\n'
     'extern int sprintf(char* d, const char* f, ...); /* 0x0049e573 */\n',
     []),
    ('the LEGOLAND_PORTABLE arm is the fix, not a hit',
     'extern int Format(char* d, const char* f, ...);  /* 0x0049e573 */\n'
     '#ifndef LEGOLAND_PORTABLE\n'
     'extern int sprintf_w(char* d, const char* f, int v); /* 0x0049e573 */\n'
     '#else\n'
     'extern int sprintf_w(char* d, const char* f, ...);   /* 0x0049e573 */\n'
     '#endif\n',
     []),
    ('`(void)` is zero parameters, not an unspecified list',
     '// FUNCTION: LEGOLAND 0x00401000\n'
     'void Tick(void) {}\n'
     'extern void Tick2(void);   /* 0x00401000 */\n',
     []),
    ('two addresses, two functions: no group, no conflict',
     'extern int Fmt(char* d, const char* f, ...);  /* 0x0049e573 */\n'
     'extern int Other(char* d, const char* f, int v); /* 0x00401000 */\n',
     []),
    ('a prototype INSIDE a body is pass A\'s, and agrees',
     'extern int Fmt(char* d, const char* f, ...);  /* 0x0049e573 */\n'
     'void Caller(void) {\n'
     '    extern int Fmt2(char* d, const char* f, ...);  /* 0x0049e573 */\n'
     '    Fmt2(0, 0);\n'
     '}\n',
     []),
    ('a function POINTER parameter is not a declarator of its own',
     'extern void Sort(int (*cmp)(const void*, const void*), int n); '
     '/* 0x00401000 */\n'
     'extern void Sort2(int (*cmp)(const void*, const void*), int n); '
     '/* 0x00401000 */\n',
     []),
    ('a struct definition at file scope is not a prototype',
     'typedef struct P { int x; int y; } P;\n'
     'extern int Fmt(char* d, const char* f, ...);  /* 0x0049e573 */\n'
     'extern int Fmt2(char* d, const char* f, ...); /* 0x0049e573 */\n',
     []),
    ('`...` in prose, and the word extern in prose',
     '/* the extern below takes a format and ... whatever follows, at '
     '0x0049e573 */\n'
     'extern int Fmt(char* d, const char* f, ...);  /* 0x0049e573 */\n'
     'extern int Fmt2(char* d, const char* f, ...); /* 0x0049e573 */\n',
     []),
    ('a macro body is not a declaration',
     '#define LOG(x) Fmt(buf, "%s", x)\n'
     'extern int Fmt(char* d, const char* f, ...);  /* 0x0049e573 */\n'
     'extern int Fmt2(char* d, const char* f, ...); /* 0x0049e573 */\n',
     []),
    # the two phantoms this sweep reported before its reader was right. Both
    # were loud -- one filed 34 honest declarations as conflicts -- so both are
    # controls now.
    ("PORT-M3's `_vc6_body` rename: the VC6 body is not the callee",
     '///// FILE: sweep1.c\n'
     '#ifdef LEGOLAND_PORTABLE\n'
     '#define Log Log_vc6_body\n'
     '#endif\n'
     '// FUNCTION: LEGOLAND 0x00453a20\n'
     'void Log(void)\n'
     '{\n'
     '}\n'
     '#ifdef LEGOLAND_PORTABLE\n'
     '#undef Log\n'
     'void Log(const char* fmt, ...) { (void)fmt; }\n'
     '#endif\n'
     '///// FILE: caller.c\n'
     'extern void Log(const char* fmt, ...);   /* 0x00453a20 */\n',
     []),
    ('an address in the prose ABOVE a declaration is not its address',
     '/* 0x00420640 LoadLmsModel calls this; see the CODEGEN note. */\n'
     'int Fmt(char* d, const char* f, ...);\n'
     '// FUNCTION: LEGOLAND 0x00420640\n'
     'void* LoadLmsModel(const char* name)\n'
     '{\n'
     '    return 0;\n'
     '}\n',
     []),
]


def selftest():
    import tempfile
    fails = []
    root = tempfile.mkdtemp()
    for i, (title, text, want) in enumerate(_SELFTEST_CASES):
        sub = os.path.join(root, f'case{i:02d}')
        os.makedirs(sub, exist_ok=True)
        # `///// FILE: name.c` splits a case across translation units, which the
        # `_vc6_body` control needs: the rename is in force in the file that
        # owns the body and nowhere else.
        if text.startswith('///// FILE:'):
            for part in text.split('///// FILE:')[1:]:
                head, _nl, rest = part.partition('\n')
                with open(os.path.join(sub, head.strip()), 'w') as f:
                    f.write(rest)
        else:
            with open(os.path.join(sub, 'case.c'), 'w') as f:
                f.write(text)
        got = []
        for g in census(sub):
            got += [code for _d, code, _w in g.conflicts]
        if sorted(got) != sorted(want):
            fails.append(f'{title}: got {sorted(got)}, want {sorted(want)}')
    # the arity claim itself: a VA-SLOT row's two spellings really do have the
    # same wasm signature, which is the whole reason no link gate sees it
    if wasm_arity((FIXED, 3)) != wasm_arity((VARIADIC, 2)):
        fails.append('a 3-fixed declaration and a 2-fixed variadic one must '
                     'have the same wasm arity')
    # and the scanner has to keep finding the real tree's declarations: a reader
    # that stops working is a gate that always passes
    if os.path.isdir(SRC):
        decls = scan_decls()
        nvar = sum(1 for d in decls if d.shape[0] == VARIADIC)
        if len(decls) < 4000 or nvar < 50:
            fails.append(f'the sources scan looks broken: {len(decls)} '
                         f'declarations, {nvar} variadic (expected >4000 / >50)')
        if not any(d.defn for d in decls):
            fails.append('no DEFINITION found in LEGOLAND/*.c: pass B is broken')
    for f in fails:
        print(f'FAIL {f}')
    print(f'variadic_sweep selftest: {len(fails)} failure(s)')
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--src', default=SRC, help='the directory to sweep')
    ap.add_argument('--selftest', action='store_true',
                    help='the shapes, positive and negative; no sources needed')
    ap.add_argument('--census', action='store_true',
                    help='every variadic function and its declarations')
    ap.add_argument('--markdown', action='store_true',
                    help="the manifest section gen_link.py embeds")
    ap.add_argument('--quiet', action='store_true',
                    help='exit status and the one-line verdict only')
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    groups = census(args.src)
    if args.markdown:
        print('\n'.join(markdown(groups)))
        return 1 if any(g.conflicts for g in groups) else 0
    if args.census and not args.quiet:
        census_text(groups)
        print()
    report(groups)
    n = sum(len(g.conflicts) for g in groups)
    print(f'variadic_sweep gate: {n} conflict(s) in '
          f'{len(groups)} variadic function(s), '
          f'{sum(len(g.decls) for g in groups)} declaration(s)')
    return 1 if n else 0


if __name__ == '__main__':
    sys.exit(main())
