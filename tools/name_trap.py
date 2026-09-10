#!/usr/bin/env python3
"""Name the first trap the headless harness hits, in one command.

    python3 portable/tools/name_trap.py                 # the whole WinMain spine
    python3 portable/tools/name_trap.py -- --stages      # InitSession step by step
    python3 portable/tools/name_trap.py --resmount       # (-- is optional)

The port has three ways of dying and only one of them says so by itself:

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
* **A real fault** (out-of-bounds, a null vtable call): also `RuntimeError`, but
  with a game function at the top of the stack rather than a mismatch stub.

Telling the second from the third is the whole job, and it needs a link that
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

Exit code: 0 if the run finished without a trap, 1 if a trap was named,
2 if the harness could not be built or run.
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import linkreport as lr  # noqa: E402

# `    at legoland_headless_debug.wasm.InitSession (wasm://wasm/...:0x1234)`
# The module prefix is the executable name; the symbol may contain `:` and `$`.
FRAME_RE = re.compile(r'^\s*at\s+(?P<mod>[\w.-]+?)\.wasm\.(?P<sym>\S+)\s*\(')
JS_FRAME_RE = re.compile(r'^\s*at\s+(?P<sym>[\w.<>$]+)\s*\(?(?P<where>\S*)')
TRAP_RE = re.compile(r'^TRAP (?P<dll>\S+) (?P<sym>\S+) from (?P<from>.*)$')
ERR_RE = re.compile(r'^(?:\w+\.)*(?P<kind>RuntimeError|Error|Aborted)\b.*$')
MISMATCH_RE = re.compile(r'^signature_mismatch:(?P<callee>.+)$')


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


def report(stdout, stderr, status, trace_tail):
    lines = stderr.splitlines()

    trap = next((m for m in (TRAP_RE.match(ln) for ln in lines) if m), None)
    host = [ln for ln in lines if ln.startswith('HOST ')]
    stage = [ln for ln in lines if '--- ' in ln]

    if host and trace_tail:
        print(f'\n== last {min(trace_tail, len(host))} host calls')
        for ln in host[-trace_tail:]:
            print('   ' + ln)
    if stage:
        print(f'\n== last stage reached: {stage[-1].split("--- ", 1)[1]}')

    if trap:
        print(f'\n== TRAP: {trap.group("sym")} ({trap.group("dll")})')
        print(f'   referenced by {trap.group("from")}')
        print('   A generated body: the host shim or a game source has to '
              'define it.')
        return 1

    # The stack: the first error line, then its `at ...` frames.
    err, frames = None, []
    for i, ln in enumerate(lines):
        m = ERR_RE.match(ln.strip())
        if m and not frames:
            err = ln.strip()
            for tail in lines[i + 1:]:
                f = FRAME_RE.match(tail)
                if f:
                    frames.append(f.group('sym'))
                elif tail.strip().startswith('at '):
                    frames.append('[js] ' + (JS_FRAME_RE.match(tail).group('sym')
                                             if JS_FRAME_RE.match(tail) else '?'))
                elif frames:
                    break
    if not err:
        print(f'\n== no trap: the harness exited {status}')
        if stdout.strip():
            print('\n'.join('   ' + ln for ln in stdout.splitlines()[-10:]))
        return 0

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
    print(f'\n== not a prototype conflict: the innermost named frame is '
          f'{wasm[0]}, a real body.')
    print('   A fault inside it (a null vtable slot, an out-of-bounds store, '
          'an LL_UNPORTED_ASM stub compiled to a trap).')
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
    ap.add_argument('--no-trace', action='store_true',
                    help='do not set LL_HOST_TRACE')
    ap.add_argument('--trace-tail', type=int, default=12,
                    help='host-call lines of context to print (0 for none)')
    ap.add_argument('--timeout', type=float, default=180.0)
    ap.add_argument('--raw', action='store_true',
                    help="also print the harness's whole output")
    ap.add_argument('args', nargs=argparse.REMAINDER,
                    help='passed to the harness (a leading -- is stripped)')
    a = ap.parse_args()

    if not a.no_build and not build(a.build, a.target, a.raw):
        return 2
    js = os.path.join(a.build, a.target + '.js')
    if not os.path.exists(js):
        print(f'name_trap: {js} does not exist', file=sys.stderr)
        return 2

    env = dict(os.environ)
    env['LL_CD_DIR'] = a.cd_dir
    env['LL_DATA_DIR'] = a.data_dir
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
    r = report(stdout, stderr, status, a.trace_tail)
    if isinstance(r, str):
        explain_mismatch(r, a.build)
        print('\n   The fix is a caller-side prototype under '
              '`#ifdef LEGOLAND_PORTABLE` in the DECLARING file (never between '
              'a `// FUNCTION:` marker and its signature), re-gated with '
              'audit.py and relocs.py. The definition is right: it is the one '
              'the matcher validated against the original bytes.')
        return 1
    return r


if __name__ == '__main__':
    sys.exit(main())
