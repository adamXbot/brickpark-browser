#!/usr/bin/env python3
"""Drop the generated traps for symbols the PORT-B host shim defines.

gen_link.py (PORT-A) scans ONE object directory and emits a trapping stub for
every Win32 import and every unresolved name that directory's objects do not
define. The browser target adds a second object directory -- the host shim
(portable/src/hostwin/{ddraw,user32,gdi32,dinput,winmm,dsound}.c) -- whose
definitions gen_link.py cannot see, so its `host_stubs.c` and `stubs.c` would
collide with the shim at link time.

Rather than hand-write stubs (docs/SCOPE_PORT_WAVE.md rule 2 forbids that) or
teach gen_link.py a second scan directory (it is PORT-A's file), this rewrites
the generated C in place: every `void NAME(void) { ll_unhosted/ll_unwritten... }`
line whose NAME is defined by the shim's objects is commented out, with the
reason, so the generated file still reads as the census it is.

Usage:
  closure_filter.py --gen <gen dir> --objs <dir with the shim's .o files>
"""
import argparse
import os
import re
import subprocess
import sys

STUB_RE = re.compile(r'^void ([A-Za-z_][A-Za-z0-9_]*)\(void\) \{ ll_(unhosted|unwritten)\(')


def nm_defined(objdir):
    """Names defined (not undefined, not common) by the objects under objdir."""
    names = set()
    nm = os.environ.get('NM', 'nm')
    objs = []
    for d, _, files in os.walk(objdir):
        objs += [os.path.join(d, f) for f in files if f.endswith('.o')]
    for obj in objs:
        try:
            out = subprocess.run([nm, '-g', obj], capture_output=True, text=True,
                                 check=False).stdout
        except FileNotFoundError:
            sys.exit(f'{nm}: not found (set NM)')
        for line in out.splitlines():
            parts = line.split()
            if len(parts) >= 2 and parts[-2] in ('T', 't', 'D', 'd', 'B', 'b', 'R', 'r'):
                names.add(parts[-1].lstrip('_'))
            elif len(parts) == 2 and parts[0] in ('T', 'D', 'B', 'R'):
                names.add(parts[1].lstrip('_'))
    return names


def filter_file(path, provided):
    if not os.path.exists(path):
        return 0
    out = []
    dropped = 0
    for line in open(path):
        m = STUB_RE.match(line)
        if m and m.group(1) in provided:
            out.append('/* provided by the PORT-B host shim: '
                       + m.group(1) + ' */\n')
            dropped += 1
        else:
            out.append(line)
    with open(path, 'w') as f:
        f.writelines(out)
    return dropped


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', required=True)
    ap.add_argument('--objs', required=True, action='append')
    args = ap.parse_args()

    provided = set()
    for d in args.objs:
        if os.path.isdir(d):
            provided |= nm_defined(d)
    if not provided:
        print('closure_filter: no shim objects found; nothing to filter')
        return

    total = 0
    for name in ('host_stubs.c', 'stubs.c'):
        total += filter_file(os.path.join(args.gen, name), provided)
    print(f'closure_filter: dropped {total} generated traps the host shim owns')


if __name__ == '__main__':
    main()
