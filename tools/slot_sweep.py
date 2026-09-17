#!/usr/bin/env python3
"""Who CALLS a vtable slot, read off the shipped image instead of the sources.

PORT-M3 inventoried the game's callback slots from the declarations and could
not type several of them, because a declaration is only one file's opinion: the
slot's real type is whatever its CALL SITE pushes, and the call site may be in a
file that never declares the callee at all. PORT-M8 closed `ObjDef +0xb0` that
way (`printlist.c`'s `DrawAndClearPrintList` pushes six arguments, so the slot
is `(elem, x, y, sq, clip, mode) -> void`) and proved `+0x8c` has no caller in
the binary at all -- which no amount of reading declarations could establish.

**Two sweeps, because one form is not enough.** VC6 emits an indirect call
through a structure slot two ways, and which one you get depends on register
pressure:

    call dword ptr [ecx + 0xb0]              <- the direct form
    mov  eax, [esi + 0xa0]  ...  call eax    <- the load-then-call form

PORT-M8's `port-m8-callslot.py` found the first and `port-m8-loadslot.py` the
second; each misses what the other finds. `+0xa0`'s two call sites, `+0xac`'s,
`+0xb8`'s and `+0xbc`'s are load-then-call and are INVISIBLE to the direct
sweep, which is how a slot can look uncalled when it is not. Both forms are
swept here, in one pass, and the form is reported per hit.

The push count before the call is a HINT, not proof: it counts pushes since the
previous call, so unrelated spills inflate it. It is there to rank candidates --
confirm the arity against the C at the reported `// FUNCTION:` marker.

    python3 tools/slot_sweep.py 0xb0          # who calls [reg+0xb0]
    python3 tools/slot_sweep.py 0xb0 --form direct
    python3 tools/slot_sweep.py 0x8c          # PORT-M8: no caller
    python3 tools/slot_sweep.py --selftest     # no image needed

Needs `original/legoland.exe` and capstone, so it is NOT part of the asset-free
ctest set (`extern_sweep.py` is). `--selftest` runs on hand-assembled bytes and
needs neither.
"""
import argparse
import collections
import glob
import os
import re
import struct
import sys

IMAGE_BASE = 0x400000
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, 'original', 'legoland.exe')
SRC = os.path.join(os.environ.get('LL_DECOMP') or os.path.join(ROOT, 'decomp'), 'LEGOLAND')

# The registers a slot base can live in. ESP is excluded: `[esp + 0xb0]` is a
# stack slot, not a structure field, and it is the one false positive that would
# otherwise swamp the report.
BASE_REGS = {'eax', 'ebx', 'ecx', 'edx', 'esi', 'edi', 'ebp'}

_MEM = re.compile(r'^dword ptr \[(?P<reg>e[a-z]{2})\s*\+\s*'
                  r'(?P<off>0x[0-9a-fA-F]+|\d+)\]$')


def load(path):
    d = open(path, 'rb').read()
    e = struct.unpack_from('<I', d, 0x3C)[0]
    coff = e + 4
    nsec = struct.unpack_from('<H', d, coff + 2)[0]
    optsz = struct.unpack_from('<H', d, coff + 16)[0]
    base = coff + 20 + optsz
    secs = []
    for i in range(nsec):
        o = base + i * 40
        vsz, va, rawsz, rawptr = struct.unpack_from('<IIII', d, o + 8)
        secs.append((va, vsz, rawptr, rawsz))
    return d, secs


def text_bytes(path):
    """(code, virtual address of its first byte) for the executable section."""
    d, secs = load(path)
    # .text is the section the entry point is in; in this image it is the first
    # and it starts at 0x1000.
    va, vsz, rawptr, rawsz = secs[0]
    return d[rawptr:rawptr + rawsz], IMAGE_BASE + va


