#!/usr/bin/env python3
"""Symbol census of the portable build: what is still missing before the game
links.

Reads every object the portable CMake build produced, collects defined and
undefined symbols with `nm`, and sorts the undefined ones into:

  game-fn     a game function the decompilation has not written yet (its
              address is known from an `extern ... /* 0x... */` comment)
  game-data   a global the sources only ever declare `extern`; its storage
              lives in the original binary's .data/.rdata (gen_link.py
              recreates it from the exe)
  alias       an extern declared under one name whose address is DEFINED under
              another name (the stale extern names HANDOFF.md describes):
              a rename, not new work
  host        Win32 / DirectX / Windows-CRT API from the import table of
              legoland.exe: the host shim's job
  crt         C library
  unknown     none of the above: externs without an address comment

It also lists symbols defined more than once (duplicate bodies) and the
inline-asm bodies that are still LL_UNPORTED_ASM stubs.

    python3 portable/tools/linkreport.py portable/build [--out report.md]

gen_link.py imports the helpers below.
"""
import argparse
import collections
import glob
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
SRC = os.path.join(ROOT, 'LEGOLAND')

CRT = set('''
abs atan atan2 atof atoi atol ceil cos exp fabs floor fmod log log10 pow sin
sqrt tan acos asin sinh cosh tanh ldexp frexp modf rand srand qsort bsearch
malloc calloc realloc free exit abort atexit getenv system memset memcpy
memmove memcmp memchr strlen strcpy strncpy strcat strncat strcmp strncmp
strchr strrchr strstr strtok strspn strcspn strpbrk strdup strtol strtoul
strtod sprintf printf fprintf vsprintf vfprintf sscanf fscanf fopen fclose
fread fwrite fseek ftell rewind fflush fgets fputs fgetc fputc feof ferror
remove rename tmpnam clearerr setvbuf toupper tolower isalpha isdigit isalnum
isspace isupper islower isxdigit ispunct iscntrl isprint isgraph time clock
localtime gmtime mktime difftime asctime ctime strftime longjmp setjmp
va_start va_end errno bzero bcopy clock_gettime
_ftol _CIsqrt _CIsin _CIcos _CIatan2 _CIpow _CIfmod _CIexp _CIlog _allmul
_alldiv _allrem _allshl _allshr _aulldiv _aullrem _chkstk _fltused _purecall
_strdup _strupr _strlwr _stricmp _strnicmp stricmp strnicmp strupr strlwr
_itoa itoa _ltoa ltoa _ultoa _splitpath _makepath _fullpath _access _unlink
_open _close _read _write _lseek _tell _eof _filelength _mkdir _rmdir _chdir
_getcwd _findfirst _findnext _findclose _stat _fstat _sopen _creat _commit
_getdrive _chdrive _heapmin _msize _expand _rotl _rotr _lrotl _lrotr _finite
_isnan _fpclass _control87 _clearfp _statusfp _set_new_handler _getch _kbhit
_putch _cgets _cputs _cprintf __errno errno _errno _except_handler3
_local_unwind2 _global_unwind2 __except_list _CxxThrowException
'''.split())

# PE layout of legoland.exe (ImageBase 0x400000); used to size and classify
# data symbols without needing pefile.
SECTIONS = (('.text', 0x401000, 0x4ab000), ('.rdata', 0x4ab000, 0x4b4000),
            ('.data', 0x4b4000, 0x834000), ('.rsrc', 0x834000, 0x836000))

GLOBAL_TYPES = 'TDBCRS'      # nm -P single-letter types we treat as definitions


def is_crt(name):
    return name in CRT or name.startswith('__') or name.startswith('_Default')


def find_nm():
    for cand in ('llvm-nm', 'nm'):
        if subprocess.call(['which', cand], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) == 0:
            return cand
    sys.exit('nm not found')


