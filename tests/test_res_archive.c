/* PORT-C test: the RES archive layer, mounted over the real gamedata/disc.
 *
 * Entry points (read-only game code):
 *   RES_OpenVolume         0x00489750  LEGOLAND/data2.c
 *   RES_LoadDirectory      0x004895a0  LEGOLAND/resaudio2.c  (via the above)
 *   AddMasterDir           0x00489440  LEGOLAND/pathobj2.c   (via the above)
 *   RES_FindVolumeDir      0x00489550  LEGOLAND/pathobj2.c
 *   RES_OpenFileFromVolume 0x00489a00  LEGOLAND/data2.c
 *   RES_ReadFile           0x00489cf0  LEGOLAND/res.c
 *   RES_SetFilePointer     0x00489d70  LEGOLAND/memdb.c
 *   RES_GetFilePointer     0x00489db0  LEGOLAND/sweep4.c
 *   RES_GetFileSize        0x00489ce0  LEGOLAND/sweep4.c
 *   RES_CloseFile          0x00489de0  LEGOLAND/sweep4.c
 *
 * Oracle: tools/oracle_res.py over tools/resfile.py and tools/leveldata.py.
 *
 * WASM32 ONLY -- AND THE REASON IS A GOOD ONE
 * -------------------------------------------
 * RES_LoadDirectory rewrites the image's offset links into pointers IN PLACE:
 *
 *     node->sub += (int)base;   RES_LoadDirectory((RImgNode*)node->sub, ...)
 *
 * `base` is a malloc'd pointer and `node->sub` is an `int`. On ILP32 (wasm32,
 * i386) that is exactly right and is how the format works. On a 64-bit host
 * the pointer is truncated to 32 bits and then sign-extended back, so the walk
 * dereferences garbage. This is the cleanest example in the tree of why the
 * 64-bit build is a symbol census and not a runnable game, and it is why this
 * test is registered only for the wasm32 build.
 *
 * RUN DIRECTORY: gamedata/disc. RES_OpenVolume tries ".\volumes\<name>.res"
 * first and then sprintf("%s%s.res", g_res_path, name), so with g_res_path
 * "./" the second attempt is "./<name>.res" in the working directory.
 *
 * GLOBALS THIS TEST INITIALISES:
 *   g_res_path    0x00813b04  the alternate volume prefix -- set to ""
 *   g_master_vols 0x00798628  mounted-volume list head -- left as the closure
 *                             has it (the exe's .data; zero before InitSession)
 *   g_master_dirs             master-directory bucket list, likewise
 * Everything else the mount touches is allocated by the loader itself.
 */
#include "ll_tests.h"
#include "oracle_res.h"

#include <stdlib.h>

/* ---- the RES records, field for field as LEGOLAND/resaudio2.c declares them
 * (data2.c and res.c carry the same layouts). Pointers, not pads: the test
 * must see the same offsets the game's own compiler computed. */
typedef struct TRDir TRDir;
typedef struct TRVol TRVol;

typedef struct TRDirEnt {
    struct TRDirEnt* anext;   /* +0x00 every member, all volumes */
    struct TRDirEnt* next;    /* +0x04 next in the directory bucket */
    struct TRDirEnt* vnext;   /* +0x08 next in this volume */
    TRDir*           dir;     /* +0x0c the bucket it is filed under */
    TRVol*           vol;     /* +0x10 */
    int              size;    /* +0x14 */
    int              base;    /* +0x18 offset of the member in the volume */
    char*            name;    /* +0x1c */
} TRDirEnt;

struct TRDir {
    TRDir*    next;           /* +0x00 */
    TRDirEnt* files;          /* +0x04 */
    char*     name;           /* +0x08 */
};

struct TRVol {
    TRVol*    next;           /* +0x00 */
    TRDirEnt* files;          /* +0x04 first member of this volume */
    char      name[0x14];     /* +0x08 upper-cased volume name */
    int       handle;         /* +0x1c */
    void*     cur;            /* +0x20 */
    int       refcount;       /* +0x24 */
};

typedef struct TRFile {
    int    size;              /* +0x00 */
    int    base;              /* +0x04 */
    TRVol* vol;               /* +0x08 */
    int    pos;               /* +0x0c */
} TRFile;

extern TRVol*  RES_OpenVolume(const char* path);                  /* 0x00489750 */
extern TRFile* RES_OpenFileFromVolume(const char* path,
                                      const char* volname);       /* 0x00489a00 */
extern TRDir*  RES_FindVolumeDir(const char* vol, const char* dir,
                                 TRDirEnt** member);              /* 0x00489550 */
extern int     RES_ReadFile(TRFile* f, void* buf, int count);     /* 0x00489cf0 */
extern int     RES_SetFilePointer(TRFile* f, int pos);            /* 0x00489d70 */
extern int     RES_GetFilePointer(TRFile* f);                     /* 0x00489db0 */
extern int     RES_GetFileSize(TRFile* f);                        /* 0x00489ce0 */
extern int     RES_CloseFile(TRFile* f);                          /* 0x00489de0 */

