/* Exercise the official x86 LWJGL shader libraries through their public C API.
 * Declarations follow shaderc/shaderc.h and spirv_cross_c.h. */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void require(int ok, const char *message)
{
    if (!ok) { fprintf(stderr, "FAIL guest shaders: %s (%s)\n", message, dlerror()); exit(91); }
}
#define LOAD(target, library, name) do { *(void **)(&(target)) = dlsym(library, name); require((target) != NULL, name); } while (0)
int main(int argc, char **argv)
{
    require(argc == 3, "shaderc and SPIRV-Cross library paths");
    void *shaderc = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    require(shaderc != NULL, "load guest Shaderc");
    void *(*initialize)(void), *(*compile)(void *, const char *, size_t, int, const char *, const char *, const void *);
    void (*release)(void *), (*result_release)(void *);
    int (*status)(void *); size_t (*length)(void *); const char *(*bytes)(void *), *(*message)(void *);
    LOAD(initialize, shaderc, "shaderc_compiler_initialize"); LOAD(release, shaderc, "shaderc_compiler_release");
    LOAD(compile, shaderc, "shaderc_compile_into_spv"); LOAD(status, shaderc, "shaderc_result_get_compilation_status");
    LOAD(length, shaderc, "shaderc_result_get_length"); LOAD(bytes, shaderc, "shaderc_result_get_bytes");
    LOAD(message, shaderc, "shaderc_result_get_error_message"); LOAD(result_release, shaderc, "shaderc_result_release");
    void *compiler = initialize(); require(compiler != NULL, "initialize Shaderc");
    const char source[] = "#version 450\nvoid main() { gl_Position = vec4(0.0, 0.0, 0.0, 1.0); }\n";
    void *result = compile(compiler, source, strlen(source), 0, "probe.vert", "main", NULL);
    require(result != NULL, "compile vertex shader");
    if (status(result)) fprintf(stderr, "Shaderc: %s\n", message(result));
    size_t size = length(result); const uint32_t *spirv = (const uint32_t *)bytes(result);
    require(status(result) == 0 && size >= 20 && size % 4 == 0 && spirv[0] == UINT32_C(0x07230203), "valid compiled SPIR-V");
    printf("PASS guest Shaderc: vertex shader -> %zu SPIR-V bytes\n", size); fflush(stdout);
    void *cross = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL); require(cross != NULL, "load guest SPIRV-Cross");
    int (*create)(void **), (*parse)(void *, const uint32_t *, size_t, void **);
    int (*create_compiler)(void *, int, void *, int, void **), (*cross_compile)(void *, const char **);
    void (*destroy)(void *);
    LOAD(create, cross, "spvc_context_create"); LOAD(destroy, cross, "spvc_context_destroy");
    LOAD(parse, cross, "spvc_context_parse_spirv"); LOAD(create_compiler, cross, "spvc_context_create_compiler");
    LOAD(cross_compile, cross, "spvc_compiler_compile");
    void *context = NULL, *ir = NULL, *translator = NULL; const char *output = NULL;
    require(create(&context) == 0 && context != NULL, "create SPIRV-Cross context");
    require(parse(context, spirv, size / 4, &ir) == 0 && ir != NULL, "parse compiled shader");
    require(create_compiler(context, 1, ir, 1, &translator) == 0 && translator != NULL, "create GLSL compiler");
    require(cross_compile(translator, &output) == 0 && output != NULL && strstr(output, "gl_Position") && strstr(output, "void main"), "SPIR-V back to GLSL");
    puts("PASS guest SPIRV-Cross: compiled SPIR-V -> GLSL");
    destroy(context); result_release(result); release(compiler);
    require(dlclose(cross) == 0 && dlclose(shaderc) == 0, "close shader libraries");
    return 0;
}
