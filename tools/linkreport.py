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

    python3 tools/linkreport.py build [--out report.md]

gen_link.py imports the helpers below.
"""
import argparse
import collections
import glob
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..'))
# The matching decompilation is the decomp submodule (LL_DECOMP overrides).
DECOMP = os.environ.get('LL_DECOMP') or os.path.join(ROOT, 'decomp')
SRC = os.path.join(DECOMP, 'LEGOLAND')

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
iprintf siprintf fiprintf sniprintf viprintf vsiprintf vfiprintf vsniprintf
lrint lrintf lrintl llrint llrintf rint rintf nearbyint nearbyintf round roundf trunc truncf
wcscpy wcslen wcscmp wcsncpy wcscat
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


def nm_candidates():
    """The nm programs to try, best first.

    The wasm objects emcc writes carry symbol flags older readers reject
    ("invalid symbol type: 16" from the nm Xcode ships), so the llvm-nm that
    belongs to the Emscripten install is tried before the system one. $LL_NM
    overrides everything."""
    cands = []
    if os.environ.get('LL_NM'):
        cands.append(os.environ['LL_NM'])
    em_config = shutil.which('em-config')
    if em_config:
        # em-config is Emscripten's own answer for where its llvm lives, and
        # the layouts differ (Homebrew keeps it under libexec, emsdk under
        # upstream/bin).
        try:
            root = subprocess.run([em_config, 'LLVM_ROOT'], capture_output=True,
                                  text=True, timeout=30).stdout.strip()
            if root:
                cands.append(os.path.join(root, 'llvm-nm'))
        except (OSError, subprocess.SubprocessError):
            pass
    cands += ['llvm-nm', 'nm']
    out = []
    for c in cands:
        p = c if os.path.sep in c else shutil.which(c)
        if p and os.path.exists(p) and p not in out:
            out.append(p)
    if not out:
        sys.exit('nm not found')
    return out


def find_nm(sample=None):
    """The first candidate that can actually read `sample`."""
    cands = nm_candidates()
    if sample is None:
        return cands[0]
    for nm in cands:
        if subprocess.run([nm, '-P', sample], capture_output=True).returncode == 0:
            return nm
    sys.exit(f'no usable nm: none of {", ".join(cands)} could read {sample}')


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
    # One nm has to read every object: the system one may manage the game's
    # and then choke on a shim object (see nm_candidates), so a failure
    # restarts the scan with the next candidate rather than losing symbols.
    cands = nm_candidates()
    for attempt, nm in enumerate(cands):
        defined = collections.defaultdict(set)     # name -> {object basename}
        strong = collections.defaultdict(set)      # non-common definitions
        undefined = collections.defaultdict(set)
        try:
            for obj in objs:
                base = os.path.basename(obj).replace('.c.o', '.c').replace('.o', '')
                for name, typ in run_nm(nm, obj):
                    if typ == 'U':
                        undefined[name].add(base)
                    elif typ in GLOBAL_TYPES:
                        defined[name].add(base)
                        if typ != 'C':
                            strong[name].add(base)
        except subprocess.CalledProcessError as e:
            if attempt + 1 == len(cands):
                sys.exit(f'{nm} cannot read {e.cmd[-1]}: {e.stderr.strip()}')
            continue
        return objs, defined, strong, undefined



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


def wasm_sig_conflict_detail(objs):
    """Why each conflicting import conflicts: who emitted which signature.

    `collect_wasm_sigs` votes and returns the winner, which is all the
    generator needs to EMIT; this says what the vote was, so the manifest can
    attribute the conflict instead of listing a bare name (PORT-A8). A symbol
    that is DEFINED by one of the objects has a body whose signature is a fact,
    and then every spelling that disagrees with it is a game-side declaration
    defect in a named file -- the PORT-M1/M2/M7 class, one `#ifdef
    LEGOLAND_PORTABLE` prototype each.

    Returns name -> {'defined': (sig, file) or None,
                     'votes': {sig: [file, ...]}}, conflicts only."""
    votes = collections.defaultdict(lambda: collections.defaultdict(list))
    defined = {}
    for obj in objs:
        try:
            imp, dfn, _udata = wasm_object_sigs(obj)
        except (IndexError, ValueError):
            continue
        if imp is None:
            continue
        base = os.path.basename(obj).replace('.c.o', '.c').replace('.o', '')
        for name, sig in imp.items():
            votes[name][(tuple(sig[0]), tuple(sig[1]))].append(base)
        for name, sig in dfn.items():
            defined.setdefault(name, ((tuple(sig[0]), tuple(sig[1])), base))
    return {n: {'defined': defined.get(n),
                'votes': {s: sorted(w) for s, w in v.items()}}
            for n, v in votes.items() if len(v) > 1}


def extern_decl_sites(names, src=SRC):
    """name -> [(file, line, statement)] for every `extern` DECLARATION of one
    of `names`, anywhere in the sources.

    `scan_sources` keeps one address per name because that is what sizing a
    global needs; a declaration DEFECT needs the opposite -- every file that
    spells the name, with a line number a matching lane can open. No address
    comment is required: a disagreeing prototype often has none."""
    want = set(names)
    out = collections.defaultdict(list)
    if not want:
        return out
    for path in sorted(glob.glob(os.path.join(src, '*.c'))) + \
            sorted(glob.glob(os.path.join(src, '*.h'))):
        base = os.path.basename(path)
        text = open(path, encoding='utf-8', errors='replace').read()
        pos = 0
        for start in _extern_positions(text):
            if start < pos:
                continue
            stmt, _comments, pos = _statement_at(text, start)
            hits = [n for n, _k in extern_decl_names(stmt) if n in want]
            if not hits:
                continue
            line = text.count('\n', 0, start) + 1
            flat = ' '.join(stmt.split())
            for n in hits:
                out[n].append((base, line, flat))
    return out


def sig_text(sig):
    """A wasm signature the way wasm-ld prints it: (i32, i32) -> void."""
    names = {0x7f: 'i32', 0x7e: 'i64', 0x7d: 'f32', 0x7c: 'f64'}
    params, results = sig
    return '(' + ', '.join(names.get(p, hex(p)) for p in params) + ') -> ' + \
           (names.get(results[0], '?') if results else 'void')


def wasm_prototype_conflicts(objs):
    """Symbols the game's own sources disagree about.

    One file declares `extern void SetupControllers(void)` and calls it while
    another defines `int SetupControllers(void)`: harmless in x86 cdecl, where
    the caller simply ignores EAX, but wasm calls are type-checked, so wasm-ld
    routes the mismatched call through a stub that traps (`RuntimeError:
    unreachable`, with no symbol name). Each row here is a call that WILL trap
    at runtime if the game reaches it, and the fix is one `#ifdef
    LEGOLAND_PORTABLE` prototype in the declaring file -- or a matching-lane
    correction, since one of the two prototypes is simply wrong about the
    recovered function.

    Returns [(name, defined_sig, defining_obj, {referenced_sig: [objs]})]."""
    defs = {}
    refs = collections.defaultdict(lambda: collections.defaultdict(list))
    for obj in objs:
        base = os.path.basename(obj).replace('.c.o', '.c').replace('.o', '')
        try:
            imp, dfn, _ = wasm_object_sigs(obj)
        except (IndexError, ValueError):
            continue
        if imp is None:
            return []                       # not a wasm build: nothing to check
        for name, sig in dfn.items():
            defs.setdefault(name, ((tuple(sig[0]), tuple(sig[1])), base))
        for name, sig in imp.items():
            refs[name][(tuple(sig[0]), tuple(sig[1]))].append(base)
    out = []
    for name, (dsig, dobj) in defs.items():
        bad = {s: sorted(o) for s, o in refs[name].items() if s != dsig}
        if bad:
            out.append((name, dsig, dobj, bad))
    return sorted(out)


def load_win32():
    win32 = {}
    for line in open(os.path.join(HERE, 'win32_imports.txt')):
        if line.startswith('#') or not line.strip():
            continue
        name, dll = line.rstrip('\n').split('\t')
        win32[name] = dll
    return win32


# ---- the extern scan: a STATEMENT, not a line ------------------------------
# One regular expression per physical line missed 29 of the 81 names PORT-M4
# had to classify by hand (docs/lanes/scope-port-m4.md section 2), in three
# ways, all of them silent:
#
#   1. the `/* 0x... */` sat on a CONTINUATION line of a multi-line `extern`
#      (20 names). A declaration may be formatted normally now: the scanner
#      reads the whole statement, up to the `;` that ends it, and takes the
#      address from any comment inside it.
#   2. the type had a brace-enclosed body -- `extern struct FortArea { int x0,
#      y0, x1, y1; } g_fort_area;` -- whose own semicolons the line regex could
#      not cross. The body is removed before the declarators are read.
#   3. the type was a FUNCTION POINTER: in `extern int (*g_present)(void);` the
#      lazy match stopped at `int`, which is followed by `(`, so the TYPE was
#      captured as the symbol name and the real global went unclassified. That
#      is the dangerous one -- gen_link.py gives an unclassified data name a
#      256-byte ZEROED placeholder, so if another file names the same address
#      under a spelling the scanner can read, the program ends up with two
#      objects for one global (`g_active_input_cb` / `g_icon_handler2` at
#      0x006687c0 were exactly that pair).
#
# The declarator rules below are the C ones, restricted to what a declaration
# can look like: the name is the last identifier outside any parameter list,
# except in the `(*name)` form, where it is the identifier the star introduces.
_EXTERN_START = re.compile(r'\bextern\b')


def _extern_positions(text):
    """Where the `extern` KEYWORD occurs in code -- not in a comment, not in a
    string. The word is common in the recovered files' prose ("the extern's
    types are the caller's codegen lever"), and a match inside a comment starts
    a `statement` that then swallows the real declaration below it."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i + 1] == '*':
            j = text.find('*/', i + 2)
            i = n if j < 0 else j + 2
            continue
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            j = text.find('\n', i)
            i = n if j < 0 else j + 1
            continue
        if c in '"\'':
            q, j = c, i + 1
            while j < n and text[j] != q:
                j += 2 if text[j] == '\\' else 1
            i = j + 1
            continue
        if c.isalpha() or c == '_':
            j = i
            while j < n and (text[j].isalnum() or text[j] == '_'):
                j += 1
            if text[i:j] == 'extern':
                out.append(i)
            i = j
            continue
        i += 1
    return out
