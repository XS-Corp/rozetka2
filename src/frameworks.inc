/* Named native OS scopes expose declared guest stubs, never ARM entry points. */
typedef struct {
    const char *name, *path;
    void **library;
    size_t *references;
} native_framework_scope;
static void framework_scopes(r2_native_runtime *runtime, native_framework_scope scopes[4])
{
    scopes[0] = (native_framework_scope){"System", "/usr/lib/libSystem.B.dylib", &runtime->system_library, &runtime->system_opens};
    scopes[1] = (native_framework_scope){"IOKit", "/System/Library/Frameworks/IOKit.framework/IOKit", &runtime->iokit, &runtime->iokit_opens};
    scopes[2] = (native_framework_scope){"CoreFoundation", "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", &runtime->core_foundation, &runtime->core_foundation_opens};
    scopes[3] = (native_framework_scope){"objc.A", "/usr/lib/libobjc.A.dylib", &runtime->objc, &runtime->objc_opens};
}
static bool framework_path(const native_framework_scope *scope, const char *path)
{
    if (strcmp(scope->path, path) == 0) return true;
    char candidate[256]; snprintf(candidate, sizeof(candidate), "lib%s.dylib", scope->name);
    if (strcmp(candidate, path) == 0) return true;
    if (strcmp(scope->name, "System") == 0)
        return strcmp(path, "libSystem.B.dylib") == 0 || strcmp(path, "/usr/lib/libSystem.dylib") == 0;
    if(strcmp(scope->name,"objc.A")==0)return strcmp(path,"libobjc.dylib")==0||strcmp(path,"/usr/lib/libobjc.dylib")==0;
    size_t prefix = strlen(scope->path) - strlen(scope->name);
    snprintf(candidate, sizeof(candidate), "%.*sVersions/A/%s", (int)prefix, scope->path, scope->name);
    if (strcmp(candidate, path) == 0) return true;
    snprintf(candidate, sizeof(candidate), "%.*sVersions/Current/%s", (int)prefix, scope->path, scope->name);
    return strcmp(candidate, path) == 0;
}
static r2_status framework_open(r2_native_runtime *runtime, native_framework_scope *scope, unsigned index,
                                 uint64_t flags, uint64_t *handle, char *error, size_t error_len)
{
    int supported = RTLD_LAZY | RTLD_NOW | RTLD_LOCAL | RTLD_GLOBAL | RTLD_NODELETE | RTLD_NOLOAD | RTLD_FIRST;
    if (flags & ~(uint64_t)supported) {
        r2_set_error(error, error_len, "unsupported guest %s dlopen flags", scope->name); return R2_ERR_UNSUPPORTED;
    }
    if (!*scope->library && (flags & RTLD_NOLOAD) && index != 0) {
        snprintf(runtime->loader_error, sizeof(runtime->loader_error), "%s is not loaded", scope->name);
        runtime->loader_error_pending = true; *handle = 0; return R2_OK;
    }
    if (!*scope->library) *scope->library = dlopen(scope->path, RTLD_NOW | RTLD_LOCAL);
    if (!*scope->library) {
        snprintf(runtime->loader_error, sizeof(runtime->loader_error), "%s: %s", scope->name, dlerror());
        runtime->loader_error_pending = true; *handle = 0; return R2_OK;
    }
    (*scope->references)++;
    if (!(flags & RTLD_LOCAL) || (flags & RTLD_GLOBAL)) runtime->framework_global |= 1u << index;
    *handle = (uintptr_t)scope->references; return R2_OK;
}
static r2_status framework_symbol(r2_native_runtime *runtime, const native_framework_scope *scope,
                                   const char *symbol, uint64_t *address, char *error, size_t error_len)
{
    *address = 0; void *function = dlsym(*scope->library, symbol + 1);
    if(!function&&strcmp(scope->name,"objc.A")==0&&(strcmp(symbol+1,"objc_msgSend_stret")==0||strcmp(symbol+1,"objc_msgSend_fpret")==0))
        return r2_native_symbol(runtime,symbol,address,error,error_len);
    if (!function) {
        snprintf(runtime->loader_error, sizeof(runtime->loader_error), "%s symbol not found: %.200s", scope->name, symbol + 1);
        runtime->loader_error_pending = true; return R2_OK;
    }
    runtime->override_name = symbol + 1; runtime->override_function = function;
    r2_status status = r2_native_symbol(runtime, symbol, address, error, error_len);
    runtime->override_name = NULL; runtime->override_function = NULL; return status;
}
static bool remember_native_port(r2_native_runtime *runtime, mach_port_t port)
{
    if (port == MACH_PORT_NULL) return true;
    size_t empty = SIZE_MAX;
    for (size_t i = 0; i < 1024; i++) {
        if (runtime->ports[i].references && runtime->ports[i].port == port) { runtime->ports[i].references++; return true; }
        if (!runtime->ports[i].references && empty == SIZE_MAX) empty = i;
    }
    if (empty == SIZE_MAX) { mach_port_deallocate(mach_task_self(), port); return false; }
    runtime->ports[empty].port = port; runtime->ports[empty].references = 1; return true;
}
