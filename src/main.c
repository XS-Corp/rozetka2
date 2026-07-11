#include "internal.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *data;
    size_t len;
} file_bytes;

static void usage(FILE *stream)
{
    fprintf(stream,
            "rozetka2\n"
            "\n"
            "Usage:\n"
            "  rozetka2 run [--engine interp|jit] <raw-x86_64-function.bin>\n"
            "  rozetka2 inspect <mach-o-file>\n"
            "  rozetka2 launch <mach-o-file-or-app-bundle> [-- guest-arg ...]\n"
            "\n"
            "The run command executes a supported raw x86_64 function blob ending in ret.\n"
            "The launch command resolves app targets and runs supported simple x86_64 Mach-O files.\n");
}

static bool read_file(const char *path, file_bytes *out, char *error, size_t error_len)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        r2_set_error(error, error_len, "open %s failed: %s", path, strerror(errno));
        return false;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        r2_set_error(error, error_len, "seek %s failed: %s", path, strerror(errno));
        fclose(file);
        return false;
    }

    long size = ftell(file);
    if (size < 0) {
        r2_set_error(error, error_len, "tell %s failed: %s", path, strerror(errno));
        fclose(file);
        return false;
    }

    if (fseek(file, 0, SEEK_SET) != 0) {
        r2_set_error(error, error_len, "rewind %s failed: %s", path, strerror(errno));
        fclose(file);
        return false;
    }

    uint8_t *data = NULL;
    if (size > 0) {
        data = malloc((size_t)size);
        if (data == NULL) {
            r2_set_error(error, error_len, "out of memory reading %s", path);
            fclose(file);
            return false;
        }

        size_t got = fread(data, 1, (size_t)size, file);
        if (got != (size_t)size) {
            r2_set_error(error, error_len, "read %s failed", path);
            free(data);
            fclose(file);
            return false;
        }
    }

    fclose(file);
    out->data = data;
    out->len = (size_t)size;
    return true;
}

r2_status r2_execute(const uint8_t *code,
                     size_t code_len,
                     r2_engine engine,
                     r2_result *result,
                     char *error,
                     size_t error_len)
{
    if (code == NULL || result == NULL) {
        r2_set_error(error, error_len, "null argument");
        return R2_ERR_BAD_ARGUMENT;
    }

    r2_block block;
    r2_status status = r2_decode_block(code, code_len, &block, error, error_len);
    if (status != R2_OK) {
        return status;
    }

    if (engine == R2_ENGINE_INTERP) {
        return r2_interp_block(&block, result, error, error_len);
    }
    if (engine == R2_ENGINE_JIT) {
        return r2_jit_block(&block, result, error, error_len);
    }

    r2_set_error(error, error_len, "unknown engine");
    return R2_ERR_BAD_ARGUMENT;
}

static bool parse_engine(const char *raw, r2_engine *engine)
{
    if (strcmp(raw, "interp") == 0) {
        *engine = R2_ENGINE_INTERP;
        return true;
    }
    if (strcmp(raw, "jit") == 0) {
        *engine = R2_ENGINE_JIT;
        return true;
    }
    return false;
}

static int run_command(int argc, char **argv)
{
    r2_engine engine = R2_ENGINE_JIT;
    const char *path = NULL;

    for (int i = 2; i < argc; i += 1) {
        if (strcmp(argv[i], "--engine") == 0) {
            if (i + 1 >= argc || !parse_engine(argv[i + 1], &engine)) {
                fprintf(stderr, "invalid --engine value\n");
                return 2;
            }
            i += 1;
        } else if (path == NULL) {
            path = argv[i];
        } else {
            fprintf(stderr, "unexpected argument: %s\n", argv[i]);
            return 2;
        }
    }

    if (path == NULL) {
        usage(stderr);
        return 2;
    }

    char error[R2_ERROR_CAP] = {0};
    file_bytes bytes = {0};
    if (!read_file(path, &bytes, error, sizeof(error))) {
        fprintf(stderr, "rozetka2: %s\n", error);
        return 1;
    }

    r2_result result = {0};
    r2_status status = r2_execute(bytes.data, bytes.len, engine, &result, error, sizeof(error));
    free(bytes.data);

    if (status != R2_OK) {
        fprintf(stderr, "rozetka2: %s: %s\n", r2_status_name(status), error);
        return 1;
    }

    printf("rax = %" PRIu64 " (0x%016" PRIx64 ")\n", result.rax, result.rax);
    return 0;
}

static int inspect_command(int argc, char **argv)
{
    if (argc != 3) {
        usage(stderr);
        return 2;
    }

    char error[R2_ERROR_CAP] = {0};
    r2_status status = r2_inspect_macho_file(argv[2], error, sizeof(error));
    if (status != R2_OK) {
        fprintf(stderr, "rozetka2: %s: %s\n", r2_status_name(status), error);
        return 1;
    }

    return 0;
}

static int launch_command(int argc, char **argv)
{
    if (argc < 3) {
        usage(stderr);
        return 2;
    }

    const char *path = argv[2];
    int guest_start = argc;
    if (argc > 3) {
        if (strcmp(argv[3], "--") != 0) {
            fprintf(stderr, "launch guest arguments must follow --\n");
            return 2;
        }
        guest_start = 4;
    }

    size_t extra_argc = (size_t)(argc - guest_start);
    const char **guest_argv = calloc(extra_argc + 1u, sizeof(*guest_argv));
    if (guest_argv == NULL) {
        fprintf(stderr, "out of memory building guest argv\n");
        return 1;
    }
    guest_argv[0] = path;
    for (size_t i = 0; i < extra_argc; i += 1) {
        guest_argv[i + 1u] = argv[guest_start + (int)i];
    }

    char error[R2_ERROR_CAP] = {0};
    r2_status status =
        r2_launch_path(path, guest_argv, extra_argc + 1u, error, sizeof(error));
    free(guest_argv);
    if (status != R2_OK) {
        fprintf(stderr, "rozetka2: %s: %s\n", r2_status_name(status), error);
        return 1;
    }

    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(stderr);
        return 2;
    }

    if (strcmp(argv[1], "run") == 0) {
        return run_command(argc, argv);
    }
    if (strcmp(argv[1], "inspect") == 0) {
        return inspect_command(argc, argv);
    }
    if (strcmp(argv[1], "launch") == 0) {
        return launch_command(argc, argv);
    }
    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        usage(stdout);
        return 0;
    }

    fprintf(stderr, "unknown command: %s\n", argv[1]);
    usage(stderr);
    return 2;
}
