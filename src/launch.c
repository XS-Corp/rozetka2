#include "internal.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static bool has_suffix(const char *value, const char *suffix)
{
    size_t value_len = strlen(value);
    size_t suffix_len = strlen(suffix);
    return value_len >= suffix_len &&
           strcmp(value + value_len - suffix_len, suffix) == 0;
}

static const char *last_path_component(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash == NULL ? path : slash + 1;
}

static bool read_text_file(const char *path, char **out, char *error, size_t error_len)
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
    if (size < 0 || size > 1024 * 1024) {
        r2_set_error(error, error_len, "Info.plist is missing or too large");
        fclose(file);
        return false;
    }

    if (fseek(file, 0, SEEK_SET) != 0) {
        r2_set_error(error, error_len, "rewind %s failed: %s", path, strerror(errno));
        fclose(file);
        return false;
    }

    char *data = calloc(1, (size_t)size + 1);
    if (data == NULL) {
        r2_set_error(error, error_len, "out of memory reading %s", path);
        fclose(file);
        return false;
    }

    if (size > 0 && fread(data, 1, (size_t)size, file) != (size_t)size) {
        r2_set_error(error, error_len, "read %s failed", path);
        free(data);
        fclose(file);
        return false;
    }

    fclose(file);
    *out = data;
    return true;
}

static bool parse_cf_bundle_executable(const char *plist, char *name, size_t name_len)
{
    const char *key = strstr(plist, "<key>CFBundleExecutable</key>");
    if (key == NULL) {
        return false;
    }

    const char *start = strstr(key, "<string>");
    if (start == NULL) {
        return false;
    }
    start += strlen("<string>");

    const char *end = strstr(start, "</string>");
    if (end == NULL || end <= start) {
        return false;
    }

    size_t len = (size_t)(end - start);
    if (len >= name_len) {
        return false;
    }

    memcpy(name, start, len);
    name[len] = '\0';
    return true;
}

static bool fallback_bundle_executable_name(const char *bundle_path, char *name, size_t name_len)
{
    const char *base = last_path_component(bundle_path);
    size_t len = strlen(base);
    if (len > 4 && strcmp(base + len - 4, ".app") == 0) {
        len -= 4;
    }
    if (len == 0 || len >= name_len) {
        return false;
    }

    memcpy(name, base, len);
    name[len] = '\0';
    return true;
}

static r2_status resolve_app_executable(const char *bundle_path,
                                        char *out,
                                        size_t out_len,
                                        char *error,
                                        size_t error_len)
{
    char plist_path[PATH_MAX];
    int wrote = snprintf(plist_path, sizeof(plist_path), "%s/Contents/Info.plist", bundle_path);
    if (wrote < 0 || (size_t)wrote >= sizeof(plist_path)) {
        r2_set_error(error, error_len, "bundle path is too long");
        return R2_ERR_BAD_ARGUMENT;
    }

    char exe_name[PATH_MAX] = {0};
    char *plist = NULL;
    if (read_text_file(plist_path, &plist, error, error_len)) {
        (void)parse_cf_bundle_executable(plist, exe_name, sizeof(exe_name));
        free(plist);
    }

    if (exe_name[0] == '\0' &&
        !fallback_bundle_executable_name(bundle_path, exe_name, sizeof(exe_name))) {
        r2_set_error(error, error_len, "could not resolve bundle executable name");
        return R2_ERR_DECODE;
    }

    wrote = snprintf(out, out_len, "%s/Contents/MacOS/%s", bundle_path, exe_name);
    if (wrote < 0 || (size_t)wrote >= out_len) {
        r2_set_error(error, error_len, "bundle executable path is too long");
        return R2_ERR_BAD_ARGUMENT;
    }

    return R2_OK;
}

r2_status r2_launch_path(const char *path,
                         const char *const *guest_argv,
                         size_t guest_argc,
                         char *error,
                         size_t error_len)
{
    if (path == NULL) {
        r2_set_error(error, error_len, "null path");
        return R2_ERR_BAD_ARGUMENT;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        r2_set_error(error, error_len, "stat %s failed: %s", path, strerror(errno));
        return R2_ERR_IO;
    }

    char target[PATH_MAX];
    if (S_ISDIR(st.st_mode)) {
        if (!has_suffix(path, ".app")) {
            r2_set_error(error, error_len, "directory launch expects a .app bundle");
            return R2_ERR_BAD_ARGUMENT;
        }

        r2_status status = resolve_app_executable(path, target, sizeof(target), error, error_len);
        if (status != R2_OK) {
            return status;
        }
        printf("Bundle: %s\n", path);
        printf("Executable: %s\n", target);
    } else {
        int wrote = snprintf(target, sizeof(target), "%s", path);
        if (wrote < 0 || (size_t)wrote >= sizeof(target)) {
            r2_set_error(error, error_len, "path is too long");
            return R2_ERR_BAD_ARGUMENT;
        }
        printf("Executable: %s\n", target);
    }

    r2_status status = r2_inspect_macho_file(target, error, error_len);
    if (status != R2_OK) {
        return status;
    }

    r2_result result = {0};
    status = r2_run_macho_file(target, guest_argv, guest_argc, &result, error, error_len);
    if (status != R2_OK) {
        return status;
    }

    printf("launched simple x86_64 Mach-O via interpreter\n");
    printf("rax = %" PRIu64 " (0x%016" PRIx64 ")\n", result.rax, result.rax);
    return R2_OK;
}