_COMMENT = re.compile(r'/\*.*?\*/|//[^\n]*', re.S)
_ADDR_IN_COMMENT = re.compile(r'/\*[^*]*?(0x[0-9a-fA-F]{6,8})|//[^\n]*?(0x[0-9a-fA-F]{6,8})')
_TOKENS = re.compile(r'[A-Za-z_]\w*|0[xX][0-9a-fA-F]+|\d+|\S')


def _statement_at(text, i):
    """The `extern ...;` statement starting at `i`: (code, comments, end).

    Brace-aware, so a `{ ... }` type body's own semicolons do not end it."""
    depth = 0
    j = i
    code, comments = [], []
    n = len(text)
    while j < n:
        if text.startswith('/*', j):
            k = text.find('*/', j + 2)
            k = n if k < 0 else k + 2
            comments.append(text[j:k])
            j = k
            continue
        if text.startswith('//', j):
            k = text.find('\n', j)
            k = n if k < 0 else k
            comments.append(text[j:k])
            j = k
            continue
        c = text[j]
        if c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
        elif c == ';' and depth <= 0:
            code.append(c)
            j += 1
            # The address comment is usually TRAILING -- `extern int g_x;
            # /* 0x004b1234 */` -- so the statement's comments include what
            # follows the semicolon up to the end of that line (and a block
            # comment opened there, however many lines it runs).
            while j < n and text[j] in ' \t':
                j += 1
            if text.startswith('/*', j):
                k = text.find('*/', j + 2)
                k = n if k < 0 else k + 2
                comments.append(text[j:k])
                j = k
            elif text.startswith('//', j):
                k = text.find('\n', j)
                k = n if k < 0 else k
                comments.append(text[j:k])
                j = k
            return ''.join(code), comments, j
        code.append(c)
        j += 1
    return ''.join(code), comments, n


