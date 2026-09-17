#!/usr/bin/env python3
"""A global read through a pointer wider than the block gen_link emits for it.

The portable closure (`gen_link.py`) sizes every extern-only global by the
extent its DECLARATIONS give it -- `sizeof` of the widest declared type, merged
with the named addresses inside that extent -- and emits each block
`aligned(16)`. A read of a global through a pointer to a wider type than any
declaration gives it runs past its block: into alignment padding (zero) or into
whatever the linker placed next. On x86 the same read took the next bytes of the
ORIGINAL image, which is what the code meant.

Found on game level 1 (2026-09-15). goalstate.c declared

    extern int  g_entrance_x;      /* 0x004b8320 */

and BlokeAction_LeavePark routed every leaving visitor with
`SuggestNextMove(&b->world, (Pos*)&g_entrance_x, &out)`. mapobj.c and savegame.c
write the other half as `g_entrance_y` (0x004b8324), so the closure had two
4-byte blocks, `to->y` read padding, and every leaver walked to map row 0 before
turning back for the gate. `extern Pos g_entrance_x` (a88d0c2e) makes it one
8-byte block with g_entrance_y an interior alias.

Nothing else in the tree can see this. VC6 emits the same bytes for either
declaration, so audit.py, relocs.py, match.py and verify.py are silent in both
states; `cdecl.py` sizes objects from declarations, and a cast is not one; and
wasm-ld links two well-formed objects. Like extern_sweep, addr_sweep,
bvstruct_sweep and variadic_sweep, a PORTABLE-BUILD-ONLY class with no other
witness.

## What it checks

Every expression in LEGOLAND/*.c that takes the address of a global -- `&g_x`,
`&g_x.f`, `&g_x[3]` -- and every read made through the resulting pointer:

  * dereferenced after a cast: `*(T*)&g_x`, `((T*)&g_x)->f`, `((T*)&g_x)[k]`;
  * indexed as its own type: `(&g_x)[k]`, `*(&g_x + k)` -- element k;
  * passed to a function: what the callee's DEFINITION reads through that
    parameter, followed through the calls it makes; when the tree has no body,
    `sizeof` of the pointee the calling file's prototype declares;
  * stored in a local pointer: every read through that local afterwards;
  * parked in a global pointer: every read through that global, anywhere in
    the tree -- uimisc.c parks `(Icon*)&g_side_icons` in `g_side_icon_tail`,
    which is only ever read at `->next`.

Each read is laid against the block gen_link emits at the global's address,
computed by gen_link's own layout functions (`address_map`, `merge_interiors`,
`planned_blocks`) over the same sources -- or, with `--closure`, read out of a
generated globals.c. A read that ends past the block, or starts before it, is a
row.

Following definitions instead of prototypes is what separates the class from a
cast that only LOOKS wide. coaster.c hands `(TrackNode*)&g_track_desc_flat` to
TrackFitCheck, whose prototype says `TrackNode*`; the body reads only
`n->state` (+0) and passes the pointer on to TrackFitCheckChain/Span, whose
definitions (coaster5.c) take `TrackDesc*`. Every byte really read is inside the
descriptor's block, so it is not a row.

Not the class, and not reported:

  * an address INTO a pointer's target -- `(MapCell*)&g_map_rows[y][x]`,
    `(Footprint*)&g_edit_object->rect` -- which is heap or table memory, not the
    global's own block;
  * the `#ifndef LEGOLAND_PORTABLE` arm. The sweep reads what the portable build
    compiles and expands the object-like macros those arms define: misc3.c's and
    fpui3.c's `g_query_block` is `(*(QueryBlock*)&g_query_cursor.raw[0x1404])`,
    checked as 8 bytes at +0x1404 of the query cursor's block.

Limits. A length known only at run time (`memcpy(&g_x, src, n)`, `p[i]`,
`p++`) counts one element. A global the address is parked in is followed by its
NAME; when nothing reads it under that name but other names share its storage,
or when the pointer goes to a function pointer or a function-like macro, the
read counts `sizeof` of the pointer's static pointee. A record whose layout
cdecl cannot complete (coaster.c's CoasterRec) leaves its reads unsized: those
sites are counted in the verdict line, not judged.

## The fix, always

Declare the object as the type it is read as -- in the file that reads it, or by
making the narrow spelling a view of the wide one. gen_link sizes the block by
the widest declaration, and the named neighbours inside it become interior
aliases. No bytes move, so re-gate the file with tools/audit.py and
tools/relocs.py.

Usage:

    python3 tools/cast_extent_sweep.py                   # sweep LEGOLAND/
    python3 tools/cast_extent_sweep.py --census          # every site
    python3 tools/cast_extent_sweep.py --baseline FILE   # the ctest gate
    python3 tools/cast_extent_sweep.py --closure DIR/gen/globals.c
    python3 tools/cast_extent_sweep.py --selftest

Exit status is the gate: 0 when every row is accepted by the baseline (no
baseline: when there is no row), 1 otherwise. A baseline row that is no longer
reported prints FIXED and passes. Sources only unless `--closure` is given -- no
gamedata/, no image, no build products -- so it runs in CI on both toolchains.
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
import cdecl                       # noqa: E402
import bvstruct_sweep as bvs       # noqa: E402
import gen_link as gl              # noqa: E402

SRC = lr.SRC


# ---- the emitted layout --------------------------------------------------------

class Layout:
    """Where the portable link puts each data address: the block containing it.

    `addr_of` is the ONE address gen_link gives each name -- the first
    declaration in `lr.scan_sources` order. A name declared at two addresses is
    addr_sweep.py's class, and gated there."""

    def __init__(self, blocks, addr_of, how):
        self.blocks = blocks            # start -> (size, primary name)
        self.starts = sorted(blocks)
        self.addr_of = addr_of          # data name -> address
        self.how = how
        self._names_in = None           # block start -> [names inside it]

    def other_names(self, name):
        """The names other than `name` for any byte of the block `name` is in
        -- ['?'] when it has no block, whose storage nothing here describes."""
        block = self.block_at(self.addr_of.get(name, -1))
        if block is None:
            return ['?']
        if self._names_in is None:
            self._names_in = collections.defaultdict(list)
            for n, a in self.addr_of.items():
                b = self.block_at(a)
                if b is not None:
                    self._names_in[b[0]].append(n)
        return [n for n in self._names_in[block[0]] if n != name]

    def block_at(self, addr):
        """(start, size, primary) of the block that contains `addr`, or None."""
        i = bisect.bisect_right(self.starts, addr) - 1
        if i < 0:
            return None
        start = self.starts[i]
        size, primary = self.blocks[start]
        return (start, size, primary) if addr < start + size else None


def sources_layout(src, referenced, defined=frozenset(), raw_tus=None):
    """The blocks gen_link would emit for `src`, from gen_link's own functions.

    Two facts gen_link reads out of the BUILD are read out of the sources here:
    `referenced` -- data names some live code uses, which stand in for the
    game objects' undefined symbols -- and `defined`, data names a translation
    unit defines rather than declares (none, today). `raw_tus` is cdecl's parse
    of the files as written, both arms of every conditional, because that is
    the parse gen_link sizes objects with."""
    externs, defined_at, _stubs = lr.scan_sources(src)
    data_names, known, next_addr = gl.address_map(externs, defined_at)
    if raw_tus is None:
        tus, headers = cdecl.parse_tree(src)
        raw_tus = tus + list(headers.values())
    src_ext = cdecl.scan(src, raw_tus)
    decls = gl.scan_pointer_decls(src)
    win32 = lr.load_win32()
    # lr.classify's 'game-data' row: referenced, undefined, not a host import
    # and not a CRT name
    missing = {n for n, (_a, kind) in externs.items()
               if kind == 'data' and n in referenced and n not in defined
               and n not in win32 and not lr.is_crt(n)}

    def declared_extent(addr):
        return gl.declared_extent_at(addr, src_ext, decls, data_names)

    absorbed, _inter, host_end, _log = gl.merge_interiors(
        data_names, known, next_addr, declared_extent, defined, missing)
    planned, _cross = gl.planned_blocks(data_names, absorbed, host_end,
                                        next_addr, defined, missing)
    blocks = {addr: (size, primary) for addr, primary, size, _r in planned}
    addr_of = {n: a for n, (a, kind) in externs.items() if kind == 'data'}
    return Layout(blocks, addr_of, 'sources')


# gen_link's emitter writes one header comment per block, then the definition:
#   /* 0x004b8320 .data 8 bytes (+other) interior: g_entrance_y+0x4 */
#   __attribute__((aligned(16))) unsigned int g_entrance_x[2] = { ...
_BLOCK_HEAD = re.compile(
    r'^/\* (0x[0-9a-f]{8}) \S+ (\d+) bytes(?: \(\+([^)]*)\))?'
    r'(?: interior: (.*?))? \*/$')
_BLOCK_DEF = re.compile(r'\bunsigned (?:char|int) ([A-Za-z_]\w*)\[')


