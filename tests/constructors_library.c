#include <stdio.h>
#include <stdlib.h>

int constructor_order;
static void library_exit(void)
{
    if (constructor_order != 12345) abort();
    puts("R2_INIT library exit: 12345");
}
__attribute__((constructor)) static void first(void)
{
    constructor_order = 1;
    if (atexit(library_exit) != 0) abort();
}
__attribute__((constructor)) static void second(void)
{
    constructor_order = constructor_order * 10 + 2;
}
