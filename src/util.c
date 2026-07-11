#include "internal.h"

#include <stdarg.h>
#include <stdio.h>

void r2_set_error(char *error, size_t error_len, const char *fmt, ...)
{
    if (error == NULL || error_len == 0) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(error, error_len, fmt, args);
    va_end(args);
    error[error_len - 1] = '\0';
}

const char *r2_status_name(r2_status status)
{
    switch (status) {
    case R2_OK:
        return "ok";
    case R2_ERR_BAD_ARGUMENT:
        return "bad argument";
    case R2_ERR_DECODE:
        return "decode error";
    case R2_ERR_UNSUPPORTED:
        return "unsupported";
    case R2_ERR_NO_MEMORY:
        return "out of memory";
    case R2_ERR_JIT:
        return "jit error";
    case R2_ERR_IO:
        return "io error";
    default:
        return "unknown";
    }
}

const char *r2_reg_name(uint8_t reg)
{
    static const char *names[16] = {
        "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
        "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15",
    };

    if (reg < 16) {
        return names[reg];
    }
    return "?";
}
