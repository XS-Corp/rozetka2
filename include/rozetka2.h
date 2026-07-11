#ifndef ROZETKA2_H
#define ROZETKA2_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define R2_ERROR_CAP 256

typedef enum {
    R2_OK = 0,
    R2_ERR_BAD_ARGUMENT,
    R2_ERR_DECODE,
    R2_ERR_UNSUPPORTED,
    R2_ERR_NO_MEMORY,
    R2_ERR_JIT,
    R2_ERR_IO
} r2_status;

typedef enum {
    R2_ENGINE_INTERP = 0,
    R2_ENGINE_JIT = 1
} r2_engine;

typedef struct {
    uint64_t rax;
} r2_result;

r2_status r2_execute(const uint8_t *code,
                     size_t code_len,
                     r2_engine engine,
                     r2_result *result,
                     char *error,
                     size_t error_len);

const char *r2_status_name(r2_status status);

#ifdef __cplusplus
}
#endif

#endif