extern char   g_res_path[];      /* 0x00813b04 */
extern TRVol* g_master_vols;     /* 0x00798628 */

/* ---- the canonical rendering tools/oracle_res.py hashes ------------------ */
static ll_u64 member_line(ll_u64 h, const TRDirEnt* e)
{
    const char* p;
    char upper[256];
    int i = 0;
    for (p = e->name; *p && i < (int)sizeof upper - 1; p++, i++)
        upper[i] = (*p >= 'a' && *p <= 'z') ? (char)(*p - 32) : *p;
    upper[i] = 0;
    h = ll_fnv_str(h, upper);
    h = ll_fnv_str(h, ";");
    h = ll_fnv_int(h, e->size);
    h = ll_fnv_int(h, e->base);
    return h;
}

static int cmp_ent(const void* pa, const void* pb)
{
    const TRDirEnt* a = *(const TRDirEnt* const*)pa;
    const TRDirEnt* b = *(const TRDirEnt* const*)pb;
    const char* x = a->name;
    const char* y = b->name;
    for (; *x && *y; x++, y++) {
        int cx = (*x >= 'a' && *x <= 'z') ? *x - 32 : *x;
        int cy = (*y >= 'a' && *y <= 'z') ? *y - 32 : *y;
        if (cx != cy)
            return cx - cy;
    }
    if (*x != *y)
        return *x ? 1 : -1;
    if (a->base != b->base)
        return a->base < b->base ? -1 : 1;
    return a->size < b->size ? -1 : (a->size > b->size);
}

#define MAX_MEMBERS 4096

