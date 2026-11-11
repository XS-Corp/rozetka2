#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
static void require(int ok, const char *message)
{
    if (!ok) { fprintf(stderr, "FAIL weak bind: %s\n", message); abort(); }
}
static void *open_provider(const char *path, int local, int value)
{
    void *handle = dlopen(path, RTLD_NOW | (local ? RTLD_LOCAL : RTLD_GLOBAL));
    require(handle != NULL, "load provider");
    int (*function)(void) = dlsym(handle, "provider_value");
    int (*data)(void) = dlsym(handle, "provider_data");
    require(function && data && function() == value && data() == value + 1,
            "canonical function and weak data with interior addend");
    return handle;
}
static void *address(void *handle)
{
    void *(*function)(void) = dlsym(handle, "provider_address");
    require(function != NULL, "address getter");
    return function();
}
int main(int argc, char **argv)
{
    require(argc == 7, "arguments");
    void *local = open_provider(argv[1], 1, 3);
    void *first = open_provider(argv[2], 0, 17);
    void *second = open_provider(argv[3], 0, 17);
    require(address(first) == address(second) && address(local) != address(first),
            "first visible weak wins; hidden provider excluded");
    void *strong = open_provider(argv[4], 0, 42);
    void *last = open_provider(argv[5], 0, 42);
    void *hidden_client = open_provider(argv[6], 1, 42);
    require(address(last) == address(strong) && address(hidden_client) == address(strong),
            "strong wins for new clients, including hidden clients");
    puts("R2_WEAK canonical functions, data, addends and visibility: OK");
    return 42;
}
