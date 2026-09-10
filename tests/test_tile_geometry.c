/* PORT-C test: the isometric tile<->screen geometry and the walker tile tests.
 *
 * Entry points (read-only game code):
 *   GetTileDimensions 0x00460540  LEGOLAND/map.c
 *   GetTileCentre     0x0045ad60  LEGOLAND/tilehelp.c
 *   GetTileBounds     0x0045acc0  LEGOLAND/pathbuild.c
 *   OverNewTile       0x00483650  LEGOLAND/tilehelp.c
 *   CrossTileCentre   0x004837d0  LEGOLAND/tilehelp.c
 *
 * Oracle: tools/oracle_tilegeom.py, whose scroll/origin-zero case is
 * tools/tilemap.py's own per-cell placement and whose sampled grid coordinates
 * come from a real shipped level (GLONE.MAP inside gamedata/disc/Legoland.res).
 *
 * WHY THIS ONE RUNS ON THE NATIVE 64-BIT BUILD TOO
 * ------------------------------------------------
 * Nothing here is read from a file by the C: the test supplies the three
 * inputs the engine reads out of memory (the default ground sprite's height,
 * the map header's viewport origin, the 8.8 scroll pair) and checks integer
 * arithmetic. No serialised struct, no `long`, no pointer in a stream. The one
 * layout-sensitive sibling, FreeTileSpace (0x0045aa90), is NOT tested here:
 * it memsets `n * 4` bytes over g_tile_sprites[], which is correct only where
 * a slot is 4 bytes, so it is a wasm32-only check (see docs/lanes/scope-port-c.md).
 *
 * GLOBALS THIS TEST INITIALISES (the closure rebuilds .data from the exe, so
 * all of them already hold a shipped value; the test overwrites them):
 *   g_tile_sprites 0x00805f60  slot [g_default_tile] must point at a Sprite
 *                              whose +0x16 height is the ground tile height
 *   g_default_tile 0x00667ca4  set to 0 so only the first slot is touched
 *   g_map          (Map*)      header with width/height +0x14/+0x16 and the
 *                              viewport origin at +0x20/+0x22
 *   g_scroll_x/y   0x00667cb4 / 0x00667cb8   8.8 fixed point
 */
#include "ll_tests.h"
#include "oracle_tilegeom.h"

/* legoland.h's own types, spelled locally: this file must not include the
 * game header (it is the matching build's, and PORT-C does not edit it). */
typedef struct TestPos { int x; int y; } TestPos;
typedef struct TestBounds { int left, top, right, bottom; } TestBounds;
typedef struct TestSprite { char pad[0x14]; short w; short h; } TestSprite;

/* A walking entity, only the fields the two predicates read
 * (LEGOLAND/tilehelp.c's Walker). */
typedef struct TestWalker {
    char           pad00[0x62];
    unsigned short flags62;
    char           pad64[4];
    int            wx;      /* +0x68 world x, 24.8 */
    int            wy;      /* +0x6c world y, 24.8 */
    char           pad70[2];
    unsigned char  dir;     /* +0x72 */
    char           pad73[0x80 - 0x73];
} TestWalker;

extern void GetTileDimensions(int* out_w, int* out_h);        /* 0x00460540 */
extern void GetTileCentre(TestPos* tile, TestPos* out);       /* 0x0045ad60 */
extern void GetTileBounds(TestPos* tile, TestBounds* out);    /* 0x0045acc0 */
extern int  OverNewTile(TestWalker* w, int x, int y);         /* 0x00483650 */
extern int  CrossTileCentre(TestWalker* w, int x, int y);     /* 0x004837d0 */

extern void*        g_map;              /* Map*    (header, +0x14 w, +0x16 h) */
extern TestSprite* g_tile_sprites[];    /* 0x00805f60 */
extern int          g_default_tile;     /* 0x00667ca4 */
extern int          g_scroll_x;         /* 0x00667cb4 */
extern int          g_scroll_y;         /* 0x00667cb8 */

static unsigned char map_header[0x40];
static TestSprite    ground_tile;