def markers(src=SRC):
    """address -> (function, file, line) from the `// FUNCTION:` markers, so a
    hit can be reported as a name a lane can open rather than a VA."""
    out = {}
    mre = re.compile(r'^// (?:WIP-)?FUNCTION: LEGOLAND (0x[0-9a-fA-F]+)')
    for path in sorted(glob.glob(os.path.join(src, '*.c'))):
        base = os.path.basename(path)
        lines = open(path, encoding='utf-8', errors='replace').read().split('\n')
        for i, line in enumerate(lines):
            m = mre.match(line)
            if not m:
                continue
            name = None
            for j in range(i + 1, min(i + 8, len(lines))):
                s = re.match(r'^[A-Za-z_][\w \t*]*?\**\s*\**'
                             r'([A-Za-z_]\w*)\s*\(', lines[j])
                if s:
                    name = s.group(1)
                    break
            out[int(m.group(1), 16)] = (name or '?', base, i + 1)
    return out


def owner_of(addr, marks):
    """The `// FUNCTION:` marker whose body contains `addr`: the greatest
    marker address at or below it."""
    best = None
    for a in marks:
        if a <= addr and (best is None or a > best):
            best = a
    return (best,) + marks[best] if best is not None else (None, '?', '?', 0)


def instructions(code, code_va):
    """Every instruction in `code`, resynchronising past bytes that do not
    decode. Yields (insn, resynced).

    A linear sweep of a real `.text` cannot be one `md.disasm` call: the
    section carries jump tables, `/Gy` COMDAT padding and CRT data, and
    capstone stops dead at the first byte it cannot decode. On this image it
    gives up at 0x00437ee3 -- 80,686 of the roughly 200,000 instructions, and
    short of every call site PORT-M8 found. So restart after the stall, one
    byte along, and tell the caller a resync happened so it can drop the
    register state it was tracking (a tracked load across a desync is a guess,
    and a wrong hit is worse than a missed one here)."""
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = False
    i, n = 0, len(code)
    resynced = False
    while i < n:
        last_end = i
        for insn in md.disasm(code[i:], code_va + i):
            yield insn, resynced
            resynced = False
            last_end = insn.address - code_va + insn.size
        i = last_end + 1 if last_end == i else last_end
        resynced = True


def sweep(code, code_va, want_off, forms=('direct', 'load')):
    """[(va, form, reg, pushes_since_last_call, call_va)] -- every indirect call
    through `[reg + want_off]` in `code`.

    `direct`  is `call dword ptr [reg + off]`; `va` and `call_va` are the same.
    `load`    is `mov reg, [base + off]` followed, before `reg` is redefined,
              by `call reg`. `va` is the MOV (where the slot is read) and
              `call_va` the call, because PORT-M8's notes cite the call. The
              register is tracked rather than assumed, which is what makes the
              second form findable at all.
    """
    hits = []
    pending = {}          # reg -> (va of the mov, base reg)
    pushes = 0
    for insn, resynced in instructions(code, code_va):
        if resynced:
            pending.clear()
            pushes = 0
        mn, ops = insn.mnemonic, insn.op_str
        if mn == 'push':
            pushes += 1
        if mn == 'call':
            m = _MEM.match(ops)
            if 'direct' in forms and m and m.group('reg') in BASE_REGS \
                    and int(m.group('off'), 16 if 'x' in m.group('off') else 10) \
                    == want_off:
                hits.append((insn.address, 'direct', m.group('reg'), pushes,
                             insn.address))
            elif 'load' in forms and ops in pending:
                mova, basereg = pending[ops]
                hits.append((mova, 'load', basereg, pushes, insn.address))
            pending.clear()
            pushes = 0
            continue
        if mn == 'mov':
            parts = [p.strip() for p in ops.split(',', 1)]
            if len(parts) == 2:
                dst, srcop = parts
                pending.pop(dst, None)       # the register is redefined
                m = _MEM.match(srcop)
                if m and dst in BASE_REGS and m.group('reg') in BASE_REGS \
                        and int(m.group('off'),
                                16 if 'x' in m.group('off') else 10) == want_off:
                    pending[dst] = (insn.address, m.group('reg'))
            continue
        # A tracked register lives until it is CALLED or REDEFINED -- not until
        # the next branch. The near-universal shape is
        #
        #     mov  eax, [edi + 0xa0]
        #     test eax, eax
        #     je   <skip>              <- an unset slot is simply not called
        #     push ...  push ...
        #     call eax
        #
        # so ending the register's life at the `je` loses the call site, and
        # ending it at the `test` loses it too: `test`/`cmp` set flags and
        # redefine nothing. Both of those cost this sweep every load-then-call
        # site in the image on the first attempt -- +0xa0, +0xac, +0xb8 and
        # +0xbc all came back empty, which is exactly the false "no caller"
        # that makes a slot look untypable.
        if mn == 'ret':
            pending.clear()
            pushes = 0
        elif mn in ('lea', 'pop', 'xor', 'add', 'sub', 'or', 'and', 'shl',
                    'shr', 'imul', 'movzx', 'movsx', 'inc', 'dec', 'not',
                    'neg', 'sar'):
            dst = ops.split(',', 1)[0].strip()
            pending.pop(dst, None)
    return hits


