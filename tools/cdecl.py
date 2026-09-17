#!/usr/bin/env python3
"""Object extents computed from the game's own C, not from a hand table.

`gen_link.py` sizes every global by the gap to the next NAMED address. For an
object several names reach into at different offsets that is wrong in the worst
possible way: the record comes out as one block per named field, 16-byte
aligned, and the files that write it and the files that read it address
different memory. Silent, total, and it has now bitten five times
(`g_key_state`, `g_gpu_state`, `GameInput`, `PopUpUI`, `Profile`,
`BlitCtx`/`HitInfo`, `CurProfile`) -- every one found only after a symptom,
because the extent of a struct-typed object was a row somebody had to add by
hand to `STRUCT_EXTENTS`.

The extent is not a judgement call: it is `sizeof` of the type the game's own
source declares the object with. This module computes it.

    extents, diag = scan()          # addr -> Extent, plus the diagnostics

What it parses, out of `LEGOLAND/*.c` and `LEGOLAND/*.h`:

  * `typedef struct Tag { ... } Name;` / `typedef union ... ;` / `struct Tag
    { ... };` / `enum`, nested and anonymous aggregates, arrays of them,
    function pointers, and plain `typedef <type> Alias;`
  * `extern <T> g_x;` / `extern <T> g_x[N][M];` with the `/* 0x... */` address
    comment anywhere in the statement (statement-oriented, so a comment on a
    continuation line counts)

How it lays them out -- MSVC on x86, ILP32:

  * `char` 1, `short` 2, `int`/`long`/`float`/any pointer 4, `double`/`__int64`
    8, `enum` 4; alignment equals size for the primitives
  * a member's alignment is `min(#pragma pack in force, natural alignment)`,
    the aggregate's alignment is the max of its members' (after that clamp),
    and its size is rounded up to its own alignment -- so `pack(1)` is dense
    and `Profile` is 0x110 rather than 0x114.  The pack in force is taken at
    the line of each member, from the file's `#pragma pack(push,N)` / `(pop)` /
    `(N)` / `()` sequence; the default is MSVC's 8
  * a union is the max of its members at offset 0
  * an array is `count * sizeof(element)` (the element is already padded)

**Scope is a translation unit.** The sources define their types locally on
purpose ("so this file does not depend on legoland.h"), so the same type name
can be laid out differently in two files -- `Profile` (0x110, `profiles.c`) and
`CurProfile` are the same 270 bytes of image with the fields in a different
order, and `GameRec` is spelled differently in a dozen files. A type name is
therefore resolved in the declaring file first and in `legoland.h` second, and
nothing else; when two files disagree about the extent of the SAME address the
disagreement is reported and the MAX is used, which is the only choice that can
ever cover the whole record (and which `gen_link.py`'s clamp then limits to
storage that was going to be tiled to those names anyway).

A type this module cannot size (a `windows.h` struct, an array whose bound is a
macro) yields no extent at all, exactly as before: the gap tiling keeps the
object as it was. Nothing here guesses.

Run it directly for the table:

    python3 tools/cdecl.py              # every struct-typed extern
    python3 tools/cdecl.py --selftest   # the layout rules
"""
import argparse
import collections
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from linkreport import SRC  # noqa: E402  (decomp/LEGOLAND, or $LL_DECOMP)

# ILP32 / MSVC x86. `long` is 4 here and that is the premise of the whole
# target: the image was built by VC6 for i386.
PRIM = {
    'void': (1, 1),
    'char': (1, 1), 'signed char': (1, 1), 'unsigned char': (1, 1),
    'short': (2, 2), 'short int': (2, 2), 'unsigned short': (2, 2),
    'unsigned short int': (2, 2), 'signed short': (2, 2),
    'int': (4, 4), 'signed': (4, 4), 'signed int': (4, 4),
    'unsigned': (4, 4), 'unsigned int': (4, 4),
    'long': (4, 4), 'long int': (4, 4), 'unsigned long': (4, 4),
    'unsigned long int': (4, 4), 'signed long': (4, 4),
    'float': (4, 4), 'double': (8, 8), 'long double': (8, 8),
    '__int64': (8, 8), 'unsigned __int64': (8, 8),
    'long long': (8, 8), 'unsigned long long': (8, 8),
}

# The Win32 spellings the sources use for primitives. Anything whose size is a
# language fact only once you know the typedef goes here; aggregates do not.
PRIM_ALIASES = {
    'BYTE': 'unsigned char', 'CHAR': 'char', 'TCHAR': 'char',
    'bool': 'char', '_Bool': 'char',
    'WORD': 'unsigned short', 'SHORT': 'short', 'USHORT': 'unsigned short',
    'ATOM': 'unsigned short',
    'DWORD': 'unsigned long', 'UINT': 'unsigned int', 'INT': 'int',
    'BOOL': 'int', 'LONG': 'long', 'ULONG': 'unsigned long',
    'COLORREF': 'unsigned long', 'UINT_PTR': 'unsigned int',
    'LRESULT': 'long', 'WPARAM': 'unsigned int', 'LPARAM': 'long',
    'HRESULT': 'long', 'FLOAT': 'float', 'SIZE_T': 'unsigned int',
    'time_t': 'long', 'size_t': 'unsigned int', 'uintptr_t': 'unsigned int',
    'intptr_t': 'int', 'ptrdiff_t': 'int',
}

# Every HANDLE-shaped Win32 typedef is a pointer, i.e. 4 bytes on ILP32. These
# appear inside recovered records (an `HWND` field, an `HINSTANCE` global).
PTR_ALIASES = set("""
HANDLE HWND HDC HBITMAP HPALETTE HINSTANCE HMODULE HMENU HICON HCURSOR
HBRUSH HPEN HFONT HRGN HGDIOBJ HGLOBAL HLOCAL HKEY HRSRC HFILE HMIDIOUT
HWAVEOUT HMMIO FARPROC WNDPROC LPVOID LPSTR LPCSTR PVOID PSTR PCSTR LPBYTE
LPWORD LPDWORD LPARAM_PTR HTASK HHOOK HACCEL HDROP HMETAFILE HMIXER
""".split())

SPEC_KEYWORDS = {
    'char', 'short', 'int', 'long', 'float', 'double', 'signed', 'unsigned',
    'void', '__int64', '_Bool',
}
QUALIFIERS = {
    'const', 'volatile', 'register', 'inline', '__inline', '_inline',
    '__forceinline', '__cdecl', '__stdcall', '__fastcall', '_cdecl',
    '_stdcall', '__restrict', 'restrict',
}
STORAGE = {'extern', 'static', 'auto', 'typedef'}


