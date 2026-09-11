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

**A pointer FIELD is a pointer too**, and reading pointer-ness off the
top-level declaration alone was the port's second real blocker.
`extern LevelMarker g_low_markers[5]` has pointer depth 0, so the ten
`const char*` sprite names of the two progress screens kept the ORIGINAL
binary's addresses, `LoadSprite` failed, `g_low_markers[i].lit` stayed NULL and
`while (KillSprite(NULL) == 0) ;` hung the page on BOTH doors into the park
(docs/lanes/scope-port-b8.md, docs/lanes/scope-port-a7.md). `cdecl.py` has laid
these structs out since the extents work; `cdecl.pointer_offsets` now walks the
laid-out type and says which of its words are pointers, per array element, so
the rule is the one the declaration always implied: **a word is a pointer slot
when some declaration's type puts a pointer field at that ADDRESS.** Keyed by
address, not by an offset into whichever block is emitted -- `g_level_markers`
is declared at 0x004beb80 by screens3.c and, eight bytes in and with its fields
rotated to match, at 0x004beb88 by bigscreens.c, and the block lands at the
second of those.

The value still gets one vote, and only ever a veto: a declaration that claims
a pointer where the image holds something that cannot be an address in any
layout (`void* g_music_sys` holding 1) is rejected for that array element and
reported. `gen/pointers.md` is the census -- every declared pointer word, what
became of it, and **`raw pointer words`, which must be 0**: the count of
declared pointer words inside the image that nothing re-pointed. The words left
raw on purpose are listed there with their reason (a word into `.text` is a
function-table index on wasm, so an interior offset into one is meaningless).

**ONE block per object, and the extent comes from `sizeof`.** The same gap
tiling that swallows unnamed literals also splits a named object that several
names reach into at different offsets.
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

The declared extent is `sizeof` of the declared type, computed by `cdecl.py`
from the game's own `typedef struct` definitions -- not just the array bounds of
a primitive element type, which is all this generator could see before and which
is why a struct-typed record (`GameInput`, `PopUpUI`, `Profile`, `CurProfile`,
the 12-byte hit record) had to be entered in a hand table once somebody noticed
the symptom. `gen/extents.md` is the resulting table: every object whose
computed extent exceeds the gap-tiled size, which is to say every object that
would have been split, with the citations, the disagreements between
translation units, and the residue the parser still cannot size.

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
import collections
import glob
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import linkreport as lr  # noqa: E402
import cdecl  # noqa: E402

IMAGE_BASE = 0x400000

# An unclassified extern used as data gets a placeholder block this big: the
# sources give it no address, so neither its real size nor its contents are
# known. Generous, because some of these names are structs.
UNKNOWN_DATA_SIZE = 256

# ---- records whose extent is a STRUCT: now the REGRESSION FIXTURE ----------
# `cdecl.py` computes every object's extent from the game's own `typedef struct`
# definitions, so this table is no longer how a struct gets its size. It is kept
# as the regression test for that parser: each row was established by hand, from
# two independent citations in the sources, after the split record it describes
# had broken something visible. `gen/extents.md` prints one line per row saying
# whether the parser reproduces the number, and gen_link warns on stderr if it
# does not. The extents themselves are still taken as a FLOOR (declared_extent
# maxes over all three sources), so a parser that ever stops seeing one of these
# records cannot silently re-split it.
#
# What the class was: the interior-alias pass takes an object's extent from the
# game's own declaration, which worked when the declaration is `extern unsigned
# char g_key_state[256]` -- array bounds times an element size that is a
# language fact -- and gave NOTHING for `extern GameInput g_input;`, because a
# struct type has no size until somebody parses the struct. Such an object fell
# back to the gap tiling, which sizes it by the distance to the next NAMED
# address -- and for a record whose fields other files declare individually, the
# next named address is its own SECOND FIELD. The record came out as one block
# per field, 16-byte aligned, in declaration order, and the two halves of the
# game disagreed about where every field is: the files that write
# `g_input.point` write at `g_input + 4` (inside a FOUR-byte block, i.e. into
# the padding after it), the files that read `g_gfx_point` read a different
# block entirely. Silent, and total.
#
# The rows were found by the sweep in docs/lanes/scope-port-a5.md -- every
# extern whose address comment attributes it to a record
# (`/* 0x007cad80  g_temp_profile.age */`) -- which is exactly the limitation
# the parser removes: a record whose fields were all recovered under separate
# names, with no comment tying them together, was still split and still silent.
STRUCT_EXTENTS = {
    # GameInput @ 0x00813a40, the one the front end reads every frame.
    # bighelp.c:35 lays it out to `move_tick` at +0xa0 (0x00813ae0), and
    # bubblecache.c:120 declares 0x00813a70 as `g_input.fp_h` (+0x30). Split, it
    # was nine blocks: the cursor point (+4), fp_w (+0x2c), fp_h (+0x30) and the
    # five GameButton pairs from +0x84 on were all separate objects, so
    # ReadGameButtons (bighelp.c 0x00452460) wrote a record nothing read.
    0x00813a40: (0xa4, 'GameInput, bighelp.c:35-64 (move_tick +0xa0); '
                       'bubblecache.c:120 g_input.fp_h +0x30'),
    # PopUpUI @ 0x007fdea4. popup.c:474-507 ends at `spr_happy` +0x174, whose
    # comment gives 0x007fe018 = 0x007fdea4 + 0x174; misc3.c:216 declares
    # 0x007fdfc4 as `g_popup.icon_next` (+0x120).
    0x007fdea4: (0x178, 'PopUpUI, popup.c:474-507 (spr_happy +0x174 = 0x007fe018); '
                        'misc3.c:216 g_popup.icon_next +0x120'),
    # Profile @ 0x007cad60, the PLAYER DETAILS screen's scratch profile.
    # profiles.c:64-77 is 0x110 bytes (`f10f` at +0x10f); unref7.c:276 declares
    # 0x007cad80 as `g_temp_profile.age` (+0x20, the `f20` of that layout).
    0x007cad60: (0x110, 'Profile, profiles.c:64-77 (0x110, f10f +0x10f); '
                        'unref7.c:276 g_temp_profile.age +0x20'),
    # BlitCtx / HitInfo @ 0x004bdd00 (PORT-B6): PrintSprite copies the caller's
    # 12-byte record here and UpdateFocussedIconPtr reads +0x04; split into three
    # objects, g_icon_value was never written and no front-end icon could be
    # focussed or clicked. printlist.c:61-65 lays it out to +0x08 and
    # printlist.c:245 declares g_hit_ctx here; gameframe.c:261 / workorder2.c:308
    # declare 0x004bdd08 (the last field, 4 bytes) -> 12 bytes.
    0x004bdd00: (12, 'BlitCtx/HitInfo, printlist.c:61-65 (+0x08 last field); '
                     'gameframe.c:261 g_hit_cell 0x004bdd08'),
    # CurProfile @ 0x0080ffa0 (PORT-B6): the writer stores profile_slot at +0x43,
    # bigscreens.c:416 reads g_cur_profile.profile_slot from what was a 32-byte
    # object, so EnterNewProfile never ran. bigscreens.c:73-87 / profiles.c:89-101
    # (#pragma pack(1)) end at block[200] @ +0x46 -> 0x10e; screens3.c:234 and
    # unref7.c:271 declare 0x0080ffe3 as CurProfile+0x43.
    0x0080ffa0: (0x10e, 'CurProfile, bigscreens.c:73-87 (block[200] @ +0x46); '
                        'screens3.c:234 g_profile_slot +0x43'),
}

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


