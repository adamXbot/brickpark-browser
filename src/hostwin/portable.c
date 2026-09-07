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

/* Reached through a generated stub (portable/tools/gen_link.py). */
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
