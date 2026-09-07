#!/usr/bin/env python3
"""Close the link: generate the C that the portable build is still missing.

Given the compiled objects of legoland_core, writes into --out:

  globals.c     storage for every global the sources only declare `extern`,
                sized by the gap to the next known address and initialised
                from the original binary's .data/.rdata bytes (zero when the
                exe is not available or the address is in BSS)
  aliases.c     one symbol forwarding to another: stale extern names whose
                address is defined under a different name, and globals that
                several sources declare under different names
  stubs.c       trapping bodies for referenced game functions that are not
                written yet, and for externs with no address comment
  host_stubs.c  trapping bodies for Win32/DirectX imports the host shim does
                not implement yet
  manifest.md   what was generated and why

Every generated body aborts with its name when reached, so a run stops at
the first missing piece instead of silently misbehaving.

With --ilp32 (wasm32 / i386 builds) 4-byte words inside a global whose value
is exactly a known symbol address are emitted as `(unsigned)&Symbol`, which
re-points the original's pointer tables at the rebuilt symbols. On a 64-bit
host the raw bytes are kept and such tables are wrong by construction; the
64-bit build is a link census, not a runnable game.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import linkreport as lr  # noqa: E402

IMAGE_BASE = 0x400000


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


def emit_words(name, size, data, symbol_at, align=16):
    """ILP32: 4-byte words, with exact symbol addresses re-pointed."""
    words = []
    refs = set()
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
    decls = ''.join(f'extern unsigned char {s}[];\n' for s in sorted(refs))
    if n == 0:
        return decls + f'__attribute__((aligned({align}))) unsigned int {name}[{size // 4}];\n'
    body = ',\n    '.join(', '.join(words[i:i + 4]) for i in range(0, n, 4))
    return decls + f'__attribute__((aligned({align}))) unsigned int {name}[{size // 4}] = {{\n    {body}\n}};\n'


def fn_alias(alias, real):
    return (
        f'#if defined(__APPLE__) && defined(__aarch64__)\n'
        f'__asm__(".text\\n.globl _{alias}\\n.p2align 2\\n_{alias}:\\n b _{real}\\n");\n'
        f'#elif defined(__APPLE__) && defined(__x86_64__)\n'
        f'__asm__(".text\\n.globl _{alias}\\n_{alias}:\\n jmp _{real}\\n");\n'
        f'#else\n'
        f'extern void {real}(void);\n'
        f'void {alias}(void) __attribute__((alias("{real}")));\n'
        f'#endif\n')


def data_alias(alias, real):
    return (
        f'#if defined(__APPLE__)\n'
        f'__asm__(".globl _{alias}\\n.set _{alias}, _{real}\\n");\n'
        f'#else\n'
        f'extern unsigned char {real}[];\n'
        f'extern unsigned char {alias}[] __attribute__((alias("{real}")));\n'
        f'#endif\n')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('build_dir')
    ap.add_argument('--out', required=True)
    ap.add_argument('--exe', default=os.path.join(lr.ROOT, 'original', 'legoland.exe'))
    ap.add_argument('--ilp32', action='store_true')
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    objs, _, _, undefined = lr.collect_objects(args.build_dir)          # what the game references
    _, defined, _, _ = lr.collect_objects(args.build_dir, only_game=False)  # what anything provides
    win32 = lr.load_win32()
    externs, defined_at, _stubs = lr.scan_sources()
    cats = lr.classify(defined, undefined, externs, defined_at, win32)
    read = load_image(args.exe)

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
               '#include "ll_gen.h"', '']
    host_c = ['/* generated by portable/tools/gen_link.py: host API the shim does not implement yet */',
              '#include "ll_gen.h"', '']

    # ---- globals ---------------------------------------------------------
    n_def = n_alias_data = total_bytes = 0
    for addr in sorted(data_names):
        names = sorted(data_names[addr])
        want = [n for n in names if n in missing_data and c_ident_ok(n)]
        if not want:
            continue
        existing = [n for n in names if n in defined]
        if existing:
            for n in want:
                aliases_c.append(data_alias(n, existing[0]))
                n_alias_data += 1
            symbol_at.setdefault(addr, existing[0])
            continue
        primary, rest = want[0], want[1:]
        size = next_addr.get(addr, 4) - addr
        if size <= 0:
            size = 4
        data = read(addr, size) if read else None
        sec = next((s for s, a, b in lr.SECTIONS if a <= addr < b), '?')
        globals_c.append(f'/* 0x{addr:08x} {sec} {size} bytes{" (+" + ", ".join(rest) + ")" if rest else ""} */')
        if args.ilp32 and data is not None and size % 4 == 0 and addr % 4 == 0:
            globals_c.append(emit_words(primary, size, data, symbol_at))
        else:
            globals_c.append(emit_bytes(primary, size, data))
        for n in rest:
            aliases_c.append(data_alias(n, primary))
            n_alias_data += 1
        symbol_at[addr] = primary
        n_def += 1
        total_bytes += size

    # ---- function aliases -------------------------------------------------
    n_alias_fn = 0
    for n, detail, _ in cats['alias']:
        real = detail.split(' is ')[1].split(' (')[0]
        aliases_c.append(fn_alias(n, real))
        n_alias_fn += 1

    # ---- stubs ------------------------------------------------------------
    for n, detail, users in cats['game-fn']:
        stubs_c.append(f'void {n}(void) {{ ll_unwritten("{n}", {detail}); }}')
    stubs_c.append('')
    stubs_c.append('/* externs with no address comment: unresolved names, see the report */')
    for n, _, users in cats['unknown']:
        if c_ident_ok(n):
            stubs_c.append(f'void {n}(void) {{ ll_unwritten("{n}", 0); }}')
    for n, dll, users in cats['host']:
        if c_ident_ok(n):
            host_c.append(f'void {n}(void) {{ ll_unhosted("{n}", "{dll}"); }}')

    manifest += [
        f'- objects scanned: {len(objs)}',
        f'- exe: {args.exe if read else "not available, globals zero-initialised"}',
        f'- globals defined: {n_def} ({total_bytes} bytes), data aliases: {n_alias_data}',
        f'- function aliases (stale extern names): {n_alias_fn}',
        f"- unwritten game function stubs: {len(cats['game-fn'])}",
        f"- unresolved-name stubs: {len(cats['unknown'])}",
        f"- host API stubs: {len(cats['host'])}",
        f'- pointer re-pointing (ilp32): {"on" if args.ilp32 else "off"}',
    ]
    for fname, body in (('globals.c', globals_c), ('aliases.c', aliases_c),
                        ('stubs.c', stubs_c), ('host_stubs.c', host_c), ('manifest.md', manifest)):
        with open(os.path.join(args.out, fname), 'w') as f:
            f.write('\n'.join(body) + '\n')
    with open(os.path.join(args.out, 'll_gen.h'), 'w') as f:
        f.write('void ll_unwritten(const char* name, unsigned int address);\n'
                'void ll_unhosted(const char* name, const char* dll);\n')
    print('\n'.join(manifest[2:]))


if __name__ == '__main__':
    main()
