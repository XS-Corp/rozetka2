#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static _Thread_local long initialized = 17;
static _Thread_local long zeroed;
static _Thread_local const char *text = "guest TLS";
static _Thread_local unsigned char aligned[80] __attribute__((aligned(64)));
static _Thread_local double scalar = 1.25;
__attribute__((noinline)) static long update(long a, long b, long c, long d, long e, long f)
{
    initialized += a + b + c + d + e + f;
    return initialized + ++zeroed;
}
__attribute__((noinline)) static double floating(double input) { return input + scalar; }
int main(void)
{
    if (initialized != 17 || zeroed != 0 || strcmp(text, "guest TLS") != 0 ||
        (uintptr_t)aligned % 64 != 0 || aligned[79] != 0) abort();
    long *address = &initialized;
    aligned[79] = 42;
    if (update(1,2,3,4,5,6) != 39 || update(0,0,0,0,0,0) != 40 ||
        address != &initialized || *address != 38 || aligned[79] != 42 || floating(2.5) != 3.75) abort();
    puts("R2_TLS data, zero-fill, rebased pointer, alignment and register preservation: OK");
    return 42;
}