class Type:
    """A sized C type. `size`/`align` are None when the size is not a language
    fact -- a struct the sources have not laid out, an array bound that is a
    macro. `fields` is [(name, offset, Type, count)] for an aggregate, which is
    what lets an interior offset be checked against a real field boundary."""

    __slots__ = ('kind', 'name', 'size', 'align', 'fields', 'where')

    def __init__(self, kind, name, size, align, fields=None, where=None):
        self.kind = kind          # prim ptr struct union enum array unknown
        self.name = name
        self.size = size
        self.align = align
        self.fields = fields or []
        self.where = where        # 'file:line' of the definition

    def __repr__(self):
        return f'<{self.kind} {self.name} size={self.size} align={self.align}>'


def prim(name):
    sz, al = PRIM[name]
    return Type('prim', name, sz, al)


PTR = Type('ptr', 'void*', 4, 4)
UNKNOWN = Type('unknown', '?', None, None)
# A function TYPE, not a pointer to one. It exists only so the declarator
# evaluator can tell `int f(void)` (not an object) from `int (*f)(void)` (four
# bytes): `int (*f)(void)` is a pointer whose target is this. Unsized on
# purpose -- `sizeof` of a function is not a thing.
FUNC = Type('func', 'function', None, None)


def array_of(elem, count):
    if count is None or elem.size is None:
        return Type('unknown', f'{elem.name}[?]', None, None)
    return Type('array', f'{elem.name}[{count}]', elem.size * count,
                elem.align, [(None, i * elem.size, elem, 1)
                             for i in range(min(count, 1))], elem.where)


# ---- tokenizer -------------------------------------------------------------

_TOK = re.compile(r"""
    (?P<pp>^[ \t]*\#[^\n]*)
  | (?P<bc>/\*.*?\*/)
  | (?P<lc>//[^\n]*)
  | (?P<ws>[ \t\r\n]+)
  | (?P<str>"(?:\\.|[^"\\])*")
  | (?P<ch>'(?:\\.|[^'\\])*')
  | (?P<id>[A-Za-z_]\w*)
  | (?P<num>0[xX][0-9a-fA-F]+[uUlL]*|\d+\.?\d*(?:[eE][-+]?\d+)?[uUlLfF]*)
  | (?P<op>\.\.\.|->|\+\+|--|<<=|>>=|<<|>>|[<>=!+\-*/%&|^]=|&&|\|\||
           [-+*/%&|^~!<>=?:;,.\[\]{}()@$\\])
""", re.VERBOSE | re.DOTALL | re.MULTILINE)

_PRAGMA_PACK = re.compile(r'#\s*pragma\s+pack\s*\((.*?)\)')
_ADDR = re.compile(r'\b(0x00[0-9a-fA-F]{6})\b')

DEFAULT_PACK = 8


class Tokens:
    """Tokens without comments, plus the two things the comments carry: the
    `#pragma pack` state per line and the `/* 0x... */` addresses per line."""

    def __init__(self, text, path):
        self.path = path
        self.toks = []            # (kind, text, line)
        self.addr_at = collections.defaultdict(list)   # line -> [addr]
        packs = []                # (line, pack value in force AFTER this line)
        stack = []
        pack = DEFAULT_PACK
        line = 1
        i, n = 0, len(text)
        while i < n:
            m = _TOK.match(text, i)
            if not m:
                i += 1
                continue
            kind = m.lastgroup
            s = m.group()
            if kind == 'pp':
                pm = _PRAGMA_PACK.search(s)
                if pm:
                    arg = pm.group(1).replace(' ', '')
                    if arg.startswith('push'):
                        stack.append(pack)
                        rest = arg[4:].lstrip(',')
                        if rest.isdigit():
                            pack = int(rest)
                    elif arg.startswith('pop'):
                        pack = stack.pop() if stack else DEFAULT_PACK
                    elif arg == '':
                        pack = DEFAULT_PACK
                    elif arg.isdigit():
                        pack = int(arg)
                    packs.append((line + s.count('\n'), pack))
            elif kind in ('bc', 'lc'):
                for a in _ADDR.findall(s):
                    self.addr_at[line + s[:s.index(a)].count('\n')].append(
                        int(a, 16))
            elif kind not in ('ws',):
                self.toks.append((kind, s, line))
            line += s.count('\n')
            i = m.end()
        self.packs = packs

    def pack_at(self, line):
        pack = DEFAULT_PACK
        for ln, p in self.packs:
            if ln > line:
                break
            pack = p
        return pack


# ---- declaration parser ----------------------------------------------------

class Object:
    __slots__ = ('name', 'typename', 'type', 'count', 'addr', 'file', 'line',
                 'def_where')

    def __init__(self, name, typename, type_, count, addr, file, line):
        self.name, self.typename, self.type = name, typename, type_
        self.count, self.addr = count, addr
        self.file, self.line = file, line
        self.def_where = type_.where

    @property
    def extent(self):
        if self.type.size is None or self.count is None:
            return None
        return self.type.size * self.count

    @property
    def full_type(self):
        """The type of the OBJECT, not of one element: `extern LevelMarker
        g_low_markers[5]` is an array of five records and every field offset
        past the first element belongs to it. None when it has no size."""
        if self.type.size is None or self.count is None:
            return None
        return array_of(self.type, self.count) if self.count != 1 else self.type


