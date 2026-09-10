/* PORT-C test: LoadPos, the .pos position/orientation table loader.
 *
 * Entry points (read-only game code):
 *   LoadPos              0x0043f660  LEGOLAND/loaders.c
 *   BuildYRotationMatrix 0x00443360  LEGOLAND/math3d.c
 *   MatrixMultiply       0x00443270  LEGOLAND/math3d.c
 *   CopyMatrix           0x00443490  LEGOLAND/math3d.c
 *   UnloadPos            0x0043f7d0  LEGOLAND/math3d.c
 *   RES_OpenFile         0x00489b60  LEGOLAND/res.c  (reached by LoadPos)
 *
 * Oracle: tools/oracle_geom.py over tools/geom.py, with geom.py's one missing
 * step -- the quarter-turn about Y that the loader applies to every matrix as
 * it reads it -- added in float32 and in the loader's own order.
 *
 * WASM32 ONLY
 * -----------
 * The 48-byte record itself holds no pointer, but LoadPos reaches it through
 * RES_OpenFile, and RES_LoadDirectory stores pointers in `int` fields
 * (see portable/tests/test_res_archive.c for the detail). So the whole RES path
 * is ILP32-only and so is this test.
 *
 * RUN DIRECTORY: gamedata/disc.
 *
 * GLOBALS THIS TEST INITIALISES:
 *   g_res_path 0x00813b04  set to "./" -- it is BOTH the volume-file prefix
 *                          RES_OpenVolume's second attempt uses
 *                          (sprintf("%s%s.res", g_res_path, name)) AND the
 *                          drive root RES_FindVolumeOnResPath probes.
 * Everything else is allocated by the loader.
 *
 * NOTE FOR THE INTEGRATOR: RES_OpenFile calls RES_EnsureMounted(0), which
 * loops on RES_FindVolumeOnResPath until a CDFS volume named "LEGOLAND" is
 * found and otherwise shows a modal retry box. Headless, that is an infinite
 * loop through two traps. The host shim must answer GetVolumeInformationA with
 * fsname "CDFS" and volume name "LEGOLAND"; this lane's stopgap
 * portable/tests/support/ll_test_host.c does exactly that, and PORT-A's
 * kernel32 shim will need to as well.
 */
#include "ll_tests.h"
#include "oracle_geom.h"

#include <math.h>
#include <stdlib.h>

typedef struct TMatrix { float m[9]; } TMatrix;

typedef struct TPosItem {
    int     a;        /* +0x00 */
    int     b;        /* +0x04 */
    int     c;        /* +0x08 */
    TMatrix m;        /* +0x0c */
} TPosItem;

typedef struct TPosTable {
    int        per;          /* +0x00 items per frame */
    int        count;        /* +0x04 frames */
    float      sx, sy, sz;   /* +0x08 unit scale, all 1.0 */
    int        f14, f18;     /* +0x14, +0x18  both zeroed */
    char       pad1c[0x24 - 0x1c];
    TPosItem** items;        /* +0x24 */
} TPosTable;

/* The RES records, only as far as this test walks them (same layout as
 * portable/tests/test_res_archive.c). */
typedef struct TRDir2 { struct TRDir2* next; void* files; char* name; } TRDir2;
typedef struct TRDirEnt2 {
    struct TRDirEnt2* anext;
    struct TRDirEnt2* next;
    struct TRDirEnt2* vnext;
    TRDir2*           dir;
    void*             vol;
    int               size;
    int               base;
    char*             name;
} TRDirEnt2;
typedef struct TRVol2 {
    struct TRVol2* next;
    TRDirEnt2*     files;
    char           name[0x14];
    int            handle;
    void*          cur;
    int            refcount;
} TRVol2;

extern TRVol2*    RES_OpenVolume(const char* path);           /* 0x00489750 */
extern TPosTable* LoadPos(const char* name);                  /* 0x0043f660 */
extern void       UnloadPos(TPosTable* t);                    /* 0x0043f7d0 */
extern void       BuildYRotationMatrix(float angle, TMatrix* m); /* 0x00443360 */
extern void       MatrixMultiply(TMatrix* a, TMatrix* b, TMatrix* out); /* 0x00443270 */
extern void       CopyMatrix(TMatrix* src, TMatrix* dst);     /* 0x00443490 */

extern char g_res_path[];   /* 0x00813b04 */

/* tools/oracle_geom.py quantises with floor(v * 4096 + 0.5); floor(), not a
 * cast, so that negatives round the same way on both sides. */
static int quant_floor(float v)
{
    return (int)floor((double)v * (double)ORACLE_GEOM_QUANT + 0.5);
}

