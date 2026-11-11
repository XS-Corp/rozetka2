#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
extern int leaf_answer(void);
static int initialized;
static int (*relocated_function)(void) = leaf_answer;
static void report_exit(void) { puts("R2_DLOPEN exit"); }
__attribute__((constructor)) static void initialize(void)
{
    initialized++;
    if (atexit(report_exit)) abort();
    puts("R2_DLOPEN init");
}
int dynamic_answer(void) { return 40 + initialized + relocated_function(); }
int open_neighbor(void)
{
    void *library = dlopen("@loader_path/libextra.dylib", RTLD_NOW | RTLD_LOCAL);
    if (!library) return -1;
    int (*answer)(void) = dlsym(library, "extra_answer");
    int result = answer ? answer() : -2;
    dlclose(library); return result;
}
