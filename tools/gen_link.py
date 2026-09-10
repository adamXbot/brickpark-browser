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
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import linkreport as lr  # noqa: E402

IMAGE_BASE = 0x400000

# An unclassified extern used as data gets a placeholder block this big: the
# sources give it no address, so neither its real size nor its contents are
# known. Generous, because some of these names are structs.
UNKNOWN_DATA_SIZE = 256

# ---- wasm object signatures ----------------------------------------------
# wasm valtype -> (C type, zero literal). i32 covers every 32-bit C type and
# every pointer on wasm32, which is what the game's prototypes lower to.
WASM_TYPES = {0x7f: ('unsigned int', '0'), 0x7e: ('long long', '0'),
              0x7d: ('float', '0'), 0x7c: ('double', '0')}


def _leb(b, i):
    r = s = 0
    while True:
        x = b[i]
        i += 1
        r |= (x & 0x7f) << s
        if not x & 0x80:
            return r, i
        s += 7


def _skip_limits(b, i):
    flags = b[i]
    i += 1
    _, i = _leb(b, i)
    if flags & 1:
        _, i = _leb(b, i)
    return i


def wasm_object_sigs(path):
    """(imported, defined): name -> (params, results) for the functions this
    wasm object imports (the signature the compiler gave the call site) and
    the functions it defines (the signature the body really has). Returns
    (None, None) for anything that is not a wasm object.

    The function index space is imports first, then the Function section in
    order; the names of the defined ones come from the "linking" custom
    section's symbol table."""
    with open(path, 'rb') as f:
        b = f.read()
    if b[:4] != b'\0asm':
        return None, None, None
    types = []
    imported = {}
    import_fns = 0                 # how many function indices the imports take
    local_types = []               # type index per locally defined function
    symtab = None
    i = 8
    while i < len(b):
        sid = b[i]
        i += 1
        size, i = _leb(b, i)
        end = i + size
        if sid == 1:                                   # type section
            n, i = _leb(b, i)
            for _ in range(n):
                if b[i] != 0x60:
                    break
                i += 1
                np, i = _leb(b, i)
                params = list(b[i:i + np])
                i += np
                nr, i = _leb(b, i)
                results = list(b[i:i + nr])
                i += nr
                types.append((params, results))
        elif sid == 2:                                 # import section
            n, i = _leb(b, i)
            for _ in range(n):
                ml, i = _leb(b, i)
                i += ml
                fl, i = _leb(b, i)
                field = b[i:i + fl].decode('utf-8', 'replace')
                i += fl
                kind = b[i]
                i += 1
                if kind == 0:                          # function
                    t, i = _leb(b, i)
                    import_fns += 1
                    if t < len(types):
                        imported[field] = types[t]
                elif kind == 1:                        # table
                    i += 1
                    i = _skip_limits(b, i)
                elif kind == 2:                        # memory
                    i = _skip_limits(b, i)
                elif kind == 3:                        # global
                    i += 2
                elif kind == 4:                        # tag
                    i += 1
                    _, i = _leb(b, i)
                else:
                    break
        elif sid == 3:                                 # function section
            n, i = _leb(b, i)
            for _ in range(n):
                t, i = _leb(b, i)
                local_types.append(t)
        elif sid == 0:                                 # custom section
            nl, j = _leb(b, i)
            name = b[j:j + nl]
            if name == b'linking':
                symtab = (j + nl, end)
        i = end

    defined = {}
    undef_data = set()
    if symtab is not None:
        i, end = symtab
        _version, i = _leb(b, i)
        while i < end:
            sub = b[i]
            i += 1
            sz, i = _leb(b, i)
            stop = i + sz
            if sub == 8:                               # WASM_SYMBOL_TABLE
                count, i = _leb(b, i)
                for _ in range(count):
                    kind = b[i]
                    i += 1
                    flags, i = _leb(b, i)
                    undef = bool(flags & 0x10)
                    if kind == 1:                      # DATA: name always
                        nl, i = _leb(b, i)
                        nm = b[i:i + nl].decode('utf-8', 'replace')
                        i += nl
                        if not undef:
                            _, i = _leb(b, i)          # segment
                            _, i = _leb(b, i)          # offset
                            _, i = _leb(b, i)          # size
                        else:
                            undef_data.add(nm)
                        continue
                    index, i = _leb(b, i)
                    nm = None
                    if not undef or (flags & 0x40):    # EXPLICIT_NAME
                        nl, i = _leb(b, i)
                        nm = b[i:i + nl].decode('utf-8', 'replace')
                        i += nl
                    if kind == 0 and nm and not undef:
                        k = index - import_fns
                        if 0 <= k < len(local_types) and local_types[k] < len(types):
                            defined[nm] = types[local_types[k]]
            i = stop
    return imported, defined, undef_data


def collect_wasm_sigs(objs):
    """(imported, defined, undefined-data, conflicts) over every object, or
    (None, None, None, []) when the objects are not wasm: Mach-O and ELF need
    neither signature nor function/data agreement, and carry no such
    information for undefined symbols."""
    votes = collections.defaultdict(collections.Counter)
    defined = {}
    undef_data = set()
    any_wasm = False
    for obj in objs:
        try:
            imp, dfn, udata = wasm_object_sigs(obj)
        except (IndexError, ValueError):
            continue
        if imp is None:
            continue
        any_wasm = True
        for name, sig in imp.items():
            votes[name][(tuple(sig[0]), tuple(sig[1]))] += 1
        for name, sig in dfn.items():
            defined.setdefault(name, (tuple(sig[0]), tuple(sig[1])))
        undef_data |= udata
    if not any_wasm:
        return None, None, None, []
    out = {}
    conflicts = []
    for name, counter in votes.items():
        winner, _ = counter.most_common(1)[0]
        out[name] = winner
        if len(counter) > 1:
            conflicts.append(name)
    return out, defined, undef_data, sorted(conflicts)