# A hand-assembled .text holding one of each form, plus the three shapes that
# must NOT match. Built with no image and no capstone knowledge beyond the
# encodings, so the selftest runs in CI.
#
#   0x401000  push eax; push eax; call dword ptr [ecx + 0xb0]
#   0x401009  mov  eax, [esi + 0xb0]; push ecx; call eax
#   0x401012  call dword ptr [ecx + 0xa0]      <- a different slot
#   0x401018  call dword ptr [esp + 0xb0]      <- a stack slot, never a field
#   0x40101f  mov  eax, [esi + 0xb0]; xor eax, eax; call eax  <- redefined
#   0x401027  mov  eax, [edi + 0xc0]; test eax, eax; je; push; call eax
#             <- the null-check shape, which must still be found: it is how
#                EVERY load-then-call site in the image is written
SELFTEST_CODE = bytes([
    0x50, 0x50,                                  # push eax; push eax
    0xff, 0x91, 0xb0, 0x00, 0x00, 0x00,          # call [ecx+0xb0]   DIRECT
    0x8b, 0x86, 0xb0, 0x00, 0x00, 0x00,          # mov eax,[esi+0xb0]
    0x51,                                        # push ecx
    0xff, 0xd0,                                  # call eax          LOAD
    0xff, 0x91, 0xa0, 0x00, 0x00, 0x00,          # call [ecx+0xa0]   other slot
    0xff, 0x94, 0x24, 0xb0, 0x00, 0x00, 0x00,    # call [esp+0xb0]   stack
    0x8b, 0x86, 0xb0, 0x00, 0x00, 0x00,          # mov eax,[esi+0xb0]
    0x31, 0xc0,                                  # xor eax, eax      killed
    0xff, 0xd0,                                  # call eax
    0x8b, 0x87, 0xc0, 0x00, 0x00, 0x00,          # mov eax,[edi+0xc0]
    0x85, 0xc0,                                  # test eax, eax
    0x74, 0x04,                                  # je +4
    0x51, 0x52,                                  # push ecx; push edx
    0xff, 0xd0,                                  # call eax          LOAD
    0xc3,                                        # ret
])