# ---- the libc prototypes the game's CRT aliases reach -----------------------
#
# A CRT name the game aliases is not defined by any translation unit in the
# tree: the body comes out of libc, so **libc's prototype is the only correct
# one**. Taking the declaration from the ALIAS instead was the port's last
# wasm-ld signature mismatch (docs/lanes/scope-port-m2.md section 5):
# `DebugPrint` is a second name for 0x0049e5c5 = `printf`, spelled
# `void DebugPrint(const char*)` by logflume2.c, and the alias machinery wrote
#
#     extern void printf(unsigned int);            /* the ALIAS's signature */
#     void DebugPrint(unsigned int a0) { printf(a0); }
#
# against libc's `(i32, i32) -> i32`. wasm-ld then resolved every call through
# that declaration -- including the game's own `printf` calls from
# bigrender.c / printlist.c / unref7.c -- to a trapping stub.
#
# Each row is `(return type, fixed parameter types, variadic?)` in C, exactly as
# the C library declares it. The wasm signature follows from the row: one i32
# per pointer or int, f32/f64/i64 for the wider scalars, and -- for a variadic
# callee -- ONE extra i32, because that is how clang lowers `...` on wasm32 (the
# varargs arrive through a single pointer).
#
# Only the names the game actually reaches need a row; `crt_proto` falls back to
# the old behaviour with a manifest note for anything missing, so a new alias
# target shows up in the report instead of silently mis-declaring itself.
CRT_PROTOS = {
    # stdio
    'printf':    ('int',    ('const char*',), True),
    'fprintf':   ('int',    ('void*', 'const char*'), True),
    'sprintf':   ('int',    ('char*', 'const char*'), True),
    'vsprintf':  ('int',    ('char*', 'const char*', 'void*'), False),
    'vfprintf':  ('int',    ('void*', 'const char*', 'void*'), False),
    'sscanf':    ('int',    ('const char*', 'const char*'), True),
    'fopen':     ('void*',  ('const char*', 'const char*'), False),
    'fclose':    ('int',    ('void*',), False),
    'fread':     ('unsigned int', ('void*', 'unsigned int', 'unsigned int',
                                   'void*'), False),
    'fwrite':    ('unsigned int', ('const void*', 'unsigned int',
                                   'unsigned int', 'void*'), False),
    'fseek':     ('int',    ('void*', 'long', 'int'), False),
    'ftell':     ('long',   ('void*',), False),
    'fflush':    ('int',    ('void*',), False),
    'fgets':     ('char*',  ('char*', 'int', 'void*'), False),
    'fputs':     ('int',    ('const char*', 'void*'), False),
    'remove':    ('int',    ('const char*',), False),
    'rename':    ('int',    ('const char*', 'const char*'), False),
    # stdlib
    'malloc':    ('void*',  ('unsigned int',), False),
    'calloc':    ('void*',  ('unsigned int', 'unsigned int'), False),
    'realloc':   ('void*',  ('void*', 'unsigned int'), False),
    'free':      ('void',   ('void*',), False),
    'rand':      ('int',    (), False),
    'srand':     ('void',   ('unsigned int',), False),
    'abs':       ('int',    ('int',), False),
    'atoi':      ('int',    ('const char*',), False),
    'atol':      ('long',   ('const char*',), False),
    'atof':      ('double', ('const char*',), False),
    'strtol':    ('long',   ('const char*', 'char**', 'int'), False),
    'strtoul':   ('unsigned long', ('const char*', 'char**', 'int'), False),
    'strtod':    ('double', ('const char*', 'char**'), False),
    'exit':      ('void',   ('int',), False),
    'abort':     ('void',   (), False),
    'getenv':    ('char*',  ('const char*',), False),
    'qsort':     ('void',   ('void*', 'unsigned int', 'unsigned int',
                             'void*'), False),
    'bsearch':   ('void*',  ('const void*', 'const void*', 'unsigned int',
                             'unsigned int', 'void*'), False),
    # string / memory
    'strlen':    ('unsigned int', ('const char*',), False),
    'strcpy':    ('char*',  ('char*', 'const char*'), False),
    'strncpy':   ('char*',  ('char*', 'const char*', 'unsigned int'), False),
    'strcat':    ('char*',  ('char*', 'const char*'), False),
    'strncat':   ('char*',  ('char*', 'const char*', 'unsigned int'), False),
    'strcmp':    ('int',    ('const char*', 'const char*'), False),
    'strncmp':   ('int',    ('const char*', 'const char*', 'unsigned int'), False),
    'strchr':    ('char*',  ('const char*', 'int'), False),
    'strrchr':   ('char*',  ('const char*', 'int'), False),
    'strstr':    ('char*',  ('const char*', 'const char*'), False),
    'strtok':    ('char*',  ('char*', 'const char*'), False),
    'strdup':    ('char*',  ('const char*',), False),
    'strcasecmp':  ('int',  ('const char*', 'const char*'), False),
    'strncasecmp': ('int',  ('const char*', 'const char*', 'unsigned int'), False),
    'memset':    ('void*',  ('void*', 'int', 'unsigned int'), False),
    'memcpy':    ('void*',  ('void*', 'const void*', 'unsigned int'), False),
    'memmove':   ('void*',  ('void*', 'const void*', 'unsigned int'), False),
    'memcmp':    ('int',    ('const void*', 'const void*', 'unsigned int'), False),
    'memchr':    ('void*',  ('const void*', 'int', 'unsigned int'), False),
    # ctype
    'toupper':   ('int',    ('int',), False),
    'tolower':   ('int',    ('int',), False),
    # the MSVC spellings the game uses; these have real bodies in
    # portable/src/hostwin/msvcrt.c, so `crt_proto` only reaches them if that
    # file ever stops defining one -- the row keeps the declaration honest.
    '_stricmp':  ('int',    ('const char*', 'const char*'), False),
    '_strnicmp': ('int',    ('const char*', 'const char*', 'unsigned int'), False),
    '_strdup':   ('char*',  ('const char*',), False),
    '_strupr':   ('char*',  ('char*',), False),
    '_strlwr':   ('char*',  ('char*',), False),
    # math (f64 in, f64 out: the only rows where the wasm types are not all i32)
    'sqrt':      ('double', ('double',), False),
    'sin':       ('double', ('double',), False),
    'cos':       ('double', ('double',), False),
    'tan':       ('double', ('double',), False),
    'atan':      ('double', ('double',), False),
    'atan2':     ('double', ('double', 'double'), False),
    'pow':       ('double', ('double', 'double'), False),
    'exp':       ('double', ('double',), False),
    'log':       ('double', ('double',), False),
    'log10':     ('double', ('double',), False),
    'fabs':      ('double', ('double',), False),
    'fmod':      ('double', ('double', 'double'), False),
    'floor':     ('double', ('double',), False),
    'ceil':      ('double', ('double',), False),
}

