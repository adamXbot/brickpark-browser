/* PORT-C headless test driver: `legoland_tests <subcommand>`.
 *
 * Linked against the whole of legoland_core plus the generated closure, so a
 * subcommand that reaches a piece of the game nobody has ported yet dies in
 * gen_link's trap with the symbol's name -- which is itself the useful
 * result, and is what the lane notes record.
 *
 * Run from the directory the game's own relative paths are written against
 * (`gamedata/main` for LEGOLAND.ICM, see docs/runtime/assets.md); the ctest
 * wiring sets WORKING_DIRECTORY for every test. */
#include "ll_tests.h"

#include <stdarg.h>
#include <stdlib.h>

int ll_checks;
int ll_fails;
int ll_notes;

static int ll_verbose = 1;

void ll_pass(const char* what)
{
    if (ll_verbose)
        printf("ok       %s\n", what);
}

void ll_fail(const char* what, const char* fmt, ...)
{
    va_list ap;
    ll_fails++;
    printf("FAIL     %s: ", what);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

void ll_note(const char* what, const char* fmt, ...)
{
    va_list ap;
    ll_notes++;
    printf("note     %s: ", what);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

ll_u64 ll_fnv_init(void) { return LL_FNV_BASIS; }

ll_u64 ll_fnv_bytes(ll_u64 h, const void* p, unsigned int n)
{
    const unsigned char* b = (const unsigned char*)p;
    unsigned int i;
    for (i = 0; i < n; i++) {
        h ^= b[i];
        h *= LL_FNV_PRIME;
    }
    return h;
}

ll_u64 ll_fnv_str(ll_u64 h, const char* s)
{
    return ll_fnv_bytes(h, s, (unsigned int)strlen(s));
}

ll_u64 ll_fnv_int(ll_u64 h, long long v)
{
    char buf[32];
    sprintf(buf, "%lld;", v);
    return ll_fnv_str(h, buf);
}

struct entry {
    const char* name;
    void (*fn)(void);
    const char* what;
};

static const struct entry entries[] = {
    { "save_framing",  test_save_framing,
      "BeginMeasuredBlock/EndMeasuredBlock/SaveGameWrite/SaveGameRead/FindeIneList" },
    { "tile_geometry", test_tile_geometry,
      "GetTileCentre/GetTileBounds/GetTileDimensions/OverNewTile/CrossTileCentre" },
};

int main(int argc, char** argv)
{
    unsigned int i;

    if (argc < 2) {
        printf("usage: legoland_tests <test>\n\n");
        for (i = 0; i < sizeof entries / sizeof entries[0]; i++)
            printf("  %-14s %s\n", entries[i].name, entries[i].what);
        return 2;
    }

    for (i = 0; i < sizeof entries / sizeof entries[0]; i++) {
        if (strcmp(argv[1], entries[i].name) == 0) {
            printf("== %s: %s\n", entries[i].name, entries[i].what);
            entries[i].fn();
            printf("== %s: %d checks, %d failed, %d notes\n",
                   entries[i].name, ll_checks, ll_fails, ll_notes);
            return ll_fails ? 1 : 0;
        }
    }
    printf("unknown test \"%s\"\n", argv[1]);
    return 2;
}