def closure_layout(path):
    """The blocks a GENERATED globals.c really has -- the ground truth the
    sources layout stands in for, when a build is at hand."""
    blocks, addr_of = {}, {}
    lines = open(path, encoding='utf-8', errors='replace').read().split('\n')
    for i, ln in enumerate(lines):
        m = _BLOCK_HEAD.match(ln)
        if not m:
            continue
        addr, size = int(m.group(1), 16), int(m.group(2))
        d = _BLOCK_DEF.search(lines[i + 1]) if i + 1 < len(lines) else None
        primary = d.group(1) if d else f'?{addr:08x}'
        blocks[addr] = (size, primary)
        addr_of[primary] = addr
        for n in (m.group(3) or '').split(','):
            if n.strip():
                addr_of[n.strip()] = addr
        for item in (m.group(4) or '').split(','):
            if '+0x' in item:
                n, off = item.strip().rsplit('+0x', 1)
                addr_of[n] = addr + int(off, 16)
    return Layout(blocks, addr_of, path)


# ---- the portable view of a file ---------------------------------------------

def live_text(text):
    """(text, live lines): the file as the portable build compiles it. Every
    line of a dead `LEGOLAND_PORTABLE` arm is blanked, so line numbers still
    match the file. `bvstruct_sweep.portable_lines` decides which arm is live --
    the reader every portable-aware sweep in the tree shares, and the reason a
    fix written as a portable arm is not reported as a fresh hit."""
    live = bvs.portable_lines(text)
    return '\n'.join(ln if i in live else ''
                     for i, ln in enumerate(text.split('\n'), 1)), live


_DEFINE = re.compile(r'^\s*#\s*define\s+([A-Za-z_]\w*)(?![\w(])[ \t]*(.*)$')
_UNDEF = re.compile(r'^\s*#\s*undef\s+([A-Za-z_]\w*)')
_COMMENTS = re.compile(r'/\*.*?\*/|//[^\n]*', re.S)
MAX_EXPANSION = 8


def macro_table(text):
    """name -> [(line, body tokens or None)] for every OBJECT-like `#define` and
    every `#undef` on a live line, in file order. Function-like macros are not
    expanded: a call of one is an unknown callee, the conservative reading."""
    table = collections.defaultdict(list)
    lines = text.split('\n')
    i = 0
    while i < len(lines):
        ln, first = lines[i], i + 1
        while ln.endswith('\\') and i + 1 < len(lines):
            i += 1
            ln = ln[:-1] + ' ' + lines[i]
        i += 1
        m = _DEFINE.match(ln)
        if m:
            body = _COMMENTS.sub(' ', m.group(2)).split('/*')[0]
            toks = [(k, t, first) for k, t, _l in cdecl.Tokens(body, '#define').toks]
            table[m.group(1)].append((first, toks))
            continue
        m = _UNDEF.match(ln)
        if m:
            table[m.group(1)].append((first, None))
    return table


def macro_at(table, name, line):
    """(define line, body tokens) of `name` in force at `line`, or None."""
    hit = None
    for ln, body in table.get(name, ()):
        if ln >= line:
            break
        hit = None if body is None else (ln, body)
    return hit


def expand(toks, table, _depth=0, _active=()):
    """`toks` with every object-like macro in force replaced by its body.

    Tokens come out as (kind, text, line, via): `line` is the USE site's, and
    `via` is (macro, define line) for a token an expansion produced -- so a row
    found inside `g_query_block` can say that it came from fpui3.c:269."""
    out = []
    for t in toks:
        kind, text, line = t[0], t[1], t[2]
        via = t[3] if len(t) > 3 else None
        hit = macro_at(table, text, line) \
            if kind == 'id' and text not in _active else None
        if hit is None or _depth >= MAX_EXPANSION:
            out.append((kind, text, line, via))
            continue
        defline, body = hit
        inner = [(k, x, line, via or (text, defline)) for k, x, _l in body]
        out += expand(inner, table, _depth + 1, _active + (text,))
    return out


def expr_text(toks):
    """Tokens back into compact C: `(Pos*)&g_entrance_x`, not `( Pos * ) & ...`."""
    out = ''
    for t in toks:
        x = t[1]
        if out and (out[-1].isalnum() or out[-1] == '_') and \
                (x[0].isalnum() or x[0] == '_'):
            out += ' '
        out += x
    return out


# ---- types ---------------------------------------------------------------------
# A read's width needs what a pointer POINTS AT, and cdecl collapses every
# pointer to one 4-byte PTR. So a type here is a small tree over cdecl's sized
# `Type`s:
#
#   ('t', Type)            a cdecl type: prim, struct, union, enum, array, PTR
#   ('ptr', td)            pointer to td
#   ('arr', n, td)         n of td (n None: an unbounded `[]`)
#   ('fn', td, params)     function returning td; params is its token list

def size_of(td):
    if td is None:
        return None
    if td[0] == 't':
        return td[1].size
    if td[0] == 'ptr':
        return 4
    if td[0] == 'arr':
        es = size_of(td[2])
        return None if td[1] is None or es is None else td[1] * es
    return None


def is_pointer(td):
    return td is not None and (td[0] == 'ptr' or
                               (td[0] == 't' and td[1].kind == 'ptr'))


def element(td):
    """The element type of an ARRAY (subscripting stays inside the object)."""
    if td is None:
        return None
    if td[0] == 'arr':
        return td[2]
    if td[0] == 't' and td[1].kind == 'array' and td[1].fields:
        return ('t', td[1].fields[0][2])
    return None


def pointee(td):
    """What a pointer -- or an array, decayed -- points at; None if unknown
    (a pointer typedef cdecl collapsed, `LPVOID`)."""
    if td is not None and td[0] == 'ptr':
        return td[1]
    return element(td)


def field_of(td, name, _base=0, _depth=0):
    """(offset, type) of member `name`, looking through anonymous members.

    Only a COMPLETE layout answers: cdecl keeps the fields of a record it
    could not size, but the offsets after the unsized member are wrong."""
    if td is None or td[0] != 't' or _depth > 8 or \
            td[1].kind not in ('struct', 'union'):
        return None
    for nm, off, fty, count in td[1].fields:
        if nm == name:
            ftd = ('t', fty) if count == 1 else ('arr', count, ('t', fty))
            return _base + off, ftd
    for nm, off, fty, _count in td[1].fields:
        if nm is None:
            hit = field_of(('t', fty), name, _base + off, _depth + 1)
            if hit:
                return hit
    return None


def type_text(td):
    if td is None:
        return '?'
    if td[0] == 't':
        return td[1].name
    if td[0] == 'ptr':
        return type_text(td[1]) + '*'
    if td[0] == 'arr':
        return f'{type_text(td[2])}[{"" if td[1] is None else td[1]}]'
    return type_text(td[1]) + '()'


class _Decl(cdecl.TU):
    """cdecl's declaration parser over any token list, with one TU's type names
    in scope: the parameter lists, locals and casts its file-scope pass never
    visits. One change: a function declarator keeps its parameter tokens."""

    def __init__(self, tu, toks):                  # noqa -- no file to parse
        self.path, self.file, self.tk = tu.path, tu.file, tu.tk
        self.toks, self.i = [t[:3] for t in toks], 0     # cdecl's (kind, text, line)
        self.tags, self.typedefs, self.shared = tu.tags, tu.typedefs, tu.shared
        self.objects, self.functions, self.notes = [], [], []

    def _direct_decl_tree(self):
        node = ('name', None)
        if self.txt() == '(':
            self.i += 1
            node = self._decl_tree()
            if self.txt() == ')':
                self.i += 1
        elif self.tok()[0] == 'id':
            node = ('name', self.txt())
            self.i += 1
        while True:
            if self.txt() == '[':
                node = ('arr', self._dim(), node)
            elif self.txt() == '(':
                start = self.i
                self.skip_balanced('(', ')')
                node = ('fn', node, self.toks[start + 1:self.i - 1])
            else:
                return node


def eval_tree(node, td):
    """(name, type) of a declarator tree over the base type `td`, by C's rule:
    `* D` gives D a pointer to td, `D[n]` an array of td, `D(...)` a function."""
    while node[0] != 'name':
        if node[0] == 'ptr':
            td, node = ('ptr', td), node[1]
        elif node[0] == 'arr':
            td, node = ('arr', node[1], td), node[2]
        else:
            td, node = ('fn', td, node[2] if len(node) > 2 else []), node[1]
    return node[1], td


TYPE_WORDS = cdecl.SPEC_KEYWORDS | cdecl.QUALIFIERS | cdecl.STORAGE | \
    {'struct', 'union', 'enum', '__declspec'}


def starts_declaration(tu, toks, i):
    """Does a declaration start at toks[i]? A type keyword, or a type name
    followed by what a declarator starts with (or by nothing: a cast)."""
    if i >= len(toks) or toks[i][0] != 'id':
        return False
    if toks[i][1] in TYPE_WORDS:
        return True
    if tu.lookup_typedef(toks[i][1]) is None:
        return False
    if i + 1 >= len(toks):
        return True
    return toks[i + 1][1] in ('*', '(', ')') or toks[i + 1][0] == 'id'