def _strip_braces(stmt):
    out, depth = [], 0
    for c in stmt:
        if c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
        elif depth == 0:
            out.append(c)
    return ''.join(out)


def extern_decl_names(stmt):
    """[(name, 'fn'|'data')] for one `extern ...;` statement.

    The declared name is the last identifier at paren depth 0 -- everything
    inside a parameter list belongs to the parameters -- unless the declarator
    is the `(*name)` form, where the name is the identifier after the star.
    `fn` when that name is immediately followed by a parameter list."""
    toks = _TOKENS.findall(_strip_braces(stmt))
    out = []
    piece, depth = [], 0
    for t in toks + [',']:
        if t == '(':
            depth += 1
        elif t == ')':
            depth -= 1
        if t in (',', ';') and depth == 0:
            hit = _declarator_name(piece)
            if hit:
                out.append(hit)
            piece = []
            continue
        piece.append(t)
    return out


def _declarator_name(toks):
    depth = 0
    last, kind = None, 'data'
    for i, t in enumerate(toks):
        if t == '[':
            # An array BOUND is not a declarator: `extern char
            # g_key_prev[KEYMAP_COUNT];` declares g_key_prev, and the macro is
            # not a second name for the same address.
            depth += 1
            continue
        if t == ']':
            depth -= 1
            continue
        if t == '(':
            # `(*name)` / `(*name[4])`: a pointer declarator, not a call. Only
            # at depth 0 -- the same shape one level in is a function-POINTER
            # PARAMETER (`extern void SortSpriteWithCallback(int (*cb)(void))`)
            # and the declared name is still the function's.
            if depth == 0 and i + 1 < len(toks) and toks[i + 1] == '*':
                k = i + 1
                while k < len(toks) and toks[k] == '*':
                    k += 1
                if k < len(toks) and toks[k][:1].isalpha() or \
                        (k < len(toks) and toks[k][:1] == '_'):
                    return (toks[k], 'data')
            depth += 1
            continue
        if t == ')':
            depth -= 1
            continue
        if depth == 0 and (t[:1].isalpha() or t[:1] == '_') and \
                t not in ('extern', 'const', 'volatile', '__declspec',
                          'struct', 'union', 'enum', 'unsigned', 'signed',
                          '__cdecl', '__stdcall', 'CALLBACK', 'WINAPI',
                          'APIENTRY', 'dllimport'):
            last = t
            kind = 'fn' if i + 1 < len(toks) and toks[i + 1] == '(' else 'data'
    if last is None:
        return None
    return (last, kind)