class TU:
    """One translation unit: the file's own types, then `legoland.h`'s."""

    def __init__(self, path, shared=None, text=None):
        """`text` stands in for the file's contents: `cast_extent_sweep.py`
        parses the arm of `#ifndef LEGOLAND_PORTABLE` the portable build
        compiles, with the other arm's lines blanked."""
        self.path = path
        self.file = os.path.basename(path)
        if text is None:
            text = open(path, encoding='utf-8', errors='replace').read()
        self.tk = Tokens(text, path)
        self.toks = self.tk.toks
        self.i = 0
        self.tags = {}             # ('struct'|'union'|'enum', tag) -> Type
        self.typedefs = {}         # name -> Type
        self.shared = shared       # another TU (legoland.h) to fall back on
        self.objects = []          # file-scope object declarations
        self.functions = []        # (name, line, first token, last token)
        self.notes = []            # (line, message)
        self._parse()

    # -- token helpers
    def tok(self, k=0):
        j = self.i + k
        return self.toks[j] if j < len(self.toks) else ('eof', '', -1)

    def txt(self, k=0):
        return self.tok(k)[1]

    def at_end(self):
        return self.i >= len(self.toks)

    def skip_balanced(self, open_t='{', close_t='}'):
        """Consumes from the current `open_t` past its match."""
        depth = 0
        while not self.at_end():
            t = self.txt()
            self.i += 1
            if t == open_t:
                depth += 1
            elif t == close_t:
                depth -= 1
                if depth == 0:
                    return

    def skip_to_semi(self):
        depth = 0
        while not self.at_end():
            t = self.txt()
            self.i += 1
            if t in '{([':
                depth += 1
            elif t in '})]':
                depth -= 1
            elif t == ';' and depth <= 0:
                return

    # -- type lookup
    def lookup_typedef(self, name):
        if name in self.typedefs:
            return self.typedefs[name]
        if name in PTR_ALIASES:
            return PTR
        if name in PRIM_ALIASES:
            return prim(PRIM_ALIASES[name])
        if self.shared and name in self.shared.typedefs:
            return self.shared.typedefs[name]
        return None

    def lookup_tag(self, kind, tag):
        key = (kind, tag)
        if key in self.tags:
            return self.tags[key]
        if self.shared and key in self.shared.tags:
            return self.shared.tags[key]
        return None

    # -- the file-scope loop
    def _parse(self):
        while not self.at_end():
            kind, t, line = self.tok()
            if t in (';', ',', ')', ']', '}'):
                self.i += 1
                continue
            if t == '{':
                self.skip_balanced()
                continue
            if kind == 'id' and (t in STORAGE or t in QUALIFIERS or
                                 t in SPEC_KEYWORDS or
                                 t in ('struct', 'union', 'enum') or
                                 t == '__declspec' or
                                 self.lookup_typedef(t) is not None):
                if not self._declaration():
                    self.skip_to_semi()
                continue
            self.i += 1

    def _eval_dim(self, toks):
        """An array bound: a constant expression of integer literals, or the
        size of nothing at all."""
        expr = ''
        for kind, t, _ln in toks:
            if kind == 'num':
                expr += t.rstrip('uUlL')
            elif kind == 'op' and t in '+-*()':
                expr += t
            else:
                return None
        if not expr.strip():
            return None
        try:
            v = eval(expr, {'__builtins__': {}}, {})  # literals only
        except Exception:
            return None
        return v if isinstance(v, int) and v >= 0 else None

    def _dim(self):
        """Consumes ONE `[a]`, returns its bound or None. One at a time, not
        the product: in `T (*p)[2]` the `[2]` belongs to the pointer's target,
        and only a per-dimension parse can see that."""
        assert self.txt() == '['
        self.i += 1
        inner = []
        depth = 1
        while not self.at_end():
            if self.txt() == '[':
                depth += 1
            elif self.txt() == ']':
                depth -= 1
                if depth == 0:
                    self.i += 1
                    break
            inner.append(self.tok())
            self.i += 1
        return self._eval_dim(inner)

    def _specifier(self):
        """The type specifier of a declaration. Returns (Type, typename,
        is_typedef, is_extern) with the cursor on the first declarator."""
        is_typedef = is_extern = False
        base = None
        words = []
        name = None
        while not self.at_end():
            kind, t, line = self.tok()
            if kind != 'id':
                break
            if t == 'typedef':
                is_typedef = True
                self.i += 1
            elif t == 'extern':
                is_extern = True
                self.i += 1
                if self.tok()[0] == 'str':       # extern "C"
                    self.i += 1
            elif t in STORAGE or t in QUALIFIERS:
                self.i += 1
            elif t == '__declspec':
                self.i += 1
                if self.txt() == '(':
                    self.skip_balanced('(', ')')
            elif t in ('struct', 'union'):
                base, name = self._struct_or_union(t)
                break
            elif t == 'enum':
                base, name = self._enum()
                break
            elif t in SPEC_KEYWORDS:
                words.append(t)
                self.i += 1
            elif words:
                break                            # the declarator's identifier
            else:
                td = self.lookup_typedef(t)
                if td is None:
                    if base is not None:
                        break
                    # an unknown identifier in type position: a type this TU
                    # does not define (a windows.h struct, a type from another
                    # file). Sizeless on purpose.
                    base, name = UNKNOWN, t
                    self.i += 1
                    break
                base, name = td, t
                self.i += 1
                break
        if base is None and words:
            key = ' '.join(words)
            if key not in PRIM:
                key = ' '.join(w for w in words if w != 'int') or 'int'
            base = prim(key) if key in PRIM else UNKNOWN
            name = ' '.join(words)
        if base is None:
            return None, None, is_typedef, is_extern
        return base, name, is_typedef, is_extern

    def _enum(self):
        self.i += 1                       # 'enum'
        tag = None
        if self.tok()[0] == 'id' and self.txt() not in QUALIFIERS:
            tag = self.txt()
            self.i += 1
        if self.txt() == '{':
            self.skip_balanced()
        ty = prim('int')
        if tag:
            self.tags[('enum', tag)] = ty
        return ty, f'enum {tag or ""}'.strip()

    def _struct_or_union(self, kw):
        line = self.tok()[2]
        self.i += 1
        tag = None
        if self.tok()[0] == 'id' and self.txt() not in QUALIFIERS:
            tag = self.txt()
            self.i += 1
        if self.txt() != '{':
            ty = self.lookup_tag(kw, tag) or Type(
                'unknown', f'{kw} {tag}', None, None)
            return ty, f'{kw} {tag or ""}'.strip()
        ty = self._aggregate_body(kw, tag, line)
        if tag:
            self.tags[(kw, tag)] = ty
        return ty, f'{kw} {tag or ""}'.strip()

    def _aggregate_body(self, kw, tag, line):
        """Lays out `{ ... }` as MSVC would, with the pack in force at each
        member's own line."""
        assert self.txt() == '{'
        self.i += 1
        off = 0
        maxalign = 1
        size = 0
        fields = []
        unsized = None
        while not self.at_end() and self.txt() != '}':
            if self.txt() == ';':
                self.i += 1
                continue
            start = self.i
            base, _tn, _td, _ex = self._specifier()
            if base is None:
                self.i = start
                self.skip_to_semi()
                continue
            members = []
            while not self.at_end():
                nptr, nm, count, is_func = self._declarator()
                mty = PTR if (nptr or is_func) else base
                members.append((nm, mty, count))
                if self.txt() == ',':
                    self.i += 1
                    continue
                break
            if self.txt() == ':':                  # a bitfield: out of scope
                unsized = unsized or 'bitfield'
                self.skip_to_semi()
                continue
            if self.txt() == ';':
                self.i += 1
            mline = self.toks[start][2]
            pack = self.tk.pack_at(mline)
            for nm, mty, count in members:
                if mty.size is None or count is None:
                    unsized = unsized or (mty.name or '?')
                    continue
                esz, eal = mty.size, mty.align
                al = min(pack, eal)
                off = (off + al - 1) // al * al
                fields.append((nm, off, mty, count))
                if kw == 'union':
                    size = max(size, esz * count)
                    off = 0
                else:
                    off += esz * count
                    size = off
                maxalign = max(maxalign, al)
        if self.txt() == '}':
            self.i += 1
        if unsized is not None:
            return Type('unknown', f'{kw} {tag or "<anon>"}', None, None,
                        fields, f'{self.file}:{line}')
        size = (size + maxalign - 1) // maxalign * maxalign
        return Type('union' if kw == 'union' else 'struct',
                    f'{kw} {tag or "<anon>"}', size, maxalign, fields,
                    f'{self.file}:{line}')

    # -- declarators
    #
    # A C declarator is recursive and the old flat `(nptr, name, count)` model
    # could not express it: `int (*p)[2]` is a POINTER TO an array of two ints
    # (4 bytes) and came out as `int *p[2]`, an ARRAY OF two pointers (8), which
    # moved every field after it and invented two pointer words
    # (docs/lanes/scope-port-m8.md M8-3, `MeshDesc` 0x28 instead of 0x1c).
    #
    # So parse the declarator into a tree and evaluate it by the language's own
    # rule, which is outside-in: a declarator D over a base type T has
    #
    #   D = * D1          -> type(D1, pointer to T)
    #   D = D1 [N]        -> type(D1, array N of T)
    #   D = D1 (params)   -> type(D1, function returning T)
    #   D = ( D1 )        -> type(D1, T)
    #   D = identifier    -> the name, with type T
    #
    # On ILP32 every pointer is four bytes, so "pointer to T" collapses to PTR
    # and the target type is never needed -- which is why the tree only has to
    # be deep enough to get the ORDER of the derivations right.

    def _decl_tree(self):
        """The declarator as a tree: ('name', str|None) | ('ptr', sub) |
        ('arr', count|None, sub) | ('fn', sub). Consumes the declarator."""
        while self.txt() in QUALIFIERS:
            self.i += 1
        if self.txt() == '*':
            self.i += 1
            return ('ptr', self._decl_tree())
        return self._direct_decl_tree()

    def _direct_decl_tree(self):
        node = ('name', None)
        if self.txt() == '(':
            # A parenthesised declarator. Before any identifier has been seen a
            # `(` cannot be a parameter list -- that suffix is handled below,
            # after the direct declarator -- so this is always a grouping.
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
                self.skip_balanced('(', ')')
                node = ('fn', node)
            else:
                return node

    @staticmethod
    def _eval_decl_tree(node):
        """`(nptr, name, count, is_func)` -- the declared object's own shape.

        The derivations are applied outside-in, so the type under construction
        is "array of `dims` of `core`" and each derivation rewrites it:
        `* D` makes the core a pointer and THROWS AWAY the dimensions it had
        (they belonged to the pointer's target), `D[N]` adds a dimension, and
        `D(...)` makes it a function. The type the identifier ends up with is
        whatever is left when the walk reaches it, which is why `int (*p)[2]`
        is one pointer (`ptr` is the last derivation applied) and `int *p[2]`
        is two (`arr` is)."""
        core = None                  # None = the specifier's type, or PTR/FUNC
        dims = []
        while node[0] != 'name':
            kind = node[0]
            if kind == 'ptr':
                core, dims, node = PTR, [], node[1]
            elif kind == 'fn':
                core, dims, node = FUNC, [], node[1]
            else:                                    # ('arr', count, sub)
                dims.append(node[1])
                node = node[2]
        count = 1
        for n in dims:
            count = None if (n is None or count is None) else count * n
        return 1 if core is PTR else 0, node[1], count, core is FUNC

    def _declarator(self):
        """Returns (pointer-ness, name, element count, is_function). The two
        callers turn that back into a type: `PTR` when pointer-ness is set,
        otherwise the specifier's type, `count` of them. `is_function` means a
        function DECLARATOR (`int f(void)`), never a pointer to one."""
        return self._eval_decl_tree(self._decl_tree())

    def _declaration(self):
        """One file-scope declaration. True when it consumed a statement."""
        start = self.i
        line0 = self.tok()[2]
        base, typename, is_typedef, is_extern = self._specifier()
        if base is None:
            self.i = start
            return False
        last_fn = None
        while not self.at_end():
            nptr, name, count, is_func = self._declarator()
            ty = PTR if nptr else base
            if is_func and name and not is_typedef:
                last_fn = (name, self.tok()[2])
            if is_typedef and name:
                self.typedefs[name] = (array_of(ty, count) if count != 1
                                       else ty)
                if is_func:
                    self.typedefs[name] = PTR
            elif name and not is_func and not is_typedef:
                addr = self._addr_for(line0)
                if addr is not None and is_extern:
                    self.objects.append(Object(
                        name, typename, ty, count, addr, self.file,
                        self.tok()[2]))
            if self.txt() == ',':
                self.i += 1
                continue
            break
        if self.txt() == '{':                      # a function body
            first = self.i
            self.skip_balanced()
            if last_fn:
                self.functions.append((last_fn[0], last_fn[1], first, self.i))
            return True
        self.skip_to_semi()
        return True

    def _addr_for(self, line0):
        """The `/* 0x... */` address of the statement that started at `line0`
        and ends at the cursor -- statement-oriented, so a comment on a
        continuation line counts."""
        line1 = self.tok()[2]
        j = self.i
        while j < len(self.toks) and self.toks[j][1] != ';':
            line1 = max(line1, self.toks[j][2])
            j += 1
        if j < len(self.toks):
            line1 = max(line1, self.toks[j][2])
        for ln in range(line0, line1 + 1):
            if self.tk.addr_at.get(ln):
                return self.tk.addr_at[ln][0]
        return None


