#!/usr/bin/env python3
"""Close the link: generate the C that the portable build is still missing.

Given the compiled objects of legoland_core, writes into --out:

  globals.c     storage for every global the sources only declare `extern`,
                sized by the gap to the next known address and initialised
                from the original binary's .data/.rdata bytes (zero when the
                exe is not available or the address is in BSS), plus the
                aliases for the other names the same address goes by
  aliases.c     one function forwarding to another: stale extern names whose
                address is defined under a different name
  stubs.c       trapping bodies for referenced game functions that are not
                written yet, and for externs with no address comment
  host_stubs.c  trapping bodies for Win32/DirectX imports the host shim does
                not implement yet
  manifest.md   what was generated and why

Every generated body prints `TRAP <dll> <symbol> from <callers>` on stderr and
exits 70 when reached, so a run stops at the first missing piece with its name
instead of silently misbehaving.

With --ilp32 (wasm32 / i386 builds) 4-byte words inside a global whose value
is exactly a known symbol address are emitted as `(unsigned)&Symbol`, which
re-points the original's pointer tables at the rebuilt symbols. On a 64-bit
host the raw bytes are kept and such tables are wrong by construction; the
64-bit build is a link census, not a runnable game.

**Pointers INTO a global, not AT one.** Requiring the word to equal a symbol
address exactly is not enough, and it was the port's first real blocker: the
literals a pointer table points at mostly have no extern name of their own.
`g_volume_names` (0x004bcba4) holds three `const char*` into the `.rdata`
strings "Legoland.res", "Graphics2.res", "Graphics1.res", which sit at
0x004bcbf8..0x004bcc18 — INSIDE the block this generator emits for 0x004bcbf4
(`g_map` and thirteen other names for it), because every global is sized by
the gap to the next named address and so swallows the unnamed literals that
follow it. With the words left raw the game opened `"D:\\.res"` and the browser
page died in `RES_OpenVolume`.

So a word that is a pointer is re-pointed to `base + offset` of whatever
rebuilt block contains it, and to a synthetic `ll_gap_<start>` block
(initialised from the exe like any other global) when nothing does.

Which words are pointers is decided by the game's own C, not by what the value
looks like: `scan_pointer_decls()` reads the pointer depth and the array extent
out of the `extern` declaration, so only the three words of
`extern const char* g_volume_names[3]` are candidates. Guessing from the value
instead is catastrophic here -- a three-character string at the end of a word
("tan\0" = 0x006e6174) lands squarely inside the image, and a value-only rule
re-points 1,650 words of which 1,209 are string TEXT, 264 of them inside the
`GUID_NULL` block alone. The declaration rule re-points 441, every one of them
a real string pointer.

**ONE block per object.** The same gap tiling that swallows unnamed literals
also splits a named object that several names reach into at different offsets.
`extern unsigned char g_key_state[256]` at 0x007fdda0 is also `g_left_ctrl`
(+0x1d), `g_left_shift` (+0x2a), `g_right_shift` (+0x36) and `g_right_ctrl`
(+0x9d); tiled, those become five separately aligned arrays, so a 256-byte
`GetDeviceState` write overruns a 29-byte object and every alias reads a byte
of the wrong key. `extern char g_gpu_state[0x3d8]` is the same shape over 23
addresses, one of which is the `DDSURFACEDESC` the renderer locks through, and
`util.c` clears the lot with one `memset`. So an object whose DECLARED extent
(the game's own array bounds times a known element size) contains other named
addresses is emitted once and those names become `.set name, host+off` offset
aliases -- the only interior-alias form there is, and it survives both the
Mach-O and the wasm backends. A declaration whose element type has no known
size hosts nothing and keeps the tiling, and the extent is clamped at any
address that is not an extern-only data name.

Two things are decided by the object format, not by a flag:

* **Declaration agreement.** A global re-pointed this way is emitted as
  `unsigned int[N]`, so every OTHER mention of it in the generated C (a
  forward declaration for a table that points at it, an alias of it) has to
  use that same type. The emitter therefore plans all of globals.c first,
  then writes one `extern` prologue whose types match the definitions.
* **wasm signatures.** wasm calls are type-checked: a stub or a forwarder
  whose signature differs from what the caller emitted is replaced by
  wasm-ld with a trapping stub, which would turn every `TRAP <symbol>`
  message into a bare `RuntimeError: unreachable`. When the objects are wasm
  the generator reads the signature each undefined symbol is imported with
  and emits prototypes that match it. Mach-O/ELF objects need none of this
  (the C ABI tolerates mismatched prototypes), and keep the plain
  `void name(void)` bodies and the assembler-level aliases.
"""
import argparse
import bisect
import glob
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import linkreport as lr  # noqa: E402