void test_loadpos(void)
{
    TRVol2* v;
    int     mi;
    int     si;

    g_res_path[0] = '.';
    g_res_path[1] = '/';
    g_res_path[2] = 0;

    v = RES_OpenVolume(ORACLE_GEOM_VOLUME);
    LL_CHECK_TRUE("RES_OpenVolume mounted " ORACLE_GEOM_VOLUME, v != 0);
    if (!v)
        return;

    /* ---- BuildYRotationMatrix on its own -------------------------------- */
    {
        TMatrix r;
        int k;
        BuildYRotationMatrix(ORACLE_GEOM_ANGLE, &r);
        for (k = 0; k < 9; k++) {
            char what[64];
            sprintf(what, "BuildYRotationMatrix term %d", k);
            LL_CHECK_INT(what, quant_floor(r.m[k]),
                         quant_floor(oracle_geom_rot[k]));
        }
        /* MatrixMultiply against the identity must be a copy, and CopyMatrix
         * must round-trip: the two helpers LoadPos leans on. */
        {
            TMatrix id, out, cp;
            int bad = 0;
            memset(&id, 0, sizeof id);
            id.m[0] = id.m[4] = id.m[8] = 1.0f;
            MatrixMultiply(&r, &id, &out);
            for (k = 0; k < 9; k++)
                if (quant_floor(out.m[k]) != quant_floor(r.m[k]))
                    bad++;
            LL_CHECK_INT("MatrixMultiply by the identity is a copy", bad, 0);
            CopyMatrix(&out, &cp);
            bad = 0;
            for (k = 0; k < 9; k++)
                if (cp.m[k] != out.m[k])
                    bad++;
            LL_CHECK_INT("CopyMatrix copies all nine terms", bad, 0);
        }
    }

    /* ---- every picked .pos member --------------------------------------- */
    for (mi = 0; mi < ORACLE_GEOM_MEMBER_N; mi++) {
        TRDirEnt2* e;
        TRDirEnt2* found = 0;
        TPosTable* t;
        char       path[512];
        char       what[256];
        ll_u64     h;
        int        i, j, k;

        for (e = v->files; e; e = e->vnext) {
            const char* x = e->name;
            const char* y = oracle_geom_members[mi].name;
            for (; *x && *y; x++, y++) {
                int cx = (*x >= 'A' && *x <= 'Z') ? *x + 32 : *x;
                int cy = (*y >= 'A' && *y <= 'Z') ? *y + 32 : *y;
                if (cx != cy)
                    break;
            }
            if (!*x && !*y && e->size == oracle_geom_members[mi].size) {
                found = e;
                break;
            }
        }
        sprintf(what, "%s is in the directory", oracle_geom_members[mi].name);
        LL_CHECK_TRUE(what, found != 0);
        if (!found)
            continue;

        /* The path the engine itself filed the member under. */
        sprintf(path, "%s%s", found->dir->name, found->name);
        t = LoadPos(path);
        sprintf(what, "LoadPos(\"%s\")", path);
        LL_CHECK_TRUE(what, t != 0);
        if (!t)
            continue;

        sprintf(what, "%s items per frame", oracle_geom_members[mi].name);
        LL_CHECK_INT(what, t->per, oracle_geom_members[mi].per);
        sprintf(what, "%s frame count", oracle_geom_members[mi].name);
        LL_CHECK_INT(what, t->count, oracle_geom_members[mi].count);
        sprintf(what, "%s unit scale is 1,1,1", oracle_geom_members[mi].name);
        LL_CHECK_TRUE(what, t->sx == 1.0f && t->sy == 1.0f && t->sz == 1.0f);
        sprintf(what, "%s spare fields zeroed", oracle_geom_members[mi].name);
        LL_CHECK_TRUE(what, t->f14 == 0 && t->f18 == 0);
        /* The member's whole payload must be exactly the table: 8 + per*count*48. */
        sprintf(what, "%s payload is 8 + per*count*48 with nothing left over",
                oracle_geom_members[mi].name);
        LL_CHECK_INT(what, 8 + t->per * t->count * 48,
                     oracle_geom_members[mi].bytes_used);

        /* ---- the digest over every element, rotation applied ------------ */
        h = ll_fnv_init();
        h = ll_fnv_int(h, t->per);
        h = ll_fnv_int(h, t->count);
        for (i = 0; i < t->count; i++) {
            for (j = 0; j < t->per; j++) {
                TPosItem* it = &t->items[i][j];
                h = ll_fnv_int(h, it->a);
                h = ll_fnv_int(h, it->b);
                h = ll_fnv_int(h, it->c);
                for (k = 0; k < 9; k++)
                    h = ll_fnv_int(h, quant_floor(it->m.m[k]));
            }
        }
        sprintf(what, "%s digest (per;count;a,b,c;m*4096 rounded)",
                oracle_geom_members[mi].name);
        LL_CHECK_HEX(what, h, oracle_geom_members[mi].digest);

        /* Keep the table around while the samples are checked. */
        for (si = 0; si < ORACLE_GEOM_SAMPLE_N; si++) {
            int idx, fi, ei;
            TPosItem* it;
            if (oracle_geom_samples[si].member != mi)
                continue;
            idx = oracle_geom_samples[si].elem;
            fi = idx / t->per;
            ei = idx % t->per;
            it = &t->items[fi][ei];
            sprintf(what, "%s element %d a/b/c",
                    oracle_geom_members[mi].name, idx);
            LL_CHECK_TRUE(what,
                          it->a == oracle_geom_samples[si].a &&
                          it->b == oracle_geom_samples[si].b &&
                          it->c == oracle_geom_samples[si].c);
            for (k = 0; k < 9; k++) {
                int got = quant_floor(it->m.m[k]);
                int want = quant_floor(oracle_geom_samples[si].m[k]);
                int d = got - want;
                if (d < 0)
                    d = -d;
                sprintf(what, "%s element %d rotated m[%d]",
                        oracle_geom_members[mi].name, idx, k);
                ll_checks++;
                if (d <= ORACLE_GEOM_TOL)
                    ll_pass(what);
                else
                    ll_fail(what, "got %d, want %d (quantiser units)",
                            got, want);
            }
        }

        UnloadPos(t);
    }
}
