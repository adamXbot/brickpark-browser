/* The smallest program that links the whole recovered game: proves every
 * symbol resolves (through the generated closure) and nothing more. The real
 * host entry point replaces this once the SDL shim exists. */
#include <stdio.h>

int main(void)
{
    puts("legoland_linkcheck: every symbol resolved");
    return 0;
}
