#!/usr/bin/env python3
"""Name the first trap the headless harness hits, in one command.

    python3 portable/tools/name_trap.py                 # the whole WinMain spine
    python3 portable/tools/name_trap.py -- --stages      # InitSession step by step
    python3 portable/tools/name_trap.py --resmount       # (-- is optional)
    python3 portable/tools/name_trap.py --table          # what the table holds
    python3 portable/tools/name_trap.py --at 0x2e9f1c    # explain one call site
    python3 portable/tools/name_trap.py --at 0x929af --kind table
    python3 portable/tools/name_trap.py --va-literals    # the B9-4 class, swept
    python3 portable/tools/name_trap.py --continue       # EVERY blocker, one run

`--continue` is the "find every blocker in one run" mode: it sets
`LL_TRAP_CONTINUE=1`, which makes `gen_link.py`'s trap helper print each distinct
symbol once and RETURN instead of exiting. A generated trap is usually a logging
call on ordinary data (`ODFError`) or a subsystem the port does not have
(AVIFIL32), and stopping at the first one hides every other. One such run on
this tree lists nine. The run past a trap is on made-up data -- a returning trap
hands its caller a zero it never computed -- so the mode produces a LIST OF WORK
and never the claim that something works; say which mode produced a result.

The port has six ways of dying and only one of them says so by itself:

* **A generated trap.** `gen_link.py` gives every Win32 import and every
  unwritten body a real C body that prints `TRAP <dll> <symbol> from <callers>`
  and exits 70. Nothing to decode; this script just surfaces the line.
* **A prototype conflict.** One source declares `extern void RES_CloseFile(void*)`
  while another DEFINES `int RES_CloseFile(RVol*)`. On x86 cdecl the caller
  simply ignores EAX; on wasm the two prototypes are two function types, so
  wasm-ld replaces the mismatched call with a stub it names
  `signature_mismatch:<callee>` whose whole body is `unreachable`. It is not a
  TRAP and not an undefined symbol, and at emcc's link-time `-O2` wasm-opt
  INLINES that one-instruction body into its callers, so all that reaches node
  is `RuntimeError: unreachable at wasm-function[26]`.
* **An indirect call whose target's type is not the call site's.** A function
  pointer cast to a type the body does not have: `screen.c`'s tables, the
  geometry vtable, every `void Foo()`-in-a-table row of the census. wasm checks
  the type AT THE CALL, so this one survives a completely clean wasm-ld run --
  the type is an immediate on the `call_indirect` instruction, not a property of
  a symbol, and there is no declaration for the linker to compare. node reports
  `RuntimeError: function signature mismatch` and NOTHING else: no index, no
  caller, no types. The static half of this file supplies all three, from the
  byte offset V8 puts in the innermost wasm frame -- `wasm-function[N]:0xOFF`,
  where 0xOFF is the instruction's offset in the module file, exactly the number
  llvm-objdump prints. That offset names one `call_indirect`, whose type
  immediate is the type the slot was expected to hold; the module's element
  segment says what is in the table; and gen_link's re-pointed words say which of
  those functions the game can actually have stored, which is what keeps the
  candidate list short. Binaryen's `directize` can also rewrite such a call into
  a plain trap before the runtime sees it, in which case the message is
  `unreachable` instead -- `--indirect` forces the same report.

* **A table index that is not an index.** `RuntimeError: table index is out of
  bounds`. The slot held a value past the end of the function table -- not a
  function of the wrong TYPE, a number that was never a table index. This is
  PORT-B9's **B9-4** class: the recovered C carries function addresses as
  INTEGER LITERALS, because the original instruction is
  `mov dword ptr [ecx+0x98], 0x45efe0` and the recovery spelled the immediate
  instead of the symbol. It matched the original bytes perfectly and is
  unrepresentable on wasm, where a function "pointer" is a table index and
  4,587,488 is not one. `sweep3.c:164-168` does it five times in
  `SetStandardCallbacks`, which runs for EVERY ODF class, so it fires on any
  park load; `loaders.c:161-191` 21 more and `coaster10.c` three. **The
  generator can never see these** -- they are immediates in CODE, not pointer
  words in `.data`, so `gen/pointers.md`'s gate cannot cover them. This script
  decodes the `call_indirect` at the reported offset, says where its index
  operand came from, and when the index is a raw x86 VA prints the
  `// FUNCTION:` marker that owns that address. When the index was LOADED from
  memory (the usual case, since the literal is stored by a different function)
  it falls back to sweeping the module for every such literal -- `--va-literals`
  does that on its own.
* **A raw address dereferenced.** `RuntimeError: memory access out of bounds`.
  The `.data` half of the same mistake (`coaster10.c`'s
  `job.shader = (void*)0x004b5648`): an image address used as a POINTER rather
  than as a table index.
* **A real fault** (a null vtable call, an `LL_UNPORTED_ASM` stub): also
  `RuntimeError`, but with a game function at the top of the stack rather than a
  mismatch stub, and none of the messages above.

Telling these apart is the whole job, and it needs a link that
wasm-opt did not touch: `legoland_headless_debug` (portable/cmake/headless.cmake)
is `legoland_headless` with `-O0 -g2` at LINK time only, so the stub survives as
a real function with its name. The game objects are shared with the optimised
harness, so building it costs one link.

What this script prints: the tail of the host-call trace for context, the named
stack innermost first, and -- when the innermost frame is a mismatch stub -- the
two signatures and the files on each side, read out of the wasm objects by
`linkreport.wasm_prototype_conflicts`, plus the declaring lines themselves. That
is the complete work item for a matching lane: the file and line to put an
`#ifdef LEGOLAND_PORTABLE` prototype in, and which side is right (the one with
the body always is).

Exit code: 0 if the run finished without a trap, 1 if a trap was named (or, under
`--continue`, if any trap was hit at all), 2 if the harness could not be built or
run.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import linkreport as lr  # noqa: E402

# `    at legoland_headless_debug.wasm.InitSession (wasm://wasm/...:0x1234)`
# The module prefix is the executable name; the symbol may contain `:` and `$`.
# The trailing `wasm-function[N]:0xOFF` is what makes an indirect-call trap
# nameable: V8's 0xOFF is the instruction's byte offset in the MODULE FILE, the
# same number llvm-objdump prints, so it points at one exact `call_indirect`.
FRAME_RE = re.compile(r'^\s*at\s+(?P<mod>[\w.-]+?)\.wasm\.(?P<sym>\S+)\s*\('
                      r'(?:[^)]*?wasm-function\[(?P<idx>\d+)\]:'
                      r'(?P<off>0x[0-9a-fA-F]+))?')
JS_FRAME_RE = re.compile(r'^\s*at\s+(?P<sym>[\w.<>$]+)\s*\(?(?P<where>\S*)')
# `TRAP <dll> <symbol> from <callers>`. The dll field is not always one token:
# an unwritten GAME body with a known address prints `GAME 0x00481234`, so dll
# is non-greedy and the LAST token before ` from ` is the symbol.
TRAP_RE = re.compile(r'^TRAP (?P<dll>.+?) (?P<sym>\S+) from (?P<from>.*)$')
ERR_RE = re.compile(r'^(?:\w+\.)*(?P<kind>RuntimeError|Error|Aborted)\b.*$')
MISMATCH_RE = re.compile(r'^signature_mismatch:(?P<callee>.+)$')

# The runtime face of a `call_indirect` whose target's type is not the type the
# call site encodes. V8 says nothing else: no index, no caller, no types --
# which is the whole reason for the static half below.
#
#   RuntimeError: function signature mismatch
#   RuntimeError: null function or function signature mismatch
#
# `unreachable` is in here too because binaryen's `directize` pass can rewrite a
# constant-index `call_indirect` whose types disagree into a trap before the
# runtime ever sees the indirect call (docs/lanes/scope-port-m2.md section 4), so
# the same defect reaches node under two different messages.
INDIRECT_ERR_RE = re.compile(r'\b(?:null function or )?function signature '
                             r'mismatch\b')

# Two more first-class kinds (PORT-A8). Neither is a signature mismatch and
# neither is a generated trap, and before this they both landed in the "a real
# body faulted, good luck" arm.
#
#   RuntimeError: table index is out of bounds
#
# The index the game loaded was past the end of the function table. That is
# PORT-B9's B9-4 class: the recovered C carries function ADDRESSES AS INTEGER
# LITERALS (`p->f3 = (void*)0x45efe0;` -- sweep3.c:164-168, loaders.c:161-191,
# coaster10.c), because the original instruction is
# `mov dword ptr [ecx+0x98], 0x45efe0` and the recovery spelled the immediate
# instead of the symbol. It matched perfectly on x86 and is unrepresentable on
# wasm, where a function "pointer" is a table index and 4,587,488 is not one.
# These are literals in CODE, so `gen_link.py` never sees them -- no amount of
# generator work can reach them, which is exactly why the trap has to name them.
#
#   RuntimeError: memory access out of bounds
#
# A load or store outside linear memory, which in this port almost always means
# a raw x86 address was used as a POINTER rather than as a table index -- the
# same defect one slot over (B9-1/B9-4's `.data` half: coaster10.c's
# `job.shader = (void*)0x004b5648`).
TABLE_OOB_RE = re.compile(r'\btable index is out of bounds\b')
MEMORY_OOB_RE = re.compile(r'\b(?:memory access|out of bounds memory access)\b'
                           r'|\bmemory access out of bounds\b')

# The image's `.text`, and ONLY .text. The whole image range is useless as a
# discriminator: emcc lays the rebuilt globals out in linear memory from a low
# base, so a perfectly ordinary pointer to a rebuilt object is an `i32.const` in
# 0x004ab000..0x00836000 -- 2,585 of them in this module. Narrowed to .text the
# same sweep returns FIVE, and the one that matters is the one whose value is
# EXACTLY a `// FUNCTION:` marker address. Exactness is the real test; the range
# only keeps the candidate list short.
TEXT_LO, TEXT_HI = 0x00401000, 0x004ab000
# `i32.load offset=0x98` / `i32.const 4587488` / `local.get 3`
OD_I32_CONST_RE = re.compile(r'^i32\.const\s+(?P<v>-?\d+|0x[0-9a-fA-F]+)')
# Two adjacent 4-byte stores of constants are merged into ONE i64 store, so a
# pair of recovered addresses arrives as a single 64-bit immediate: sweep3.c's
# five literals come out as one `i32.const` and two `i64.const`s, and a sweep
# that reads only i32 finds one of the five. Both halves of an i64 constant are
# therefore candidates in their own right.
OD_I64_CONST_RE = re.compile(r'^i64\.const\s+(?P<v>-?\d+|0x[0-9a-fA-F]+)')
OD_I32_LOAD_RE = re.compile(r'^i32\.load\b.*?(?:offset=(?P<off>\d+|0x[0-9a-fA-F]+))?')

# `     33b: 11 03 00     	call_indirect	 3`  and  `000002d4 <Other>:`
OD_INSN_RE = re.compile(r'^\s*(?P<off>[0-9a-f]+):\s+(?P<bytes>(?:[0-9a-f]{2} )+)'
                        r'\s*(?P<text>\S.*?)\s*$')
OD_FUNC_RE = re.compile(r'^(?P<off>[0-9a-f]{8})\s+<(?P<name>[^>]*)>:\s*$')
OD_CALLI_RE = re.compile(r'^call_indirect\s+(?P<type>\d+)')
# The re-pointed words gen_link writes: `(unsigned int)(__UINTPTR_TYPE__)&Name`
# inside `unsigned int g_some_table[N] = { ... }`.
GEN_ARRAY_RE = re.compile(r'unsigned int (?P<name>\w+)\[(?P<n>\d+)\]\s*=')
GEN_REF_RE = re.compile(r'&(?P<name>[A-Za-z_]\w*)')


# ---------------------------------------------------------------------------
# The LINKED module: types, function types, names, and the indirect table.
#
# linkreport.wasm_object_sigs reads OBJECT files, where the "linking" custom
# section carries a symbol table. A linked module has no symbol table at all:
# the only names in it come from the `name` custom section, which is why
# --profiling-funcs (or -g2) is not optional for this report. Everything here is
# read straight out of the binary -- five sections, no dependencies.
# ---------------------------------------------------------------------------

def _leb(b, i):
    r = s = 0
    while True:
        x = b[i]
        i += 1
        r |= (x & 0x7f) << s
        if not x & 0x80:
            return r, i
        s += 7


def _skip_expr(b, i):
    """Step over a constant expression (an element segment's offset)."""
    depth = 0
    while i < len(b):
        op = b[i]
        i += 1
        if op == 0x0b and depth == 0:          # end
            return i
        if op in (0x02, 0x03, 0x04):           # block / loop / if
            depth += 1
            i += 1
        elif op == 0x0b:
            depth -= 1
        elif op in (0x41, 0x23, 0x20, 0x21, 0x22, 0x24, 0xd2):  # i32.const etc.
            _, i = _leb(b, i)
        elif op == 0x42:                       # i64.const
            _, i = _leb(b, i)
        elif op == 0x43:                       # f32.const
            i += 4
        elif op == 0x44:                       # f64.const
            i += 8
        elif op == 0xd0:                       # ref.null
            i += 1
    return i


class Module(object):
    """The pieces of a linked wasm module this report needs."""

    def __init__(self, path):
        with open(path, 'rb') as f:
            b = f.read()
        self.path = path
        self.ok = b[:4] == b'\0asm'
        self.types = []            # [(params, results)]
        self.fn_types = []         # type index per function, in index space order
        self.names = {}            # function index -> name
        self.table = {}            # table slot -> function index
        self.n_imports = 0
        if not self.ok:
            return
        i = 8
        while i < len(b):
            sid = b[i]
            i += 1
            size, i = _leb(b, i)
            end = i + size
            try:
                if sid == 1:
                    self._types(b, i)
                elif sid == 2:
                    self._imports(b, i)
                elif sid == 3:
                    n, i2 = _leb(b, i)
                    for _ in range(n):
                        t, i2 = _leb(b, i2)
                        self.fn_types.append(t)
                elif sid == 9:
                    self._elements(b, i)
                elif sid == 0:
                    nl, j = _leb(b, i)
                    if b[j:j + nl] == b'name':
                        self._names(b, j + nl, end)
            except (IndexError, ValueError):
                pass               # a section this reader does not understand
            i = end

    def _types(self, b, i):
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
            self.types.append((params, results))

    def _imports(self, b, i):
        n, i = _leb(b, i)
        imported_fns = []
        for _ in range(n):
            ml, i = _leb(b, i)
            i += ml
            fl, i = _leb(b, i)
            field = b[i:i + fl].decode('utf-8', 'replace')
            i += fl
            kind = b[i]
            i += 1
            if kind == 0:
                t, i = _leb(b, i)
                imported_fns.append((field, t))
            elif kind == 1:        # table
                i += 1
                flags = b[i]
                i += 1
                _, i = _leb(b, i)
                if flags & 1:
                    _, i = _leb(b, i)
            elif kind == 2:        # memory
                flags = b[i]
                i += 1
                _, i = _leb(b, i)
                if flags & 1:
                    _, i = _leb(b, i)
            elif kind == 3:        # global
                i += 2
            elif kind == 4:        # tag
                i += 1
                _, i = _leb(b, i)
            else:
                break
        # Imports come first in the function index space, so their type indices
        # have to be at the head of fn_types for func_type() to be right.
        self.n_imports = len(imported_fns)
        self.fn_types = [t for _n, t in imported_fns]
        for k, (nm, _t) in enumerate(imported_fns):
            self.names[k] = nm

    def _elements(self, b, i):
        n, i = _leb(b, i)
        for _ in range(n):
            flags, i = _leb(b, i)
            base = 0
            if flags in (0, 2, 4, 6):           # active: has an offset expr
                if flags in (2, 6):
                    _, i = _leb(b, i)           # table index
                j = _skip_expr(b, i)
                # The offset is `i32.const <base>` in every module emcc emits.
                if b[i] == 0x41:
                    base, _ = _leb(b, i + 1)
                i = j
            elif flags in (1, 5):
                pass
            if flags in (1, 2, 5, 6):
                i += 1                          # elem kind / reftype
            cnt, i = _leb(b, i)
            if flags in (4, 5, 6, 7):           # vector of expressions
                for k in range(cnt):
                    if b[i] == 0xd2:            # ref.func
                        f, i = _leb(b, i + 1)
                        self.table[base + k] = f
                    i = _skip_expr(b, i)
            else:                               # vector of function indices
                for k in range(cnt):
                    f, i = _leb(b, i)
                    self.table[base + k] = f

    def _names(self, b, i, end):
        while i < end:
            sub = b[i]
            i += 1
            sz, i = _leb(b, i)
            stop = i + sz
            if sub == 1:                        # function names
                cnt, i = _leb(b, i)
                for _ in range(cnt):
                    idx, i = _leb(b, i)
                    nl, i = _leb(b, i)
                    self.names[idx] = b[i:i + nl].decode('utf-8', 'replace')
                    i += nl
            i = stop

    def func_type(self, idx):
        if 0 <= idx < len(self.fn_types) and self.fn_types[idx] < len(self.types):
            return self.types[self.fn_types[idx]]
        return None

    def func_name(self, idx):
        return self.names.get(idx, 'wasm-function[%d]' % idx)

    def type_text(self, t):
        return lr.sig_text(self.types[t]) if 0 <= t < len(self.types) else '?'


def objdump():
    """llvm-objdump from the Emscripten install, which is the one that reads the
    wasm emcc just wrote. $LL_OBJDUMP overrides."""
    if os.environ.get('LL_OBJDUMP'):
        return os.environ['LL_OBJDUMP']
    em = shutil.which('em-config')
    if em:
        try:
            root = subprocess.run([em, 'LLVM_ROOT'], capture_output=True,
                                  text=True).stdout.strip()
            cand = os.path.join(root, 'llvm-objdump')
            if os.path.exists(cand):
                return cand
        except OSError:
            pass
    return shutil.which('llvm-objdump')


def disassemble(wasm):
    """(instructions, owner) -- `instructions` maps a module byte offset to the
    instruction text, `owner` maps it to the enclosing function's name."""
    od = objdump()
    if not od:
        return None, None
    try:
        out = subprocess.run([od, '-d', wasm], capture_output=True,
                             text=True).stdout
    except OSError:
        return None, None
    insns, owner, cur = {}, {}, '?'
    for line in out.splitlines():
        m = OD_FUNC_RE.match(line)
        if m:
            cur = m.group('name')
            continue
        m = OD_INSN_RE.match(line)
        if m:
            off = int(m.group('off'), 16)
            insns[off] = m.group('text')
            owner[off] = cur
    return insns, owner


def data_fn_refs(build_dir):
    """Every function whose ADDRESS the rebuilt globals hold: name -> [(global,
    word index)], read out of gen_link's own output.

    This is the set of functions the game reaches through a `call_indirect`, and
    it is what turns "some table slot had the wrong type" into a short list of
    named candidates: a function nothing stores in .data cannot be in a slot the
    game loaded from .data."""
    refs = {}
    for sub in ('gen', 'gen-browser'):
        path = os.path.join(build_dir, sub, 'globals.c')
        if not os.path.exists(path):
            continue
        array, word = None, 0
        with open(path, encoding='utf-8', errors='replace') as f:
            for line in f:
                m = GEN_ARRAY_RE.search(line)
                if m:
                    array, word = m.group('name'), 0
                    continue
                if array is None:
                    continue
                if line.strip() in ('};', '}'):
                    array = None
                    continue
                for hit in GEN_REF_RE.finditer(line):
                    refs.setdefault(hit.group('name'), set()).add(
                        (array, word + line[:hit.start()].count(',')))
                word += line.count(',')
        # The first file that exists is enough; both closures are generated from
        # the same plan and gen-browser only drops host stubs.
        if refs:
            break
    return {k: sorted(v) for k, v in refs.items()}


_MARKERS = None


def markers():
    """address -> (function, file, line) from every `// FUNCTION:` /
    `// WIP-FUNCTION:` marker, so a raw x86 VA in the wasm can be turned into
    the recovered body that owns it."""
    global _MARKERS
    if _MARKERS is not None:
        return _MARKERS
    out = {}
    mre = re.compile(r'^// (?:WIP-)?FUNCTION: LEGOLAND (0x[0-9a-fA-F]+)')
    sig = re.compile(r'^[A-Za-z_][\w \t*]*?\**\s*\**([A-Za-z_]\w*)\s*\(')
    import glob
    for path in sorted(glob.glob(os.path.join(ROOT, 'LEGOLAND', '*.c'))):
        base = os.path.basename(path)
        lines = open(path, encoding='utf-8', errors='replace').read().split('\n')
        for i, line in enumerate(lines):
            m = mre.match(line)
            if not m:
                continue
            name = next((s.group(1) for s in
                         (sig.match(lines[j]) for j in
                          range(i + 1, min(i + 8, len(lines)))) if s), '?')
            out[int(m.group(1), 16)] = (name, base, i + 1)
    _MARKERS = out
    return out


def owning_marker(addr):
    """(va, name, file, line) of the body that contains `addr`, or None.

    Exact is the interesting case -- a recovered `(void*)0x45efe0` is meant to BE
    a function -- but an interior address is worth naming too: it says the
    literal is a jump into the middle of a body, which is a different mistake."""
    marks = markers()
    if addr in marks:
        return (addr,) + marks[addr]
    below = [a for a in marks if a < addr]
    if not below:
        return None
    a = max(below)
    # Only claim ownership within a plausible body span; past that the nearest
    # marker below is just the nearest marker below and says nothing.
    return (a,) + marks[a] if addr - a < 0x4000 else None


def va_constants(insns, owner):
    """[(module offset, owner function, value, exact_marker)] -- every constant
    in the module whose value lands in the image's `.text`.

    This is PORT-B9's B9-4 sweep done on the LINKED MODULE instead of the
    sources: a function address written as an integer literal in the recovered C
    survives compilation as such a constant, and reading the module rather than
    the C catches it however it was spelled.

    Two things make it sharp rather than noisy. The range is `.text` only, not
    the whole image -- linear memory overlaps the image's DATA range, so the
    wider window returns thousands of perfectly ordinary pointers. And an
    `i64.const` is split into both halves, because the optimiser merges two
    adjacent 4-byte stores of constants into one 8-byte store: `sweep3.c`'s five
    literals reach the module as one `i32.const` and two `i64.const`s.

    `exact_marker` is the verdict. A value that is EXACTLY a `// FUNCTION:`
    marker address is a recovered function address and nothing else; a value
    merely inside the range is almost always an ordinary integer (a mask, a
    buffer size, an `open` flag) and is reported apart from the real hits."""
    marks = markers()
    out = []
    for off in sorted(insns):
        text = insns[off]
        vals = []
        m = OD_I32_CONST_RE.match(text)
        if m:
            try:
                vals = [int(m.group('v'), 0) & 0xffffffff]
            except ValueError:
                vals = []
        else:
            m = OD_I64_CONST_RE.match(text)
            if m:
                try:
                    q = int(m.group('v'), 0) & 0xffffffffffffffff
                    vals = [q & 0xffffffff, (q >> 32) & 0xffffffff]
                except ValueError:
                    vals = []
        for v in vals:
            if TEXT_LO <= v < TEXT_HI:
                out.append((off, owner.get(off, '?'), v, v in marks))
    return out


def explain_table_index(wasm, frames, offset, build_dir, limit=12):
    """Name a `table index is out of bounds`: the `call_indirect`, where its
    index operand came from, and -- when the index is a raw x86 VA -- the
    `// FUNCTION:` marker that owns that address."""
    mod = Module(wasm)
    insns, owner = disassemble(wasm)
    if insns is None:
        print('   (llvm-objdump is not available: set $LL_OBJDUMP to the one in'
              ' the Emscripten install to decode the call site)')
        return 1
    site = None
    if offset is not None:
        for off in sorted(o for o in insns if o <= offset)[::-1][:4]:
            m = OD_CALLI_RE.match(insns[off])
            if m:
                site = (off, int(m.group('type')))
                break
    print('\n== TABLE INDEX OUT OF BOUNDS')
    if site is None:
        print('   (no call_indirect at the offset the stack reported; relink at'
              ' -O0, which legoland_headless_debug already does)')
    else:
        off, tidx = site
        print(f'   caller      {owner.get(off, frames[0] if frames else "?")}'
              f'   (module offset 0x{off:x})')
        print(f'   call site   call_indirect type {tidx} = '
              f'{mod.type_text(tidx) if mod.ok else "?"}')
        print(f'   table       {len(mod.table)} slots in the element segment')
        # The index operand is produced by the instruction(s) immediately before
        # the call. One level is enough to tell a constant from a memory load,
        # which is the distinction that matters.
        before = [o for o in sorted(insns) if o < off][-6:]
        print('   operand source, innermost last:')
        for o in before:
            print(f'      0x{o:<8x} {insns[o]}')
        src = insns[before[-1]] if before else ''
        mc = OD_I32_CONST_RE.match(src)
        if mc:
            v = int(mc.group('v'), 0) & 0xffffffff
            if TEXT_LO <= v < TEXT_HI:
                print(f'\n   THE INDEX IS A RAW x86 ADDRESS: {v:#x} ({v}).')
                _name_va(v)
                _b9_4_advice()
                return 1
            print(f'\n   The index is the constant {v}, and the table has '
                  f'{len(mod.table)} slots.')
        elif src.startswith('i32.load'):
            print('\n   The index was LOADED from memory, so the bad value was'
                  ' written by some')
            print('   earlier store and this call site is only where it was'
                  ' USED. That is the')
            print('   B9-4 shape: a function address written as an integer'
                  ' literal in the C')
            print('   (`p->f3 = (void*)0x45efe0;`) is stored into a callback'
                  ' slot and reaches')
            print('   the call as a number that was never a table index.')
        elif src.startswith('local.get') or src.startswith('global.get'):
            print(f'\n   The index came from {src.strip()}, not from a constant'
                  f' or a load here, so')
            print('   this frame did not choose it -- it was passed in, or'
                  ' loaded further up.')
            print('   Read the caller in the named stack above; the sweep below'
                  ' says what bad')
            print('   value is available to be passed.')
        # Either way, list the literals in the module: the write site is what
        # has to be fixed, and this finds it without a debugger.
        exact = [r for r in va_constants(insns, owner) if r[3]]
        if not exact:
            print('\n   No constant in the module is exactly a `// FUNCTION:`'
                  ' marker address, so the')
            print('   index did not come from a recovered address literal.'
                  ' Look for an index')
            print('   computed from a count, or an uninitialised slot,'
                  ' instead.')
            return 1
        print(f'\n== {len(exact)} recovered function address(es) spelled as a'
              f' literal -- B9-4, swept')
        print('   Each is an integer literal in the recovered C whose value is'
              ' exactly a marker')
        print('   address. One of these was stored into the slot this call'
              ' loaded.')
        by_owner = {}
        for _o, own, v, _e in exact:
            by_owner.setdefault(own, set()).add(v)
        for own in sorted(by_owner, key=lambda k: -len(by_owner[k]))[:limit]:
            print(f'   {own}')
            for v in sorted(by_owner[own])[:8]:
                _va, nm, base, line = owning_marker(v)
                print(f'      {v:#010x} -> {nm}  ({base}:{line})')
        if len(by_owner) > limit:
            print(f'   ... and {len(by_owner) - limit} more functions')
        _b9_4_advice()
    return 1


def _name_va(v):
    mk = owning_marker(v)
    if not mk:
        print(f'   Nothing in LEGOLAND/*.c has a `// FUNCTION:` marker at or'
              f' just below {v:#x};')
        print(f'   it may be a .data address used as a function (coaster10.c\'s'
              f' shape) or a gap.')
        return
    va, name, base, line = mk
    if va == v:
        print(f'   // FUNCTION: LEGOLAND {va:#010x} is {name}  ({base}:{line})')
        print(f'   So the slot was meant to hold {name} and holds its x86'
              f' address instead.')
    else:
        print(f'   {v:#x} is 0x{v - va:x} INTO {name} ({base}:{line}, marker'
              f' {va:#010x})')
        print('   -- an interior address, so the literal is not even a function'
              ' entry point.')


def _b9_4_advice():
    print('\n   FIX (B9-4, docs/lanes/scope-port-b9.md §4): in the DECLARING'
          ' file, take the')
    print('   body\'s address instead of spelling the immediate --'
          ' `p->f3 = (void*)&AddBasicObject;`')
    print('   -- under `#ifdef LEGOLAND_PORTABLE`, then re-gate with audit.py'
          ' and relocs.py.')
    print('   On x86 taking a function\'s address compiles to exactly that'
          ' immediate, so it is')
    print('   worth trying UNGUARDED first and letting the byte gates say'
          ' whether anything moved.')


def explain_memory_oob(wasm, frames, offset, build_dir, limit=12):
    """Name a `memory access out of bounds`. Same root cause one slot over: a
    raw x86 address used as a POINTER rather than as a table index."""
    insns, owner = disassemble(wasm)
    print('\n== MEMORY ACCESS OUT OF BOUNDS')
    if insns is None:
        print('   (llvm-objdump is not available: set $LL_OBJDUMP to decode the'
              ' faulting access)')
        return 1
    if offset is not None:
        before = [o for o in sorted(insns) if o <= offset][-6:]
        print(f'   faulting access in {owner.get(before[-1], frames[0] if frames else "?")}'
              f'   (module offset 0x{offset:x})')
        for o in before:
            print(f'      0x{o:<8x} {insns[o]}')
    exact = [r for r in va_constants(insns, owner) if r[3]]
    if not exact:
        print('\n   No recovered function address survives as a literal in this'
              ' module, so this is')
        print('   an ordinary out-of-bounds: an index past the end of a rebuilt'
              ' object, or a')
        print('   pointer the game never initialised. gen/pointers.md\'s `raw'
              ' pointer words` gate')
        print('   covers the .data half of the address-literal class; this'
              ' sweep only sees .text,')
        print('   because linear memory overlaps the image\'s DATA range and a'
              ' wider window')
        print('   reports thousands of perfectly ordinary pointers.')
        return 1
    print(f'\n   {len(exact)} recovered function address(es) are spelled as'
          f' literals in this module.')
    print('   A load or store through one is exactly this fault -- the same'
          ' recovery mistake')
    print('   as B9-4, one slot over (coaster10.c\'s'
          ' `job.shader = (void*)0x004b5648` is its')
    print('   `.data` form, which this .text-only sweep does NOT see):')
    seen = set()
    for _o, own, v, _e in exact:
        if (own, v) in seen:
            continue
        seen.add((own, v))
        _va, nm, base, line = owning_marker(v)
        print(f'      {v:#010x} in {own}  -> {nm} ({base}:{line})')
        if len(seen) >= limit:
            break
    _b9_4_advice()
    return 1


def type_distance(want, have):
    """How far `have` is from `want`, so the nearest candidate sorts first.

    A different arity or a different return type means a different table; the
    interesting case is the same shape with one parameter retyped, which is what
    a float passed as its bit pattern, or a struct flattened into ints, looks
    like on wasm (docs/lanes/scope-port-m2.md sections 2 and 3)."""
    if want is None or have is None:
        return 99
    wp, wr = want
    hp, hr = have
    d = 0 if wr == hr else 8
    d += abs(len(wp) - len(hp)) * 4
    d += sum(1 for x, y in zip(wp, hp) if x != y)
    return d


def explain_indirect(wasm, frames, offset, build_dir, limit=12):
    """Name a `call_indirect` type mismatch: the caller, the type the call site
    encodes, and the functions that could be in the slot with another type."""
    mod = Module(wasm)
    if not mod.ok:
        print(f'   ({wasm} is not a wasm module)')
        return
    insns, owner = disassemble(wasm)
    if insns is None:
        print('   (llvm-objdump is not available: set $LL_OBJDUMP to the one in'
              ' the Emscripten install to get the call site)')
        return

    site = None
    if offset is not None:
        # V8 reports the offset of the trapping instruction itself; accept a
        # small window anyway, because a trap attributed to a folded call can
        # land on the instruction before it.
        for off in sorted(o for o in insns if o <= offset)[::-1][:4]:
            m = OD_CALLI_RE.match(insns[off])
            if m:
                site = (off, int(m.group('type')))
                break
    if site is None:
        print('   (no call_indirect at the offset the stack reported; the frame'
              ' may have been inlined -- relink the harness at -O0, which'
              ' legoland_headless_debug already does)')
        return

    off, tidx = site
    caller = owner.get(off, frames[0] if frames else '?')
    want = mod.types[tidx] if tidx < len(mod.types) else None
    print(f'\n== INDIRECT CALL TYPE MISMATCH')
    print(f'   caller      {caller}   (module offset 0x{off:x})')
    print(f'   call site   call_indirect type {tidx} = {mod.type_text(tidx)}')
    print('   The slot the game loaded held a function of a DIFFERENT wasm type,')
    print('   so the call trapped. wasm-ld cannot warn about this: the type is')
    print('   encoded at the call site, not in a symbol declaration.')

    # Where the index came from: the instructions just before the call name the
    # data address of the callback table, which is the game global to look at.
    before = [o for o in sorted(insns) if o < off][-8:]
    # Only the constants big enough to be addresses: a `i32.const 2` feeding the
    # i32.shl that scales the index is not a table base.
    consts = []
    for o in before:
        if insns[o].startswith('i32.const'):
            try:
                v = int(insns[o].split()[-1])
            except ValueError:
                continue
            if v >= 1024:
                consts.append(v)
    if before:
        print('\n== the call site, in order')
        for o in before:
            print(f'   {o:#8x}  {insns[o]}')
        print(f'   {off:#8x}  {insns[off]}        <-- trapped here')
    if consts:
        print('   the constants above are linear-memory addresses, so the'
              ' callback table')
        print('   the index came out of is at '
              + ' or '.join(f'0x{v:x} ({v})' for v in consts[-3:])
              + ' -- grep gen/globals.c')
        print('   for the rebuilt global whose words land there.')

    # The candidates: functions whose address the rebuilt globals hold (so the
    # game really can have put them in a slot) whose real type is not `want`.
    # Without that filter the list is the whole table; with it, it is the set of
    # functions a STATIC game callback table can reach.
    stored = data_fn_refs(build_dir)
    by_name = {}
    for idx in sorted(set(mod.table.values())):
        by_name.setdefault(mod.func_name(idx), idx)
    bad, other = [], []
    for name, idx in sorted(by_name.items()):
        t = mod.func_type(idx)
        if t is None or want is None or t == want:
            continue
        (bad if name in stored else other).append(
            (name, lr.sig_text(t), stored.get(name, []), t))
    if bad:
        # Ranked by how CLOSE the real type is to the expected one, because that
        # is the shape of the real defect: PORT-M2's geometry vtable expected
        # `(i32, i32, i32) -> void` and the slots held `(i32, f32, i32) -> void`
        # -- same arity, one parameter retyped. A candidate with a different
        # arity is a different table, and alphabetical order buries the answer
        # under 200 rows of unrelated callbacks.
        bad.sort(key=lambda row: (type_distance(want, row[3]), row[0]))
        print(f'\n== candidate targets: {len(bad)} function(s) whose address a'
              f' rebuilt global holds and whose real type is not'
              f' {mod.type_text(tidx)},')
        print('   nearest type first (same arity and return, fewest parameters'
              ' retyped)')
        rows = bad
    else:
        print(f'\n== candidate targets: no function the rebuilt globals store'
              f' has a type other than {mod.type_text(tidx)}, so the slot was'
              f' written at RUNTIME')
        print('   (LLIDB_RegisterNewElement and the screen tables do that).'
              ' Falling back to every table entry of the wrong type:')
        rows = other
    for row in rows[:limit]:
        name, text, where = row[0], row[1], row[2]
        slots = ', '.join(f'{g}[{w}]' for g, w in where[:3]) or '(runtime)'
        print(f'   {name:<34s} {text:<30s} stored in {slots}')
    if len(rows) > limit:
        print(f'   ... and {len(rows) - limit} more (--table for the census)')

    # The table, not the function, is the work item: one global holds a whole
    # family of bodies the call site is typed wrongly for, and fixing the
    # declaration of that global fixes every slot in it at once.
    tables = {}
    for row in bad:
        for g, _w in row[2]:
            tables.setdefault(g, {}).setdefault(row[1], 0)
            tables[g][row[1]] += 1
    if tables:
        print('\n== by callback table: the global holding the wrong-typed'
              ' bodies is what to re-declare')
        ranked = sorted(tables.items(),
                        key=lambda kv: -sum(kv[1].values()))
        for g, kinds in ranked[:8]:
            shapes = ', '.join(f'{k} x{v}' for k, v in
                               sorted(kinds.items(), key=lambda kv: -kv[1])[:3])
            print(f'   {g:<34s} {shapes}')
        if len(ranked) > 8:
            print(f'   ... and {len(ranked) - 8} more globals')
        print('   (the name is the BLOCK\'s head symbol: gen_link emits one'
              ' block per object and')
        print('    the rest as offset aliases, so gen/manifest.md\'s'
              ' interior-alias table says')
        print('    which declared name the slot really belongs to.)')
    print('\n   The fix is a typed-callback pass in the DECLARING game file: the')
    print('   table must be declared with the signature the bodies in it have,')
    print('   under `#ifdef LEGOLAND_PORTABLE`, re-gated with audit.py and')
    print('   relocs.py. gen/manifest.md\'s "Cast forwarders" table is the same')
    print('   defect class seen statically.')


def va_literal_census(wasm):
    """Every raw x86 address literal in the linked module, by owning function.

    PORT-B9 swept the SOURCES for this class (29 sites in three files). Sweeping
    the module instead catches it wherever it is written and however it is
    spelled -- a macro, a cast, a table initialiser in a function body -- and
    proves the negative when there is nothing left. Exit status is the gate."""
    if not os.path.exists(wasm):
        print(f'name_trap: {wasm} does not exist', file=sys.stderr)
        return 2
    insns, owner = disassemble(wasm)
    if insns is None:
        print('name_trap: llvm-objdump is not available (set $LL_OBJDUMP)',
              file=sys.stderr)
        return 2
    vas = va_constants(insns, owner)
    exact = [r for r in vas if r[3]]
    loose = [r for r in vas if not r[3]]
    print(f'== {os.path.basename(wasm)}: {len(vas)} constant(s) in the image\'s '
          f'.text ({TEXT_LO:#x}..{TEXT_HI:#x}),')
    print(f'   of which {len(exact)} are EXACTLY a `// FUNCTION:` marker '
          f'address.')
    if not exact:
        print('\n   No recovered function address survives as an integer '
              'literal in code.')
        if loose:
            print(f'   ({len(loose)} constant(s) fall in the range without '
                  f'matching a marker; those are')
            print('    ordinary integers -- a mask, a size, an `open` flag -- '
                  'and are listed below.)')
            _print_loose(loose)
        return 0
    print('\n== the B9-4 class: a function address spelled as a number')
    print('   On x86 each of these matched the original immediate exactly. On '
          'wasm a function')
    print('   "pointer" is a table index, so the value is not a function at '
          'all -- it reaches a')
    print('   `call_indirect` as an index far past the end of the table. The '
          'generator cannot')
    print('   help: these are immediates in CODE, not pointer words in .data.')
    by_owner = {}
    for _o, own, v, _e in exact:
        by_owner.setdefault(own, set()).add(v)
    for own in sorted(by_owner, key=lambda k: (-len(by_owner[k]), k)):
        print(f'\n   {own}  ({len(by_owner[own])} distinct)')
        for v in sorted(by_owner[own]):
            _va, nm, base, line = owning_marker(v)
            print(f'      {v:#010x}  -> {nm}  ({base}:{line})')
    _b9_4_advice()
    if loose:
        print(f'\n== {len(loose)} other constant(s) in the range, marker-exact: '
              f'no')
        print('   Reported apart because they are almost certainly ordinary '
              'integers that happen')
        print('   to land in .text. Check one only if its owner has no reason '
              'to hold a big number.')
        _print_loose(loose)
    return 1


def _print_loose(loose):
    seen = set()
    for _o, own, v, _e in loose:
        if (own, v) in seen:
            continue
        seen.add((own, v))
        print(f'      {v:#010x}  in {own}')


def table_census(wasm, build_dir):
    """Every distinct wasm type in the indirect table, with how many slots hold
    it -- the map of what a `call_indirect` can land on."""
    mod = Module(wasm)
    if not mod.ok:
        print(f'name_trap: {wasm} is not a wasm module', file=sys.stderr)
        return 2
    stored = data_fn_refs(build_dir)
    counts, examples = {}, {}
    for slot, idx in sorted(mod.table.items()):
        t = mod.func_type(idx)
        key = lr.sig_text(t) if t else '?'
        counts[key] = counts.get(key, 0) + 1
        if key not in examples:
            examples[key] = f'{mod.func_name(idx)} (slot {slot})'
    print(f'== {os.path.basename(wasm)}: {len(mod.table)} table slots, '
          f'{len(counts)} distinct signatures, {len(mod.types)} types')
    for key in sorted(counts, key=lambda k: -counts[k]):
        print(f'   {counts[key]:6d}  {key:<34s} e.g. {examples[key]}')
    in_data = sum(1 for idx in set(mod.table.values())
                  if mod.func_name(idx) in stored)
    print(f'   {in_data} of the table\'s distinct functions have their address'
          f' in a rebuilt global (gen_link\'s re-pointed words), so those are'
          f' the ones a game callback table can reach.')
    return 0


def build(build_dir, target, quiet):
    if not os.path.isdir(build_dir):
        print(f'name_trap: no build directory {build_dir}\n'
              f'  emcmake cmake -S portable -B {build_dir} -G Ninja '
              f'-DCMAKE_BUILD_TYPE=Release -DLL_ILP32=ON', file=sys.stderr)
        return False
    r = subprocess.run(['ninja', '-C', build_dir, target],
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stdout + r.stderr)
        print(f'name_trap: could not build {target}', file=sys.stderr)
        return False
    if not quiet:
        # wasm-ld's own mismatch warnings are the static half of this report:
        # every one of them is a call site that WILL trap if the game reaches it.
        warned = sorted(set(re.findall(r'function signature mismatch: (\S+)',
                                       r.stdout + r.stderr)))
        print(f'== {target}: linked, wasm-ld warned about {len(warned)} '
              f'mismatched signatures')
        if warned:
            print('   ' + ', '.join(warned))
    return True


def run(js, args, env, timeout):
    """node's stdout+stderr, interleaved in order, and the exit status."""
    out = subprocess.run(['node', js] + args, cwd=os.path.dirname(js) or '.',
                         env=env, capture_output=True, text=True,
                         timeout=timeout, stdin=subprocess.DEVNULL)
    # The harness writes its progress to stderr and the game's own printf to
    # stdout; the trap and the stack are both on stderr, so stderr is the
    # stream that carries the story and stdout goes after it.
    return out.stdout, out.stderr, out.returncode


def declared_lines(name):
    """Every `extern`-style declaration of `name` in the game sources, with the
    file and line, so the report says exactly what to edit."""
    hits = []
    pat = re.compile(r'\b' + re.escape(name) + r'\b')
    for fn in sorted(os.listdir(lr.SRC)):
        if not fn.endswith('.c'):
            continue
        path = os.path.join(lr.SRC, fn)
        with open(path, encoding='utf-8', errors='replace') as f:
            for n, line in enumerate(f, 1):
                if line.lstrip().startswith('extern') and pat.search(line):
                    hits.append((fn, n, line.rstrip()))
    return hits


def explain_mismatch(callee, build_dir):
    """The two signatures and the files on each side, from the wasm objects."""
    objs = []
    for sub in ('legoland_core.dir', 'legoland_hostwin.dir'):
        d = os.path.join(build_dir, 'CMakeFiles', sub)
        if os.path.isdir(d):
            for root, _dirs, files in os.walk(d):
                objs += [os.path.join(root, f) for f in files if f.endswith('.o')]
    if not objs:
        print(f'   (no wasm objects under {build_dir}/CMakeFiles: '
              f'cannot read the two signatures)')
        return
    for name, dsig, dobj, bad in lr.wasm_prototype_conflicts(objs):
        if name != callee:
            continue
        print(f'   DEFINED  {lr.sig_text(dsig):28s} in {dobj}   <-- has the body,'
              f' so this is the right one')
        for sig, users in sorted(bad.items()):
            print(f'   DECLARED {lr.sig_text(sig):28s} by {", ".join(users)}')
        break
    else:
        print('   (not in the prototype-conflict census: the stub may come from'
              ' a host shim signature, not a game one)')
    for fn, n, text in declared_lines(callee):
        print(f'   LEGOLAND/{fn}:{n}: {text.strip()}')


def report(stdout, stderr, status, trace_tail, trap_continue=False):
    lines = stderr.splitlines()

    traps = [m for m in (TRAP_RE.match(ln) for ln in lines) if m]
    trap = traps[0] if traps else None
    host = [ln for ln in lines if ln.startswith('HOST ')]
    stage = [ln for ln in lines if '--- ' in ln]

    if host and trace_tail:
        print(f'\n== last {min(trace_tail, len(host))} host calls')
        for ln in host[-trace_tail:]:
            print('   ' + ln)
    if stage:
        print(f'\n== last stage reached: {stage[-1].split("--- ", 1)[1]}')

    if trap_continue:
        # Every trap on the path, in the order the game reached them. The run
        # did not stop at any of them, so the stack analysis below still has to
        # run: what killed this run (if anything did) is something else.
        print(f'\n== {len(traps)} trap(s) on the path, in the order hit '
              '(LL_TRAP_CONTINUE)')
        for m in traps:
            print(f'   {m.group("dll"):16} {m.group("sym"):24} '
                  f'<- {m.group("from")}')
        if not traps:
            print('   (none)')
        print('   Each returned a zero its caller did not ask for: this is a '
              'list of work, not a passing run.')
    elif trap:
        print(f'\n== TRAP: {trap.group("sym")} ({trap.group("dll")})')
        print(f'   referenced by {trap.group("from")}')
        print('   A generated body: the host shim or a game source has to '
              'define it.')
        return 1

    # The stack: the first error line, then its `at ...` frames. `sites` keeps
    # the (function index, module offset) V8 puts in each wasm frame, which is
    # what pins an indirect-call trap to one instruction.
    err, frames, sites = None, [], []
    for i, ln in enumerate(lines):
        m = ERR_RE.match(ln.strip())
        if m and not frames:
            err = ln.strip()
            for tail in lines[i + 1:]:
                f = FRAME_RE.match(tail)
                if f:
                    frames.append(f.group('sym'))
                    sites.append((int(f.group('idx')) if f.group('idx') else None,
                                  int(f.group('off'), 16) if f.group('off') else None))
                elif tail.strip().startswith('at '):
                    frames.append('[js] ' + (JS_FRAME_RE.match(tail).group('sym')
                                             if JS_FRAME_RE.match(tail) else '?'))
                elif frames:
                    break
    if not err:
        print(f'\n== no fault: the harness exited {status}')
        if stdout.strip():
            print('\n'.join('   ' + ln for ln in stdout.splitlines()[-10:]))
        return 1 if traps else 0

    wasm = [f for f in frames if not f.startswith('[js] ')]
    print(f'\n== {err}')
    print('== named stack, innermost first')
    for f in frames:
        print(f'   {f}')
    if not wasm:
        print('\n   No wasm frame was named. The link kept no name section: '
              'build legoland_headless_debug (-O0 -g2 at link), not '
              'legoland_headless.')
        return 1

    m = MISMATCH_RE.match(wasm[0])
    if m:
        callee = m.group('callee')
        caller = wasm[1] if len(wasm) > 1 else '?'
        print(f'\n== PROTOTYPE CONFLICT, live: '
              f'signature_mismatch:{callee} <- {caller}')
        return callee

    # A `call_indirect` whose target's type is not the call site's. V8 says only
    # "function signature mismatch"; the offset in the innermost wasm frame is
    # what turns that into a named work item.
    if INDIRECT_ERR_RE.search(err):
        off = next((o for _i, o in sites if o is not None), None)
        print('\n== INDIRECT CALL, not a prototype conflict: the type is at the'
              ' CALL SITE')
        return ('indirect', wasm, off)

    # Two more kinds that are NOT "a real body faulted" (PORT-A8). Both are the
    # B9-4 class -- a recovered function address spelled as an integer literal --
    # and both used to fall through to the generic arm below, which said only
    # that the innermost frame was a real body.
    if TABLE_OOB_RE.search(err):
        off = next((o for _i, o in sites if o is not None), None)
        print('\n== TABLE INDEX, not a type mismatch: the value in the slot is'
              ' not a table index at all')
        return ('table', wasm, off)
    if MEMORY_OOB_RE.search(err):
        off = next((o for _i, o in sites if o is not None), None)
        print('\n== MEMORY ACCESS, not a type mismatch: something was'
              ' dereferenced that is not a pointer here')
        return ('memory', wasm, off)

    print(f'\n== not a prototype conflict: the innermost named frame is '
          f'{wasm[0]}, a real body.')
    print('   A fault inside it (a null vtable slot, an out-of-bounds store, '
          'an LL_UNPORTED_ASM stub compiled to a trap).')
    print('   If it should have been an indirect call, re-run with --indirect:'
          ' binaryen\'s directize can rewrite a constant-index call_indirect'
          ' whose')
    print('   types disagree into a plain trap, which arrives as `unreachable`'
          ' rather than `function signature mismatch`.')
    return 1


def main():
    ap = argparse.ArgumentParser(
        description='name the first trap the headless harness hits')
    ap.add_argument('--build', default=os.path.join(ROOT, 'portable', 'build-wasm'),
                    help='the Emscripten build directory')
    ap.add_argument('--target', default='legoland_headless_debug',
                    help='the harness to run (must be linked -O0, or no frame '
                         'is named)')
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--cd-dir', default=os.path.join(ROOT, 'gamedata', 'disc'),
                    help="the directory presented as the game's CD drive")
    ap.add_argument('--data-dir', default=os.path.join(ROOT, 'gamedata', 'main'),
                    help="the install directory the loaders' relative paths "
                         'resolve against')
    ap.add_argument('--continue', dest='trap_continue', action='store_true',
                    help='LL_TRAP_CONTINUE=1: every generated trap prints once '
                         'and RETURNS, so ONE run enumerates every blocker on '
                         'the path instead of stopping at the first. The run '
                         'past a trap is on made-up data (a returning trap '
                         'hands its caller a zero), so this mode makes a LIST '
                         '-- it never says something works.')
    ap.add_argument('--no-trace', action='store_true',
                    help='do not set LL_HOST_TRACE')
    ap.add_argument('--trace-tail', type=int, default=12,
                    help='host-call lines of context to print (0 for none)')
    ap.add_argument('--timeout', type=float, default=180.0)
    ap.add_argument('--raw', action='store_true',
                    help="also print the harness's whole output")
    # ---- the indirect-call half, which can also run on its own -------------
    ap.add_argument('--indirect', action='store_true',
                    help='explain the trap as a call_indirect type mismatch '
                         'even when the message did not say so (binaryen can '
                         'turn one into a plain `unreachable`)')
    ap.add_argument('--at', default=None,
                    help='a module byte offset (0x...) to explain, instead of '
                         'running anything: the number in a `wasm-function[N]:'
                         '0xOFF` frame')
    ap.add_argument('--wasm', default=None,
                    help='the .wasm to read for --at / --table (default: the '
                         "--target's)")
    ap.add_argument('--table', action='store_true',
                    help='print the indirect table\'s type census and exit: '
                         'every signature a call_indirect can land on, and how '
                         'many slots hold it')
    ap.add_argument('--kind', choices=('indirect', 'table', 'memory'),
                    default='indirect',
                    help='how --at should explain the offset (default '
                         'indirect); `table` for a table-index-out-of-bounds '
                         'and `memory` for an out-of-bounds access')
    ap.add_argument('--va-literals', action='store_true',
                    help='sweep the module for i32 constants in the image\'s '
                         'address space and exit: PORT-B9\'s B9-4 class (a '
                         'function address spelled as an integer literal in the '
                         'recovered C), read off the linked wasm')
    ap.add_argument('args', nargs=argparse.REMAINDER,
                    help='passed to the harness (a leading -- is stripped)')
    a = ap.parse_args()

    wasm = a.wasm or os.path.join(a.build, a.target + '.wasm')
    if a.table:
        return table_census(wasm, a.build)
    if a.va_literals:
        return va_literal_census(wasm)
    if a.at is not None:
        # Explain one call site without running the game: the workflow for a
        # trap someone else reported, or for a page that died in the browser.
        if not os.path.exists(wasm):
            print(f'name_trap: {wasm} does not exist', file=sys.stderr)
            return 2
        {'indirect': explain_indirect,
         'table': explain_table_index,
         'memory': explain_memory_oob}[a.kind](wasm, [], int(a.at, 0), a.build)
        return 1

    if not a.no_build and not build(a.build, a.target, a.raw):
        return 2
    js = os.path.join(a.build, a.target + '.js')
    if not os.path.exists(js):
        print(f'name_trap: {js} does not exist', file=sys.stderr)
        return 2

    env = dict(os.environ)
    # BOTH must be absolute. The harness chdir()s to LL_DATA_DIR before it calls
    # WinMain, so a relative path is resolved against whatever cwd node happens
    # to have and fails with a bare message; LL_CD_DIR is then resolved from the
    # new cwd and silently names nothing, which looks like a missing CD.
    env['LL_CD_DIR'] = os.path.abspath(a.cd_dir)
    env['LL_DATA_DIR'] = os.path.abspath(a.data_dir)
    for var in ('LL_CD_DIR', 'LL_DATA_DIR'):
        if not os.path.isdir(env[var]):
            print(f'name_trap: {var}={env[var]} is not a directory',
                  file=sys.stderr)
            return 2
    if a.trap_continue:
        env['LL_TRAP_CONTINUE'] = '1'
    if not a.no_trace:
        env['LL_HOST_TRACE'] = '1'
    args = a.args[1:] if a.args[:1] == ['--'] else a.args

    try:
        stdout, stderr, status = run(js, args, env, a.timeout)
    except subprocess.TimeoutExpired:
        print(f'name_trap: the harness did not finish in {a.timeout:.0f}s. '
              'A modal loop with no exit (RES_EnsureMounted retries on a '
              "MessageBoxA answer) looks like this; check --cd-dir.", file=sys.stderr)
        return 2
    except FileNotFoundError:
        print('name_trap: node is not on PATH', file=sys.stderr)
        return 2

    if a.raw:
        sys.stderr.write(stderr)
        sys.stdout.write(stdout)
    print(f'== {a.target} {" ".join(args) or "(default switches)"}: '
          f'exit {status}')
    r = report(stdout, stderr, status, a.trace_tail, a.trap_continue)
    if isinstance(r, str):
        explain_mismatch(r, a.build)
        print('\n   The fix is a caller-side prototype under '
              '`#ifdef LEGOLAND_PORTABLE` in the DECLARING file (never between '
              'a `// FUNCTION:` marker and its signature), re-gated with '
              'audit.py and relocs.py. The definition is right: it is the one '
              'the matcher validated against the original bytes.')
        return 1
    if isinstance(r, tuple):
        kind, frames, off = r
        {'indirect': explain_indirect,
         'table': explain_table_index,
         'memory': explain_memory_oob}[kind](wasm, frames, off, a.build)
        return 1
    if a.indirect and r == 1:
        # Asked for explicitly: the trap arrived as something other than
        # "function signature mismatch" but the caller believes it is one.
        off = None
        for ln in stderr.splitlines():
            m = FRAME_RE.match(ln)
            if m and m.group('off'):
                off = int(m.group('off'), 16)
                break
        explain_indirect(wasm, [], off, a.build)
    return r


if __name__ == '__main__':
    sys.exit(main())