IMAGE_BASE = 0x400000

# An unclassified extern used as data gets a placeholder block this big: the
# sources give it no address, so neither its real size nor its contents are
# known. Generous, because some of these names are structs.
UNKNOWN_DATA_SIZE = 256

# The initialised data sections: a pointer re-pointed by offset has to land in
# one of these. .text is excluded on purpose -- a wasm function "address" is a
# table index, so an offset into the middle of a function body means nothing,
# and a word that equals a function's address exactly is handled by the
# exact-match path with a real function declaration.
DATA_LO, DATA_HI = 0x4ab000, 0x836000

# `extern <type> <name><dims>;   /* 0x... */` -- the declaration, not the value,
# is what says whether a word is a pointer. <type> must end in whitespace or a
# star, which is what separates it from <name>.
DECL_RE = re.compile(
    r'^\s*extern\s+(?P<type>[A-Za-z_][\w\s*]*?[\s*])(?P<name>[A-Za-z_]\w*)\s*'
    r'(?P<dims>(?:\[[^\]]*\])*)\s*;.*?/\*\s*(?P<addr>0x[0-9a-fA-F]+)')

# ILP32 sizes of the element types the sources declare their globals with. Only
# the types whose size is a language fact are here: a `Pos`, a `RideDef*` array
# or an `FXEntry` table has no size until somebody writes the struct, and an
# extent this table cannot compute is simply not used (see `declared_extent`).
# Every pointer is 4 bytes, which is the premise of the whole ILP32 target.
PRIM_SIZES = {
    'char': 1, 'signed char': 1, 'unsigned char': 1, 'BYTE': 1, 'bool': 1,
    'short': 2, 'unsigned short': 2, 'WORD': 2, 'SHORT': 2, 'USHORT': 2,
    'int': 4, 'unsigned int': 4, 'long': 4, 'unsigned long': 4, 'float': 4,
    'DWORD': 4, 'UINT': 4, 'BOOL': 4, 'LONG': 4, 'ULONG': 4,
    'double': 8, '__int64': 8,
}


def elem_size(typ):
    """ILP32 size of one element of `typ`, or None when it is not a language
    fact (a struct the sources have not laid out)."""
    t = ' '.join(typ.split())
    if t.endswith('*'):
        return 4
    t = ' '.join(re.sub(r'\b(?:const|volatile|static|struct|enum)\b', ' ', t).split())
    return PRIM_SIZES.get(t)


def scan_pointer_decls():
    """name -> [(address, pointer depth, element count or None, byte extent or
    None)] for every `extern` data declaration that carries an address comment.

    The count is the product of the array dimensions, 1 for a scalar and None
    for an unbounded `[]` (whose real extent is then bounded by the data: see
    `ptr_slot_count`). The byte extent is count * element size, and is None
    whenever either factor is unknown."""
    out = {}
    for path in sorted(glob.glob(os.path.join(lr.SRC, '*.c'))):
        for line in open(path, encoding='utf-8', errors='replace'):
            m = DECL_RE.match(line)
            if not m:
                continue
            count = 1
            for dim in re.findall(r'\[([^\]]*)\]', m.group('dims')):
                dim = dim.strip()
                if not dim:
                    count = None
                    break
                try:
                    count *= int(dim, 0)
                except ValueError:
                    count = None
                    break
            esz = elem_size(m.group('type'))
            out.setdefault(m.group('name'), []).append(
                (int(m.group('addr'), 16), m.group('type').count('*'), count,
                 None if count is None or esz is None else count * esz))
    return out


def sig_decl(sig):
    """(return type, [param types], [param names]) for a wasm signature, or
    (None, None, None) when it uses a type this emitter does not map.

    The signatures themselves come from linkreport.py, which reads them out of
    the wasm objects (lr.collect_wasm_sigs)."""
    params, results = sig
    if any(p not in lr.WASM_TYPES for p in params) or len(results) > 1 or \
            (results and results[0] not in lr.WASM_TYPES):
        return None, None, None
    ret = lr.WASM_TYPES[results[0]][0] if results else 'void'
    types = [lr.WASM_TYPES[p][0] for p in params]
    return ret, types, [f'a{i}' for i in range(len(params))]


def sig_params(types, names=None):
    if not types:
        return 'void'
    if names is None:
        return ', '.join(types)
    return ', '.join(f'{t} {n}' for t, n in zip(types, names))


def c_ident_ok(name):
    return name.isidentifier()