# ---- field boundaries ------------------------------------------------------

def is_field_boundary(ty, off):
    """Does `off` land on a field of `ty` (recursively, through nested
    aggregates and array elements)? An interior alias that does NOT is either a
    byte/word name for part of a field (legitimate) or a sign the type is
    wrong (worth a human's eye), so the table reports it."""
    if off == 0:
        return True
    if ty is None or ty.size is None or off >= ty.size:
        return False
    if ty.kind == 'array':
        esz = ty.fields[0][2].size if ty.fields else None
        if not esz:
            return False
        return is_field_boundary(ty.fields[0][2], off % esz)
    if ty.kind in ('struct', 'union'):
        for _nm, foff, fty, count in ty.fields:
            span = fty.size * count
            if foff <= off < foff + span:
                inner = (off - foff) % fty.size if fty.size else 0
                if is_field_boundary(fty, inner):
                    return True
        return False
    return False


def has_pointer(ty, _depth=0):
    """Does `ty` contain a pointer anywhere inside it? Cheap pre-test so that
    `pointer_offsets` never walks an array of a pointer-free element type."""
    if ty is None or ty.size is None or _depth > MAX_WALK_DEPTH:
        return False
    if ty.kind == 'ptr':
        return True
    if ty.kind in ('struct', 'union', 'array'):
        return any(has_pointer(f[2], _depth + 1) for f in ty.fields)
    return False