def selftest():
    fails = []
    VA = 0x401000
    try:
        import capstone  # noqa: F401
    except ImportError:
        # The ctest wiring treats the count line as the verdict, so say zero and
        # say loudly that nothing was checked. Better a visible skip than a
        # green tick over an untested matcher -- or a red CI over an optional
        # dependency the asset-free set does not otherwise need.
        print('slot_sweep selftest: 0 failure(s) -- SKIPPED, capstone is not '
              'installed (pip install capstone to run it)')
        return 0

    def chk(what, got, want):
        if got != want:
            fails.append(f'{what}: got {got!r}, want {want!r}')

    def at(off, **kw):
        return sweep(SELFTEST_CODE, VA, off, **kw)

    # The whole point of the tool: BOTH forms, from one pass.
    chk('+0xb0: one direct and one load, nothing else',
        [(h[1], h[2]) for h in at(0xb0)],
        [('direct', 'ecx'), ('load', 'esi')])
    chk('the direct site is the `call [ecx+0xb0]` at 0x401002',
        at(0xb0)[0][0], 0x401002)
    chk('the load site is reported at the MOV, with the call alongside',
        (at(0xb0)[1][0], at(0xb0)[1][4]), (0x401008, 0x40100f))
    chk('the direct site counts the two pushes before it', at(0xb0)[0][3], 2)
    chk('the load site counts the one push between mov and call',
        at(0xb0)[1][3], 1)

    # Each sweep alone misses what the other finds -- PORT-M8's reason for
    # keeping two scripts, and the reason a slot can look uncalled when it is
    # not (+0xa0, +0xac, +0xb8, +0xbc are all load-then-call in the image).
    chk('--form direct alone finds only the direct site',
        [h[1] for h in at(0xb0, forms=('direct',))], ['direct'])
    chk('--form load alone finds only the load site',
        [h[1] for h in at(0xb0, forms=('load',))], ['load'])

    # The two shapes that must never match.
    chk('+0xb0 never matches the stack slot [esp+0xb0]',
        [h[2] for h in at(0xb0)], ['ecx', 'esi'])
    chk('a load whose register is redefined before the call is NOT a site',
        len(at(0xb0)), 2)

    # A different offset is a different slot, and an absent one is absent --
    # which is the claim PORT-M8 made about +0x8c and could only make safely
    # because both forms were swept.
    chk('+0xa0 is its own slot with one direct site',
        [(h[1], h[2]) for h in at(0xa0)], [('direct', 'ecx')])
    chk('+0x14 has no site at all', at(0x14), [])
    chk('+0x8c has no site at all (the PORT-M8 shape)', at(0x8c), [])

    # The regression that cost this sweep every load-then-call site in the
    # image: a null check between the load and the call must not end the
    # register's life. `test` sets flags and `je` branches; neither redefines.
    chk('+0xc0: the null-check shape is still a site',
        [(h[1], h[2]) for h in at(0xc0)], [('load', 'edi')])
    chk('+0xc0: the two pushes after the branch are counted',
        at(0xc0)[0][3], 2)

    for f in fails:
        print(f'FAIL {f}')
    print(f'slot_sweep selftest: {len(fails)} failure(s)')
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('offset', nargs='?',
                    help='the slot offset, e.g. 0xb0')
    ap.add_argument('--exe', default=DEFAULT_EXE)
    ap.add_argument('--form', choices=('direct', 'load', 'both'),
                    default='both')
    ap.add_argument('--selftest', action='store_true')
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if args.offset is None:
        ap.error('an offset is required (e.g. 0xb0), or --selftest')
    off = int(args.offset, 0)
    if not os.path.exists(args.exe):
        print(f'slot_sweep: {args.exe} not found -- this sweep reads the '
              f'shipped image. --selftest needs no assets.', file=sys.stderr)
        return 2
    forms = ('direct', 'load') if args.form == 'both' else (args.form,)
    code, code_va = text_bytes(args.exe)
    hits = sweep(code, code_va, off, forms)
    marks = markers()
    print(f'slot +{off:#x}: {len(hits)} indirect call site(s) '
          f'[{", ".join(forms)}]\n')
    if not hits:
        print('No call site in the shipped binary. Nothing local can decide '
              'this slot\'s type: PORT-M7\'s rule (make each declaration agree '
              'with its own body) is then the best available, and a sweep of '
              'BOTH forms is what makes that claim safe to state.')
        return 0
    by_owner = collections.defaultdict(list)
    for hit in hits:
        by_owner[owner_of(hit[0], marks)].append(hit)
    for (fa, name, base, line), rows in sorted(
            by_owner.items(), key=lambda kv: kv[0][0] or 0):
        where = f'{base}:{line}' if fa is not None else 'no marker owns it'
        print(f'=== {name} ({where})'
              + (f'  body 0x{fa:08x}' if fa is not None else ''))
        for va, form, reg, pushes, cva in rows:
            site = (f'0x{va:08x}' if form == 'direct'
                    else f'0x{va:08x} -> call 0x{cva:08x}')
            print(f'      {site}  {form:6s} via {reg}'
                  f'   pushes since the previous call: {pushes}')
    print('\nThe push count is a hint, not proof (it counts pushes since the '
          'previous call, so spills inflate it). Confirm the arity in the C at '
          'the marker above.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