def _complete(tu, ty):
    """A record typedef'd before its body -- `typedef struct TrackDesc
    TrackDesc;` above `struct TrackDesc { ... };`, which is how coaster.c spells
    its track records -- completed from the tag the file defines later. cdecl
    froze the typedef as unsized when it read it."""
    if ty.kind == 'unknown' and ty.size is None and \
            ty.name.startswith(('struct ', 'union ')):
        kw, _sp, tag = ty.name.partition(' ')
        done = tu.lookup_tag(kw, tag)
        if done is not None and done.size is not None:
            return done
    return ty


def parse_declaration(tu, toks):
    """(storage, [(name, type)]) for one declaration's tokens without its `;`,
    initialisers skipped; None when no type starts it. `storage` is 'typedef',
    'extern', 'static' or ''."""
    words = {t[1] for t in toks if t[0] == 'id'}
    p = _Decl(tu, list(toks))
    base, _name, is_typedef, is_extern = p._specifier()
    if base is None:
        return None
    base = _complete(tu, base)
    out = []
    while not p.at_end():
        start = p.i
        name, td = eval_tree(p._decl_tree(), ('t', base))
        out.append((name, td))
        if p.txt() == '=':
            depth = 0
            while not p.at_end():
                x = p.txt()
                if x in ('(', '[', '{'):
                    depth += 1
                elif x in (')', ']', '}'):
                    depth -= 1
                elif x == ',' and depth == 0:
                    break
                p.i += 1
        if p.txt() == ',' and p.i > start:
            p.i += 1
            continue
        break
    storage = 'typedef' if is_typedef else 'extern' if is_extern else \
        'static' if 'static' in words else ''
    return storage, out


def parse_params(tu, toks):
    """([(name, type)], variadic) for a parameter list's tokens. An array or a
    function parameter is the pointer C makes it."""
    parts, cur, depth = [], [], 0
    for t in toks:
        if t[1] in ('(', '['):
            depth += 1
        elif t[1] in (')', ']'):
            depth -= 1
        if t[1] == ',' and depth == 0:
            parts.append(cur)
            cur = []
            continue
        cur.append(t)
    if cur:
        parts.append(cur)
    params, variadic = [], False
    for part in parts:
        texts = [t[1] for t in part]
        if texts == ['...']:
            variadic = True
            continue
        if texts == ['void']:
            continue
        got = parse_declaration(tu, part)
        if not got or not got[1]:
            params.append((None, None))
            continue
        name, td = got[1][0]
        if td[0] == 'arr':
            td = ('ptr', td[2])
        elif td[0] == 'fn':
            td = ('ptr', td)
        params.append((name, td))
    return params, variadic


# ---- one file, as the portable build compiles it --------------------------------

Proto = collections.namedtuple('Proto', 'name line params variadic addr')
Defn = collections.namedtuple('Defn', 'unit name line params variadic body')


def _skip_braces(toks, i):
    """Index just past the `}` that matches the `{` at toks[i]."""
    depth = 0
    while i < len(toks):
        if toks[i][1] == '{':
            depth += 1
        elif toks[i][1] == '}':
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return len(toks)


class Unit:
    """One LEGOLAND file in its portable arm: types (cdecl's parse of the live
    text), object-like macros, prototypes with their address comments,
    function bodies (macro-expanded), and file-scope objects."""

    def __init__(self, path, shared_tu=None):
        self.path = path
        self.file = os.path.basename(path)
        raw = open(path, encoding='utf-8', errors='replace').read()
        text, _live = live_text(raw)
        self.macros = macro_table(text)
        self.tu = cdecl.TU(path, shared=shared_tu, text=text)
        self.protos = collections.defaultdict(list)     # name -> [Proto]
        self.defns = {}                                  # name -> Defn
        self.globals = {}                                # object name -> type
        self.statics = set()                             # file-scope statics
        self.defines = set()                             # objects defined here
        self.refs = set()                                # identifiers code uses
        self._walk()

    def _addr(self, line0, line1):
        for ln in range(line0, line1 + 1):
            if self.tu.tk.addr_at.get(ln):
                return self.tu.tk.addr_at[ln][0]
        return None

    def _walk(self):
        """Statement by statement at file scope. A `{` right after a `)` is a
        function body; any other `{` (a record, an initialiser) belongs to the
        statement it is in."""
        toks = self.tu.toks
        n, i = len(toks), 0
        while i < n:
            start, depth, body = i, 0, None
            while i < n:
                x = toks[i][1]
                if x in ('(', '['):
                    depth += 1
                elif x in (')', ']'):
                    depth -= 1
                elif x == '{' and depth <= 0:
                    end = _skip_braces(toks, i)
                    if i > start and toks[i - 1][1] == ')':
                        body = (i, end)
                        break
                    i = end
                    continue
                elif x == ';' and depth <= 0:
                    break
                i += 1
            if body is not None:
                self._statement(toks[start:body[0]], toks[body[0]:body[1]])
                i = body[1]
            else:
                self._statement(toks[start:i], None)
                i += 1

    def _statement(self, stmt, body):
        if not stmt:
            return
        stmt = expand(stmt, self.macros)
        texts = [t[1] for t in stmt]
        if body is None and '=' in texts:
            self.refs.update(t[1] for t in stmt[texts.index('='):]
                             if t[0] == 'id')
        if not starts_declaration(self.tu, stmt, 0):
            return
        got = parse_declaration(self.tu, stmt)
        if not got or got[0] == 'typedef':
            return
        storage, decls = got
        addr = self._addr(stmt[0][2], stmt[-1][2])
        for name, td in decls:
            if not name:
                continue
            if td[0] == 'fn':
                params, variadic = parse_params(self.tu, td[2])
                if body is None:
                    self.protos[name].append(
                        Proto(name, stmt[0][2], params, variadic, addr))
                else:
                    exp = expand(body, self.macros)
                    self.refs.update(t[1] for t in exp if t[0] == 'id')
                    self.defns[name] = Defn(self, name, stmt[0][2], params,
                                            variadic, exp)
            elif storage == 'static':
                self.statics.add(name)
            else:
                self.globals[name] = td
                if storage != 'extern':
                    self.defines.add(name)


class Tree:
    """Every .c file of `src` (and legoland.h, the one shared header), and the
    lookups that cross files."""

    def __init__(self, src):
        self.src = src
        hdr = os.path.join(src, 'legoland.h')
        self.shared = Unit(hdr) if os.path.exists(hdr) else None
        shared_tu = self.shared.tu if self.shared else None
        self.units = [Unit(p, shared_tu)
                      for p in sorted(glob.glob(os.path.join(src, '*.c')))]
        self.by_file = {u.file: u for u in self.units}
        self.defs_by_name = collections.defaultdict(list)
        for u in self.units:
            for d in u.defns.values():
                self.defs_by_name[d.name].append(d)
        _externs, self.defined_at, _stubs = lr.scan_sources(src)
        self.referenced = set()
        self.defined = set()
        for u in self.units:
            self.referenced |= u.refs
            self.defined |= u.defines

    def definition(self, unit, name, local_protos=()):
        """The body a call of `name` from `unit` reaches, or None: the file's
        own definition; else the definition at the address the file's
        prototype cites (`// FUNCTION:` markers, linkreport's reading); else
        the only definition of that name in the tree."""
        if name in unit.defns:
            return unit.defns[name]
        for p in list(local_protos) + list(reversed(unit.protos.get(name, ()))):
            if p.addr is not None and p.addr in self.defined_at:
                dname, dfile = self.defined_at[p.addr]
                du = self.by_file.get(dfile)
                if du is not None and dname in du.defns:
                    return du.defns[dname]
        cands = self.defs_by_name.get(name, ())
        return cands[0] if len(cands) == 1 else None

    def prototype(self, unit, name, local_protos=()):
        ps = list(local_protos) or unit.protos.get(name) or \
            (self.shared.protos.get(name) if self.shared else None)
        return ps[-1] if ps else None


# ---- expressions ------------------------------------------------------------------

KEYWORDS = {'if', 'while', 'for', 'switch', 'return', 'sizeof', 'case', 'do',
            'else', 'goto', 'break', 'continue', 'default'}


def _match_back(B, j):
    """Index of the `(` or `[` that opens the bracket closing at B[j]."""
    close = B[j][1]
    opener = '(' if close == ')' else '['
    depth = 0
    while j >= 0:
        if B[j][1] == close:
            depth += 1
        elif B[j][1] == opener:
            depth -= 1
            if depth == 0:
                return j
        j -= 1
    return -1


def _match_fwd(B, j):
    """Index just past the bracket that closes the one opening at B[j]."""
    opener = B[j][1]
    close = {'(': ')', '[': ']', '{': '}'}[opener]
    depth = 0
    while j < len(B):
        if B[j][1] == opener:
            depth += 1
        elif B[j][1] == close:
            depth -= 1
            if depth == 0:
                return j + 1
        j += 1
    return len(B)


def _statement_end(B, j):
    """Index of the `;` (or brace) that ends the statement B[j] is in."""
    depth = 0
    while j < len(B):
        x = B[j][1]
        if x in ('(', '['):
            depth += 1
        elif x in (')', ']'):
            depth -= 1
        elif x in (';', '{', '}') and depth <= 0:
            return j
        j += 1
    return len(B)


