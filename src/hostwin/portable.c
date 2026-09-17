/* LEGOLAND portable build -- the few helpers ll_portable.h promises. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void ll_unported_asm(const char* file, int line, const char* func)
{
    fprintf(stderr, "LEGOLAND portable: %s:%d: %s() is an inline-asm body that has not been ported yet\n",
            file, line, func);
    abort();
}

unsigned int ll_rdtsc(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned int)((unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec);
}

static int ll_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

int stricmp(const char* a, const char* b)
{
    for (;; a++, b++) {
        int d = ll_lower((unsigned char)*a) - ll_lower((unsigned char)*b);
        if (d != 0 || *a == 0)
            return d;
    }
}

int strnicmp(const char* a, const char* b, size_t n)
{
    for (; n > 0; a++, b++, n--) {
        int d = ll_lower((unsigned char)*a) - ll_lower((unsigned char)*b);
        if (d != 0 || *a == 0)
            return d;
    }
    return 0;
}

/* Reached through a generated stub (tools/gen_link.py). */
void ll_unwritten(const char* name, unsigned int address)
{
    if (address)
        fprintf(stderr, "LEGOLAND portable: %s (0x%08x) is not written yet\n", name, address);
    else
        fprintf(stderr, "LEGOLAND portable: %s has no definition and no known address\n", name);
    abort();
}

void ll_unhosted(const char* name, const char* dll)
{
    fprintf(stderr, "LEGOLAND portable: host API %s (%s) is not implemented by the shim yet\n", name, dll);
    abort();
}

/* ---- optional quality-of-life switches (ll_portable.h's LL_QOL) ---------- */

static unsigned int ll_qol_bits;

unsigned int ll_qol(void)
{
    return ll_qol_bits;
}

/* Take the host's own switches out of a game command line, in place, so the
 * game never sees them, and turn their bits on. Returns every bit now on. */
unsigned int ll_qol_take_switches(char* cmdline)
{
    static const struct { const char* name; unsigned int bit; } k_switches[] = {
        { "-ll-freeplay-all", LL_QOL_FREEPLAY_ALL },
    };
    char*  out = cmdline;
    char*  p = cmdline;
    size_t i;

    for (;;) {
        size_t n;
        int    taken = 0;

        p += strspn(p, " \t");
        if (!*p)
            break;
        n = strcspn(p, " \t");
        for (i = 0; i < sizeof k_switches / sizeof k_switches[0]; i++)
            if (strlen(k_switches[i].name) == n && strnicmp(p, k_switches[i].name, n) == 0) {
                ll_qol_bits |= k_switches[i].bit;
                taken = 1;
            }
        if (!taken) {
            /* `out` trails `p` by at least the whitespace just skipped. */
            if (out != cmdline)
                *out++ = ' ';
            memmove(out, p, n);
            out += n;
        }
        p += n;
    }
    *out = 0;
    return ll_qol_bits;
}