void test_tile_geometry(void)
{
    int i;
    int tw = 0, th = 0;

    /* ---- the three inputs the engine reads out of memory ---------------- */
    ground_tile.w = (short)ORACLE_TG_TILE_W;
    ground_tile.h = (short)ORACLE_TG_SPRITE_H;
    g_default_tile = 0;
    g_tile_sprites[0] = &ground_tile;

    memset(map_header, 0, sizeof map_header);
    *(unsigned short*)(map_header + 0x14) = (unsigned short)ORACLE_TG_WIDTH;
    *(unsigned short*)(map_header + 0x16) = (unsigned short)ORACLE_TG_HEIGHT;
    *(unsigned short*)(map_header + 0x20) = (unsigned short)ORACLE_TG_ORIGIN_X;
    *(unsigned short*)(map_header + 0x22) = (unsigned short)ORACLE_TG_ORIGIN_Y;
    g_map = map_header;
    g_scroll_x = ORACLE_TG_SCROLL_X;
    g_scroll_y = ORACLE_TG_SCROLL_Y;

    /* ---- GetTileDimensions: w = 2h, both from the sprite's height ------- */
    GetTileDimensions(&tw, &th);
    LL_CHECK_INT("GetTileDimensions height", th, ORACLE_TG_SPRITE_H);
    LL_CHECK_INT("GetTileDimensions width", tw, ORACLE_TG_TILE_W);

    /* ---- GetTileCentre / GetTileBounds over real grid coordinates ------- */
    for (i = 0; i < ORACLE_TG_CELL_N; i++) {
        const int* e = oracle_tg_cells[i];
        TestPos tile, centre;
        TestBounds b;
        char what[128];

        tile.x = e[0];
        tile.y = e[1];
        GetTileCentre(&tile, &centre);
        sprintf(what, "%s cell (%d,%d) centre x", ORACLE_TG_LEVEL, e[0], e[1]);
        LL_CHECK_INT(what, centre.x, e[2]);
        sprintf(what, "%s cell (%d,%d) centre y", ORACLE_TG_LEVEL, e[0], e[1]);
        LL_CHECK_INT(what, centre.y, e[3]);

        GetTileBounds(&tile, &b);
        sprintf(what, "%s cell (%d,%d) bounds left", ORACLE_TG_LEVEL, e[0], e[1]);
        LL_CHECK_INT(what, b.left, e[4]);
        sprintf(what, "%s cell (%d,%d) bounds top", ORACLE_TG_LEVEL, e[0], e[1]);
        LL_CHECK_INT(what, b.top, e[5]);
        sprintf(what, "%s cell (%d,%d) bounds right", ORACLE_TG_LEVEL, e[0], e[1]);
        LL_CHECK_INT(what, b.right, e[6]);
        sprintf(what, "%s cell (%d,%d) bounds bottom", ORACLE_TG_LEVEL, e[0], e[1]);
        LL_CHECK_INT(what, b.bottom, e[7]);

        /* The two functions must agree: the centre is the rect's midpoint,
         * which is what pins the formula (LEGOLAND/tilehelp.c's header). */
        sprintf(what, "%s cell (%d,%d) centre is the bounds midpoint",
                ORACLE_TG_LEVEL, e[0], e[1]);
        LL_CHECK_TRUE(what,
                      centre.x == b.left + ORACLE_TG_TILE_W / 2 &&
                      centre.y == b.top + ORACLE_TG_SPRITE_H / 2);
    }

    /* ---- OverNewTile / CrossTileCentre --------------------------------- */
    for (i = 0; i < ORACLE_TG_WALKER_N; i++) {
        const int* e = oracle_tg_walkers[i];
        TestWalker w;
        char what[128];

        memset(&w, 0, sizeof w);
        w.wx = e[0];
        w.wy = e[1];
        w.dir = (unsigned char)e[2];
        sprintf(what, "OverNewTile probe %d (dir %d)", i, e[2]);
        LL_CHECK_INT(what, OverNewTile(&w, e[3], e[4]), e[5]);
        sprintf(what, "CrossTileCentre probe %d (dir %d)", i, e[2]);
        LL_CHECK_INT(what, CrossTileCentre(&w, e[3], e[4]), e[6]);
    }
}
