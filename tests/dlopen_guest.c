#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static void require(int ok, const char *message)
{
    if (!ok) { fprintf(stderr, "FAIL guest dlopen: %s\n", message); abort(); }
}
int main(int argc, char **argv)
{
    require(argc == 4, "arguments");
    void *process = dlopen(NULL, RTLD_NOW | RTLD_LOCAL);
    void *process_again = dlopen(NULL, RTLD_LAZY | RTLD_NOLOAD);
    require(process != NULL && process_again == process, "reference-counted process scope");
    int (*pid)(void) = dlsym(process, "getpid");
    require(pid != NULL && pid() == getpid(), "process scope returns native ABI stubs");
    void *system = dlopen("libSystem.dylib", RTLD_NOW | RTLD_LOCAL);
    void *system_again = dlopen("/usr/lib/libSystem.B.dylib", RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
    require(system != NULL && system_again == system && dlsym(system, "getpid") == (void *)pid, "libSystem shared-cache ABI stubs");
    require(dlsym(system, "rozetka2_nonexistent_symbol") == NULL && dlerror() != NULL, "missing optional libSystem symbol");
    require(dlclose(system) == 0 && dlclose(system_again) == 0, "libSystem handle references");
    require(dlopen("", RTLD_NOW) == NULL && dlerror() != NULL, "empty filename is not process scope");
    void *first = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    require(first != NULL, "open guest library");
    void *second = dlopen(argv[1], RTLD_LAZY | RTLD_LOCAL | RTLD_NOLOAD);
    require(second == first, "reopen without repeating initialization");
    int (*answer)(void) = dlsym(first, "dynamic_answer");
    require(answer != NULL && answer() == 42, "execute guest symbol");
    int (*neighbor)(void) = dlsym(first, "open_neighbor");
    require(neighbor != NULL && neighbor() == 99, "loader_path is relative to the calling dylib");
    void *rpath = dlopen("@rpath/libdlopen.dylib", RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
    require(rpath == first && dlclose(rpath) == 0, "dlopen uses caller run paths");
    char *directory = strdup(argv[1]); require(directory != NULL, "directory allocation");
    char *slash = strrchr(directory, '/'); require(slash != NULL, "absolute fixture path"); *slash = 0;
    require(chdir(directory) == 0, "change to fixture directory"); free(directory);
    void *relative = dlopen("libdlopen.dylib", RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
    require(relative == first && dlclose(relative) == 0, "bare library filename in current directory");
    relative = dlopen("./libdlopen.dylib", RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
    require(relative == first && dlclose(relative) == 0, "relative library path");
    require(dlopen("librozetka2_missing_optional.dylib", RTLD_NOW) == NULL && dlerror() != NULL, "missing optional basename returns dlerror");
    int (*leaf)(void) = dlsym(first, "leaf_answer");
    require(leaf != NULL && leaf() == 1, "handle searches dependencies");
    void *only = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL | RTLD_FIRST);
    require(only != NULL && only != first, "RTLD_FIRST has a distinct handle");
    require(dlsym(only, "dynamic_answer") == (void *)answer, "RTLD_FIRST own definition");
    require(dlsym(only, "leaf_answer") == NULL && dlerror() != NULL, "RTLD_FIRST excludes ordinary dependencies");
    require(dlclose(only) == 0, "close RTLD_FIRST handle");
    Dl_info info;
    require(dladdr((void *)answer, &info) && info.dli_fname && info.dli_fbase &&
            strstr(info.dli_fname, "libdlopen.dylib"), "guest image metadata");
    require(dlsym(RTLD_DEFAULT, "dynamic_answer") == NULL, "local scope");
    require(dlsym(process, "dynamic_answer") == NULL, "process scope hides local images");
    require(dlerror() != NULL && dlerror() == NULL, "missing symbol and one-shot dlerror");
    require(dlopen("/definitely_missing_rozetka2.dylib", RTLD_NOW) == NULL && dlerror() != NULL, "missing file");
    require(dlopen(argv[3], RTLD_NOW) == NULL && dlerror() != NULL, "missing dependency rollback");
    require(answer() == 42, "existing library survives failed open");
    void *global = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
    require(global == first && dlsym(RTLD_DEFAULT, "dynamic_answer") == (void *)answer, "promote global scope");
    require(dlsym(process, "dynamic_answer") == (void *)answer, "process scope sees later global promotion");
    void *extra = dlopen(argv[2], RTLD_NOW);
    int (*other)(void) = dlsym(extra, "extra_answer");
    require(extra && other && other() == 99 && answer() == 42, "second load preserves existing fixups");
    require(dlclose(extra) == 0 && dlclose(second) == 0 && dlclose(first) == 0 && dlclose(global) == 0, "close references");
    void *again = dlopen(argv[1], RTLD_NOW);
    require(again == first && answer() == 42 && dlclose(again) == 0, "pinned image remains initialized");
    require(dlclose(process) == 0 && dlclose(process_again) == 0, "close process scope references");
    require(dlclose(process) == -1 && dlerror() != NULL, "closed process handle rejected");
    puts("R2_DLOPEN guest code and scopes: OK");
    return 42;
}