def load_image(exe):
    """Returns a function addr,size -> bytes (zeros past the raw data)."""
    if not exe or not os.path.exists(exe):
        return None
    try:
        import pefile
    except ImportError:
        print('gen_link: pefile not installed; globals are zero-initialised', file=sys.stderr)
        return None
    pe = pefile.PE(exe)
    secs = []
    for s in pe.sections:
        va = IMAGE_BASE + s.VirtualAddress
        raw = pe.__data__[s.PointerToRawData:s.PointerToRawData + s.SizeOfRawData]
        secs.append((va, va + max(s.Misc_VirtualSize, s.SizeOfRawData), raw))

    def read(addr, size):
        out = bytearray(size)
        for va, end, raw in secs:
            if va <= addr < end:
                off = addr - va
                chunk = raw[off:off + size]
                out[:len(chunk)] = chunk
                break
        return bytes(out)
    return read


def emit_bytes(name, size, data, align=16, nocommon=False):
    if data is None or not any(data):
        # `= {0}` only where it is needed: a tentative definition becomes a
        # COMMON symbol under -fcommon, and the assembler cannot resolve
        # `.set alias, common+off` at all (the alias silently stays undefined).
        # An explicit initialiser makes it a real .bss definition; the object
        # is no bigger either way.
        init = ' = {0}' if nocommon else ''
        return f'__attribute__((aligned({align}))) unsigned char {name}[{size}]{init};\n'
    # trim trailing zeros: C zero-fills the rest of the array
    n = len(data)
    while n > 0 and data[n - 1] == 0:
        n -= 1
    body = ',\n    '.join(', '.join(f'0x{b:02x}' for b in data[i:i + 16]) for i in range(0, n, 16))
    return f'__attribute__((aligned({align}))) unsigned char {name}[{size}] = {{\n    {body}\n}};\n'