def _def_block(B, k):
    """[start, end) of the code in which a later assignment to the variable
    assigned at B[k] replaces this value on every path: the braces around the
    assignment -- or its statement alone, when an if/else/loop controls it
    without braces (`if (!n) n = (TrackNode*)&g_track_desc_flat;`, or a `for`
    header). An assignment in a sibling branch replaces nothing."""
    j, depth = k, 0
    while j > 0:
        x = B[j - 1][1]
        if x in (')', ']'):
            depth += 1
        elif x in ('(', '['):
            depth = depth - 1 if depth else 0
        elif depth == 0 and x in (';', '{', '}'):
            break
        j -= 1
    if j < len(B) and B[j][1] in ('if', 'else', 'while', 'for', 'do'):
        return j, _statement_end(B, k) + 1
    opener, depth = j - 1, 0
    while opener >= 0:
        x = B[opener][1]
        if x == '}':
            depth += 1
        elif x == '{':
            if depth == 0:
                return opener, _match_fwd(B, opener)
            depth -= 1
        opener -= 1
    return 0, len(B)


def _in_for_header(B, i):
    """Is B[i] inside a `for (...)` header -- where `e += 2` runs after the
    body, not before it?"""
    j, depth = i - 1, 0
    while j >= 0:
        x = B[j][1]
        if x == ')':
            depth += 1
        elif x == '(':
            if depth == 0:
                return j > 0 and B[j - 1][1] == 'for'
            depth -= 1
        elif x in ('{', '}'):
            return False
        j -= 1
    return False


def const_value(toks):
    """The value of an integer constant expression of literals, or None."""
    expr = ''
    for t in toks:
        x = t[1]
        if t[0] == 'num':
            x = x.rstrip('uUlL')
            try:
                v = int(x, 16) if x[:2] in ('0x', '0X') else \
                    int(x, 8) if len(x) > 1 and x[0] == '0' else int(x)
            except ValueError:
                return None
            expr += str(v)
        elif x in ('+', '-', '*', '(', ')', '<<', '>>', '|', '&'):
            expr += x
        else:
            return None
    if not expr:
        return None
    try:
        v = eval(expr, {'__builtins__': {}}, {})      # literals and operators
    except Exception:
        return None
    return v if isinstance(v, int) else None


def cast_type(unit, B, close):
    """(type, index of its `(`) when the `)` at B[close] ends a cast."""
    if close < 0 or B[close][1] != ')':
        return None
    opener = _match_back(B, close)
    if opener < 0:
        return None
    inner = B[opener + 1:close]
    if not inner or not starts_declaration(unit.tu, inner, 0):
        return None
    if opener > 0:
        kind, x = B[opener - 1][0], B[opener - 1][1]
        if (kind == 'id' and x not in KEYWORDS) or x in (']', 'sizeof'):
            return None                 # a call's parentheses, or sizeof(T)
        if x == ')' and cast_type(unit, B, opener - 1) is None:
            return None                 # a call through an expression
    got = parse_declaration(unit.tu, inner)
    if not got or len(got[1]) != 1 or got[1][0][0] is not None:
        return None
    return got[1][0][1], opener


def is_unary(unit, B, i):
    """Is the operator at B[i] unary? Binary when an operand ends just before
    it: a name, a literal, `]`, or a `)` that does not close a cast."""
    if i == 0:
        return True
    kind, x = B[i - 1][0], B[i - 1][1]
    if kind in ('num', 'str', 'ch'):
        return False
    if kind == 'id':
        return x in KEYWORDS
    if x in (']', '++', '--'):
        return False
    if x == ')':
        opener = _match_back(B, i - 1)
        if opener > 0 and B[opener - 1][1] in ('if', 'while', 'for', 'switch'):
            return True                 # `if (p) *p = 0;`: a condition ended
        return cast_type(unit, B, i - 1) is not None
    return True


def walk_members(B, j, td, off):
    """Follows `.f` and constant `[k]` from B[j] through an object of type `td`
    at byte `off`. Returns (j, td, off, stop): where the walk ended, the type
    and offset there, and why it ended early -- None (it did not), 'target'
    (the next step leaves the object through a pointer: `->`, or a subscript of
    a pointer), 'index' (a subscript that is not a constant) or 'layout' (a
    member of a record whose layout is not known)."""
    while j < len(B):
        x = B[j][1]
        if x == '.' and j + 1 < len(B):
            hit = field_of(td, B[j + 1][1])
            if hit is None:
                return j, None, off, 'layout'
            off += hit[0]
            td = hit[1]
            j += 2
        elif x == '->':
            return j, td, off, 'target'
        elif x == '[':
            if is_pointer(td):
                return j, td, off, 'target'
            elem = element(td)
            if elem is None or size_of(elem) is None:
                return j, None, off, 'layout'
            end = _match_fwd(B, j)
            k = const_value(B[j + 1:end - 1])
            if k is None:
                return j, td, off, 'index'
            off += k * size_of(elem)
            td = elem
            j = end
        else:
            break
    return j, td, off, None


def body_declarations(unit, B):
    """({name: (type, storage)}, {name: [Proto]}) for the declarations at the
    start of the statements of one function body."""
    objects, protos = {}, collections.defaultdict(list)
    i, n, at_start = 1, len(B), True
    while i < n:
        if at_start and starts_declaration(unit.tu, B, i):
            end = _statement_end(B, i)
            got = parse_declaration(unit.tu, B[i:end])
            if got and got[0] != 'typedef':
                storage, decls = got
                for name, td in decls:
                    if not name:
                        continue
                    if td[0] == 'fn':
                        params, variadic = parse_params(unit.tu, td[2])
                        protos[name].append(Proto(
                            name, B[i][2], params, variadic,
                            unit._addr(B[i][2], B[max(end - 1, i)][2])))
                    else:
                        objects.setdefault(name, (td, storage))
            i = end + 1
            at_start = True
            continue
        at_start = B[i][1] in (';', '{', '}')
        i += 1
    return objects, protos


class Scope:
    """The names one function body sees: its parameters and locals (which hide
    a global of the same name), and the `extern`s declared inside it."""

    def __init__(self, tree, fn):
        self.tree, self.unit = tree, fn.unit
        objects, self.protos = body_declarations(fn.unit, fn.body)
        self.locals, self.externs = {}, {}
        for name, td in fn.params:
            if name:
                self.locals[name] = td
        for name, (td, storage) in objects.items():
            if storage == 'extern':
                self.externs[name] = td
            else:
                self.locals.setdefault(name, td)

    def is_local(self, name):
        return name in self.locals

    def global_type(self, name):
        """(is it a global, its type here): (False, None) for a local or a
        file-scope static; (None, None) when this file does not declare it."""
        if name in self.locals or name in self.unit.statics:
            return False, None
        if name in self.externs:
            return True, self.externs[name]
        if name in self.unit.globals:
            return True, self.unit.globals[name]
        shared = self.tree.shared
        if shared is not None and name in shared.globals:
            return True, shared.globals[name]
        return None, None


# ---- reads through an address ---------------------------------------------------

Site = collections.namedtuple(
    'Site', 'file line func root expr via off stop reads')
Verdict = collections.namedtuple(
    'Verdict', 'kind reach extent widest addr block')


def compress(reads):
    """The reads that decide a verdict: the one reaching furthest, one starting
    below the address, and one of unknown width."""
    known = [r for r in reads if r[1] is not None]
    out = []
    if known:
        far = max(known, key=lambda r: r[1])
        out.append(far)
        low = min(known, key=lambda r: r[0])
        if low[0] < 0 and low is not far:
            out.append(low)
    out += [r for r in reads if r[1] is None][:1]
    return out