# wasm value types, as linkreport reads them out of the type section.
W_I32, W_I64, W_F32, W_F64 = 0x7f, 0x7e, 0x7d, 0x7c

# Every C type not in here is one i32 on wasm32: `long`, `unsigned long`, every
# pointer, `int`, `unsigned int`, `char` after promotion.
C_TO_WASM = {'double': W_F64, 'float': W_F32,
             'long long': W_I64, 'unsigned long long': W_I64,
             '__int64': W_I64}


def crt_proto(name):
    """libc's own prototype for `name`, or None when this table has no row."""
    return CRT_PROTOS.get(name)


def crt_wasm_sig(proto):
    """The wasm signature libc's definition of a `CRT_PROTOS` row really has.

    A variadic callee gets ONE extra i32 parameter: clang lowers `...` on
    wasm32 by writing the variable arguments to a buffer and passing its
    address, so `int printf(const char*, ...)` is `(i32, i32) -> i32` -- which
    is exactly what wasm-ld printed for libc's printf.o."""
    cret, cparams, variadic = proto
    params = [C_TO_WASM.get(p.strip(), W_I32) for p in cparams]
    if variadic:
        params.append(W_I32)
    results = () if cret == 'void' else (C_TO_WASM.get(cret.strip(), W_I32),)
    return (tuple(params), results)


def c_cast(ctype, expr):
    """`expr` (always a wasm scalar: `unsigned int`, `double`, ...) as `ctype`.

    A pointer needs the integer to go through `__UINTPTR_TYPE__` first, or
    clang warns about an int-to-pointer conversion of the wrong width."""
    t = ctype.strip()
    if t.endswith('*'):
        return f'({t})(__UINTPTR_TYPE__)({expr})'
    return f'({t})({expr})'


def crt_alias(alias, real, sig, proto):
    """A forwarder from a game name onto a libc function, typed from LIBC.

    `sig` is the signature the game's callers emitted for `alias` -- that one is
    not negotiable, it is what the call sites encode. `proto` is libc's
    prototype for `real`. The two are bridged at the C level with per-argument
    casts, never with a cast of the function pointer: a cast call lowers to
    `call_indirect`, and binaryen's `directize` pass turns a constant-index
    `call_indirect` back into a direct call whose argument types then do not
    match, so the module fails validation 400 functions away
    (docs/lanes/scope-port-m2.md section 4).

    Two shapes, and telling them apart is the whole subtlety:

    * The alias is spelled variadic too (its wasm signature has one more
      parameter than libc's fixed count, and that last parameter IS the varargs
      pointer clang already handed it). `Format` -> `sprintf` is this: the
      pointer must be passed STRAIGHT THROUGH, so the callee is declared
      flattened -- fixed parameters plus that pointer -- which is precisely the
      wasm signature libc's variadic definition has.
    * The alias is spelled non-variadic (`DebugPrint(const char*)` -> `printf`).
      Then there is no varargs pointer to forward and the call has to go through
      the REAL variadic prototype, so that clang builds the (empty) buffer and
      emits the `(i32, i32) -> i32` call libc expects.

    The flattened spelling is declared under its own C identifier with an
    `__asm__` label naming the real symbol, so that both spellings of the same
    libc function can coexist in one translation unit (one alias of `sprintf`
    may be variadic and another not).

    Returns (declaration key, declaration, forwarder) -- the key identifies the
    SPELLING, so the caller emits each declaration once -- or None when the
    alias signature uses a wasm type this emitter does not map."""
    ret, types, names = sig_decl(sig)
    if ret is None:
        return None
    cret, cparams, variadic = proto
    note = ''
    if variadic and len(types) == len(cparams) + 1:
        # Variadic alias: declare the callee flattened and forward the buffer.
        callee = f'll_crt_{real.lstrip("_")}_va'
        decl = (f'extern {cret} {callee}({", ".join(cparams)}, void*) '
                f'__asm__("{real}");')
        args = [c_cast(t, n) for t, n in zip(cparams, names)] + \
               [c_cast('void*', names[-1])]
        return (callee, decl,
                _crt_body(alias, callee, ret, types, names, cret, args, ''))
    else:
        callee = real
        decl = (f'extern {cret} {real}('
                f'{", ".join(cparams) + (", ..." if variadic else "") or "void"});')
        args = [c_cast(t, n) for t, n in zip(cparams, names)]
        if len(types) != len(cparams):
            # An arity disagreement against libc is a game-side defect, not
            # something to bridge silently: missing arguments become 0 and extra
            # ones are dropped (which is what x86 cdecl did anyway), and the
            # manifest names the row.
            args += [c_cast(t, '0') for t in cparams[len(types):]]
            note = (f'  /* arity: callers emitted {len(types)} argument(s), '
                    f'libc takes {len(cparams)}{" + ..." if variadic else ""} */')
    return (callee, decl,
            _crt_body(alias, callee, ret, types, names, cret, args, note))


def _crt_body(alias, callee, ret, types, names, cret, args, note):
    """The forwarder body: one call, with the return value converted."""
    call = f'{callee}({", ".join(args)})'
    if ret == 'void':
        body = f'(void)({call});' if cret != 'void' else f'{call};'
    elif cret == 'void':
        body = f'{call}; return ({ret})0;'
    else:
        # A pointer return goes through __UINTPTR_TYPE__ on the way to the
        # alias's integer return type, for the same reason c_cast does.
        src = f'(__UINTPTR_TYPE__)({call})' if cret.strip().endswith('*') else call
        body = f'return ({ret})({src});'
    return f'{ret} {alias}({sig_params(types, names)}) {{ {body} }}{note}\n'


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