def run_nm(nm, path):
    out = subprocess.run([nm, '-P', path], capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 2:
            continue
        name, typ = parts[0], parts[1]
        if sys.platform == 'darwin' and name.startswith('_'):
            name = name[1:]
        yield name, typ


def collect_objects(build_dir, only_game=True):
    """Symbol tables of the objects under build_dir. With only_game, just the
    LEGOLAND/*.c objects: the shim's own libc references are not the game's."""
    objs = []
    for d, _, files in os.walk(build_dir):   # os.walk: glob skips the .claude dot-directory
        objs += [os.path.join(d, f) for f in files if f.endswith('.o')]
    if only_game:
        objs = [o for o in objs if '/LEGOLAND/' in o]
    objs.sort()
    if not objs:
        sys.exit(f'no object files under {build_dir}; build first')
    nm = find_nm()
    defined = collections.defaultdict(set)     # name -> {object basename}
    strong = collections.defaultdict(set)      # non-common definitions
    undefined = collections.defaultdict(set)
    for obj in objs:
        base = os.path.basename(obj).replace('.c.o', '.c').replace('.o', '')
        for name, typ in run_nm(nm, obj):
            if typ == 'U':
                undefined[name].add(base)
            elif typ in GLOBAL_TYPES:
                defined[name].add(base)
                if typ != 'C':
                    strong[name].add(base)
    return objs, defined, strong, undefined


def load_win32():
    win32 = {}
    for line in open(os.path.join(HERE, 'win32_imports.txt')):
        if line.startswith('#') or not line.strip():
            continue
        name, dll = line.rstrip('\n').split('\t')
        win32[name] = dll
    return win32


def scan_sources():
    """Returns (externs, defined_at, stubs):
    externs   name -> (address, 'fn' | 'data') from `extern ... /* 0x... */`
    defined_at address -> (name, file) from the // FUNCTION markers
    stubs     [(file, line, function)] LL_UNPORTED_ASM sites"""
    externs = {}
    defined_at = {}
    marker_re = re.compile(r'^// (FUNCTION|WIP-FUNCTION): LEGOLAND (0x[0-9a-fA-F]+)')
    extern_re = re.compile(r'^\s*extern\b[^;]*?\b([A-Za-z_][A-Za-z0-9_]*)\s*(\(|\[|;|=).*?/\*\s*(0x[0-9a-fA-F]+)')
    sig_re = re.compile(r'^[A-Za-z_][A-Za-z0-9_ *]*?\**\s*\**([A-Za-z_][A-Za-z0-9_]*)\s*\(')
    stubs = []
    for path in sorted(glob.glob(os.path.join(SRC, '*.c'))):
        base = os.path.basename(path)
        lines = open(path, encoding='utf-8', errors='replace').read().split('\n')
        pending = None
        cur_func = None
        for i, line in enumerate(lines):
            m = marker_re.match(line)
            if m:
                pending = int(m.group(2), 16)
                continue
            if line.lstrip().startswith(('/*', '*', '#', '//')) or not line.strip():
                continue
            s = sig_re.match(line)
            if s and not line.startswith(('extern', 'typedef', 'static inline')):
                cur_func = s.group(1)
                if pending is not None:
                    defined_at.setdefault(pending, (cur_func, base))
                    pending = None
            m = extern_re.match(line)
            if m:
                externs.setdefault(m.group(1), (int(m.group(3), 16), 'fn' if m.group(2) == '(' else 'data'))
            if 'LL_UNPORTED_ASM()' in line:
                stubs.append((base, i + 1, cur_func))
    return externs, defined_at, stubs


def classify(defined, undefined, externs, defined_at, win32):
    """undefined-and-not-defined names -> {category: [(name, detail, users)]}"""
    cats = collections.defaultdict(list)
    for n in sorted(n for n in undefined if n not in defined):
        users = sorted(undefined[n])
        if n in win32:
            cats['host'].append((n, win32[n], users))
        elif is_crt(n):
            cats['crt'].append((n, '', users))
        elif n in externs:
            addr, kind = externs[n]
            if kind == 'fn':
                if addr in defined_at and defined_at[addr][0] != n and defined_at[addr][0] in defined:
                    real, where = defined_at[addr]
                    cats['alias'].append((n, f'0x{addr:08x} is {real} ({where})', users))
                else:
                    cats['game-fn'].append((n, f'0x{addr:08x}', users))
            else:
                cats['game-data'].append((n, f'0x{addr:08x}', users))
        else:
            cats['unknown'].append((n, '', users))
    return cats


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('build_dir')
    ap.add_argument('--out', help='write a markdown report here as well')
    args = ap.parse_args()

    objs, _, strong, undefined = collect_objects(args.build_dir)            # the game's references
    _, defined, _, _ = collect_objects(args.build_dir, only_game=False)     # the shim counts as provided
    win32 = load_win32()
    externs, defined_at, stubs = scan_sources()
    cats = classify(defined, undefined, externs, defined_at, win32)
    dups = sorted((n, sorted(o)) for n, o in strong.items() if len(o) > 1)
    missing = sum(len(v) for v in cats.values())

    lines = []
    p = lines.append
    p('# Portable link report')
    p('')
    p(f'Objects: {len(objs)}. Defined symbols: {len(defined)}. Undefined: {missing}.')
    p('')
    p('| category | count | meaning |')
    p('| --- | --- | --- |')
    p(f"| game-fn | {len(cats['game-fn'])} | referenced game functions not yet written (matching lanes) |")
    p(f"| game-data | {len(cats['game-data'])} | globals only ever declared extern (gen_link.py recreates them from the exe) |")
    p(f"| alias | {len(cats['alias'])} | stale extern names whose address IS defined (rename) |")
    p(f"| host | {len(cats['host'])} | Win32/DirectX/Windows-CRT API from the import table (host shim) |")
    p(f"| crt | {len(cats['crt'])} | C library (libc/libm at link time) |")
    p(f"| unknown | {len(cats['unknown'])} | externs with no address comment: look at these first |")
    p(f"| duplicates | {len(dups)} | symbols with a strong definition in more than one file |")
    p(f"| asm stubs | {len(stubs)} | inline-asm bodies still LL_UNPORTED_ASM |")
    p('')
    for cat, title in (('unknown', 'Unclassified'), ('game-fn', 'Unwritten game functions'),
                       ('alias', 'Stale extern names'), ('host', 'Host API'),
                       ('game-data', 'Extern-only globals'), ('crt', 'C library')):
        p(f'## {title} ({len(cats[cat])})')
        p('')
        if cats[cat]:
            p('| symbol | detail | referenced from |')
            p('| --- | --- | --- |')
            for n, detail, users in cats[cat]:
                shown = ', '.join(users[:6]) + (f' +{len(users) - 6}' if len(users) > 6 else '')
                p(f'| `{n}` | {detail} | {shown} |')
        p('')
    p(f'## Duplicate definitions ({len(dups)})')
    p('')
    for n, o in dups:
        p(f'- `{n}`: {", ".join(o)}')
    p('')
    p(f'## Unported inline-asm bodies ({len(stubs)})')
    p('')
    for base, ln, fn in stubs:
        p(f'- {base}:{ln} `{fn}`')
    p('')
    text = '\n'.join(lines)
    print(text)
    if args.out:
        with open(args.out, 'w') as f:
            f.write(text)


if __name__ == '__main__':
    main()