class Engine:
    """Every address-of-global site in the tree and the reads made through it.
    A read is (lo, hi, why): bytes [lo, hi) from the address being tracked,
    hi None when the width is not known."""

    def __init__(self, tree, layout):
        self.tree, self.layout = tree, layout
        self._scopes, self._summaries, self._active = {}, {}, set()
        self._globals, self._uses = {}, None

    def scope(self, fn):
        key = (fn.unit.file, fn.name, fn.line)
        if key not in self._scopes:
            self._scopes[key] = Scope(self.tree, fn)
        return self._scopes[key]

    @staticmethod
    def _read(off, td, B, s, j, stop=None):
        size = None if stop == 'layout' else size_of(td)
        return (off, None if size is None else off + size,
                f'`{expr_text(B[s:j])}`')

    @staticmethod
    def _escape(P, rel, why):
        size = size_of(P)
        return [(rel, None if size is None else rel + size,
                 f'{why}: counts sizeof({type_text(P)})')]

    @staticmethod
    def _call_paren(unit, B, k):
        """Is the `(` at B[k] a call's (or if/while/sizeof's), not a grouping?"""
        if k <= 0:
            return False
        kind, x = B[k - 1][0], B[k - 1][1]
        if kind == 'id':
            return x not in ('return', 'case')
        return x == ']' or (x == ')' and cast_type(unit, B, k - 1) is None)

    @staticmethod
    def _call_of(B, s):
        """(callee or None, argument index) when B[s] starts an argument."""
        j, argi, depth = s - 1, 0, 0
        while j >= 0:
            x = B[j][1]
            if x in (')', ']'):
                depth += 1
            elif x in ('(', '['):
                if depth == 0:
                    break
                depth -= 1
            elif x == ',' and depth == 0:
                argi += 1
            elif x in (';', '{', '}'):
                return None
            j -= 1
        if j < 1 or B[j][1] != '(':
            return None
        kind, x = B[j - 1][0], B[j - 1][1]
        if kind == 'id':
            if x in KEYWORDS:
                return None
            if j >= 2 and B[j - 2][1] in ('.', '->'):
                return None, argi           # a function pointer in a record
            return x, argi
        return None, argi

    def consume(self, fn, B, s, e, P, rel):
        """Every read through the pointer B[s:e] evaluates to. `P` is its
        static pointee type; `rel` is its distance from the tracked address."""
        unit, n = fn.unit, len(B)
        while True:
            prev = B[s - 1][1] if s > 0 else ''
            nxt = B[e][1] if e < n else ''
            if nxt == '->' and e + 1 < n:
                hit = field_of(P, B[e + 1][1])
                if hit is None:
                    return [(rel, None, f'`{expr_text(B[s:e + 2])}`: no layout '
                                        f'for {type_text(P)}')]
                j, td, off, stop = walk_members(B, e + 2, hit[1], rel + hit[0])
                if stop is None and prev == '&' and is_unary(unit, B, s - 1):
                    s, e, P, rel = s - 1, j, td, off
                    continue
                return [self._read(off, td, B, s, j, stop)]
            if nxt == '[':
                end = _match_fwd(B, e)
                if size_of(P) is None:
                    return [(rel, None, f'`{expr_text(B[s:end])}`: '
                                        f'{type_text(P)} has no size')]
                k = const_value(B[e + 1:end - 1])
                j, td, off, stop = walk_members(B, end, P,
                                                rel + (k or 0) * size_of(P))
                if stop is None and k is not None and prev == '&' and \
                        is_unary(unit, B, s - 1):
                    s, e, P, rel = s - 1, j, td, off
                    continue
                return [self._read(off, td, B, s, j, stop)]
            if prev == '(' and nxt == ')':
                bt = B[s - 2][1] if s >= 2 else ''
                if bt in ('if', 'while', 'switch', 'sizeof'):
                    return []                      # a test, or a size
                if not self._call_paren(unit, B, s - 1):
                    s, e = s - 1, e + 1
                    continue
            if prev == ')':
                ct = cast_type(unit, B, s - 1)
                if ct is not None:
                    td, s = ct
                    if not is_pointer(td):
                        return self._escape(P, rel, f'cast to {type_text(td)}')
                    P = pointee(td)
                    continue
            if prev == '*' and is_unary(unit, B, s - 1):
                s2, e2 = s - 1, e
                while s2 > 0 and e2 < n and B[s2 - 1][1] == '(' and \
                        B[e2][1] == ')' and not self._call_paren(unit, B, s2 - 1):
                    s2, e2 = s2 - 1, e2 + 1
                if e2 < n and B[e2][1] == '.':
                    j, td, off, stop = walk_members(B, e2, P, rel)
                    if stop is None and s2 > 0 and B[s2 - 1][1] == '&' and \
                            is_unary(unit, B, s2 - 1):
                        s, e, P, rel = s2 - 1, j, td, off
                        continue
                    return [self._read(off, td, B, s2, j, stop)]
                return [self._read(rel, P, B, s - 1, e)]
            if nxt in ('+', '-') and e + 1 < n and \
                    prev in ('(', ',', '=', 'return', '?', ':'):
                k = const_value(B[e + 1:e + 2])
                after = B[e + 2][1] if e + 2 < n else ''
                if k is not None and after not in \
                        ('*', '/', '%', '[', '(', '->', '.', '++', '--'):
                    if size_of(P) is None:
                        return [(rel, None, f'`{expr_text(B[s:e + 2])}`: '
                                            f'{type_text(P)} has no size')]
                    rel += (k if nxt == '+' else -k) * size_of(P)
                    e += 2
                    continue
            if nxt in ('==', '!=', '<', '>', '<=', '>=', '&&', '||', '?') or \
                    prev in ('==', '!=', '<', '>', '<=', '>=', '&&', '||', '!'):
                return []                          # compared, not read
            if prev == '=' and nxt in (';', ',') and s >= 2 and \
                    B[s - 2][0] == 'id' and \
                    (B[s - 3][1] if s >= 3 else '') not in ('.', '->') and \
                    not (s >= 3 and B[s - 3][1] == '*' and
                         is_unary(unit, B, s - 3)):
                v = B[s - 2][1]
                scope = self.scope(fn)
                if scope.is_local(v):
                    td = scope.locals[v]
                    Pv = pointee(td) if is_pointer(td) else None
                    return self.def_use(fn, B, _statement_end(B, e) + 1, v, rel,
                                        P if Pv is None else Pv,
                                        _def_block(B, s - 2))
                is_global, td = scope.global_type(v)
                if is_global is not False:
                    got = self.global_reads(v, (pointee(td) if is_pointer(td)
                                                else None) or P)
                    if got:
                        return [(rel + lo, None if hi is None else rel + hi,
                                 why) for lo, hi, why in got]
                    if got is not None and not self.layout.other_names(v):
                        return []       # no name reads that storage at all
                # No read through that NAME, but other names share its storage
                # (`g_edit_cursor_next` is a cursor's `next`): the value's own
                # type is what is counted.
                return self._escape(P, rel, f'stored in `{v}`')
            if prev in ('(', ',') and nxt in (')', ','):
                call = self._call_of(B, s)
                if call is not None:
                    name, argi = call
                    if name is None:
                        return self._escape(P, rel, 'passed through a '
                                                    'function pointer')
                    got = self.callee_reads(fn, name, argi)
                    if got is None:
                        return self._escape(P, rel, f'passed to {name}(), '
                                                    f'which nothing types as '
                                                    f'a pointer parameter')
                    return [(rel + lo, None if hi is None else rel + hi,
                             f'{name}() argument {argi + 1} -> {why}')
                            for lo, hi, why in got]
            if prev == 'return':
                return self._escape(P, rel, 'returned')
            return self._escape(P, rel, f'`{expr_text(B[s:e])}` used as a value')

    def def_use(self, fn, B, start, v, rel, P, block=None):
        """Every read through the pointer held in `v`, from B[start] on.

        Within `block` (the def's own code, see `_def_block`) an assignment to
        `v` ends the tracking -- its right-hand side still reads the old value
        -- and so does `++`/`+=` outside a `for` header. `block` None is a
        GLOBAL: any function may have put the value there, so nothing ends it."""
        reads, n = [], len(B)
        i, stop = start, n
        while i < stop:
            if B[i][0] != 'id' or B[i][1] != v:
                i += 1
                continue
            prev = B[i - 1][1] if i > 0 else ''
            nxt = B[i + 1][1] if i + 1 < n else ''
            if prev in ('.', '->') or nxt == '(':
                i += 1
                continue                 # a member, or a function, of that name
            if nxt == '=' and not (prev == '*' and is_unary(fn.unit, B, i - 1)):
                if block is not None and block[0] <= i < block[1]:
                    stop = min(stop, _statement_end(B, i))
                i += 1
                continue                 # `v = ...` -- but `*v = ...` is a write
            if nxt in ('++', '--', '+=', '-=') or prev in ('++', '--'):
                if block is not None and not _in_for_header(B, i):
                    break
                i += 1
                continue
            if prev == '&' and is_unary(fn.unit, B, i - 1):
                reads += self._escape(P, rel, f'`&{v}` taken')
            else:
                reads += self.consume(fn, B, i, i + 1, P, rel)
            i += 1
        return reads

    def global_reads(self, name, P):
        """Every read, anywhere in the tree, through the pointer the global
        `name` holds -- which is where an address stored into it goes.
        uimisc.c parks `(Icon*)&g_side_icons` in `g_side_icon_tail`, and the
        only read through that global is `->next` at +0. None while it is
        being computed (a pointer global feeding itself)."""
        if name in self._globals:
            return self._globals[name]
        if ('global', name) in self._active:
            return None
        if self._uses is None:
            self._uses = collections.defaultdict(list)
            for unit in self.tree.units:
                for fn in unit.defns.values():
                    for ident in {t[1] for t in fn.body if t[0] == 'id'}:
                        self._uses[ident].append(fn)
        self._active.add(('global', name))
        reads = []
        try:
            for fn in self._uses.get(name, ()):
                is_global, td = self.scope(fn).global_type(name)
                if is_global is False:
                    continue
                reads += self.def_use(fn, fn.body, 1, name, 0,
                                      (pointee(td) if is_pointer(td)
                                       else None) or P)
        finally:
            self._active.discard(('global', name))
        out = [(lo, hi, f'stored in `{name}`, read {why}')
               for lo, hi, why in compress(reads)]
        self._globals[name] = out
        return out

    def callee_reads(self, fn, name, argi):
        """What `name` reads through argument `argi`, relative to it: from its
        definition when the tree has one, else from the prototype this file
        declares. [] when nothing can be read through that slot; None when
        nothing declares the callee."""
        unit = fn.unit
        local = self.scope(fn).protos.get(name, ())
        d = self.tree.definition(unit, name, local)
        if d is not None:
            got = self.summary(d, argi)
            if got is not None:
                return got
        p = self.tree.prototype(unit, name, local)
        if p is None:
            return None
        if argi >= len(p.params):
            return [] if p.variadic else None
        td = p.params[argi][1]
        if td is None or not is_pointer(td):
            return None                 # an address in an integer slot
        size = size_of(pointee(td))
        return [(0, size, f'{unit.file}:{p.line} prototype '
                          f'`{type_text(td)}`')]

    def summary(self, d, argi):
        """The reads definition `d` makes through parameter `argi`, relative to
        it. None while that summary is being computed (a recursion: the caller
        falls back to the prototype) or when `d` has no such parameter."""
        key = (d.unit.file, d.name, d.line, argi)
        if key in self._summaries:
            return self._summaries[key]
        if key in self._active:
            return None
        if argi >= len(d.params):
            return [] if d.variadic else None
        pname, td = d.params[argi]
        if not pname:
            return None
        self._active.add(key)
        try:
            reads = self.def_use(d, d.body, 1, pname, 0,
                                 pointee(td) if is_pointer(td) else None,
                                 (0, len(d.body)))
        finally:
            self._active.discard(key)
        out = [(lo, hi, f'{d.unit.file}:{d.line} {d.name}({pname}) {why}')
               for lo, hi, why in compress(reads)]
        self._summaries[key] = out
        return out

    def sites(self):
        """Every `&` of a global in every function body of the tree."""
        out = []
        for unit in self.tree.units:
            for fn in unit.defns.values():
                B = fn.body
                for i, t in enumerate(B):
                    if t[1] == '&' and is_unary(unit, B, i):
                        site = self._site(fn, B, i)
                        if site is not None:
                            out.append(site)
        return out

    def _site(self, fn, B, i):
        j, opened = i + 1, 0
        while j < len(B) and B[j][1] == '(':
            opened += 1
            j += 1
        if j >= len(B) or B[j][0] != 'id' or \
                (j + 1 < len(B) and B[j + 1][1] == '('):
            return None
        root = B[j][1]
        is_global, td = self.scope(fn).global_type(root)
        if is_global is False or \
                (is_global is None and root not in self.layout.addr_of):
            return None
        j, td, off, stop = walk_members(B, j + 1, td, 0)
        while stop is None and opened and j < len(B) and B[j][1] == ')':
            opened -= 1
            j, td, off, stop = walk_members(B, j + 1, td, off)
        if opened and stop is None:
            return None
        via = next((t[3] for t in B[i:j] if t[3]), None)
        site = Site(fn.unit.file, B[i][2], fn.name, root, expr_text(B[i:j]),
                    via, off, stop, [])
        if stop is not None:
            return site
        return site._replace(reads=compress(self.consume(fn, B, i, j, td, 0)))

    def judge(self, site):
        """The site's verdict against the block the layout emits."""
        if site.stop is not None:
            return Verdict(f'not the class ({site.stop})', None, None, None,
                           None, None)
        addr = self.layout.addr_of.get(site.root)
        if addr is None:
            return Verdict('no address', None, None, None, None, None)
        block = self.layout.block_at(addr)
        if block is None:
            return Verdict('no block', None, None, None, addr, None)
        extent = block[0] + block[1] - addr
        known = [r for r in site.reads if r[1] is not None]
        if not site.reads:
            return Verdict('no read', None, extent, None, addr, block)
        if not known:
            return Verdict('unsized', None, extent, site.reads[0], addr, block)
        far = max(known, key=lambda r: r[1])
        low = min(known, key=lambda r: r[0])
        reach = site.off + far[1]
        if reach > extent:
            return Verdict('past', reach, extent, far, addr, block)
        if addr + site.off + low[0] < block[0]:
            return Verdict('past', reach, extent, low, addr, block)
        return Verdict('inside', reach, extent, far, addr, block)