void test_res_archive(void)
{
    TRVol*     v;
    TRDirEnt*  e;
    TRDirEnt** all;
    int        n = 0;
    int        i;
    ll_u64     h;
    char       path[512];
    char       volupper[0x14 + 1];

    /* Both uses of g_res_path at once: the "%s%s.res" volume prefix and the
     * drive root RES_FindVolumeOnResPath probes. */
    g_res_path[0] = '.';
    g_res_path[1] = '/';
    g_res_path[2] = 0;

    v = RES_OpenVolume(ORACLE_RES_VOLUME);
    LL_CHECK_TRUE("RES_OpenVolume mounted " ORACLE_RES_VOLUME, v != 0);
    if (!v)
        return;
    LL_CHECK_INT("mounted volume refcount", v->refcount, 1);
    LL_CHECK_TRUE("the volume is the master list head", g_master_vols == v);

    /* The volume name is the file name, upper-cased into a 0x14-byte field. */
    for (i = 0; i < 0x14; i++) {
        char c = ORACLE_RES_VOLUME[i];
        if (!c)
            break;
        volupper[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    volupper[i] = 0;
    LL_CHECK_STR("upper-cased volume name", v->name, volupper);

    /* ---- the member set RES_LoadDirectory built --------------------------- */
    all = (TRDirEnt**)malloc(MAX_MEMBERS * sizeof *all);
    for (e = v->files; e && n < MAX_MEMBERS; e = e->vnext)
        all[n++] = e;
    LL_CHECK_INT("members filed from the directory image", n,
                 ORACLE_RES_MEMBERS);

    qsort(all, (unsigned long)n, sizeof *all, cmp_ent);
    h = ll_fnv_init();
    for (i = 0; i < n; i++)
        h = member_line(h, all[i]);
    LL_CHECK_HEX("member-set digest {NAME;size;base}", h, ORACLE_RES_DIGEST);

    /* Where the C and the Python byte scans disagree, say so out loud rather
     * than quietly matching the looser number (see the lane notes). */
    if (ORACLE_RES_MEMBERS != ORACLE_RES_FLAT_MEMBERS)
        ll_note("decoder divergence",
                "the engine files %d members, tools/resfile.py's whole-file "
                "scan finds %d, tools/leveldata.py's directory scan %d",
                ORACLE_RES_MEMBERS, ORACLE_RES_FLAT_MEMBERS,
                ORACLE_RES_DIRSCAN_MEMBERS);

    /* Every member must carry a directory bucket and point back at the
     * volume: that is the invariant RES_OpenFileFromVolume's `e->dir == d`
     * test relies on. */
    {
        int bad_dir = 0, bad_vol = 0;
        for (i = 0; i < n; i++) {
            if (!all[i]->dir || !all[i]->dir->name)
                bad_dir++;
            if (all[i]->vol != v)
                bad_vol++;
        }
        LL_CHECK_INT("every member has a named directory bucket", bad_dir, 0);
        LL_CHECK_INT("every member points back at its volume", bad_vol, 0);
    }

    /* ---- open and read the probe members -------------------------------- */
    for (i = 0; i < ORACLE_RES_PROBE_N; i++) {
        const char* want = oracle_res_probes[i].name;
        TRDirEnt*   found = 0;
        TRFile*     f;
        char        what[192];
        int         j;
        int         got;
        unsigned char* buf;

        for (j = 0; j < n; j++)
            if (all[j]->base == oracle_res_probes[i].base &&
                all[j]->size == oracle_res_probes[i].size)
                { found = all[j]; break; }

        sprintf(what, "probe %d (%s) is in the directory", i, want);
        LL_CHECK_TRUE(what, found != 0);
        if (!found)
            continue;

        /* Build the path the engine itself stored: "<bucket><member>". */
        sprintf(path, "%s%s", found->dir->name, found->name);
        f = RES_OpenFileFromVolume(path, v->name);
        sprintf(what, "probe %d RES_OpenFileFromVolume(\"%s\")", i, path);
        LL_CHECK_TRUE(what, f != 0);
        if (!f)
            continue;

        sprintf(what, "probe %d RES_GetFileSize", i);
        LL_CHECK_INT(what, RES_GetFileSize(f), oracle_res_probes[i].size);
        sprintf(what, "probe %d base offset in the volume", i);
        LL_CHECK_INT(what, f->base, oracle_res_probes[i].base);
        sprintf(what, "probe %d initial RES_GetFilePointer", i);
        LL_CHECK_INT(what, RES_GetFilePointer(f), 0);

        /* head read */
        buf = (unsigned char*)malloc((unsigned long)
                                     oracle_res_probes[i].head_n + 1);
        got = RES_ReadFile(f, buf, oracle_res_probes[i].head_n);
        sprintf(what, "probe %d RES_ReadFile returned the byte count", i);
        LL_CHECK_INT(what, got, oracle_res_probes[i].head_n);
        sprintf(what, "probe %d head digest (%d bytes)", i,
                oracle_res_probes[i].head_n);
        LL_CHECK_HEX(what,
                     ll_fnv_bytes(ll_fnv_init(), buf,
                                  (unsigned int)oracle_res_probes[i].head_n),
                     oracle_res_probes[i].head_fnv);
        sprintf(what, "probe %d cursor advanced by the read", i);
        LL_CHECK_INT(what, RES_GetFilePointer(f), oracle_res_probes[i].head_n);
        free(buf);

        /* seek + short read */
        sprintf(what, "probe %d RES_SetFilePointer(%d)", i,
                oracle_res_probes[i].seek);
        LL_CHECK_INT(what, RES_SetFilePointer(f, oracle_res_probes[i].seek),
                     oracle_res_probes[i].seek);
        buf = (unsigned char*)malloc(128);
        got = RES_ReadFile(f, buf, oracle_res_probes[i].seek_n);
        sprintf(what, "probe %d read after the seek", i);
        LL_CHECK_INT(what, got, oracle_res_probes[i].seek_n);
        sprintf(what, "probe %d digest at +%d", i, oracle_res_probes[i].seek);
        LL_CHECK_HEX(what,
                     ll_fnv_bytes(ll_fnv_init(), buf,
                                  (unsigned int)oracle_res_probes[i].seek_n),
                     oracle_res_probes[i].seek_fnv);
        free(buf);

        /* the size clamp: a read past the member's end is truncated, never
         * spills into the next member (RES_ReadFile's `count > size - pos`). */
        RES_SetFilePointer(f, oracle_res_probes[i].size - 1);
        buf = (unsigned char*)malloc(64);
        got = RES_ReadFile(f, buf, 64);
        sprintf(what, "probe %d read clamped at the member end", i);
        LL_CHECK_INT(what, got, 1);
        got = RES_ReadFile(f, buf, 64);
        sprintf(what, "probe %d read at the member end returns 0", i);
        LL_CHECK_INT(what, got, 0);
        free(buf);

        /* RES_SetFilePointer rejects out-of-range positions with -1. */
        sprintf(what, "probe %d RES_SetFilePointer rejects size", i);
        LL_CHECK_INT(what, RES_SetFilePointer(f, oracle_res_probes[i].size), -1);
        sprintf(what, "probe %d RES_SetFilePointer rejects -1", i);
        LL_CHECK_INT(what, RES_SetFilePointer(f, -1), -1);

        RES_CloseFile(f);
    }

    /* ---- RES_FindVolumeDir over the bucket the probes came from ---------- */
    {
        TRDirEnt* member = 0;
        TRDir*    d = RES_FindVolumeDir(v->name, all[0]->dir->name, &member);
        LL_CHECK_TRUE("RES_FindVolumeDir found the bucket", d != 0);
        LL_CHECK_TRUE("RES_FindVolumeDir handed back a member", member != 0);
        LL_CHECK_TRUE("RES_FindVolumeDir rejects an unknown volume",
                      RES_FindVolumeDir("NOSUCHVOLUME", "", &member) == 0);
    }

    free(all);
}
