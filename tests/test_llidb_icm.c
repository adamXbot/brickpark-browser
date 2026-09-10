/* PORT-C test: the LLIDB element database loaded from the real LEGOLAND.ICM.
 *
 * Entry points (read-only game code):
 *   LLIDB_LoadICM     0x0047aff0  LEGOLAND/data2.c
 *   LLIDB_GetCount    0x0047b2d0  LEGOLAND/llidb.c
 *   LLIDB_GetElement  0x0047b2e0  LEGOLAND/llidb.c
 *   LLIDB_FindElement 0x0047b330  LEGOLAND/llidb.c
 *   ElemID            0x0047b3f0  LEGOLAND/mapinit.c
 *
 * Oracle: tools/oracle_icm.py over tools/tilemap.py's `load_icm`.
 *
 * WASM32 ONLY
 * -----------
 * LLIDB_LoadICM reads the element records in bulk:
 *
 *     _read(fd, g_llidb_pages[page], n * sizeof(LLElem))
 *
 * and LLElem is {char* name; char* image; unsigned int type_flags; void* data;
 * unsigned int refcount} -- 20 bytes on ILP32 and 40 on LP64. On a 64-bit host
 * the loader therefore reads twice as many bytes as the file holds per record
 * and the type words land in the wrong places. The size assertion below makes
 * that explicit rather than letting the test quietly produce nonsense.
 *
 * RUN DIRECTORY: gamedata/main -- the loader opens the bare name
 * "LEGOLAND.ICM" (0x004bc120) with _open, relative to the cwd.
 *
 * NOTE FOR THE INTEGRATOR: the shipped file is named `Legoland.icm` but the
 * game asks for `LEGOLAND.ICM`. That only works on a case-insensitive
 * filesystem (macOS APFS, and node's NODERAWFS on top of it). On Linux/CI the
 * host shim's path translation will need a case-insensitive fallback, or the
 * asset tree needs normalising. Recorded in docs/lanes/scope-port-c.md.
 *
 * GLOBALS THIS TEST INITIALISES: none by hand. LLIDB_LoadICM allocates the page
 * table itself and writes g_llidb_count (0x006691a4), g_llidb_capacity
 * (0x006691a0) and g_llidb_pages (0x006691a8). The test only reads them.
 */
#include "ll_tests.h"
#include "oracle_icm.h"

/* legoland.h's LLElem, spelled locally (PORT-C does not include the game
 * header). The field offsets are the load-bearing part. */
typedef struct TLLElem {
    char*        name;        /* +0x00 */
    char*        image;       /* +0x04 */
    unsigned int type_flags;  /* +0x08 */
    void*        data;        /* +0x0c */
    unsigned int refcount;    /* +0x10 */
} TLLElem;

extern int      LLIDB_LoadICM(void);                            /* 0x0047aff0 */
extern unsigned LLIDB_GetCount(void);                           /* 0x0047b2d0 */
extern int      LLIDB_GetElement(unsigned idx, TLLElem** out);   /* 0x0047b2e0 */
extern int      LLIDB_FindElement(const char* name, TLLElem** out,
                                  unsigned* outidx);             /* 0x0047b330 */
extern TLLElem* ElemID(const char* name);                        /* 0x0047b3f0 */

extern unsigned  g_llidb_capacity;   /* 0x006691a0 */
extern unsigned  g_llidb_count;      /* 0x006691a4 */
extern TLLElem** g_llidb_pages;      /* 0x006691a8 */