# ---- rows, the baseline, the report -----------------------------------------------

Row = collections.namedtuple('Row', 'file func root reach extent hits')


def sweep(src=SRC, closure=None):
    """(engine, [(site, verdict)]) for every address-of-global site in `src`."""
    tree = Tree(src)
    layout = closure_layout(closure) if closure else \
        sources_layout(src, tree.referenced, tree.defined)
    engine = Engine(tree, layout)
    return engine, [(s, engine.judge(s)) for s in engine.sites()]


def rows_of(judged):
    """One row per (file, function, global) that anything reads past its
    block: the furthest reach and the smallest extent over its sites."""
    by = collections.OrderedDict()
    for site, v in sorted(judged, key=lambda sv: (sv[0].file, sv[0].line)):
        if v.kind == 'past':
            by.setdefault((site.file, site.func, site.root), []).append((site, v))
    return [Row(f, fn, root, max(v.reach for _s, v in hits),
                min(v.extent for _s, v in hits), hits)
            for (f, fn, root), hits in by.items()]


def parse_baseline(path):
    """({(file, function, global): (reach, extent)}, [bad lines]). One row a
    line, its reason in the comment:

        PAST goalstate.c BlokeAction_LeavePark g_entrance_x reach=8 extent=4  # why
    """
    rows, bad = {}, []
    if not os.path.exists(path):
        return rows, [f'no baseline at {path}']
    for lineno, raw in enumerate(open(path, encoding='utf-8'), 1):
        line = raw.split('#', 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        try:
            if parts[0] != 'PAST' or len(parts) != 6 or \
                    not parts[4].startswith('reach=') or \
                    not parts[5].startswith('extent='):
                raise ValueError
            rows[(parts[1], parts[2], parts[3])] = (
                int(parts[4][6:], 0), int(parts[5][7:], 0))
        except (ValueError, IndexError):
            bad.append(f'{path}:{lineno}: not a PAST row: {line}')
    return rows, bad


def check(rows, baseline):
    """(failures, notes) of the rows against a baseline file. A row not in it
    fails; a row that reads further, or whose block shrank, fails; a baseline
    row nothing reports any more is a FIXED note, and passes."""
    want, bad = parse_baseline(baseline)
    fails, notes, seen = list(bad), [], set()
    for r in rows:
        key = (r.file, r.func, r.root)
        seen.add(key)
        if key not in want:
            fails.append(f'{r.file} {r.func}: `{r.root}` is read {r.reach} '
                         f'byte(s) from its address and its block ends after '
                         f'{r.extent} -- NOT in the baseline')
        elif r.reach > want[key][0] or r.extent < want[key][1]:
            fails.append(f'{r.file} {r.func}: `{r.root}` CHANGED to reach='
                         f'{r.reach} extent={r.extent} (baseline: reach='
                         f'{want[key][0]} extent={want[key][1]})')
    for key in sorted(set(want) - seen):
        notes.append(f'FIXED {key[0]} {key[1]} {key[2]}: nothing reads past '
                     f'its block now -- drop the row')
    return fails, notes


def _via(site):
    return f'  (macro `{site.via[0]}`, {site.file}:{site.via[1]})' \
        if site.via else ''


def report_row(row):
    lines = [f'PAST {row.file} {row.func}: `{row.root}` is read {row.reach} '
             f'byte(s) from its address; the block gen_link emits for it ends '
             f'after {row.extent}']
    for site, v in row.hits:
        start, size, primary = v.block
        lo, hi, why = v.widest
        lines += [f'    {site.file}:{site.line}  {site.expr}{_via(site)}',
                  f'        reads +{site.off + lo}..+{site.off + hi}: {why}',
                  f'        `{row.root}` is 0x{v.addr:08x}; the block is '
                  f'`{primary}` 0x{start:08x}..0x{start + size:08x} '
                  f'({size} bytes)']
    lines.append(f'    FIX: declare `{row.root}` as the type it is read as (or '
                 f'as a view of the wider object) so gen_link sizes its block '
                 f'to cover the read and the named neighbours inside become '
                 f'interior aliases. No bytes move: re-gate the file with '
                 f'tools/audit.py and tools/relocs.py.')
    return lines


def census_line(site, v):
    nums = ''.join(f' {k} {x}' for k, x in (('reach', v.reach),
                                            ('extent', v.extent))
                   if x is not None)
    why = f'  [{v.widest[2]}]' if v.widest else ''
    return (f'{v.kind:26s} {site.file}:{site.line} {site.func}: '
            f'{site.expr}{_via(site)}{nums}{why}')


# ---- selftest ------------------------------------------------------------------------
# Every shape the gate has to get right, as small trees of files. Then the real
# tree: the named negatives have to be EXAMINED and found inside -- a reader that
# quietly stops seeing them is a gate that always passes -- and the declaration
# a88d0c2e fixed has to come back as a row when it is put back.

_POS = 'typedef struct Pos { int x; int y; } Pos;\n'

_MAPOBJ = '''\
extern int g_entrance_x;     /* 0x004b8320 */
extern int g_entrance_y;     /* 0x004b8324 */
extern int g_leave_dx;       /* 0x004b8328 */
// FUNCTION: LEGOLAND 0x0045a000
void PutEntrance(int x, int y)
{
    g_entrance_x = x;
    g_entrance_y = y;
    g_leave_dx = 0;
}
'''

_BNVMOVE = _POS + '''\
extern int g_suggest_x;      /* 0x006661bc */
// FUNCTION: LEGOLAND 0x00482050
int SuggestNextMove(Pos* from, Pos* to, Pos* out)
{
    g_suggest_x = to->x >> 8;
    out->y = to->y;
    return from->x;
}
'''


def _goalstate(decl):
    return _POS + f'''\
typedef struct Bloke {{ Pos world; }} Bloke;
extern {decl} g_entrance_x;                                /* 0x004b8320 */
extern int  SuggestNextMove(Pos* from, Pos* to, Pos* out); /* 0x00482050 */
// FUNCTION: LEGOLAND 0x0044ed70
void BlokeAction_LeavePark(Bloke* b)
{{
    Pos out;
    SuggestNextMove(&b->world, (Pos*)&g_entrance_x, &out);
}}
'''


_COASTER = '''\
typedef struct TrackDesc TrackDesc;
typedef struct TrackNode TrackNode;
struct TrackNode { int state; int x; int y; void* cls; TrackDesc* desc; int pad[8]; };
struct TrackDesc { int raised; int h0; int h1; int jp0[2]; };
typedef struct TrackFit { int flags; } TrackFit;
extern TrackFit  g_track_fit;                                          /* 0x004d8250 */
extern TrackDesc g_track_desc_flat;                                    /* 0x004b5d20 */
extern int       g_track_after;                                        /* 0x004b5d34 */
extern int  TrackFitCheckChain(TrackNode* n, void* p, TrackFit* out);  /* 0x0041d2e0 */
extern TrackNode* FindTrackDesc(void* elem);                           /* 0x00427c00 */
// FUNCTION: LEGOLAND 0x0041d3b0
TrackFit* TrackFitCheck(TrackNode* n, void* p)
{
    int ok = 0;
    if (n->state & 1)
        ok = TrackFitCheckChain(n, p, &g_track_fit);
    g_track_fit.flags = ok;
    return &g_track_fit;
}
// FUNCTION: LEGOLAND 0x004275d0
void Track_Update(void* elem)
{
    TrackNode* n;
    int sq;
    n = FindTrackDesc(elem);
    if (!n)
        n = (TrackNode*)&g_track_desc_flat;
    if (TrackFitCheck(n, &sq)->flags == 0)
        g_track_after = 1;
}
'''

_COASTER5 = '''\
typedef struct TrackDesc { int raised; int h0; int h1; int jp0[2]; } TrackDesc;
typedef struct TrackFit { int flags; } TrackFit;
// FUNCTION: LEGOLAND 0x0041d2e0
int TrackFitCheckChain(TrackDesc* d, const short* sq, TrackFit* f)
{
    f->flags = d->jp0[1];
    return d->raised;
}
'''

_OBJRECT = '''\
typedef struct MapCell { int a; int b; int c; } MapCell;
typedef struct Footprint { int w; int h; int cells[8]; } Footprint;
typedef struct EditObj { int kind; short rect; } EditObj;
extern MapCell** g_map_rows;                          /* 0x00801400 */
extern EditObj*  g_edit_object;                       /* 0x007fffd0 */
extern void SetEditCursorFootPrint(Footprint* f);     /* 0x00460000 */
// FUNCTION: LEGOLAND 0x00401100
MapCell* MapCellAt(int x, int y)
{
    return (MapCell*)&g_map_rows[y][x];
}
// FUNCTION: LEGOLAND 0x00401200
void Hedge_SelectForPlacement(void)
{
    SetEditCursorFootPrint((Footprint*)&g_edit_object->rect);
}
'''

_FORMS = _POS + '''\
extern int   g_pair_a;       /* 0x00400100 */
extern int   g_pair_b;       /* 0x00400104 */
extern int   g_single;       /* 0x00400108 */
extern short g_word;         /* 0x0040010c */
extern int   g_tail;         /* 0x00400110 */
extern void  TakePos(Pos* p);            /* 0x00401300 */
// FUNCTION: LEGOLAND 0x00401400
int Forms(void)
{
    Pos* p;
    int a = (&g_pair_a)[1];
    int b = *(&g_pair_b + 1);
    int t = *(int*)&g_tail;
    TakePos(&g_single);
    p = (Pos*)&g_word;
    return a + b + t + p->y;
}
'''

_QUERY = '''\
typedef struct QueryCursor { char raw[0x20]; } QueryCursor;
typedef struct QueryBlock { int x; int y; } QueryBlock;
typedef struct Wide { int a[4]; } Wide;
extern QueryCursor g_query_cursor;   /* 0x00810160 */
#ifndef LEGOLAND_PORTABLE
extern QueryBlock  g_query_block;    /* 0x00810170 */
extern QueryBlock  g_query_late;     /* 0x0081017c */
#else
#define g_query_block (*(QueryBlock*)&g_query_cursor.raw[0x10])   /* 0x00810170 */
#define g_query_late  (*(QueryBlock*)&g_query_cursor.raw[0x1c])   /* 0x0081017c */
#endif
extern int  g_after_cursor;          /* 0x00810180 */
extern int  g_after_after;           /* 0x00810184 */
extern void TakeWide(Wide* w);       /* 0x00401600 */
// FUNCTION: LEGOLAND 0x00401500
void Probe(int x, int y)
{
    g_query_block.x = x;
    g_query_block.y = y;
    g_query_late.y = y;
    g_after_cursor = g_after_after;
}
#ifndef LEGOLAND_PORTABLE
// FUNCTION: LEGOLAND 0x00401700
void DeadArm(void)
{
    TakeWide((Wide*)&g_after_cursor);
}
#endif
'''

_MISC = '''\
extern int g_flags;       /* 0x00400200 */
extern int g_mask;        /* 0x00400204 */
extern int Printf(const char* fmt, ...);   /* 0x0049e5c5 */
// FUNCTION: LEGOLAND 0x00401800
int Misc(int a)
{
    /* (Pos*)&g_flags in a comment is not a site */
    Printf("(Pos*)&g_flags %d", a & g_mask);
    return a & g_flags;
}
'''

_ICONS = '''\
typedef struct Icon { struct Icon* next; int pad[15]; } Icon;
extern Icon* g_side_icons;       /* 0x006687c8 */
extern Icon* g_icons_b;          /* 0x006687cc */
extern Icon* g_icons_c;          /* 0x006687d0 */
extern int   g_icons_end;        /* 0x006687d4 */
extern Icon* g_side_icon_tail;   /* 0x004ba87c */
extern Icon* g_other_tail;       /* 0x004ba880 */
extern Icon* g_wide_tail;        /* 0x004ba884 */
extern int   g_tails_end;        /* 0x004ba888 */
// FUNCTION: LEGOLAND 0x0046d460
void UnlinkIcon(Icon** link)
{
    if (g_side_icon_tail == *link) g_side_icon_tail = (Icon*)link;
    *link = (*link)->next;
}
// FUNCTION: LEGOLAND 0x0046d500
void LinkIcon(Icon* icon)
{
    g_side_icon_tail->next = icon;
    g_side_icon_tail = icon;
}
// FUNCTION: LEGOLAND 0x0046d540
void ParkWide(void)
{
    g_wide_tail = (Icon*)&g_icons_b;
    g_other_tail = (Icon*)&g_icons_c;
}
// FUNCTION: LEGOLAND 0x0046d580
int ReadWide(void)
{
    return g_wide_tail->pad[3] + g_icons_end + g_tails_end;
}
// FUNCTION: LEGOLAND 0x0046d5c0
void RemoveIconGroup(void)
{
    UnlinkIcon(&g_side_icons);
}
'''

_CLOSURE = '''\
extern unsigned int g_entrance_x[2];
/* 0x004b8320 .data 8 bytes interior: g_entrance_y+0x4 */
__attribute__((aligned(16))) unsigned int g_entrance_x[2] = {
    0x00003500u, 0x00002800u
};
/* 0x004b8328 .data 4 bytes (+g_leave_alias) */
__attribute__((aligned(16))) unsigned char g_leave_dx[4];
'''


def selftest():
    import shutil
    import tempfile
    fails = []
    tmp = tempfile.mkdtemp(prefix='cast-extent-')

    def chk(what, got, want):
        if got != want:
            fails.append(f'{what}: got {got!r}, want {want!r}')

    def run(name, files):
        d = os.path.join(tmp, name)
        os.makedirs(d)
        for fn, text in files.items():
            with open(os.path.join(d, fn), 'w') as f:
                f.write(text)
        _engine, judged = sweep(d)
        return judged, rows_of(judged)

    def keys(rows):
        return {(r.file, r.func, r.root) for r in rows}

    def kinds(judged, file, root):
        return sorted({v.kind for s, v in judged
                       if s.file == file and s.root == root})

    try:
        # the positive control: goalstate.c as it was before a88d0c2e
        judged, rows = run('prefix', {'goalstate.c': _goalstate('int '),
                                      'mapobj.c': _MAPOBJ,
                                      'bnvmove.c': _BNVMOVE})
        chk('`extern int g_entrance_x` read as a Pos is a row', keys(rows),
            {('goalstate.c', 'BlokeAction_LeavePark', 'g_entrance_x')})
        chk('... 8 bytes read from a 4-byte block',
            [(r.reach, r.extent) for r in rows], [(8, 4)])
        chk('... measured through the DEFINITION of SuggestNextMove',
            all('bnvmove.c' in v.widest[2] for r in rows for _s, v in r.hits),
            True)
        judged, rows = run('fixed', {'goalstate.c': _goalstate('Pos '),
                                     'mapobj.c': _MAPOBJ,
                                     'bnvmove.c': _BNVMOVE})
        chk('the fix, `extern Pos g_entrance_x`, is not a row', keys(rows),
            set())
        chk('... its site is examined and inside',
            kinds(judged, 'goalstate.c', 'g_entrance_x'), ['inside'])

        # coaster.c: a wide cast whose reads all land inside the descriptor
        judged, rows = run('coaster', {'coaster.c': _COASTER,
                                       'coaster5.c': _COASTER5})
        chk('(TrackNode*)&g_track_desc_flat is not a row', keys(rows), set())
        chk('... every byte the definitions read is inside',
            kinds(judged, 'coaster.c', 'g_track_desc_flat'), ['inside'])
        judged, rows = run('coaster-no-body', {'coaster.c': _COASTER})
        chk('... and with no body for TrackFitCheckChain its TrackNode* '
            'prototype decides -- a row', keys(rows),
            {('coaster.c', 'Track_Update', 'g_track_desc_flat')})

        # an address in a pointer's TARGET is not the global's block
        judged, rows = run('target', {'objrect.c': _OBJRECT})
        chk('`&g_map_rows[y][x]` and `&g_edit_object->rect` are not rows',
            keys(rows), set())
        chk('... `&g_map_rows[y][x]` is examined and set aside',
            kinds(judged, 'objrect.c', 'g_map_rows'), ['not the class (target)'])
        chk('... so is `&g_edit_object->rect`',
            kinds(judged, 'objrect.c', 'g_edit_object'),
            ['not the class (target)'])

        # the four read forms, and the negative beside them
        judged, rows = run('forms', {'forms.c': _FORMS})
        chk('(&g_x)[1], *(&g_x + 1), &g_x to a Pos* prototype, a local Pos*',
            keys(rows), {('forms.c', 'Forms', r) for r in
                         ('g_pair_a', 'g_pair_b', 'g_single', 'g_word')})
        chk('... `*(int*)&g_tail` is inside', kinds(judged, 'forms.c', 'g_tail'),
            ['inside'])

        # the LEGOLAND_PORTABLE arm: macros expanded, the VC6 arm unread
        judged, rows = run('query', {'query.c': _QUERY})
        chk('g_query_late reads past the cursor through its macro', keys(rows),
            {('query.c', 'Probe', 'g_query_cursor')})
        chk('... and says which macro',
            sorted({s.via[0] for r in rows for s, _v in r.hits if s.via}),
            ['g_query_late'])
        chk('... g_query_block reads inside the cursor',
            sorted({v.kind for s, v in judged
                    if s.via and s.via[0] == 'g_query_block'}), ['inside'])
        chk('... the VC6 arm is not read',
            [s for s, _v in judged if s.func == 'DeadArm'], [])

        # not sites at all: a comment, a string, a binary &
        judged, rows = run('misc', {'misc.c': _MISC})
        chk('a comment, a string and a binary `&` are not sites',
            [s.expr for s, _v in judged], [])

        # a pointer parked in a global is followed to that global's readers
        judged, rows = run('icons', {'icons.c': _ICONS})
        chk('`(Icon*)link` parked in g_side_icon_tail is read only at +0; '
            'g_wide_tail is read at +16', keys(rows),
            {('icons.c', 'ParkWide', 'g_icons_b')})
        chk('... &g_side_icons is inside',
            kinds(judged, 'icons.c', 'g_side_icons'), ['inside'])
        chk('... &g_icons_c, parked where no name reads it, is no read',
            kinds(judged, 'icons.c', 'g_icons_c'), ['no read'])

        # the baseline's verdicts
        bl = os.path.join(tmp, 'baseline.txt')
        judged, rows = run('prefix2', {'goalstate.c': _goalstate('int '),
                                       'mapobj.c': _MAPOBJ,
                                       'bnvmove.c': _BNVMOVE})
        with open(bl, 'w') as f:
            f.write('PAST goalstate.c BlokeAction_LeavePark g_entrance_x '
                    'reach=8 extent=4   # accepted\n'
                    'PAST gone.c Gone g_gone reach=8 extent=4   # fixed\n')
        f1, n1 = check(rows, bl)
        chk('an accepted row passes', f1, [])
        chk('a baseline row nothing reports prints FIXED',
            any(x.startswith('FIXED gone.c') for x in n1), True)
        with open(bl, 'w') as f:
            f.write('PAST goalstate.c BlokeAction_LeavePark g_entrance_x '
                    'reach=6 extent=4\n')
        f2, _n2 = check(rows, bl)
        chk('a row that reads further than accepted fails',
            any('CHANGED' in x for x in f2), True)
        f3, _n3 = check(rows, os.path.join(tmp, 'nope.txt'))
        chk('a missing baseline fails', any('no baseline' in x for x in f3),
            True)

        # the closure reader
        gc = os.path.join(tmp, 'globals.c')
        with open(gc, 'w') as f:
            f.write(_CLOSURE)
        lay = closure_layout(gc)
        chk('closure blocks', sorted(lay.blocks.items()),
            [(0x004b8320, (8, 'g_entrance_x')), (0x004b8328, (4, 'g_leave_dx'))])
        chk('closure interior alias', lay.addr_of.get('g_entrance_y'),
            0x004b8324)
        chk('closure data alias', lay.addr_of.get('g_leave_alias'), 0x004b8328)

        # the real tree
        if os.path.isdir(SRC):
            _engine, judged = sweep(SRC)
            chk('the real tree has hundreds of address-of-global sites',
                len(judged) > 800, True)
            for file, root, want in (
                    ('coaster.c', 'g_track_desc_flat', ['inside']),
                    ('objrect.c', 'g_map_rows', ['not the class (target)']),
                    ('fpui2.c', 'g_map_rows', ['not the class (target)']),
                    ('waterworks.c', 'g_edit_object',
                     ['not the class (target)'])):
                chk(f'real {file}: `&{root}`', kinds(judged, file, root), want)
            for file in ('misc3.c', 'fpui3.c'):
                chk(f'real {file}: g_query_block through its macro',
                    sorted({v.kind for s, v in judged if s.file == file and
                            s.via and s.via[0] == 'g_query_block'}), ['inside'])
            chk('real goalstate.c: both LeavePark routes are examined',
                len([s for s, _v in judged if s.file == 'goalstate.c' and
                     s.root == 'g_entrance_x']), 2)
            real = os.path.join(tmp, 'real')
            os.makedirs(real)
            for p in glob.glob(os.path.join(SRC, '*.[ch]')):
                shutil.copy(p, real)
            gpath = os.path.join(real, 'goalstate.c')
            text = open(gpath, encoding='utf-8', errors='replace').read()
            if 'extern Pos  g_entrance_x;' in text:
                with open(gpath, 'w', encoding='utf-8') as f:
                    f.write(text.replace('extern Pos  g_entrance_x;',
                                         'extern int  g_entrance_x;', 1))
            _engine, judged = sweep(real)
            got = [(r.reach, r.extent) for r in rows_of(judged)
                   if (r.file, r.func, r.root) ==
                   ('goalstate.c', 'BlokeAction_LeavePark', 'g_entrance_x')]
            chk('the real goalstate.c before a88d0c2e is a row', got, [(8, 4)])
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    for x in fails:
        print(f'FAIL {x}')
    print(f'cast_extent_sweep selftest: {len(fails)} failure(s)')
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--src', default=SRC, help='the directory to sweep')
    ap.add_argument('--baseline',
                    help='the accepted rows; a row not in it fails the gate')
    ap.add_argument('--closure',
                    help="a generated globals.c: take the blocks from the build "
                         "instead of computing them from the sources")
    ap.add_argument('--census', action='store_true',
                    help='every address-of-global site and its verdict')
    ap.add_argument('--quiet', action='store_true',
                    help='the verdict line and the failures, nothing else')
    ap.add_argument('--selftest', action='store_true',
                    help='the shapes, positive and negative')
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    engine, judged = sweep(args.src, args.closure)
    rows = rows_of(judged)
    if not args.quiet:
        if args.census:
            for site, v in sorted(judged, key=lambda sv: (sv[0].file,
                                                          sv[0].line)):
                print(census_line(site, v))
            print()
        for r in rows:
            print('\n'.join(report_row(r)))
            print()
    if args.baseline:
        fails, notes = check(rows, args.baseline)
    else:
        fails, notes = [f'{r.file} {r.func}: `{r.root}` is read past its '
                        f'block' for r in rows], []
    for x in notes:
        print(x)
    for x in fails:
        print(f'FAIL {x}')
    kinds = collections.Counter(v.kind.split(' (')[0] for _s, v in judged)
    print(f'cast_extent_sweep gate: {len(fails)} failure(s) over {len(judged)} '
          f'address-of-global site(s) -- '
          + ', '.join(f'{n} {k}' for k, n in sorted(kinds.items()))
          + f'; blocks from {engine.layout.how}')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