def sig_decl(sig):
    """(return type, [param types], [param names]) for a wasm signature, or
    (None, None, None) when it uses a type this emitter does not map."""
    params, results = sig
    if any(p not in WASM_TYPES for p in params) or len(results) > 1 or \
            (results and results[0] not in WASM_TYPES):
        return None, None, None
    ret = WASM_TYPES[results[0]][0] if results else 'void'
    types = [WASM_TYPES[p][0] for p in params]
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


def emit_bytes(name, size, data, align=16):
    if data is None or not any(data):
        return f'__attribute__((aligned({align}))) unsigned char {name}[{size}];\n'
    # trim trailing zeros: C zero-fills the rest of the array
    n = len(data)
    while n > 0 and data[n - 1] == 0:
        n -= 1
    body = ',\n    '.join(', '.join(f'0x{b:02x}' for b in data[i:i + 16]) for i in range(0, n, 16))
    return f'__attribute__((aligned({align}))) unsigned char {name}[{size}] = {{\n    {body}\n}};\n'


def emit_words(name, size, data, symbol_at, refs, align=16):
    """ILP32: 4-byte words, with exact symbol addresses re-pointed.

    Every symbol named here is added to `refs`; the caller declares them in
    the prologue, with the types the definitions use."""
    words = []
    for i in range(0, size, 4):
        w = struct.unpack_from('<I', data, i)[0]
        if w in symbol_at:
            sym = symbol_at[w]
            refs.add(sym)
            words.append(f'(unsigned int)(__UINTPTR_TYPE__)&{sym}')
        else:
            words.append(f'0x{w:08x}u')
    n = len(words)
    while n > 0 and words[n - 1] == '0x00000000u':
        n -= 1
    if n == 0:
        return f'__attribute__((aligned({align}))) unsigned int {name}[{size // 4}];\n'
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
    wasm_sigs, wasm_defs, wasm_undef_data, sig_conflicts = collect_wasm_sigs(all_objs)

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

    # ---- globals: plan everything before emitting anything ----------------
    # A re-pointed global is `unsigned int[N]`, a plain one `unsigned char[N]`,
    # and the same symbol may be named again later (by a table that points at
    # it, or by an alias of it). Planning first means the prologue's extern
    # declarations can use the types the definitions actually have.
    plan = []                 # (addr, primary, size, data, rest, mode)
    cross_tu_alias = []       # (alias, real): the game defines the real name
    for addr in sorted(data_names):
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
        size = next_addr.get(addr, 4) - addr
        if size <= 0:
            size = 4
        data = read(addr, size) if read else None
        words = bool(args.ilp32 and data is not None and size % 4 == 0 and addr % 4 == 0)
        plan.append((addr, primary, size, data, rest, words))
        symbol_at[addr] = primary

    decl_of = {}              # name -> (type, element count) as defined here
    for _addr, primary, size, _data, _rest, words in plan:
        decl_of[primary] = ('unsigned int', size // 4) if words else ('unsigned char', size)

    refs = set()
    n_def = n_alias_data = total_bytes = 0
    body = []
    for addr, primary, size, data, rest, words in plan:
        sec = next((s for s, a, b in lr.SECTIONS if a <= addr < b), '?')
        body.append(f'/* 0x{addr:08x} {sec} {size} bytes{" (+" + ", ".join(rest) + ")" if rest else ""} */')
        if words:
            body.append(emit_words(primary, size, data, symbol_at, refs))
        else:
            body.append(emit_bytes(primary, size, data))
        typ, count = decl_of[primary]
        for n in rest:
            body.append(data_alias(n, primary, typ, count))
            n_alias_data += 1
        n_def += 1
        total_bytes += size

    # Functions and data are different kinds of symbol on wasm (a function
    # pointer in a table is a table index, not an address), so a re-pointed
    # word that names a function has to be declared as one -- with the
    # signature the body has, or wasm-ld puts a trapping stub in the table.
    fn_names = {name for name, _file in defined_at.values()}
    fn_names |= {n for n, (_a, kind) in externs.items() if kind == 'fn'}
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
    n_alias_fn = 0
    declared = {}             # real -> the signature aliases.c declared it with
    for n, detail, _ in cats['alias']:
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
    for n, detail, users in cats['game-fn']:
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
        f'- symbols re-pointed into (ilp32): {len(refs)}',
        f'- function aliases (stale extern names): {n_alias_fn}',
        f'- data names the game defines under another name (no alias possible): {len(cross_tu_alias)}',
        f"- unwritten game function stubs: {len(cats['game-fn'])}",
        f"- unresolved-name stubs: {len(cats['unknown']) - n_unknown_data}"
        f" (+{n_unknown_data} placeholder data blocks)",
        f"- host API stubs: {len(cats['host'])}",
        f'- symbols imported with more than one signature (most common wins): {len(sig_conflicts)}',
        f'- pointer re-pointing (ilp32): {"on" if args.ilp32 else "off"}',
    ]
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