void test_llidb_icm(void)
{
    int      rc;
    unsigned i;
    ll_u64   h;

    /* The wire record size is what the bulk read depends on. */
    LL_CHECK_INT("sizeof(LLElem) matches the on-disk record",
                 (int)sizeof(TLLElem), ORACLE_ICM_LLELEM_SIZE);
    if (sizeof(TLLElem) != ORACLE_ICM_LLELEM_SIZE) {
        ll_note("llidb_icm",
                "LLElem is %d bytes here, so LLIDB_LoadICM's bulk read is "
                "wrong by construction -- this test needs an ILP32 target",
                (int)sizeof(TLLElem));
        return;
    }

    rc = LLIDB_LoadICM();
    LL_CHECK_INT("LLIDB_LoadICM returned 0", rc, 0);
    LL_CHECK_INT("element count", (int)LLIDB_GetCount(), ORACLE_ICM_COUNT);
    LL_CHECK_INT("page capacity rounded up to 0x100",
                 (int)g_llidb_capacity, ORACLE_ICM_CAPACITY);
    LL_CHECK_INT("page count", (int)(g_llidb_capacity >> 8), ORACLE_ICM_PAGES);
    LL_CHECK_TRUE("page table allocated", g_llidb_pages != 0);
    if (!g_llidb_pages)
        return;

    /* ---- every element, in index order, against the decoder -------------- */
    h = ll_fnv_init();
    for (i = 0; i < g_llidb_count; i++) {
        TLLElem* e = 0;
        const char* p;
        char upper[512];
        int k;
        if (LLIDB_GetElement(i, &e) != 0 || !e)
            break;
        h = ll_fnv_int(h, (long long)i);
        for (k = 0, p = e->name; p && *p && k < (int)sizeof upper - 1; p++, k++)
            upper[k] = (*p >= 'a' && *p <= 'z') ? (char)(*p - 32) : *p;
        upper[k] = 0;
        h = ll_fnv_str(h, upper);
        h = ll_fnv_str(h, ";");
        for (k = 0, p = e->image; p && *p && k < (int)sizeof upper - 1; p++, k++)
            upper[k] = (*p >= 'a' && *p <= 'z') ? (char)(*p - 32) : *p;
        upper[k] = 0;
        h = ll_fnv_str(h, upper);
        h = ll_fnv_str(h, ";");
        h = ll_fnv_int(h, (long long)e->type_flags);
    }
    LL_CHECK_INT("walked every element with LLIDB_GetElement",
                 (int)i, ORACLE_ICM_COUNT);
    LL_CHECK_HEX("database digest {index;NAME;IMAGE;type_flags}",
                 h, ORACLE_ICM_DIGEST);

    /* Out of range is -3 and nulls the out-pointer. */
    {
        TLLElem* e = (TLLElem*)1;
        LL_CHECK_INT("LLIDB_GetElement past the end returns -3",
                     LLIDB_GetElement(g_llidb_count, &e), -3);
        LL_CHECK_TRUE("LLIDB_GetElement past the end nulls the out-pointer",
                      e == 0);
    }

    /* Every element must have come out with the loaded bit and the refcount
     * clear: LLIDB_LoadICM drops bit 0 and zeroes +0x10 for each record, so a
     * stale `data` pointer from the file can never be mistaken for loaded
     * data. */
    {
        int loaded = 0, refs = 0;
        for (i = 0; i < g_llidb_count; i++) {
            TLLElem* e = 0;
            LLIDB_GetElement(i, &e);
            if (!e)
                continue;
            if (e->type_flags & 1)
                loaded++;
            if (e->refcount)
                refs++;
        }
        LL_CHECK_INT("no element claims loaded data after the ICM load",
                     loaded, 0);
        LL_CHECK_INT("every refcount starts at 0", refs, 0);
    }

    /* ---- LLIDB_FindElement / ElemID, case-insensitively ----------------- */
    for (i = 0; i < ORACLE_ICM_PROBE_N; i++) {
        const char* q = oracle_icm_probes[i].query;
        TLLElem* e = 0;
        unsigned idx = 0xffffffffu;
        int r;
        char what[256];

        r = LLIDB_FindElement(q, &e, &idx);
        sprintf(what, "LLIDB_FindElement(\"%s\") return", q);
        LL_CHECK_INT(what, r, oracle_icm_probes[i].found ? 0 : -3);

        if (oracle_icm_probes[i].found) {
            sprintf(what, "LLIDB_FindElement(\"%s\") index", q);
            LL_CHECK_INT(what, (int)idx, oracle_icm_probes[i].index);
            sprintf(what, "LLIDB_FindElement(\"%s\") name", q);
            LL_CHECK_STR(what, e ? e->name : 0, oracle_icm_probes[i].label);
            sprintf(what, "LLIDB_FindElement(\"%s\") image", q);
            LL_CHECK_STR(what, e ? e->image : 0, oracle_icm_probes[i].value);
            sprintf(what, "LLIDB_FindElement(\"%s\") type_flags", q);
            LL_CHECK_HEX(what, e ? e->type_flags : 0,
                         oracle_icm_probes[i].type_flags);
            sprintf(what, "ElemID(\"%s\") is the same element", q);
            LL_CHECK_TRUE(what, ElemID(q) == e);
        } else {
            sprintf(what, "LLIDB_FindElement(\"%s\") nulls the out-pointer", q);
            LL_CHECK_TRUE(what, e == 0);
            sprintf(what, "ElemID(\"%s\") is 0", q);
            LL_CHECK_TRUE(what, ElemID(q) == 0);
        }
    }

    /* A null name is -3, not a crash (LLIDB_FindElement's guard). */
    {
        TLLElem* e = (TLLElem*)1;
        LL_CHECK_INT("LLIDB_FindElement(0) returns -3",
                     LLIDB_FindElement(0, &e, 0), -3);
        LL_CHECK_TRUE("LLIDB_FindElement(0) nulls the out-pointer", e == 0);
    }
}
