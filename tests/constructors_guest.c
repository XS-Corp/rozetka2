#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int constructor_order;
extern int __cxa_atexit(void (*function)(void *), void *argument, void *dso);
static int callback_argument = 5;
static void cxx_exit(void *argument)
{
    if (argument != &callback_argument || constructor_order != 1234) abort();
    constructor_order = constructor_order * 10 + *(int *)argument;
    puts("R2_INIT C++ exit: 12345");
}
static void main_exit(void)
{
    if (constructor_order != 123) abort();
    constructor_order = 1234;
    puts("R2_INIT main exit: 1234");
}
__attribute__((constructor)) static void initialize(int argc, char **argv)
{
    if (constructor_order != 12 || argc != 2 || strcmp(argv[1], "fixture") != 0) abort();
    constructor_order = 123;
    if (__cxa_atexit(cxx_exit, &callback_argument, NULL) != 0) abort();
}
int main(void)
{
    if (constructor_order != 123 || atexit(main_exit) != 0) abort();
    puts("R2_INIT main: 123");
    return 42;
}
