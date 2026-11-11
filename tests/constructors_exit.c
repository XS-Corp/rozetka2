#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static void report_exit(void) { puts("R2_INIT_EXIT callback"); }
__attribute__((constructor)) static void first(int argc, char **argv)
{
    if (argc != 2 || atexit(report_exit) != 0) abort();
    if (strcmp(argv[1], "normal") == 0) exit(23);
    if (strcmp(argv[1], "libc-immediate") == 0) _exit(24);
    __asm__ volatile("syscall" : : "a"(0x2000001ull), "D"(24ull) : "rcx", "r11", "memory");
    __builtin_unreachable();
}
__attribute__((constructor)) static void second(void) { abort(); }
int main(void) { abort(); }
