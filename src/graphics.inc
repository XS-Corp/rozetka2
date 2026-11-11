/* Typed bridges to the configured native macOS window/GPU backends.
 * Java, HotSpot and LWJGL JNI code remain guest x86. Opaque native driver
 * objects are data; every returned function address becomes a guest stub. */
static const r2_graphics_abi *graphics_abi(int kind, const char *name)
{
    const r2_graphics_abi *table;
    size_t count;
    if (kind == GRAPHICS_VULKAN) { table = vulkan_abis; count = sizeof(vulkan_abis) / sizeof(vulkan_abis[0]); }
    else if (kind == GRAPHICS_SDL3) { table = sdl3_abis; count = sizeof(sdl3_abis) / sizeof(sdl3_abis[0]); }
    else if (kind == GRAPHICS_OPENGL) { table = opengl_abis; count = sizeof(opengl_abis) / sizeof(opengl_abis[0]); }
    else return NULL;
    size_t low = 0, high = count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        int order = strcmp(name, table[middle].definition.name);
        if (order == 0) return &table[middle];
        if (order < 0) high = middle; else low = middle + 1;
    }
    return NULL;
}
static graphics_module *graphics_handle(r2_native_runtime *runtime, uint64_t handle)
{
    for (size_t i = 0; i < GRAPHICS_MODULE_COUNT; i++)
        if (handle == (uintptr_t)&runtime->graphics[i] && runtime->graphics[i].references) return &runtime->graphics[i];
    return NULL;
}
static graphics_module *graphics_path(r2_native_runtime *runtime, const char *path)
{
    const char *name = strrchr(path, '/'); name = name ? name + 1 : path;
    int kind = strcmp(name, "libMoltenVK.dylib") == 0 || strcmp(name, "libvulkan.1.dylib") == 0 || strcmp(name, "libvulkan.dylib") == 0 ? GRAPHICS_VULKAN :
        strcmp(name, "libSDL3.dylib") == 0 || strcmp(name, "libSDL3.0.dylib") == 0 ? GRAPHICS_SDL3 :
        strcmp(path, "/System/Library/Frameworks/OpenGL.framework/OpenGL") == 0 ||
        strcmp(path, "/System/Library/Frameworks/OpenGL.framework/Versions/A/OpenGL") == 0 ||
        strcmp(path, "/System/Library/Frameworks/OpenGL.framework/Versions/Current/OpenGL") == 0 ? GRAPHICS_OPENGL : 0;
    for (size_t i = 0; i < GRAPHICS_MODULE_COUNT; i++) if (kind && runtime->graphics[i].kind == kind && runtime->graphics[i].path) return &runtime->graphics[i];
    return NULL;
}
static r2_status graphics_open(graphics_module *module, int flags, uint64_t *handle, char *error, size_t error_len)
{
    int supported = RTLD_LAZY | RTLD_NOW | RTLD_LOCAL | RTLD_GLOBAL | RTLD_NODELETE | RTLD_NOLOAD | RTLD_FIRST;
    if (flags & ~supported) { r2_set_error(error, error_len, "unsupported native graphics dlopen flags"); return R2_ERR_UNSUPPORTED; }
    *handle = 0;
    if (!module->library) {
        if (flags & RTLD_NOLOAD) { r2_set_error(error, error_len, "native graphics library is not loaded"); return R2_ERR_IO; }
        /* Metal completion handlers can outlive a failed guest launch. Keep
         * their native code pinned even after the guest handle is closed. */
        module->library = dlopen(module->path, RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);
        if (!module->library) { r2_set_error(error, error_len, "native graphics library %s: %s", module->path, dlerror()); return R2_ERR_IO; }
    }
    module->references++; module->global |= (flags & RTLD_LOCAL) == 0 || (flags & RTLD_GLOBAL) != 0;
    *handle = (uintptr_t)module; return R2_OK;
}
static void graphics_destroy(r2_native_runtime *runtime)
{
    graphics_module *sdl = &runtime->graphics[0];
    if (sdl->library) { void (*quit)(void) = dlsym(sdl->library, "SDL_Quit"); if (quit) quit(); }
    for (size_t i = 0; i < GRAPHICS_MODULE_COUNT; i++) {
        if (runtime->graphics[i].library) dlclose(runtime->graphics[i].library);
        free(runtime->graphics[i].path);
    }
}
static r2_status graphics_stub(r2_native_runtime *runtime, int kind, const char *name, void *function,
                                uint64_t *address, char *error, size_t error_len)
{
    const r2_graphics_abi *abi = graphics_abi(kind, name);
    if (abi == NULL) { r2_set_error(error, error_len, "graphics bridge has no ABI declaration for %s", name); return R2_ERR_UNSUPPORTED; }
    char symbol[256];
    if (strlen(name) + 2 > sizeof(symbol)) return R2_ERR_UNSUPPORTED;
    snprintf(symbol, sizeof(symbol), "_%s", name);
    runtime->override_name = name; runtime->override_function = function;
    runtime->override_definition = &abi->definition; runtime->override_graphics_kind = kind;
    r2_status status = r2_native_symbol(runtime, symbol, address, error, error_len);
    runtime->override_name = NULL; runtime->override_function = NULL;
    runtime->override_definition = NULL; runtime->override_graphics_kind = 0;
    return status;
}
static r2_status graphics_lookup(r2_native_runtime *runtime, graphics_module *module, const char *name,
                                  uint64_t *address, char *error, size_t error_len)
{
    void *function = dlsym(module->library, name);
    if (function == NULL) { *address = 0; return R2_OK; }
    return graphics_stub(runtime, module->kind, name, function, address, error, error_len);
}
static bool graphics_string(r2_native_runtime *runtime, uint64_t address, char value[256], char *error, size_t error_len)
{
    for (size_t i = 0; i < 256; i++) {
        const char *byte = r2_native_pointer(runtime, address + i, 1, false, error, error_len);
        if (!byte) return false;
        if ((value[i] = *byte) == 0) return true;
    }
    r2_set_error(error, error_len, "graphics procedure name is too long"); return false;
}
static r2_status sdl_native_log_stub(r2_native_runtime *runtime,native_sdl_log function,uint64_t *address,char *error,size_t error_len)
{
    static const native_definition definition={"__r2_native_sdl_log","piip",'v',-1};
    if(!function) {*address=0;return R2_OK;}
    runtime->override_name=definition.name;runtime->override_definition=&definition;runtime->override_function=(void *)function;
    r2_status status=r2_native_symbol(runtime,"___r2_native_sdl_log",address,error,error_len);
    runtime->override_name=NULL;runtime->override_definition=NULL;runtime->override_function=NULL;return status;
}
static r2_status graphics_special(r2_native_runtime *runtime, native_import *import, uint64_t regs[16],
                                   bool *handled, char *error, size_t error_len)
{
    const char *name = import->definition->name; *handled = true;
    if(import->graphics_kind==GRAPHICS_VULKAN) {
        r2_status status=vulkan_special(runtime,import,regs,handled,error,error_len);
        if(*handled||status!=R2_OK)return status;
        *handled=true;
    }
    if(import->graphics_kind==GRAPHICS_SDL3&&strcmp(name,"SDL_SetLogOutputFunction")==0) {
        void (*get)(native_sdl_log *,void **)=dlsym(runtime->graphics[0].library,"SDL_GetLogOutputFunction");
        void (*set)(native_sdl_log,void *)=import->function;
        if(!runtime->sdl_log_saved) {
            native_sdl_log original;get(&original,&runtime->sdl_original_log_argument);
            runtime->sdl_original_log=(void *)original;runtime->sdl_log_saved=true;
        }
        native_callback *callback=NULL;
        if(regs[R2_X86_RDI]) {
            callback=callback_create(runtime,regs[R2_X86_RDI],regs[R2_X86_RSI],error,error_len);
            if(!callback)return R2_ERR_UNSUPPORTED;
        }
        native_callback *old=runtime->sdl_log_callback;
        if(old)atomic_store_explicit(&old->disabled,true,memory_order_release);
        runtime->sdl_log_callback=callback;
        unsigned depth=engine_suspend(runtime);set(callback?sdl_log_proxy:NULL,callback);engine_resume(runtime,depth);
        regs[R2_X86_RAX]=0;return R2_OK;
    }
    if(import->graphics_kind==GRAPHICS_SDL3&&strcmp(name,"SDL_GetLogOutputFunction")==0) {
        uint64_t *function=r2_native_pointer(runtime,regs[R2_X86_RDI],8,true,error,error_len);
        uint64_t *argument=r2_native_pointer(runtime,regs[R2_X86_RSI],8,true,error,error_len);
        if(!function||!argument)return R2_ERR_UNSUPPORTED;
        void (*get)(native_sdl_log *,void **)=import->function;native_sdl_log host_function;void *host_argument;
        unsigned depth=engine_suspend(runtime);get(&host_function,&host_argument);engine_resume(runtime,depth);
        if(host_function==sdl_log_proxy&&host_argument==runtime->sdl_log_callback&&runtime->sdl_log_callback) {
            *function=runtime->sdl_log_callback->function;*argument=runtime->sdl_log_callback->argument;return R2_OK;
        }
        r2_status status=sdl_native_log_stub(runtime,host_function,function,error,error_len);*argument=(uintptr_t)host_argument;return status;
    }
    if(import->graphics_kind==GRAPHICS_SDL3&&strcmp(name,"SDL_GetDefaultLogOutputFunction")==0) {
        native_sdl_log (*get)(void)=import->function;
        return sdl_native_log_stub(runtime,get(),&regs[R2_X86_RAX],error,error_len);
    }
    if (strcmp(name, "vkGetInstanceProcAddr") == 0 || strcmp(name, "vkGetDeviceProcAddr") == 0) {
        char procedure[256];
        if (!graphics_string(runtime, regs[R2_X86_RSI], procedure, error, error_len)) return R2_ERR_UNSUPPORTED;
        void *(*get)(void *, const char *) = import->function;
        void *function = get((void *)(uintptr_t)regs[R2_X86_RDI], procedure);
        if (!function) { regs[R2_X86_RAX] = 0; return R2_OK; }
        return graphics_stub(runtime, GRAPHICS_VULKAN, procedure, function, &regs[R2_X86_RAX], error, error_len);
    }
    if (strcmp(name, "SDL_Vulkan_GetVkGetInstanceProcAddr") == 0) {
        void *(*get)(void) = import->function; void *function = get();
        if (!function) { regs[R2_X86_RAX] = 0; return R2_OK; }
        return graphics_stub(runtime, GRAPHICS_VULKAN, "vkGetInstanceProcAddr", function, &regs[R2_X86_RAX], error, error_len);
    }
    if (strcmp(name, "SDL_GetFunctionPointer") == 0 || strcmp(name, "SDL_GL_GetProcAddress") == 0 || strcmp(name, "SDL_EGL_GetProcAddress") == 0) {
        bool object = strcmp(name, "SDL_GetFunctionPointer") == 0; char procedure[256];
        if (!graphics_string(runtime, regs[object ? R2_X86_RSI : R2_X86_RDI], procedure, error, error_len)) return R2_ERR_UNSUPPORTED;
        void *function;
        if (object) { void *(*get)(void *, const char *) = import->function; function = get((void *)(uintptr_t)regs[R2_X86_RDI], procedure); }
        else { void *(*get)(const char *) = import->function; function = get(procedure); }
        if (!function) { regs[R2_X86_RAX] = 0; return R2_OK; }
        int kind = strncmp(procedure, "vk", 2) == 0 ? GRAPHICS_VULKAN :
            strncmp(procedure, "gl", 2) == 0 || strncmp(procedure, "CGL", 3) == 0 ? GRAPHICS_OPENGL : GRAPHICS_SDL3;
        return graphics_stub(runtime, kind, procedure, function, &regs[R2_X86_RAX], error, error_len);
    }
    *handled = false; return R2_OK;
}
static r2_status graphics_prepare(r2_native_runtime *runtime, native_import *import, uint64_t *values,
                                   char *error, size_t error_len)
{
    const r2_graphics_abi *abi = graphics_abi(import->graphics_kind, import->definition->name);
    if (!abi) return R2_ERR_UNSUPPORTED;
    unsigned count = (unsigned)strlen(abi->definition.arguments);
    for (unsigned i = 0; i < count; i++) {
        if (((abi->callbacks >> i) & 1) && values[i]) {
            r2_set_error(error, error_len, "guest graphics callback is unsupported in %s argument %u", abi->definition.name, i); return R2_ERR_UNSUPPORTED;
        }
        if ((abi->chains >> i) & 1) {
            uint64_t node = values[i]; unsigned depth = 0;
            while (node) {
                if (depth++ == 64) { r2_set_error(error, error_len, "invalid Vulkan pNext chain"); return R2_ERR_UNSUPPORTED; }
                const uint8_t *header = r2_native_pointer(runtime, node, 16, false, error, error_len);
                if (!header) return R2_ERR_UNSUPPORTED;
                uint32_t type; uint64_t next; memcpy(&type, header, 4); memcpy(&next, header + 8, 8);
                for (size_t j = 0; j < sizeof(vulkan_callback_structures) / sizeof(vulkan_callback_structures[0]); j++) {
                    if (type == vulkan_callback_structures[j]) {
                        r2_set_error(error, error_len, "guest Vulkan callback structure %u is unsupported", type); return R2_ERR_UNSUPPORTED;
                    }
                }
                node = next;
            }
        }
    }
    if (import->graphics_kind == GRAPHICS_SDL3 && strcmp(import->definition->name, "SDL_Vulkan_CreateSurface") == 0 && values[2]) {
        r2_set_error(error, error_len, "guest Vulkan allocator callback is unsupported in SDL surface creation"); return R2_ERR_UNSUPPORTED;
    }
    if (import->graphics_kind == GRAPHICS_SDL3 && strcmp(import->definition->name, "SDL_Vulkan_LoadLibrary") == 0 && runtime->graphics[1].path)
        values[0] = (uintptr_t)runtime->graphics[1].path;
    return R2_OK;
}