def emit_words(name, size, data, resolve, n_ptr, refs, align=16, nocommon=False):
    """ILP32: 4-byte words, with addresses re-pointed at the rebuilt symbols.

    `resolve(word, is_pointer_slot)` returns `(symbol, offset)` or None; the
    first `n_ptr` words are the slots the declaration says hold pointers.
    Every symbol named here is added to `refs`; the caller declares them in
    the prologue, with the types the definitions use."""
    words = []
    for i in range(0, size, 4):
        w = struct.unpack_from('<I', data, i)[0]
        hit = resolve(w, i // 4 < n_ptr)
        if hit is None:
            words.append(f'0x{w:08x}u')
            continue
        sym, off = hit
        refs.add(sym)
        words.append(f'(unsigned int)(__UINTPTR_TYPE__)&{sym}' if off == 0 else
                     f'(unsigned int)(__UINTPTR_TYPE__)((char*)({sym}) + {off})')
    n = len(words)
    while n > 0 and words[n - 1] == '0x00000000u':
        n -= 1
    if n == 0:
        init = ' = {0}' if nocommon else ''     # see emit_bytes
        return f'__attribute__((aligned({align}))) unsigned int {name}[{size // 4}]{init};\n'
    body = ',\n    '.join(', '.join(words[i:i + 4]) for i in range(0, n, 4))
    return f'__attribute__((aligned({align}))) unsigned int {name}[{size // 4}] = {{\n    {body}\n}};\n'


def fn_alias(alias, real, sig=None, real_sig=None):
    """Make `alias` reach the body defined as `real`.

    Mach-O and ELF alias at the assembler level, which costs nothing. wasm
    has no cross-translation-unit alias (clang rejects the attribute, and
    module-level asm does not survive the wasm backend), so the alias becomes
    a forwarder -- which only works if its signature is the one the callers
    emitted, hence `sig`. `real_sig` is the declaration already written for
    `real` in this file: when it differs, the call goes through a cast."""
    if sig is not None:
        ret, types, names = sig_decl(sig)
        if ret is None:
            return f'/* {alias} -> {real}: unsupported signature {sig} */\nvoid {alias}(void) {{ {real}(); }}\n'
        # The callee is `real` itself when its declaration in this file has
        # the same signature, and a cast of its address otherwise (the two
        # prototypes disagree; on x86 cdecl they were the same call anyway).
        callee = real if real_sig == sig else \
            f'(({ret} (*)({sig_params(types)}))&{real})'
        call = f'{callee}({", ".join(names)});'
        return f'{ret} {alias}({sig_params(types, names)}) {{ ' \
               f'{"return " if ret != "void" else ""}{call} }}\n'
    return (
        f'#if defined(__APPLE__) && defined(__aarch64__)\n'
        f'__asm__(".text\\n.globl _{alias}\\n.p2align 2\\n_{alias}:\\n b _{real}\\n");\n'
        f'#elif defined(__APPLE__) && defined(__x86_64__)\n'
        f'__asm__(".text\\n.globl _{alias}\\n_{alias}:\\n jmp _{real}\\n");\n'
        f'#else\n'
        f'extern void {real}(void);\n'
        f'void {alias}(void) __attribute__((alias("{real}")));\n'
        f'#endif\n')


def data_alias(alias, real, typ, count):
    """Another name for storage THIS file defines (globals.c only: clang's
    alias attribute needs the aliasee in the same translation unit, and that
    is the only form wasm has)."""
    return (
        f'#if defined(__APPLE__)\n'
        f'__asm__(".globl _{alias}\\n.set _{alias}, _{real}\\n");\n'
        f'#else\n'
        f'extern {typ} {alias}[{count}] __attribute__((alias("{real}")));\n'
        f'#endif\n')


def data_alias_at(alias, real, off):
    """Another name for the INTERIOR of storage this file defines.

    Five names in the image are offsets into one 256-byte keyboard array
    (`g_key_state` + 0x1d/0x2a/0x36/0x9d) and fifteen objects in all are named
    this way; emitted as separate blocks they get separate addresses, so a
    256-byte `GetDeviceState` write overruns its object and every alias reads
    the wrong byte. There is no offset form of `__attribute__((alias))` and
    none in C at all, but `.set sym, real+off` survives both the Mach-O and
    the wasm backends -- a wasm data symbol is a (segment, offset) pair, so an
    interior label is exactly representable. Verified on both."""
    if off == 0:
        raise ValueError('use data_alias for a zero offset')
    return (
        f'#if defined(__APPLE__)\n'
        f'__asm__(".globl _{alias}\\n.set _{alias}, _{real}+{off}\\n");\n'
        f'#else\n'
        f'__asm__(".globl {alias}\\n.set {alias}, {real}+{off}\\n");\n'
        f'#endif\n')


def trap_body(name, dll, users, sig=None):
    """A body that prints TRAP and exits; `sig` matches the callers on wasm."""
    frm = ', '.join(users[:3]) + ('...' if len(users) > 3 else '')
    call = f'll_gen_trap("{dll}", "{name}", "{frm}");'
    if sig is not None:
        ret, types, names = sig_decl(sig)
        if ret is not None:
            tail = '' if ret == 'void' else f' return ({ret})0;'
            return f'{ret} {name}({sig_params(types, names)}) {{ {call}{tail} }}'
    return f'void {name}(void) {{ {call} }}'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('build_dir')
    ap.add_argument('--out', required=True)
    ap.add_argument('--exe', default=os.path.join(lr.ROOT, 'original', 'legoland.exe'))
    ap.add_argument('--ilp32', action='store_true')
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    objs, _, _, undefined = lr.collect_objects(args.build_dir)          # what the game references
    all_objs, defined, _, _ = lr.collect_objects(args.build_dir, only_game=False)  # what anything provides
    win32 = lr.load_win32()
    externs, defined_at, _stubs = lr.scan_sources()
    cats = lr.classify(defined, undefined, externs, defined_at, win32)
    read = load_image(args.exe)
    wasm_sigs, wasm_defs, wasm_undef_data, sig_conflicts = lr.collect_wasm_sigs(all_objs)

    def sig_of(name):
        """The signature the callers emitted for an undefined symbol."""
        return wasm_sigs.get(name) if wasm_sigs is not None else None

    def def_sig_of(name):
        """The signature the defined body really has (wasm objects only)."""
        if wasm_defs is None:
            return None
        return wasm_defs.get(name) or wasm_sigs.get(name)

    # Every address we know a symbol for: sizes come from the gaps.
    data_names = {}           # addr -> [names] (extern-only data)
    for n, (addr, kind) in externs.items():
        if kind == 'data':
            data_names.setdefault(addr, []).append(n)
    known = set(data_names) | set(defined_at) | {a for a, _ in [(x[0], 0) for x in externs.values()]}
    for _, start, end in lr.SECTIONS:
        known.add(start)
        known.add(end)
    known = sorted(known)
    next_addr = {a: known[i + 1] for i, a in enumerate(known[:-1])}

    missing_data = {n for n, _, _ in cats['game-data']}
    symbol_at = {}            # addr -> a symbol name that exists after generation
    for addr, (name, _) in defined_at.items():
        if name in defined:
            symbol_at[addr] = name
    for n, (addr, kind) in externs.items():
        if n in defined and addr not in symbol_at:
            symbol_at[addr] = n

    manifest = ['# gen_link manifest', '']
    globals_c = ['/* generated by portable/tools/gen_link.py: extern-only globals rebuilt from legoland.exe */', '']
    aliases_c = ['/* generated by portable/tools/gen_link.py: symbol forwarding */', '']
    stubs_c = ['/* generated by portable/tools/gen_link.py: unwritten game functions */',
               '#include "ll_gen.h"', '#include <stdio.h>', '#include <stdlib.h>', '',
               '/* Every generated body lands here: one line naming the DLL, the symbol',
               ' * and the sources that reference it, then a non-zero exit. */',
               'void ll_gen_trap(const char* dll, const char* name, const char* from)',
               '{',
               '    fflush(stdout);',
               '    fprintf(stderr, "TRAP %s %s from %s\\n", dll, name, from);',
               '    fflush(stderr);',
               '    exit(70);',
               '}', '']
    host_c = ['/* generated by portable/tools/gen_link.py: host API the shim does not implement yet */',
              '#include "ll_gen.h"', '']

    # The declaration decides which words are pointers (see scan_pointer_decls
    # and the module docstring) and also how far an object extends, which is
    # what the interior-alias pass below needs.
    decls = scan_pointer_decls()
    fn_names = {name for name, _file in defined_at.values()}
    fn_names |= {n for n, (_a, kind) in externs.items() if kind == 'fn'}

    def declared_extent(addr):
        """The byte extent the game's own declarations give the object at
        `addr`, or 0 when no declaration there has a computable size."""
        best = 0
        for nm in data_names.get(addr, ()):
            for daddr, _depth, _count, nbytes in decls.get(nm, []):
                if daddr == addr and nbytes:
                    best = max(best, nbytes)
        return best

    # ---- ONE object per object: interior aliases ---------------------------
    # Every global is otherwise sized by the gap to the next named address, so
    # five names that are offsets into one array become five arrays. The image
    # says they are one: `extern unsigned char g_key_state[256]` at 0x007fdda0
    # covers `g_left_ctrl` (+0x1d), `g_left_shift` (+0x2a), `g_right_shift`
    # (+0x36) and `g_right_ctrl` (+0x9d), and `extern char g_gpu_state[0x3d8]`
    # covers 23 more addresses that `util.c` clears with one memset. An object
    # whose declared extent swallows other named addresses is emitted ONCE, and
    # those names become offset aliases of it.
    #
    # A declaration with no computable extent (a struct the sources have not
    # laid out) hosts nothing; the gap tiling keeps it as it was. The scan stops
    # at any address that is NOT an extern-only data name -- a function, or data
    # a game object defines -- and clamps the extent there rather than claiming
    # storage somebody else owns.
    absorbed = {}             # absorbed addr -> host addr
    interiors = {}            # host addr -> [(name, offset)]
    host_end = {}             # host addr -> end address the merge must cover
    for addr in sorted(data_names):
        if addr in absorbed:
            continue
        ext = declared_extent(addr)
        if ext <= 1:
            continue
        names = sorted(data_names[addr])
        if any(n in defined for n in names) or \
                not [n for n in names if n in missing_data and c_ident_ok(n)]:
            continue          # no block is emitted here, so it can host nothing
        taken, end = [], addr + ext
        i = bisect.bisect_right(known, addr)
        while i < len(known) and known[i] < end:
            k = known[i]
            if k not in data_names or any(n in defined for n in data_names[k]):
                end = k       # not ours: clamp the extent here
                break
            taken.append(k)
            end = max(end, next_addr.get(k, k + 4))
            i += 1
        if not taken:
            continue
        for k in taken:
            absorbed[k] = addr
            interiors.setdefault(addr, []).extend(
                (n, k - addr) for n in sorted(data_names[k])
                if n in missing_data and c_ident_ok(n))
        host_end[addr] = end

    # ---- globals: plan everything before emitting anything ----------------
    # A re-pointed global is `unsigned int[N]`, a plain one `unsigned char[N]`,
    # and the same symbol may be named again later (by a table that points at
    # it, or by an alias of it). Planning first means the prologue's extern
    # declarations can use the types the definitions actually have.
    plan = []                 # (addr, primary, size, data, rest, mode, interior)
    cross_tu_alias = []       # (alias, real): the game defines the real name
    for addr in sorted(data_names):
        if addr in absorbed:
            continue          # storage comes from the object that contains it
        names = sorted(data_names[addr])
        want = [n for n in names if n in missing_data and c_ident_ok(n)]
        if not want:
            continue
        existing = [n for n in names if n in defined]
        if existing:
            cross_tu_alias += [(n, existing[0]) for n in want]
            symbol_at.setdefault(addr, existing[0])
            continue
        primary, rest = want[0], want[1:]
        size = max(next_addr.get(addr, 4), host_end.get(addr, 0)) - addr
        if size <= 0:
            size = 4
        data = read(addr, size) if read else None
        words = bool(args.ilp32 and data is not None and size % 4 == 0 and addr % 4 == 0)
        plan.append((addr, primary, size, data, rest, words, interiors.get(addr, [])))
        symbol_at[addr] = primary

    # ---- which words are pointers, and what they point INTO ----------------
    # the leading `n_ptr` words of a global are pointers when any extern
    # declaration of a name at that address has pointer depth >= 1. An
    # unbounded `[]` is bounded by the data itself -- the table ends at the
    # first word that could not be a pointer, which is where the literals it
    # points at usually begin.

    def ptr_slot_count(addr, names, size, data):
        declared, unbounded = 0, False
        for nm in names:
            for daddr, depth, count, _nbytes in decls.get(nm, []):
                if daddr != addr or depth < 1:
                    continue
                if count is None:
                    unbounded = True
                else:
                    declared = max(declared, count)
        n = min(max(declared, size // 4 if unbounded else 0), size // 4)
        if unbounded and data is not None:
            for k in range(n):                      # stop where the table does
                w = struct.unpack_from('<I', data, k * 4)[0]
                if not (w == 0 or DATA_LO <= w < DATA_HI or w in symbol_at):
                    return k
        return n

    blocks = {}               # start -> (name, size): every rebuilt data block
    for addr, primary, size, _d, _r, _w, _i in plan:
        blocks[addr] = (primary, size)
    block_starts = sorted(blocks)
    gaps = []                 # (addr, name, size, data): synthesised blocks
    ptr_cache = {}            # target address -> (symbol, offset) or None

    def containing(addr):
        i = bisect.bisect_right(block_starts, addr) - 1
        if i < 0:
            return None
        start = block_starts[i]
        name, size = blocks[start]
        return (name, addr - start) if addr < start + size else None

    def resolve_pointer(w):
        """(symbol, offset) for a pointer into the image, or None."""
        if w in ptr_cache:
            return ptr_cache[w]
        hit = containing(w)
        if hit is None:
            # Nothing rebuilt covers it. The region between the two nearest
            # known addresses is unnamed data (a string literal, a switch
            # table); give it a name of its own, initialised from the exe.
            i = bisect.bisect_right(known, w) - 1
            base = known[i] if i >= 0 else None
            if base is None or read is None:
                hit = None
            else:
                end = next_addr.get(base, base + 4)
                owner = symbol_at.get(base)
                if owner and owner not in fn_names:
                    # The game itself defines an object at `base`: an offset
                    # into it is an offset into that object, which is right.
                    hit = (owner, w - base)
                else:
                    name = f'll_gap_{base:08x}'
                    if base not in blocks:
                        gaps.append((base, name, end - base, read(base, end - base)))
                        blocks[base] = (name, end - base)
                        block_starts.insert(bisect.bisect_left(block_starts, base), base)
                    hit = (blocks[base][0], w - base)
        ptr_cache[w] = hit
        return hit

    n_exact = n_interior = n_raw_ptr = 0

    def resolve(w, is_ptr_slot):
        nonlocal n_exact, n_interior, n_raw_ptr
        if w in symbol_at:                          # a symbol's own address
            n_exact += 1
            return (symbol_at[w], 0)
        if not is_ptr_slot or not (DATA_LO <= w < DATA_HI):
            if is_ptr_slot and w:
                n_raw_ptr += 1
            return None
        hit = resolve_pointer(w)
        if hit is None:
            n_raw_ptr += 1
            return None
        n_interior += 1
        return hit

    # Resolution runs over every pointer slot BEFORE anything is emitted, so
    # the synthesised gap blocks are part of the plan (and of the prologue's
    # declarations) by the time the first global is written.
    n_ptr_of = {}
    for addr, primary, size, data, _rest, words, _i in plan:
        n = ptr_slot_count(addr, sorted(data_names[addr]), size, data) if words else 0
        n_ptr_of[addr] = n
        for k in range(n):
            w = struct.unpack_from('<I', data, k * 4)[0]
            if w and w not in symbol_at and DATA_LO <= w < DATA_HI:
                resolve_pointer(w)
    n_exact = n_interior = n_raw_ptr = 0          # the real count is the emission
    for base, name, size, data in sorted(gaps):
        # Bytes, never words: a gap block is unnamed data (string literals,
        # switch tables), nothing declares it, so it has no pointer slots and
        # its start need not even be 4-aligned.
        plan.append((base, name, size, data, [], False, []))
    plan.sort(key=lambda p: p[0])

    decl_of = {}              # name -> (type, element count) as defined here
    for _addr, primary, size, _data, _rest, words, _i in plan:
        decl_of[primary] = ('unsigned int', size // 4) if words else ('unsigned char', size)

    refs = set()
    n_def = n_alias_data = n_alias_interior = total_bytes = 0
    body = []
    for addr, primary, size, data, rest, words, inter in plan:
        sec = next((s for s, a, b in lr.SECTIONS if a <= addr < b), '?')
        body.append(f'/* 0x{addr:08x} {sec} {size} bytes{" (+" + ", ".join(rest) + ")" if rest else ""}'
                    f'{" interior: " + ", ".join(f"{n}+0x{o:x}" for n, o in inter) if inter else ""} */')
        if words:
            body.append(emit_words(primary, size, data, resolve,
                                   n_ptr_of.get(addr, 0), refs, nocommon=bool(inter)))
        else:
            body.append(emit_bytes(primary, size, data, nocommon=bool(inter)))
        typ, count = decl_of[primary]
        for n in rest:
            body.append(data_alias(n, primary, typ, count))
            n_alias_data += 1
        for n, off in inter:
            body.append(data_alias_at(n, primary, off))
            n_alias_interior += 1
        n_def += 1
        total_bytes += size

    # Functions and data are different kinds of symbol on wasm (a function
    # pointer in a table is a table index, not an address), so a re-pointed
    # word that names a function has to be declared as one -- with the
    # signature the body has, or wasm-ld puts a trapping stub in the table.
    # (`fn_names` is built above, before the pointer resolution that needs it.)
    prologue = ['/* Forward declarations for every symbol the tables below point at.',
                ' * Types match the definitions: a re-pointed global is unsigned int[],',
                ' * and a function keeps its real signature. */']
    for sym in sorted(refs):
        if sym in decl_of:
            typ, count = decl_of[sym]
            prologue.append(f'extern {typ} {sym}[{count}];')
        elif sym in fn_names:
            sig = def_sig_of(sym)
            ret, types, _ = sig_decl(sig) if sig else (None, None, None)
            prologue.append(f'extern {ret} {sym}({sig_params(types)});' if ret is not None
                            else f'extern void {sym}();')
        else:                  # data the game objects define
            prologue.append(f'extern unsigned char {sym}[];')
    globals_c += prologue + [''] + body

    # ---- function aliases -------------------------------------------------
    # CRT thunks filed as unwritten game functions. `MemAlloc` (0x0049e4ff),
    # `Format` (0x0049e573) and ten more are not missing bodies at all: each is
    # a second name for a CRT function that the sources ALSO declare at that
    # same address (startup.c has `malloc` at 0x0049e4ff and `sprintf` at
    # 0x0049e573). Forwarding them is the same job as a stale extern name, so
    # they go through the alias machinery rather than trapping -- which is what
    # every loader in the game is waiting for. The printf-shaped ones work
    # because a variadic callee has a fixed wasm signature (its arguments
    # arrive through one pointer), so the forwarder passes exactly what it was
    # given.
    fn_at = {}
    for nm, (addr, kind) in externs.items():
        if kind == 'fn':
            fn_at.setdefault(addr, []).append(nm)
    crt_thunks = []           # (name, crt name, '0x...')
    game_fn = []
    for n, detail, users in cats['game-fn']:
        crt = sorted(m for m in fn_at.get(int(detail, 16), [])
                     if m != n and lr.is_crt(m) and c_ident_ok(m))
        if crt:
            crt_thunks.append((n, crt[0], detail))
        else:
            game_fn.append((n, detail, users))

    n_alias_fn = 0
    declared = {}             # real -> the signature aliases.c declared it with
    for n, detail, _ in [(n, f'{a} is {m} (CRT)', None) for n, m, a in crt_thunks] + \
                        [(n, d, u) for n, d, u in cats['alias']]:
        real = detail.split(' is ')[1].split(' (')[0]
        sig = sig_of(n)
        if sig is not None and real not in declared:
            # One declaration of the real body per file, with the signature
            # the body actually has; an alias whose callers disagree casts.
            rsig = def_sig_of(real) or sig
            ret, types, _ = sig_decl(rsig)
            declared[real] = rsig if ret is not None else None
            aliases_c.append(f'extern {ret} {real}({sig_params(types)});' if ret is not None
                             else f'extern void {real}();')
        aliases_c.append(fn_alias(n, real, sig, declared.get(real)))
        n_alias_fn += 1
    for n, real in cross_tu_alias:   # data the game defines under another name
        aliases_c.append(f'/* {n} is {real}: no cross-TU data alias exists on this target */')

    # ---- stubs ------------------------------------------------------------
    for n, detail, users in game_fn:
        stubs_c.append(trap_body(n, f'GAME {detail}', users, sig_of(n)))
    stubs_c.append('')
    stubs_c.append('/* externs with no address comment: unresolved names, see the report.')
    stubs_c.append(' * The ones the sources use as DATA (wasm says which; a Mach-O or ELF')
    stubs_c.append(' * object does not distinguish) get storage, not a trapping body --')
    stubs_c.append(' * nothing knows their real address or size, so the block is a')
    stubs_c.append(' * placeholder that keeps the link closed. */')
    n_unknown_data = 0
    for n, _, users in cats['unknown']:
        if not c_ident_ok(n):
            continue
        if wasm_undef_data is not None and n in wasm_undef_data:
            stubs_c.append(f'__attribute__((aligned(16))) unsigned char {n}[{UNKNOWN_DATA_SIZE}];'
                           f'   /* referenced as data by {", ".join(users[:3])} */')
            n_unknown_data += 1
        else:
            stubs_c.append(trap_body(n, 'GAME', users, sig_of(n)))
    for n, dll, users in cats['host']:
        if c_ident_ok(n):
            host_c.append(trap_body(n, dll, users, sig_of(n)))

    manifest += [
        f'- objects scanned: {len(objs)}',
        f'- object format: {"wasm (signature-matched stubs and forwarders)" if wasm_sigs is not None else "Mach-O/ELF (untyped stubs, assembler aliases)"}',
        f'- exe: {args.exe if read else "not available, globals zero-initialised"}',
        f'- globals defined: {n_def} ({total_bytes} bytes), data aliases: {n_alias_data}',
        f'- objects merged from interior-aliased names: {len(interiors)}'
        f' ({n_alias_interior} offset aliases)',
        f'- symbols re-pointed into (ilp32): {len(refs)}',
        f'- words re-pointed at a symbol address (ilp32): {n_exact}',
        f'- pointer words re-pointed INTO a block (ilp32): {n_interior}',
        f'- synthesised ll_gap_ blocks for unnamed data: {len(gaps)}'
        f' ({sum(g[2] for g in gaps)} bytes)',
        f'- pointer words left raw (outside {DATA_LO:#x}..{DATA_HI:#x}): {n_raw_ptr}',
        f'- function aliases (stale extern names): {n_alias_fn}',
        f'- data names the game defines under another name (no alias possible): {len(cross_tu_alias)}',
        f'- unwritten game function stubs: {len(game_fn)}',
        f'- CRT thunks forwarded instead of trapped: {len(crt_thunks)}'
        f" ({', '.join(n + ' -> ' + m for n, m, _ in crt_thunks)})",
        f"- unresolved-name stubs: {len(cats['unknown']) - n_unknown_data}"
        f" (+{n_unknown_data} placeholder data blocks)",
        f"- host API stubs: {len(cats['host'])}",
        f'- symbols imported with more than one signature (most common wins): {len(sig_conflicts)}',
        f'- pointer re-pointing (ilp32): {"on" if args.ilp32 else "off"}',
    ]
    if interiors:
        manifest += ['', '## Objects whose declared extent contains other named addresses', '',
                     'One block, the rest offset aliases of it. The extent comes from the',
                     "game's own `extern` declaration; a struct type with no known size",
                     'hosts nothing and keeps the gap tiling.', '']
        for addr in sorted(interiors):
            host = next(p[1] for p in plan if p[0] == addr)
            manifest.append(f'- `{host}` 0x{addr:08x} ({blocks[addr][1]} bytes, '
                            f'declared {declared_extent(addr)}): '
                            + ', '.join(f'`{n}`+0x{o:x}' for n, o in interiors[addr]))
    if sig_conflicts:
        manifest += ['', '## Conflicting wasm signatures', ''] + \
                    [f'- `{n}`' for n in sig_conflicts]
    for fname, text in (('globals.c', globals_c), ('aliases.c', aliases_c),
                        ('stubs.c', stubs_c), ('host_stubs.c', host_c), ('manifest.md', manifest)):
        with open(os.path.join(args.out, fname), 'w') as f:
            f.write('\n'.join(text) + '\n')
    with open(os.path.join(args.out, 'll_gen.h'), 'w') as f:
        f.write('void ll_gen_trap(const char* dll, const char* name, const char* from);\n'
                'void ll_unwritten(const char* name, unsigned int address);\n'
                'void ll_unhosted(const char* name, const char* dll);\n')
    print('\n'.join(manifest[2:]))


if __name__ == '__main__':
    main()