def scan_sources(src=SRC):
    """Returns (externs, defined_at, stubs):
    externs   name -> (address, 'fn' | 'data') from `extern ... /* 0x... */`,
              read a STATEMENT at a time, from LEGOLAND/*.c AND *.h
    defined_at address -> (name, file) from the // FUNCTION markers
    stubs     [(file, line, function)] LL_UNPORTED_ASM sites"""
    externs = {}
    defined_at = {}
    marker_re = re.compile(r'^// (FUNCTION|WIP-FUNCTION): LEGOLAND (0x[0-9a-fA-F]+)')
    sig_re = re.compile(r'^[A-Za-z_][A-Za-z0-9_ *]*?\**\s*\**([A-Za-z_][A-Za-z0-9_]*)\s*\(')
    stubs = []
    paths = sorted(glob.glob(os.path.join(src, '*.c'))) + \
        sorted(glob.glob(os.path.join(src, '*.h')))
    for path in paths:
        base = os.path.basename(path)
        text = open(path, encoding='utf-8', errors='replace').read()
        lines = text.split('\n')
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
            if 'LL_UNPORTED_ASM()' in line:
                stubs.append((base, i + 1, cur_func))
        # the externs, statement by statement
        pos = 0
        for start in _extern_positions(text):
            if start < pos:
                continue                  # inside the statement just read
            stmt, comments, pos = _statement_at(text, start)
            addr = None
            for c in comments:
                a = re.search(r'\b0x[0-9a-fA-F]{6,8}\b', c)
                if a:
                    addr = int(a.group(), 16)
                    break
            if addr is None:
                continue
            for name, kind in extern_decl_names(stmt):
                externs.setdefault(name, (addr, kind))
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
    protos = wasm_prototype_conflicts(objs)
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
    p(f"| prototype conflicts | {len(protos)} | the sources disagree about a signature; on wasm each such call traps (wasm32 builds only) |")
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
    p(f'## Prototype conflicts ({len(protos)})')
    p('')
    p('One file\'s declaration of a function lowers to a different wasm')
    p('signature than the body another file defines. Harmless in x86 cdecl;')
    p('on wasm the call goes through a stub that traps with no symbol name.')
    p('One of the two prototypes is wrong about the recovered function.')
    p('')
    if protos:
        p('| symbol | defined as | declared as | by |')
        p('| --- | --- | --- | --- |')
        for name, dsig, dobj, bad in protos:
            for sig, users in sorted(bad.items()):
                shown = ', '.join(users[:6]) + (f' +{len(users) - 6}' if len(users) > 6 else '')
                p(f'| `{name}` | `{sig_text(dsig)}` in {dobj} | `{sig_text(sig)}` | {shown} |')
    p('')
    text = '\n'.join(lines)
    print(text)
    if args.out:
        with open(args.out, 'w') as f:
            f.write(text)


if __name__ == '__main__':
    main()