MAX_WALK_DEPTH = 16


def pointer_offsets(ty):
    """`(offsets, ambiguous)` -- the byte offsets inside `ty` of every field
    the declaration says is a POINTER, walking nested aggregates and every
    element of every array.

    This is the other half of the extents work: `sizeof` says how far an object
    reaches, and this says which of its words hold addresses that have to be
    re-pointed at the rebuilt symbols. Before it, `gen_link.py` could only read
    pointer-ness off a TOP-LEVEL declaration (`extern char* g_x[3]`), so a
    `char*` MEMBER of a struct kept the original binary's address -- which is
    what made `g_low_markers[i].lit_name` a raw x86 VA and hung the park
    (docs/lanes/scope-port-b8.md).

    `ambiguous` is the offsets a UNION makes undecidable: one arm says pointer
    and another says it is not. The caller must not re-point those (a wrong
    re-point writes a new value where the image had a plain number), and they
    want a human's eye; the sources have no such union today, measured.
    """
    ptr, nonptr = set(), set()

    def walk(ty, base, depth=0):
        if ty is None or ty.size is None or depth > MAX_WALK_DEPTH:
            return
        if ty.kind == 'ptr':
            ptr.add(base)
            return
        if ty.kind == 'array':
            if not ty.fields:
                return
            ety = ty.fields[0][2]
            if not ety.size or not has_pointer(ety):
                nonptr.add(base)
                return
            for i in range(ty.size // ety.size):
                walk(ety, base + i * ety.size, depth + 1)
            return
        if ty.kind in ('struct', 'union'):
            for _nm, foff, fty, count in ty.fields:
                if fty.size is None:
                    continue
                if not has_pointer(fty):
                    nonptr.add(base + foff)       # a union arm that is not one
                    continue
                for i in range(count):
                    walk(fty, base + foff + i * fty.size, depth + 1)
            return
        nonptr.add(base)

    walk(ty, 0)
    amb = sorted(ptr & nonptr)
    return sorted(ptr - set(amb)), amb


def field_name_at(ty, off):
    """A readable `a.b[2].c` for `off`, or '' when it is not a boundary."""
    if ty is None or ty.size is None or off >= ty.size:
        return ''
    if off == 0 and ty.kind not in ('struct', 'union', 'array'):
        return ''
    if ty.kind == 'array':
        if not ty.fields:
            return ''
        esz = ty.fields[0][2].size
        if not esz:
            return ''
        sub = field_name_at(ty.fields[0][2], off % esz)
        return f'[{off // esz}]' + (('.' + sub) if sub else '')
    if ty.kind in ('struct', 'union'):
        for nm, foff, fty, count in ty.fields:
            span = fty.size * count
            if foff <= off < foff + span:
                rel = off - foff
                if count > 1 and fty.size:
                    idx, rel = divmod(rel, fty.size)
                    sub = field_name_at(fty, rel)
                    return f'{nm or "?"}[{idx}]' + (('.' + sub) if sub else '')
                sub = field_name_at(fty, rel)
                return (nm or '?') + (('.' + sub) if sub else '')
    return ''


# ---- the scan --------------------------------------------------------------

class Extent:
    """The extent of one address, with every citation behind it."""

    __slots__ = ('addr', 'size', 'cites', 'type', 'disagree')

    def __init__(self, addr):
        self.addr = addr
        self.size = 0
        self.cites = []        # (extent, name, typename, decl_where, def_where)
        self.type = None       # the Type of the widest citation
        self.disagree = []     # (extent, name, where) rows that are smaller

    def add(self, obj):
        ext = obj.extent
        if ext is None:
            return
        cite = (ext, obj.name, obj.typename,
                f'{obj.file}:{obj.line}', obj.def_where or '')
        self.cites.append(cite)
        if ext > self.size:
            if self.size:
                self.disagree.append((self.size, self.size, 'superseded'))
            self.size = ext
            # `extern FXEntry g_copters_fx[4]` is an ARRAY here, not one
            # FXEntry: the field-boundary check walks this type, and against
            # the element type alone every offset past the first element reads
            # as "not a field".
            self.type = (array_of(obj.type, obj.count) if obj.count != 1
                         else obj.type)

    def citation(self):
        """The one-line citation gen_link puts in the generated comment."""
        best = max(self.cites, key=lambda c: c[0]) if self.cites else None
        if not best:
            return ''
        ext, name, typename, where, defwhere = best
        s = f'{typename} {name}, {where}'
        if defwhere:
            s += f' (def {defwhere})'
        others = sorted({c[0] for c in self.cites} - {ext})
        if others:
            s += '; other TUs say ' + ', '.join(str(o) for o in others)
        return s


def parse_tree(src=SRC):
    """Returns (tus, shared) for LEGOLAND/*.c and *.h."""
    headers = {}
    for path in sorted(glob.glob(os.path.join(src, '*.h'))):
        headers[os.path.basename(path)] = TU(path)
    shared = headers.get('legoland.h')
    tus = []
    for path in sorted(glob.glob(os.path.join(src, '*.c'))):
        tus.append(TU(path, shared=shared))
    return tus, headers


def scan(src=SRC, tus=None):
    """addr -> Extent for every extern data declaration whose type has a
    computable size, plus the per-address disagreements."""
    if tus is None:
        tus, headers = parse_tree(src)
        tus = tus + list(headers.values())
    out = {}
    for tu in tus:
        for obj in tu.objects:
            if obj.extent is None:
                continue
            out.setdefault(obj.addr, Extent(obj.addr)).add(obj)
    return out


ASSIGN_OPS = {'=', '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=', '<<=',
              '>>=', '++', '--'}
WRITE_CALLS = {'memset', 'memcpy', 'memmove', 'strcpy', 'strncpy', 'strcat',
               'sprintf', 'Format', 'sprintf_w', 'ReadFile', 'fread'}


def uses_in_function(tu, first, last, names):
    """For one function body, name -> the set of 'r'/'w' uses in it.

    The hazard A3 measured needs a STORE through one name and a LOAD through
    another in the same function, so the direction matters. Written means: the
    name (after any `.f`, `[i]`, `->f` chain) is the left side of an assignment
    or an increment, or its address is the destination argument of a `memset`/
    `memcpy`-shaped call."""
    out = collections.defaultdict(set)
    toks = tu.toks
    for i in range(first, min(last, len(toks))):
        kind, t, _ln = toks[i]
        if kind != 'id' or t not in names:
            continue
        j = i + 1
        while j < last:
            if toks[j][1] in ('.', '->'):
                j += 2
            elif toks[j][1] == '[':
                depth = 0
                while j < last:
                    if toks[j][1] == '[':
                        depth += 1
                    elif toks[j][1] == ']':
                        depth -= 1
                        if depth == 0:
                            j += 1
                            break
                    j += 1
            else:
                break
        nxt = toks[j][1] if j < last else ''
        if nxt in ASSIGN_OPS:
            out[t].add('w')
            continue
        # memset(&g_x, ...) / memcpy(g_x, ...): a write through the first arg
        k = i - 1
        amp = toks[k][1] == '&' if k >= first else False
        if amp:
            k -= 1
        if k - 1 >= first and toks[k][1] == '(' and toks[k - 1][1] in WRITE_CALLS:
            out[t].add('w')
            continue
        out[t].add('r')
    return out


def overlap_pairs(src=SRC, tus=None):
    """Every pair of names ONE translation unit declares whose storage
    overlaps after the merge -- the optimiser hazard A3 documented.

    Two names at the SAME address are aliases by construction (the census's
    "stale extern names"); a name INSIDE another's declared extent becomes an
    interior alias. Either way the C type system sees two distinct objects, so
    at -O2 a store through one and a load through the other in the same
    function may keep the stale value. Returns a list of dicts."""
    if tus is None:
        tus, headers = parse_tree(src)
    ext = scan(src, tus)
    rows = []
    for tu in tus:
        objs = sorted(tu.objects, key=lambda o: (o.addr, o.name))
        byname = {}
        for o in objs:
            byname.setdefault(o.name, o)
        names = set(byname)
        fnuse = None
        for i, a in enumerate(objs):
            a_ext = ext[a.addr].size if a.addr in ext else (a.extent or 0)
            for b in objs[i + 1:]:
                if b.name == a.name:
                    continue
                same = b.addr == a.addr
                if not same and b.addr >= a.addr + max(a_ext, 1):
                    continue
                if fnuse is None:
                    fnuse = [(nm, ln, uses_in_function(tu, f, l, names))
                             for nm, ln, f, l in tu.functions]
                both = []
                for fn, ln, uses in fnuse:
                    if a.name in uses and b.name in uses:
                        ua = ''.join(sorted(uses[a.name]))
                        ub = ''.join(sorted(uses[b.name]))
                        both.append((fn, ln, ua, ub))
                rows.append({
                    'tu': tu.file,
                    'a': a.name, 'b': b.name,
                    'a_addr': a.addr, 'b_addr': b.addr,
                    'off': b.addr - a.addr,
                    'kind': 'same-address' if same else 'interior',
                    'a_type': a.typename, 'b_type': b.typename,
                    'a_ext': a_ext,
                    'funcs': both,
                    'hazard': any('w' in ua or 'w' in ub
                                  for _f, _l, ua, ub in both),
                })
    return rows


def unsized_objects(src=SRC, tus=None):
    """addr -> [(name, typename, 'file:line')] for every extern data
    declaration whose type this module cannot size, and which no OTHER TU sizes
    either. These are the objects still at the mercy of the gap tiling: a
    `windows.h` struct, an array whose bound is a macro, a type declared in a
    file that never lays it out. They are the residue of the split-record class,
    and naming them is the point -- `gen/extents.md` prints the ones with a
    named address close enough behind them to be a split record."""
    if tus is None:
        tus, headers = parse_tree(src)
        tus = tus + list(headers.values())
    sized = scan(src, tus)
    out = collections.defaultdict(list)
    for tu in tus:
        for obj in tu.objects:
            if obj.extent is None and obj.addr not in sized:
                out[obj.addr].append(
                    (obj.name, obj.typename, f'{obj.file}:{obj.line}'))
    return dict(out)


def aggregate_extents(src=SRC, tus=None):
    """The subset of `scan()` whose type is an AGGREGATE (struct/union/array of
    them) -- the extents the hand table used to carry."""
    ext = scan(src, tus)
    return {a: e for a, e in ext.items()
            if e.type is not None and e.type.kind in ('struct', 'union')}


# ---- selftest --------------------------------------------------------------

SELFTEST = r"""
#pragma pack(push, 1)
typedef struct Packed { char a; int b; short c; } Packed;   /* 7 */
#pragma pack(pop)
typedef struct Nat { char a; int b; short c; } Nat;          /* 12 */
typedef struct Pos { int x; int y; } Pos;                    /* 8 */
typedef struct Nest { char a; Pos p; double d; } Nest;        /* 24 */
typedef union U { char c[5]; int i; } U;                      /* 8 */
typedef struct Arr { Pos p[3]; char t; } Arr;                 /* 28 */
typedef struct Fp { int (*cb)(int, int); char* s; } Fp;        /* 8 */
typedef struct Dim { char b[0x10]; short w[2 * 3]; } Dim;      /* 28 */
typedef struct Unsized { struct NotHere x; int y; } Unsized;   /* none */
typedef int (*CbType)(void);
typedef struct WithCb { CbType f; int n; } WithCb;             /* 8 */
typedef struct Anon { int a; struct { int b; int c; } s; } Anon; /* 12 */
extern Packed g_packed;          /* 0x00400100 */
extern Nat    g_nat[4];          /* 0x00400200 */
extern Nest   g_nest;            /* 0x00400300 */
extern U      g_u;               /* 0x00400400 */
extern Arr    g_arr;             /* 0x00400500 */
extern Fp     g_fp;              /* 0x00400600 */
extern Dim    g_dim;             /* 0x00400700 */
extern Unsized g_unsized;        /* 0x00400800 */
extern WithCb g_withcb;          /* 0x00400900 */
extern Anon   g_anon;            /* 0x00400a00 */
extern struct Later { int a; int b; } g_later;   /* 0x00400b00 */
extern Nat
    g_continued                  /* 0x00400c00  the address on a
                                  * continuation line */
    ;
static Nat g_not_extern;         /* 0x00400d00 */
extern int  g_scalar;            /* 0x00400e00 */
extern char* g_ptr;              /* 0x00400f00 */
extern Nat  g_func(void);        /* 0x00401000 */
/* pointer fields: the record whose name pointers hung the park, and a union
 * that makes one word undecidable. */
typedef struct Marker {
    const char* lit_name;   /* +0x00 */
    const char* dim_name;   /* +0x04 */
    int         str_id;     /* +0x08 */
    int         x;          /* +0x0c */
    int         y;          /* +0x10 */
    void*       lit;        /* +0x14 */
    void*       dim;        /* +0x18 */
} Marker;                   /* 0x1c */
typedef union Amb { char* p; int i; } Amb;
typedef struct HasAmb { int n; Amb u; char* s; } HasAmb;   /* 12 */
extern Marker  g_markers[5];     /* 0x00402000 */
extern HasAmb  g_hasamb;         /* 0x00402100 */
extern char*   g_ptrs[3];        /* 0x00402200 */
extern Nat     g_noptrs[4];      /* 0x00402300 */
/* Declarator shapes. A C declarator is recursive and the suffix binds to the
 * INNER declarator, so `T (*p)[N]` and `T *p[N]` are different types with
 * different sizes. Getting that wrong cost MeshDesc 12 bytes and invented
 * three pointer words out of nothing (docs/lanes/scope-port-m8.md M8-3). */
typedef struct PtrToArr {        /* 12: three pointers, NOT 3 + 2 + 3 words */
    int   (*pairs)[2];           /* +0x00  pointer to an array of two ints */
    int   (*tris)[3];            /* +0x04  pointer to an array of three */
    char* (*strs)[8];            /* +0x08  pointer to an array of eight char* */
} PtrToArr;
typedef struct ArrOfPtr {        /* 36 */
    int*  p[4];                  /* +0x00  FOUR pointers */
    int   (*cb[5])(int);         /* +0x10  FIVE function pointers */
} ArrOfPtr;
typedef struct Mixed {           /* 20 */
    int   (*p2)[2][3];           /* +0x00  one pointer, to a 2-D array */
    int   (*q[2])[3];            /* +0x04  TWO pointers, each to int[3] */
    int   (**r)[2];              /* +0x0c  pointer to pointer to array */
    int   (*fp)(int, char*);     /* +0x10  a plain function pointer */
} Mixed;
typedef struct Nested {          /* 16 */
    Pos   (*grid)[4];            /* +0x00  pointer to an array of four Pos */
    Pos*  list[3];               /* +0x04  three pointers to Pos */
} Nested;
extern PtrToArr g_ptrtoarr;      /* 0x00403000 */
extern ArrOfPtr g_arrofptr;      /* 0x00403100 */
extern Mixed    g_mixed;         /* 0x00403200 */
extern Nested   g_nested;        /* 0x00403300 */
extern int   (*g_obj_pairs)[2];  /* 0x00403400  one pointer */
extern int   *g_obj_parr[2];     /* 0x00403500  two pointers */
extern int   (*g_obj_cb[3])(int);/* 0x00403600  three function pointers */
extern Pos   (*g_obj_grid)[4];   /* 0x00403700  one pointer */
extern Nat   g_obj_2d[2][3];     /* 0x00403800  six records */
extern int   *g_fn_ret_ptr(void);/* 0x00403900  a FUNCTION, not an object */
extern int   (*g_fn_ret_fp(int))(void); /* 0x00403a00  also a function */
"""


def selftest():
    import tempfile
    d = tempfile.mkdtemp()
    p = os.path.join(d, 'selftest.c')
    open(p, 'w').write(SELFTEST)
    tu = TU(p)
    sizes = {n: (t.size, t.align) for n, t in tu.typedefs.items()}
    objs = {o.name: o for o in tu.objects}
    fails = []

    def chk(what, got, want):
        if got != want:
            fails.append(f'{what}: got {got!r}, want {want!r}')

    chk('sizeof Packed', sizes['Packed'], (7, 1))
    chk('sizeof Nat', sizes['Nat'], (12, 4))
    chk('sizeof Pos', sizes['Pos'], (8, 4))
    chk('sizeof Nest', sizes['Nest'], (24, 8))
    chk('sizeof U', sizes['U'], (8, 4))
    chk('sizeof Arr', sizes['Arr'], (28, 4))
    chk('sizeof Fp', sizes['Fp'], (8, 4))
    chk('sizeof Dim', sizes['Dim'], (28, 2))
    chk('sizeof Unsized', sizes['Unsized'], (None, None))
    chk('sizeof WithCb', sizes['WithCb'], (8, 4))
    chk('sizeof Anon', sizes['Anon'], (12, 4))
    chk('extent g_packed', objs['g_packed'].extent, 7)
    chk('extent g_nat[4]', objs['g_nat'].extent, 48)
    chk('extent g_unsized', objs['g_unsized'].extent, None)
    chk('extent g_later', objs['g_later'].extent, 8)
    chk('extent g_continued (address on a continuation line)',
        objs['g_continued'].extent, 12)
    chk('a static is not an extern', 'g_not_extern' in objs, False)
    chk('a function is not an object', 'g_func' in objs, False)
    chk('extent g_scalar', objs['g_scalar'].extent, 4)
    chk('extent g_ptr (ILP32)', objs['g_ptr'].extent, 4)
    # field boundaries
    nest = tu.typedefs['Nest']
    chk('Nest +8 is a boundary (p.y)', is_field_boundary(nest, 8), True)
    chk('Nest +5 is not', is_field_boundary(nest, 5), False)
    chk('Nest +8 names p.y', field_name_at(nest, 8), 'p.y')
    arr = tu.typedefs['Arr']
    chk('Arr +16 names p[2].x', field_name_at(arr, 16), 'p[2].x')
    chk('Arr +20 names p[2].y', field_name_at(arr, 20), 'p[2].y')

    # pointer fields -- what gen_link re-points (PORT-A7). The whole point is
    # that these offsets are NOT readable off the top-level declaration: every
    # one of g_markers' ten name pointers is a struct MEMBER.
    off, amb = pointer_offsets(objs['g_markers'].full_type)
    chk('g_markers pointer offsets', off,
        sorted(i * 0x1c + k for i in range(5) for k in (0, 4, 0x14, 0x18)))
    chk('g_markers has no ambiguous word', amb, [])
    off, amb = pointer_offsets(objs['g_ptrs'].full_type)
    chk('g_ptrs pointer offsets', off, [0, 4, 8])
    off, amb = pointer_offsets(objs['g_noptrs'].full_type)
    chk('a pointer-free array has no pointer offsets', off, [])
    off, amb = pointer_offsets(objs['g_hasamb'].full_type)
    chk('HasAmb: the union word is ambiguous, not a pointer', (off, amb),
        ([8], [4]))
    chk('a scalar pointer is offset 0', pointer_offsets(objs['g_ptr'].full_type),
        ([0], []))
    chk('has_pointer(Fp)', has_pointer(tu.typedefs['Fp']), True)
    chk('has_pointer(Nat)', has_pointer(tu.typedefs['Nat']), False)
    chk('a function pointer is a pointer field',
        pointer_offsets(tu.typedefs['WithCb']), ([0], []))

    # Declarator shapes (M8-3). `T (*p)[N]` is ONE pointer; the suffix binds to
    # the parenthesised declarator, not to the outer type. Sizes first, then
    # the pointer offsets, because the bug invented pointer words as well as
    # bytes -- and an invented pointer word is the worse half: gen_link would
    # re-point a word the image holds a float in.
    chk('sizeof PtrToArr (T (*p)[N] is one pointer)', sizes['PtrToArr'],
        (12, 4))
    chk('sizeof ArrOfPtr (T *p[N] and T (*p[N])(args))', sizes['ArrOfPtr'],
        (36, 4))
    chk('sizeof Mixed', sizes['Mixed'], (20, 4))
    chk('sizeof Nested', sizes['Nested'], (16, 4))
    chk('PtrToArr pointer offsets', pointer_offsets(tu.typedefs['PtrToArr']),
        ([0, 4, 8], []))
    chk('ArrOfPtr pointer offsets', pointer_offsets(tu.typedefs['ArrOfPtr']),
        ([0, 4, 8, 0xc, 0x10, 0x14, 0x18, 0x1c, 0x20], []))
    chk('Mixed pointer offsets', pointer_offsets(tu.typedefs['Mixed']),
        ([0, 4, 8, 0xc, 0x10], []))
    chk('Nested pointer offsets', pointer_offsets(tu.typedefs['Nested']),
        ([0, 4, 8, 0xc], []))
    chk('extent g_ptrtoarr', objs['g_ptrtoarr'].extent, 12)
    chk('extent g_arrofptr', objs['g_arrofptr'].extent, 36)
    chk('extent g_mixed', objs['g_mixed'].extent, 20)
    chk('extent g_nested', objs['g_nested'].extent, 16)
    # ... and the same shapes as the OBJECT's own declarator, where the extent
    # is what gen_link.py sizes the rebuilt block with.
    chk('extent g_obj_pairs (int (*p)[2])', objs['g_obj_pairs'].extent, 4)
    chk('extent g_obj_parr (int *p[2])', objs['g_obj_parr'].extent, 8)
    chk('extent g_obj_cb (int (*p[3])(int))', objs['g_obj_cb'].extent, 12)
    chk('extent g_obj_grid (Pos (*p)[4])', objs['g_obj_grid'].extent, 4)
    chk('extent g_obj_2d (Nat[2][3])', objs['g_obj_2d'].extent, 72)
    chk('g_obj_pairs is one pointer word',
        pointer_offsets(objs['g_obj_pairs'].full_type), ([0], []))
    chk('g_obj_parr is two pointer words',
        pointer_offsets(objs['g_obj_parr'].full_type), ([0, 4], []))
    chk('g_obj_grid points at Pos[4], so it is one word and NOT eight',
        pointer_offsets(objs['g_obj_grid'].full_type), ([0], []))
    chk('a function returning a pointer is not an object',
        'g_fn_ret_ptr' in objs, False)
    chk('a function returning a function pointer is not an object',
        'g_fn_ret_fp' in objs, False)

    # The park's record, from the real sources: LevelMarker must yield two name
    # pointers per 0x1c-byte element for all five tutorial markers.
    real_tus, real_headers = parse_tree()
    real_objs = [o for t in real_tus + list(real_headers.values())
                 for o in t.objects]
    low = [o for o in real_objs
           if o.name == 'g_low_markers' and o.addr == 0x004beca0]
    chk('g_low_markers is declared at 0x004beca0', bool(low), True)
    if low:
        off, _amb = pointer_offsets(low[0].full_type)
        # lit_name/dim_name at +0/+4 and the two Sprite* at +0x14/+0x18, per
        # 0x1c-byte record; the first two of record 1 are at +0x1c/+0x20.
        chk('g_low_markers name pointers (B2)', off[:6],
            [0, 4, 0x14, 0x18, 0x1c, 0x20])
        chk('g_low_markers has 4 pointer fields x 5 records', len(off), 20)

    # The regression test proper: the five rows the hand table carried. Each
    # must be REPRODUCED -- cited by some TU at exactly the hand value -- and
    # the extent used must cover it. (0x0080ffa0 comes out at 272 rather than
    # 270 because gamemain.c declares the same address as an opaque
    # `char bytes[0x110]`; that is the MAX rule and the disagreement is
    # reported. Both numbers cover the record.)
    real = scan()
    for addr, want, what in (
            (0x00813a40, 0xa4, 'GameInput'),
            (0x007fdea4, 0x178, 'PopUpUI'),
            (0x007cad60, 0x110, 'Profile'),
            (0x004bdd00, 12, 'BlitCtx/HitInfo'),
            (0x0080ffa0, 0x10e, 'CurProfile')):
        e = real.get(addr)
        cited = sorted({c[0] for c in e.cites}) if e else []
        chk(f'{what} @ 0x{addr:08x} is cited at {want}', want in cited, True)
        chk(f'{what} @ 0x{addr:08x} extent covers {want}',
            e is not None and e.size >= want, True)

    for f in fails:
        print('FAIL', f)
    print(f'cdecl selftest: {len(fails)} failure(s)')
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('--src', default=SRC)
    ap.add_argument('--all', action='store_true',
                    help='every extern with an extent, not just aggregates')
    ap.add_argument('--overlaps', action='store_true',
                    help='the pairs one TU declares whose storage overlaps')
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if args.overlaps:
        rows = overlap_pairs(args.src)
        haz = [r for r in rows if r['funcs']]
        print(f'{len(rows)} overlapping declaration pairs inside a single TU, '
              f'{len(haz)} of them touched by one function')
        for r in sorted(rows, key=lambda r: (not r['funcs'], r['tu'],
                                             r['a_addr'])):
            fn = '; '.join(f'{f} (line {l}): {r["a"]}={ua} {r["b"]}={ub}'
                           for f, l, ua, ub in r['funcs']) or '-'
            print(f'{r["tu"]:20s} {r["kind"]:12s} 0x{r["a_addr"]:08x} '
                  f'{r["a"]} ({r["a_type"]}, {r["a_ext"]}) + 0x{r["off"]:x} '
                  f'{r["b"]} ({r["b_type"]}) | {fn}')
        return 0
    ext = scan(args.src) if args.all else aggregate_extents(args.src)
    print(f'{len(ext)} addresses with a source-derived extent')
    for addr in sorted(ext):
        e = ext[addr]
        print(f'0x{addr:08x}  {e.size:#7x}  {e.citation()}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
