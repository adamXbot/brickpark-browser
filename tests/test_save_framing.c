/* PORT-C test: the save-file chunk framing.
 *
 * Entry points (LEGOLAND/profiles.c, LEGOLAND/saveprof.c -- read-only here):
 *   BeginMeasuredBlock  0x0047d790
 *   EndMeasuredBlock    0x0047d800
 *   SaveGameWrite       0x0047d760
 *   SaveGameRead        0x0047d730
 *   FindeIneList        0x0047d880   [sic: the export's own spelling]
 *
 * Oracle: tools/oracle_savechunks.py (the framing contract in
 * docs/runtime/persistence.md + the header comment of LEGOLAND/profiles.c).
 * The oracle emits a script of framing operations and the exact bytes the
 * engine must produce; this replays the script through the game's own
 * functions and compares size, digest and every back-patched word.
 *
 * WHY THIS ONE RUNS ON THE NATIVE 64-BIT BUILD TOO
 * ------------------------------------------------
 * Nothing in the serialised layout is a pointer or a `long`: the placeholder
 * is a 4-byte absolute file offset, the payload is bytes, and the globals this
 * touches (g_savefile_fd int, g_chunk_offsets int[16], g_chunk_depth int,
 * g_elist_count int) are all 4-byte on both ILP32 and LP64. g_elist is a
 * pointer, but it is only ever set and dereferenced in memory, never
 * serialised. So the ILP32 and LP64 results are identical by construction and
 * this test is meaningful today, before PORT-A's wasm32 closure lands.
 *
 * GLOBALS THIS TEST INITIALISES (the closure rebuilds .data from the exe, so
 * each of these already holds its shipped value; the test overwrites them):
 *   g_savefile_fd   0x006691b0  the open save descriptor -- set to our temp fd
 *   g_chunk_depth   0x006691fc  placeholder stack depth -- zeroed
 *   g_chunk_offsets 0x006691bc  placeholder stack (16 ints) -- written by Begin
 *   g_elist         0x00669200  element table -- pointed at the oracle's
 *   g_elist_count   0x006691b4  its length
 */
#include "ll_tests.h"
#include "oracle_savechunks.h"

#include <stdlib.h>

/* ---- the game's own functions and globals (addresses from the sources) --- */
extern int BeginMeasuredBlock(void);                       /* 0x0047d790 */
extern int EndMeasuredBlock(void);                         /* 0x0047d800 */
extern int SaveGameWrite(const void* buf, unsigned int n); /* 0x0047d760 */
extern int SaveGameRead(void* buf, unsigned int n);        /* 0x0047d730 */
extern int FindeIneList(int* pval);                        /* 0x0047d880 */

extern int  g_savefile_fd;      /* 0x006691b0 */
extern int  g_chunk_offsets[];  /* 0x006691bc */
extern int  g_chunk_depth;      /* 0x006691fc */
extern int* g_elist;            /* 0x00669200 */
extern int  g_elist_count;      /* 0x006691b4 */

/* MSVC CRT low-level I/O, as LEGOLAND/saveprof.c uses it. */
extern int  _open(const char* path, int oflag, ...);
extern int  _close(int fd);
extern long _lseek(int fd, long off, int origin);
extern long _tell(int fd);

#define MS_O_RDWR   0x0002
#define MS_O_CREAT  0x0100
#define MS_O_TRUNC  0x0200
#define MS_O_BINARY 0x8000

static const char* kTmp = "ll_test_save_framing.bin";