def emit_words(name, size, data, resolve, ptr_words, refs, align=16,
               nocommon=False, base=0):
    """ILP32: 4-byte words, with addresses re-pointed at the rebuilt symbols.

    `resolve(word, is_pointer_slot, va)` returns `(symbol, offset)` or None;
    `ptr_words` is the set of WORD INDICES the declarations say hold pointers.
    It is a set rather than a count because a block can now host interior
    aliases: `g_volume_names`'s three pointers are words 0..2 of the object at
    its own address, but a pointer declared at +0x30 of a merged record is word
    12 of the host, and sizing the pointer region by a count would silently
    stop re-pointing it (the bytes would stay the ORIGINAL addresses, which no
    longer exist). Every symbol named here is added to `refs`; the caller
    declares them in the prologue, with the types the definitions use."""
    words = []
    for i in range(0, size, 4):
        w = struct.unpack_from('<I', data, i)[0]
        hit = resolve(w, i // 4 in ptr_words, base + i)
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


RAW_PTR_RE = re.compile(r'^- \*\*raw pointer words: (\d+)\*\*')


def check_pointers(build_dir):
    """The ctest (`pointer_words`): every manifest this build wrote must say
    `raw pointer words: 0`.

    A non-zero count means some word the game's own declarations call a pointer
    still holds an address from the ORIGINAL binary. Nothing is there any more,
    so the game dereferences a number: the failure mode is a NULL where a
    loaded asset should be, and the one that cost this port a lane was an
    infinite loop with no host call in it at all, on the only two paths into the
    park (docs/lanes/scope-port-b8.md). It is silent, it is not a link error and
    no byte gate can see it, so it gets a test.

    Asset-free: it reads the manifests the build already wrote.
    """
    found, bad = [], []
    for root, _dirs, files in os.walk(build_dir):
        if 'manifest.md' not in files:
            continue
        path = os.path.join(root, 'manifest.md')
        n = None
        for line in open(path, encoding='utf-8', errors='replace'):
            m = RAW_PTR_RE.match(line)
            if m:
                n = int(m.group(1))
                break
        rel = os.path.relpath(path, build_dir)
        if n is None:
            bad.append((rel, 'no "raw pointer words" line -- a stale manifest '
                        'from before PORT-A7, or gen_link did not run'))
            continue
        found.append((rel, n))
        if n:
            bad.append((rel, f'{n} raw pointer words -- see '
                        f'{os.path.dirname(rel)}/pointers.md'))
    for rel, n in found:
        print(f'  {rel}: raw pointer words: {n}')
    if not found and not bad:
        print('FAIL no manifest.md under ' + build_dir +
              ': build the closure before running this test')
        return 1
    for rel, why in bad:
        print(f'FAIL {rel}: {why}')
    print(f'pointer gate: {len(bad)} failure(s) in {len(found)} manifest(s)')
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('build_dir')
    ap.add_argument('--out')
    ap.add_argument('--exe', default=os.path.join(lr.ROOT, 'original', 'legoland.exe'))
    ap.add_argument('--ilp32', action='store_true')
    ap.add_argument('--check-pointers', action='store_true',
                    help='the ctest: every manifest.md under build_dir must '
                         'report `raw pointer words: 0`')
    args = ap.parse_args()
    if args.check_pointers:
        sys.exit(check_pointers(args.build_dir))
    if not args.out:
        ap.error('--out is required')
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

    def defined_here(name):
        """True when some object in this tree really DEFINES `name`.

        Distinct from `def_sig_of`, which falls back to the signature the
        references voted for. For a CRT name that matters: `malloc` is
        referenced by 40 game objects and defined by none of them, so
        `def_sig_of` answers with the GAME's declaration while the body that
        gets linked is libc's. Only this predicate says whose prototype wins."""
        return wasm_defs is not None and name in wasm_defs

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
               '#include "ll_gen.h"', '#include <stdio.h>', '#include <stdlib.h>',
               '#include <string.h>', '',
               '/* Every generated body lands here: one line naming the DLL, the symbol',
               ' * and the sources that reference it, then a non-zero exit.',
               ' *',
               ' * LL_TRAP_CONTINUE=1 in the environment turns that into "print once per',
               ' * distinct symbol and RETURN", so one run enumerates every blocker on the',
               ' * path instead of stopping at the first. It is opt-in and the default is',
               ' * unchanged, because a returning trap is a LIE: the caller gets a zero it',
               ' * did not ask for and everything after it is running on made-up data. Use',
               ' * it to make a list, never to call something fixed. The environment is read',
               ' * once -- a trap can be hit inside a frame loop and getenv is not free.',
               ' *',
               ' * The de-duplication compares the `name` POINTER, not its bytes: every call',
               ' * site passes the string literal the generator emitted for that one symbol,',
               ' * so one pointer is one symbol whether or not the linker merged literals. */',
               'static int  g_trap_continue = -1;          /* -1 = not yet read */',
               '#define LL_TRAP_SEEN_MAX 256',
               'static const char* g_trap_seen[LL_TRAP_SEEN_MAX];',
               'static int         g_trap_seen_n;',
               '',
               'static int ll_trap_first_time(const char* name)',
               '{',
               '    int i;',
               '    for (i = 0; i < g_trap_seen_n; i++)',
               '        if (g_trap_seen[i] == name || strcmp(g_trap_seen[i], name) == 0)',
               '            return 0;',
               '    if (g_trap_seen_n < LL_TRAP_SEEN_MAX)',
               '        g_trap_seen[g_trap_seen_n++] = name;',
               '    return 1;',
               '}',
               '',
               'void ll_gen_trap(const char* dll, const char* name, const char* from)',
               '{',
               '    if (g_trap_continue < 0) {',
               '        const char* v = getenv("LL_TRAP_CONTINUE");',
               '        g_trap_continue = (v && *v && *v != \'0\') ? 1 : 0;',
               '        if (g_trap_continue)',
               '            fprintf(stderr, "TRAP-CONTINUE: traps print once and return\\n");',
               '    }',
               '    if (g_trap_continue && !ll_trap_first_time(name))',
               '        return;',
               '    fflush(stdout);',
               '    fprintf(stderr, "TRAP %s %s from %s\\n", dll, name, from);',
               '    fflush(stderr);',
               '    if (g_trap_continue)',
               '        return;',
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

    # Every extent the sources compute: `sizeof` of the declared type, per
    # translation unit, from cdecl.py's parse of the game's own `typedef
    # struct` definitions. This is what STRUCT_EXTENTS used to carry by hand,
    # for the five records somebody had noticed.
    src_tus, src_headers = cdecl.parse_tree()
    src_tus = src_tus + list(src_headers.values())
    src_ext = cdecl.scan(tus=src_tus)
    src_objs = [o for tu in src_tus for o in tu.objects]

    # ---- which WORDS hold pointers: the declared type, walked --------------
    #
    # PORT-A2 read pointer-ness off the TOP-LEVEL declaration only: `extern
    # const char* g_volume_names[3]` is three pointer slots, and a `char*`
    # MEMBER of a struct was "a known limit, nothing on the spine needs it
    # yet". It did: `extern LevelMarker g_low_markers[5]` has pointer depth 0,
    # so the ten sprite-name words of the progress screens kept the ORIGINAL
    # binary's addresses, `LoadSprite("\x40\xed\x4b\x00...")` failed, and
    # `while (KillSprite(NULL) == 0) ;` took the page's main thread with it --
    # the only two doors into the park (docs/lanes/scope-port-b8.md §2).
    #
    # cdecl.py has laid out these structs since PORT-A6; it was only ever asked
    # for their SIZE. Asked for their pointer fields too, the rule becomes the
    # one the declaration always implied: a word is a pointer slot when some
    # declaration's type puts a pointer FIELD at that address.
    #
    # Three properties this has and the old scan did not:
    #
    # * It is keyed by ABSOLUTE address, not by an offset into the block that
    #   happens to be emitted. `g_level_markers` is declared at 0x004beb80 by
    #   screens3.c and at 0x004beb88 by bigscreens.c (deliberately, eight bytes
    #   in -- it only uses the five fields past the names), and `externs` keeps
    #   one address per name, so the BLOCK is at 0x004beb88 and every pointer
    #   in it is at +0x14/+0x18 of a 0x1c record. An offset-into-the-block rule
    #   cannot see that; an address rule does, and it also re-points the first
    #   record's two names, which live in the tail of `g_mode_wplus`'s block.
    # * A framing that is wrong is rejected, not believed. The value of a word
    #   may never decide that a word IS a pointer (PORT-A2: 1,209 of 1,650
    #   value-chosen words are string TEXT), but it can veto a declaration that
    #   claims one: 0x00000019 is not an address in any layout. Corroboration
    #   is per ARRAY ELEMENT, so one odd entry costs its own record and not the
    #   whole table, and every rejection is a row of gen/pointers.md.
    # * A union whose arms disagree is ambiguous and is left alone, reported.
    TEXT_LO, TEXT_HI = lr.SECTIONS[0][1], lr.SECTIONS[0][2]
    MAX_PTR_SCAN = 1 << 20    # bytes of one declared type worth walking

    def plausible_addr(w):
        """Could `w` be an address in this image at all? The only use of a
        word's VALUE in the whole pass, and only ever to REJECT a claim."""
        return w == 0 or TEXT_LO <= w < DATA_HI or w in symbol_at

    ptr_va = {}               # VA -> (object, field, 'file:line')
    ptr_amb = []              # (VA, object, field, cite): a union's two arms
    ptr_reject = []           # (addr, name, typename, cite, nbad, nclaim, [ex])
    ptr_toobig = []           # (addr, name, typename, size)
    for obj in sorted(src_objs, key=lambda o: (o.addr, o.name)):
        ty = obj.full_type
        if ty is None or not cdecl.has_pointer(ty):
            continue
        if ty.size > MAX_PTR_SCAN:
            ptr_toobig.append((obj.addr, obj.name, obj.typename, ty.size))
            continue
        offs, amb = cdecl.pointer_offsets(ty)
        if not offs and not amb:
            continue
        cite = f'{obj.file}:{obj.line}'
        data = read(obj.addr, ty.size) if read else None
        # One corroboration group per array element: a wrong framing is wrong
        # in every element, a sentinel is wrong only in its own.
        esz = ty.fields[0][2].size if ty.kind == 'array' and ty.fields else None
        groups = {}
        for o in offs:
            groups.setdefault(o // esz if esz else 0, []).append(o)
        nbad, examples = 0, []
        for _g, gofs in sorted(groups.items()):
            bad = []
            for o in gofs:
                if o % 4:
                    bad.append((obj.addr + o, None))
                    continue
                if data is None:
                    continue
                w = struct.unpack_from('<I', data, o)[0]
                if not plausible_addr(w):
                    bad.append((obj.addr + o, w))
            if bad:
                nbad += len(gofs)
                examples += bad[:2]
                continue
            for o in gofs:
                ptr_va.setdefault(obj.addr + o,
                                  (obj.name, cdecl.field_name_at(ty, o), cite))
        if nbad:
            ptr_reject.append((obj.addr, obj.name, obj.typename, cite, nbad,
                               len(offs), examples[:4]))
        for o in amb:
            ptr_amb.append((obj.addr + o, obj.name,
                            cdecl.field_name_at(ty, o), cite))

    def declared_extent(addr):
        """The byte extent the game's own declarations give the object at
        `addr`, or 0 when no declaration there has a computable size.

        Three sources, widest wins: the computed `sizeof` of the declared type
        (cdecl.py -- structs, unions, arrays of them, per TU), the array bounds
        the pointer scan already reads, and STRUCT_EXTENTS, which is now a
        REGRESSION FIXTURE rather than the mechanism (see check_hand_table)."""
        best = STRUCT_EXTENTS.get(addr, (0, ''))[0]
        e = src_ext.get(addr)
        if e is not None:
            best = max(best, e.size)
        for nm in data_names.get(addr, ()):
            for daddr, _depth, _count, nbytes in decls.get(nm, []):
                if daddr == addr and nbytes:
                    best = max(best, nbytes)
        return best

    def extent_cite(addr):
        """file:line of the declaration and of the struct definition behind the
        extent at `addr`."""
        e = src_ext.get(addr)
        if e is not None:
            return e.citation()
        return STRUCT_EXTENTS.get(addr, (0, ''))[1]

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
    split_log = []            # every object the tiling would have split
    for addr in sorted(data_names):
        if addr in absorbed:
            continue
        ext = declared_extent(addr)
        if ext <= 1:
            continue
        tiled = next_addr.get(addr, addr + 4) - addr
        log = None
        if ext > tiled:
            # The object is wider than its tile, so the tiling WOULD have split
            # it: one block per named field. Every one of these is a row of
            # gen/extents.md whatever happens next.
            log = {'addr': addr, 'ext': ext, 'tiled': tiled, 'state': '',
                   'names': sorted(data_names[addr]), 'taken': [],
                   'end': addr + ext, 'cite': extent_cite(addr)}
            split_log.append(log)
        names = sorted(data_names[addr])
        if any(n in defined for n in names) or \
                not [n for n in names if n in missing_data and c_ident_ok(n)]:
            if log:
                log['state'] = 'no block here (a game object defines it)'
            continue          # no block is emitted here, so it can host nothing
        taken, end = [], addr + ext
        clamped = None
        i = bisect.bisect_right(known, addr)
        while i < len(known) and known[i] < end:
            k = known[i]
            if k not in data_names or any(n in defined for n in data_names[k]):
                end = k       # not ours: clamp the extent here
                clamped = k
                break
            taken.append(k)
            end = max(end, next_addr.get(k, k + 4))
            i += 1
        if log:
            log['taken'] = taken
            log['end'] = end
            log['state'] = ('merged' if taken else 'nothing named inside it') + \
                (f' (clamped at 0x{clamped:08x})' if clamped else '')
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
    # A word of a global is a pointer slot when an extern declaration of a name
    # AT THAT WORD has pointer depth >= 1 -- the name at the block's own address
    # or any interior alias of it. An unbounded `[]` is bounded by the data
    # itself -- the table ends at the first word that could not be a pointer,
    # which is where the literals it points at usually begin.

    def ptr_slot_words(addr, names, size, data, inter):
        """The word indices of `addr`'s block that hold pointers.

        One pass per name in the block -- the host's own names at offset 0 and
        every interior alias at its own offset -- because a merged record's
        pointer fields are reached through the interior name, not through the
        host. Before the extents were computed from the sources this could only
        ever be a leading count, and an interior pointer word would have kept
        the original binary's address."""
        slots = set()
        nwords = size // 4
        for nm, off in [(n, 0) for n in names] + list(inter):
            if off % 4:
                continue                            # not a word boundary
            base = off // 4
            declared, unbounded = 0, False
            for daddr, depth, count, _nbytes in decls.get(nm, []):
                if daddr != addr + off or depth < 1:
                    continue
                if count is None:
                    unbounded = True
                else:
                    declared = max(declared, count)
            n = min(max(declared, nwords - base if unbounded else 0),
                    nwords - base)
            if unbounded and data is not None:
                for k in range(n):                  # stop where the table does
                    w = struct.unpack_from('<I', data, (base + k) * 4)[0]
                    if not (w == 0 or DATA_LO <= w < DATA_HI or w in symbol_at):
                        n = k
                        break
            slots.update(range(base, base + n))
            for k in range(base, base + n):
                ptr_bounds.setdefault(addr + 4 * k, nm)
        # ...and every word some declared TYPE puts a pointer field at. This is
        # the part that does not care which name the block is emitted under:
        # the pointer fields of a struct are at absolute addresses, and an
        # unbounded `[]` that the loop above bounds by the data is the one case
        # no type can describe, which is why both passes run.
        for k in range(nwords):
            if addr + 4 * k in ptr_va:
                slots.add(k)
        return slots

    ptr_bounds = {}           # VA -> name: pointer slots from the ARRAY-BOUNDS
                              # pass (an `extern char* g_x[8]`), so that a word
                              # left raw can name the declaration behind it.

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
    raw_words = []            # (VA, value, reason): every slot left raw

    def resolve(w, is_ptr_slot, va=None):
        nonlocal n_exact, n_interior, n_raw_ptr
        if w in symbol_at:                          # a symbol's own address
            n_exact += 1
            return (symbol_at[w], 0)
        if not is_ptr_slot or not (DATA_LO <= w < DATA_HI):
            if is_ptr_slot and w:
                n_raw_ptr += 1
                raw_words.append((va, w, 'into .text (function-table index)'
                                  if TEXT_LO <= w < TEXT_HI else
                                  'the value is not an address'))
            return None
        hit = resolve_pointer(w)
        if hit is None:
            n_raw_ptr += 1
            raw_words.append((va, w, 'in the image, but no block and no gap'))
            return None
        n_interior += 1
        return hit

    # Resolution runs over every pointer slot BEFORE anything is emitted, so
    # the synthesised gap blocks are part of the plan (and of the prologue's
    # declarations) by the time the first global is written.
    n_ptr_of = {}
    for addr, primary, size, data, _rest, words, inter in plan:
        slots = ptr_slot_words(addr, sorted(data_names.get(addr, [])), size,
                               data, inter) if words else set()
        n_ptr_of[addr] = slots
        for k in sorted(slots):
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
                                   n_ptr_of.get(addr, ()), refs,
                                   nocommon=bool(inter), base=addr))
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
    # every loader in the game is waiting for.
    #
    # A CRT target is declared from CRT_PROTOS, never from the alias: the body is
    # libc's and only libc's prototype is right. Both spellings are handled --
    # an alias that is itself variadic (`Format` -> `sprintf`) forwards the
    # varargs pointer straight through, and one that is not (`DebugPrint` ->
    # `printf`) calls the real variadic prototype so clang builds the buffer.
    # See CRT_PROTOS and crt_alias at the top of this file.
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
    crt_decls = set()         # the CRT spellings already declared in this file
    crt_typed = []            # (alias, real) taken from CRT_PROTOS, not the alias
    crt_untabled = []         # (alias, real) a CRT name with no CRT_PROTOS row
    cast_fwd = []             # (alias, real, alias sig, real sig): see below
    for n, detail, _ in [(n, f'{a} is {m} (CRT)', None) for n, m, a in crt_thunks] + \
                        [(n, d, u) for n, d, u in cats['alias']]:
        real = detail.split(' is ')[1].split(' (')[0]
        sig = sig_of(n)
        # A CRT name that no translation unit in the tree defines: the body comes
        # out of libc, so the declaration must come from libc too (CRT_PROTOS).
        # Declaring it from the ALIAS was the port's last signature mismatch.
        from_crt = sig is not None and lr.is_crt(real) and not defined_here(real)
        made = crt_alias(n, real, sig, crt_proto(real)) if \
            from_crt and crt_proto(real) else None
        if made is not None:
            key, decl, fwd = made
            if key not in crt_decls:
                crt_decls.add(key)
                aliases_c.append(decl)
            aliases_c.append(fwd)
            declared[real] = crt_wasm_sig(crt_proto(real))
            crt_typed.append((n, real))
            n_alias_fn += 1
            continue
        if from_crt:
            crt_untabled.append((n, real))
        if sig is not None and real not in declared:
            # One declaration of the real body per file, with the signature
            # the body actually has; an alias whose callers disagree casts.
            rsig = def_sig_of(real) or sig
            ret, types, _ = sig_decl(rsig)
            declared[real] = rsig if ret is not None else None
            aliases_c.append(f'extern {ret} {real}({sig_params(types)});' if ret is not None
                             else f'extern void {real}();')
        if sig is not None and declared.get(real) is not None and \
                declared[real] != sig:
            cast_fwd.append((n, real, sig, declared[real]))
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
        f'- forwarders typed from libc, not from the alias: {len(crt_typed)}'
        + (f" ({', '.join(a + ' -> ' + r for a, r in crt_typed)})" if crt_typed else ''),
        f'- CRT targets with no CRT_PROTOS row (typed from the alias, may mismatch):'
        f' {len(crt_untabled)}'
        + (f" ({', '.join(a + ' -> ' + r for a, r in crt_untabled)})"
           if crt_untabled else ''),
        f'- cast forwarders (latent indirect-call type mismatch): {len(cast_fwd)}',
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
    if cast_fwd:
        # These are the rows `name_trap.py --indirect` explains. A forwarder
        # whose callers' signature disagrees with the body's is emitted as a
        # CAST of the function pointer, which lowers to `call_indirect` with the
        # cast type: the call traps at runtime if the game reaches it, and
        # binaryen's `directize` may rewrite it into an invalid direct call
        # before that (docs/lanes/scope-port-m2.md section 4). Every row is a
        # game-side defect -- a stale extern name whose spelling disagrees with
        # the body it resolves to -- and the fix belongs in the DECLARING file.
        manifest += ['', '## Cast forwarders: a stale name typed unlike its body', '',
                     'One row per alias whose callers emitted a signature the real body',
                     'does not have. On x86 cdecl these were the same call; on wasm the',
                     'forwarder has to cast, the call becomes `call_indirect`, and it',
                     'traps (or binaryen directizes it into an invalid direct call).',
                     'Fix the DECLARING file, do not bridge it here.', '',
                     '| alias | body | callers emitted | the body has |',
                     '| --- | --- | --- | --- |']
        for a, r, asig, rsig in sorted(cast_fwd):
            manifest.append(f'| `{a}` | `{r}` | `{lr.sig_text(asig)}` | '
                            f'`{lr.sig_text(rsig)}` |')
    # ---- gen/extents.md: every object the gap tiling would have split -------
    # The whole split-record class in one table, computed rather than noticed:
    # an object whose type is wider than the gap to the next named address was
    # going to come out as one block per named field, with the writers and the
    # readers addressing different memory. The hand table at the top of this
    # file is now the regression test for the parser, not the mechanism.
    ext_md = ['# Source-derived object extents', '',
              "Computed by `portable/tools/cdecl.py` from the game's own",
              '`typedef struct` definitions and `extern` declarations (MSVC x86',
              'layout, `#pragma pack` honoured, ILP32 sizes). Every row is an object',
              'whose computed extent exceeds the size the gap tiling would have given',
              'it -- i.e. every object that WOULD have been split into one block per',
              'named field, with the files that write it and the files that read it',
              'addressing different memory.', '']
    hand_rows = []
    for addr in sorted(STRUCT_EXTENTS):
        want, cite = STRUCT_EXTENTS[addr]
        e = src_ext.get(addr)
        cited = sorted({c[0] for c in e.cites}) if e else []
        hand_rows.append((addr, want, e.size if e else 0, want in cited, cite))
    ext_md += ['## Regression: the rows the hand table carried', '',
               'Each must be reproduced by the parser -- cited by some translation unit',
               'at exactly the hand-written number -- and the extent actually used must',
               'cover it. A `NO` in either column is a parser regression, not a new',
               'finding.', '',
               "| address | hand table | computed | reproduced | the hand row's citation |",
               '| --- | --- | --- | --- | --- |']
    for addr, want, got, ok, cite in hand_rows:
        ext_md.append(f'| `0x{addr:08x}` | {want} (0x{want:x}) | {got} (0x{got:x}) '
                      f'| {"yes" if ok and got >= want else "**NO**"} | {cite} |')
    n_repro = sum(1 for _a, w, g, ok, _c in hand_rows if ok and g >= w)
    ext_md += ['', f'{n_repro}/{len(hand_rows)} reproduced.', '']
    if n_repro != len(hand_rows):
        print(f'gen_link: WARNING {len(hand_rows) - n_repro} STRUCT_EXTENTS row(s) '
              f'not reproduced by cdecl.py -- see gen/extents.md', file=sys.stderr)

    ext_md += ['## Every object whose computed extent exceeds the gap-tiled size',
               '',
               '`state` is what the interior-alias pass did with it: `merged` (one',
               'block, the other names offset aliases of it), `clamped` at an address',
               'the pass may not claim, `nothing named inside it` (the extent is real',
               'but no other name reaches into it, so the tiling was already right), or',
               '`no block here` (a game object defines this address and the generator',
               'emits nothing for it). `field` names the struct field each interior',
               'offset lands on; `NOT A FIELD` marks an offset that is not on a field',
               'boundary of the declared type -- legitimate for a byte name inside a',
               'dword, and the first thing to check if a type looks wrong.', '',
               '| address | object | type, extent | tiled | state | interior aliases |',
               '| --- | --- | --- | --- | --- | --- |']
    n_hand = 0
    for log in split_log:
        addr = log['addr']
        e = src_ext.get(addr)
        ty = e.type if e else None
        inter = interiors.get(addr, [])
        bits = []
        for nm, off in inter:
            fname_ = cdecl.field_name_at(ty, off) if ty else ''
            ok = cdecl.is_field_boundary(ty, off) if ty else None
            bits.append(f'`{nm}`+0x{off:x}' +
                        (f' ({fname_})' if fname_ else '') +
                        ('' if ok is not False else ' **NOT A FIELD**'))
        if addr in STRUCT_EXTENTS:
            n_hand += 1
        ext_md.append(
            f'| `0x{addr:08x}` | ' + ', '.join(f'`{n}`' for n in log['names'][:4]) +
            (' ...' if len(log['names']) > 4 else '') +
            f' | {log["cite"] or "-"} -> {log["ext"]} (0x{log["ext"]:x}) '
            f'| {log["tiled"]} | {log["state"]}'
            f'{" [hand table]" if addr in STRUCT_EXTENTS else ""} '
            f'| {", ".join(bits) if bits else "-"} |')
    merged = [lg for lg in split_log if lg['state'].startswith('merged')]
    ext_md += ['', f'{len(split_log)} objects would have been split; '
               f'{len(merged)} are merged here ({n_hand} of them were in the hand '
               f'table, {len(merged) - n_hand} are new).', '']

    dis = [(a, e) for a, e in sorted(src_ext.items())
           if len({c[0] for c in e.cites}) > 1]
    ext_md += ['## Translation units that disagree about an extent', '',
               'The sources define their types locally on purpose, so the same address',
               'can be declared as a 4-byte pointer in one file and a 36-byte table in',
               'another. The widest declaration wins (it is the only one that can cover',
               'the whole record) and the clamp keeps it to storage the tiling was going',
               "to hand these names anyway -- but a disagreement is worth a human's",
               'eye, because one of the two files is wrong about the image.', '',
               '| address | used | the declarations |', '| --- | --- | --- |']
    for a, e in dis:
        rows = ', '.join(f'`{nm}` {tn} = {ext} ({where})'
                         for ext, nm, tn, where, _d in
                         sorted(e.cites, key=lambda c: -c[0]))
        ext_md.append(f'| `0x{a:08x}` | {e.size} | {rows} |')
    ext_md += ['', f'{len(dis)} addresses with more than one computed extent.', '']

    unsized = cdecl.unsized_objects()
    near = []
    for a in sorted(unsized):
        if a not in data_names:
            continue
        tile = next_addr.get(a, a + 4) - a
        if tile > 64:
            continue            # nothing close enough behind it to be a field
        near.append((a, tile, unsized[a]))
    ext_md += ['## The residue: objects whose type the parser cannot size', '',
               'A type no translation unit lays out (a `windows.h` struct, an array',
               'whose bound is a macro) still has no extent, so these objects are still',
               'sized by the gap to the next named address. Listed are the ones whose',
               'tile is 64 bytes or less -- small enough that the next named address',
               'could be their own second field. A split record is still the first thing',
               'to suspect for any "the game ignores X" report, and this is the list.', '',
               '| address | tile | declarations with no computable size |',
               '| --- | --- | --- |']
    for a, tile, rows in near:
        ext_md.append(f'| `0x{a:08x}` | {tile} | ' +
                      ', '.join(f'`{nm}` {tn} ({w})' for nm, tn, w in rows[:6]) +
                      (' ...' if len(rows) > 6 else '') + ' |')
    ext_md += ['', f'{len(near)} addresses (of {len(unsized)} unsized declarations).',
               '']

    # ---- gen/pointers.md: the raw-pointer-word census ----------------------
    #
    # PORT-B8 wrote this census by hand against the generated globals.c after
    # the park wedged on a word that was never re-pointed, and asked for it to
    # be part of the generator. It is, and it is the DECLARATION's census, not
    # the value's: B8's script counts any word whose value happens to land in
    # an emitted object, which over-counts by 469 (string TEXT -- `"Appr"` is
    # 0x72707041 and three-character strings are small enough to look like
    # addresses; PORT-A2 measured 1,209 such false positives out of 1,650).
    # The number that has to be zero is the number of words the game's own
    # declarations say are pointers and that the closure still emits raw.
    pl_starts = sorted(p[0] for p in plan)
    pl_at = {p[0]: p for p in plan}
    raw_by_reason = collections.Counter()
    for _va, _w, why in raw_words:
        raw_by_reason[why] += 1
    n_bytes_block = n_noblock = 0
    bytes_block, noblock = [], []
    # Only under --ilp32 is there anything to be raw ABOUT: on a 64-bit host
    # every pointer table is wrong by construction and the raw bytes are kept
    # on purpose, so the gate is vacuous there rather than 7,348 failures.
    for va in sorted(ptr_va) if args.ilp32 else ():
        i = bisect.bisect_right(pl_starts, va) - 1
        p = pl_at[pl_starts[i]] if i >= 0 else None
        if p is None or va >= p[0] + p[2]:
            n_noblock += 1
            noblock.append(va)
            continue
        if not p[5]:                      # emitted as bytes, never re-pointed
            n_bytes_block += 1
            bytes_block.append((va, p[1]))
    n_unexplained = raw_by_reason['in the image, but no block and no gap'] + \
        n_bytes_block
    ptr_md = ['# Pointer words in the rebuilt closure', '',
              'Every 4-byte word the game\'s own declarations say is a POINTER, and',
              'what the generator did with it. A pointer word that keeps the ORIGINAL',
              "binary's address is a live defect: the rebuilt objects are nowhere near",
              '0x004b4000, so the game dereferences a number that means nothing. That',
              'is what hung both doors into the park (docs/lanes/scope-port-b8.md).', '',
              'Pointer-ness is read off the DECLARED TYPE, never off the value',
              '(PORT-A2); the value is used only to REJECT a declaration that claims a',
              'pointer where the image has something that cannot be an address.', '',
              f'- re-pointing (--ilp32): {"on" if args.ilp32 else "OFF -- the 64-bit build keeps the raw bytes by construction, so there is nothing for the gate to check"}',
              f'- declared pointer words: {len(ptr_va)}',
              f'- of those, in a block emitted as WORDS (so offered to the resolver): '
              f'{len(ptr_va) - n_bytes_block - n_noblock if args.ilp32 else 0}',
              f'- in a block emitted as BYTES (NOT re-pointed): {n_bytes_block}',
              f'- in no emitted block (the game defines the object, or nothing emits '
              f'it): {n_noblock}',
              f'- re-pointed at a symbol address: {n_exact}',
              f'- re-pointed INTO a block: {n_interior}',
              f'- left raw: {n_raw_ptr}' +
              (' (' + ', '.join(f'{n} {why}' for why, n in
                                sorted(raw_by_reason.items())) + ')'
               if raw_words else ''),
              f'- **raw pointer words: {n_unexplained}** -- the gate: a declared',
              '  pointer word, inside the image, that nothing re-pointed and nothing',
              '  can explain.', '']
    if raw_words:
        ptr_md += ['## Pointer words left raw, with the reason', '',
                   'A word into `.text` is a function address the wasm target cannot',
                   'offset into at all (a function "pointer" is a table index), and it',
                   'is left raw on purpose: an EXACT function address is re-pointed by',
                   'the symbol path above, and an interior one is meaningless. A word',
                   'whose value is not an address is a DECLARATION to check -- some',
                   'file says pointer where the image holds a number.', '',
                   '| word | value | declared by | field | reason |',
                   '| --- | --- | --- | --- | --- |']
        for va, w, why in sorted(raw_words)[:200]:
            nm, fld, cite = ptr_va.get(
                va, (ptr_bounds.get(va, '-'), '', 'array bounds'))
            ptr_md.append(f'| `0x{va:08x}` | `0x{w:08x}` | `{nm}` ({cite}) '
                          f'| {fld or "-"} | {why} |')
        ptr_md += ['', f'{len(raw_words)} rows'
                   + (' (first 200 shown)' if len(raw_words) > 200 else '') + '.', '']
    if bytes_block:
        ptr_md += ['## Pointer words in a block emitted as BYTES', '',
                   'A block is emitted as words only when its address and size are both',
                   '4-aligned and the exe could be read. Any pointer in one of these',
                   'keeps the original address: a real defect of the B2 class.', '',
                   '| word | block |', '| --- | --- |'] + \
            [f'| `0x{va:08x}` | `{nm}` |' for va, nm in bytes_block[:50]] + ['']
    if ptr_reject:
        ptr_md += ['## Declarations whose pointer claim the image rejects', '',
                   'The declaration says this word holds a pointer and the image holds',
                   'something that cannot be an address in any layout, so the claim is',
                   'dropped for that array ELEMENT and the word is left exactly as the',
                   'image has it. Every row is a file that is wrong about this address --',
                   'usually a deliberate framing (`bigscreens.c` declares',
                   '`g_level_markers` eight bytes into the record because it only uses',
                   'the fields past the two names), occasionally a type to fix.', '',
                   '| address | object | type | declared by | words claimed | rejected | e.g. |',
                   '| --- | --- | --- | --- | --- | --- | --- |']
        for addr, nm, tn, cite, nbad, nclaim, ex in ptr_reject:
            egs = ', '.join(f'`0x{v:08x}`=' +
                            (f'`0x{w:08x}`' if w is not None else 'unaligned')
                            for v, w in ex)
            ptr_md.append(f'| `0x{addr:08x}` | `{nm}` | `{tn}` | {cite} | {nclaim} '
                          f'| {nbad} | {egs} |')
        ptr_md += ['', f'{len(ptr_reject)} declarations, '
                   f'{sum(r[4] for r in ptr_reject)} words.', '']
    if ptr_amb:
        ptr_md += ['## Words a union makes undecidable', '',
                   'One arm of a union says pointer and another says it is not. Nothing',
                   're-points these: a wrong re-point writes an address where the image',
                   'had a plain number. Worth a human deciding which arm the image',
                   'holds.', '', '| word | object | field | declared by |',
                   '| --- | --- | --- | --- |'] + \
            [f'| `0x{va:08x}` | `{nm}` | {fld or "-"} | {cite} |'
             for va, nm, fld, cite in ptr_amb[:50]] + ['']
    if ptr_toobig:
        ptr_md += ['## Declared types too large to walk', '',
                   f'Over {MAX_PTR_SCAN} bytes.', ''] + \
            [f'- `0x{a:08x}` `{nm}` `{tn}` {sz} bytes' for a, nm, tn, sz in ptr_toobig] + ['']

    manifest += ['', '## Pointer words (gen/pointers.md)', '',
                 f'- declared pointer words: {len(ptr_va)}'
                 + ('' if args.ilp32 else ' (re-pointing off: --ilp32 not given)'),
                 f'- **raw pointer words: {n_unexplained}**',
                 f'- pointer words left raw with a reason: {n_raw_ptr}'
                 + (' (' + ', '.join(f'{n} {why}' for why, n in
                                     sorted(raw_by_reason.items())) + ')'
                    if raw_words else ''),
                 f'- pointer claims the image rejects (wrong framing or wrong type): '
                 f'{sum(r[4] for r in ptr_reject)} words in {len(ptr_reject)} '
                 f'declarations',
                 f'- words a union makes undecidable (left alone): {len(ptr_amb)}']
    if n_unexplained:
        print(f'gen_link: WARNING {n_unexplained} raw pointer word(s) -- '
              f'see gen/pointers.md', file=sys.stderr)

    manifest += ['', '## Source-derived extents (gen/extents.md)', '',
                 f'- objects the gap tiling would have split: {len(split_log)}',
                 f'- of those, merged into one block: {len(merged)} '
                 f'({n_hand} from the hand table, {len(merged) - n_hand} new)',
                 f'- STRUCT_EXTENTS rows reproduced by cdecl.py: '
                 f'{n_repro}/{len(hand_rows)}',
                 f'- addresses two TUs give different extents: {len(dis)}',
                 f'- declarations with no computable size next to a close '
                 f'neighbour: {len(near)}']

    for fname, text in (('globals.c', globals_c), ('aliases.c', aliases_c),
                        ('stubs.c', stubs_c), ('host_stubs.c', host_c),
                        ('manifest.md', manifest), ('extents.md', ext_md),
                        ('pointers.md', ptr_md)):
        with open(os.path.join(args.out, fname), 'w') as f:
            f.write('\n'.join(text) + '\n')
    with open(os.path.join(args.out, 'll_gen.h'), 'w') as f:
        f.write('void ll_gen_trap(const char* dll, const char* name, const char* from);\n'
                'void ll_unwritten(const char* name, unsigned int address);\n'
                'void ll_unhosted(const char* name, const char* dll);\n')
    print('\n'.join(manifest[2:]))


if __name__ == '__main__':
    main()