void test_save_framing(void)
{
    int            fd;
    int            i;
    int            payload = 0;
    int            depth_seen = 0;
    int            ok = 1;
    unsigned char  buf[512];
    unsigned char* file;
    long           size;
    ll_u64         h;

    fd = _open(kTmp, MS_O_RDWR | MS_O_CREAT | MS_O_TRUNC | MS_O_BINARY, 0666);
    if (fd < 0) {
        ll_checks++;
        ll_fail("open temp file", "_open(\"%s\") failed", kTmp);
        return;
    }
    g_savefile_fd = fd;
    g_chunk_depth = 0;

    /* ---- replay the oracle's script through the engine ------------------ */
    for (i = 0; i < ORACLE_SAVE_SCRIPT_N; i++) {
        int op = oracle_save_script[i].op;
        int n = oracle_save_script[i].n;
        if (op == ORACLE_SAVE_OP_BEGIN) {
            if (!BeginMeasuredBlock())
                ok = 0;
            if (g_chunk_depth > depth_seen)
                depth_seen = g_chunk_depth;
        } else if (op == ORACLE_SAVE_OP_WRITE) {
            int k;
            int left = n;
            while (left > 0) {
                int chunk = left > (int)sizeof buf ? (int)sizeof buf : left;
                for (k = 0; k < chunk; k++)
                    buf[k] = (unsigned char)((payload + k) & 0xff);
                if (!SaveGameWrite(buf, (unsigned int)chunk))
                    ok = 0;
                payload += chunk;
                left -= chunk;
            }
        } else {
            if (!EndMeasuredBlock())
                ok = 0;
        }
    }

    LL_CHECK_TRUE("every Begin/Write/End reported success", ok);
    LL_CHECK_INT("placeholder stack unwound to 0", g_chunk_depth, 0);
    LL_CHECK_INT("deepest nesting", depth_seen, ORACLE_SAVE_MAX_DEPTH);
    LL_CHECK_INT("payload bytes written", payload, ORACLE_SAVE_PAYLOAD);

    size = _tell(fd);
    LL_CHECK_INT("file size after the script", size, ORACLE_SAVE_SIZE);

    /* ---- read the whole file back through SaveGameRead ------------------ */
    file = (unsigned char*)malloc(ORACLE_SAVE_SIZE);
    _lseek(fd, 0, 0);
    LL_CHECK_TRUE("SaveGameRead returns 1 for a full-length read",
                  SaveGameRead(file, ORACLE_SAVE_SIZE) == 1);
    h = ll_fnv_bytes(ll_fnv_init(), file, ORACLE_SAVE_SIZE);
    LL_CHECK_HEX("file digest", h, ORACLE_SAVE_READBACK_FNV);

    /* A read past EOF must report failure (it is a short read). */
    LL_CHECK_TRUE("SaveGameRead returns 0 at EOF", SaveGameRead(buf, 16) == 0);

    /* ---- every back-patched word is the block's ABSOLUTE END offset ------ */
    for (i = 0; i < ORACLE_SAVE_PATCH_N; i++) {
        int at = oracle_save_patches[i].at;
        int got;
        char what[96];
        memcpy(&got, file + at, 4);
        sprintf(what, "block %d placeholder at +%d holds its end offset", i, at);
        LL_CHECK_INT(what, got, oracle_save_patches[i].end);
        /* The trap the format invites: it is NOT a length. */
        if (got == oracle_save_patches[i].end - at - 4 &&
            oracle_save_patches[i].end - at - 4 != oracle_save_patches[i].end)
            ll_note("framing word", "block %d read as a length would give %d",
                    i, got);
    }

    /* ---- FindeIneList --------------------------------------------------- */
    g_elist = (int*)oracle_elist;
    g_elist_count = ORACLE_ELIST_N;
    for (i = 0; i < ORACLE_ELIST_PROBE_N; i++) {
        int v = oracle_elist_probes[i].in;
        int r;
        char what[96];
        r = FindeIneList(&v);
        sprintf(what, "FindeIneList probe %d return", i);
        LL_CHECK_INT(what, r, oracle_elist_probes[i].ret);
        sprintf(what, "FindeIneList probe %d rewrote *pval", i);
        LL_CHECK_INT(what, v, oracle_elist_probes[i].out);
    }

    free(file);
    _close(fd);
    remove(kTmp);
}
