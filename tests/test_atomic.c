#include "internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
static void check(bool ok, const char *message)
{
    if (!ok) { fprintf(stderr, "FAIL atomic: %s\n", message); exit(1); }
    checks++;
}

static uint64_t partial(uint64_t original, uint64_t value, unsigned width)
{
    uint64_t mask = width == 8 ? UINT64_MAX : (UINT64_C(1) << (8 * width)) - 1;
    return (value & mask) | (width < 4 ? original & ~mask : 0);
}

int main(void)
{
    r2_block *block = malloc(sizeof(*block));
    check(block != NULL, "allocate block");
    const uint64_t values[] = {0, 1, UINT64_MAX, UINT64_C(0x8000000080008080)};
    for (unsigned width = 1; width <= 8; width *= 2)
    for (unsigned op = 0; op < 3; op++)
    for (unsigned memory = 0; memory < 2; memory++)
    for (unsigned mismatch = 0; mismatch < 2; mismatch++)
    for (unsigned v = 0; v < 4; v++) {
        uint64_t mask = width == 8 ? UINT64_MAX : (UINT64_C(1) << (width * 8)) - 1;
        uint64_t old = values[v] & mask, source = (values[(v + 1) % 4]) & mask;
        uint64_t accumulator = old ^ mismatch;
        uint64_t data = values[v], snapshot[4] = {0};
        uint8_t code[64] = {0x83, 0xfb, 0x01}; /* cmp ebx,1: CF=SF=PF=1 */
        size_t n = 3;
        if (memory && op != 0) code[n++] = 0xf0;
        if (width == 2) code[n++] = 0x66;
        if (width == 8) code[n++] = 0x48;
        if (op != 0) code[n++] = 0x0f;
        code[n++] = (op == 0 ? 0x86 : op == 1 ? 0xb0 : 0xc0) + (width != 1);
        code[n++] = memory ? 0x0f : 0xca; /* rcx -> [rdi] or rdx */
        const uint8_t capture[] = {
            0x48,0x89,0x06, 0x48,0x89,0x4e,0x08, 0x48,0x89,0x56,0x10,
            0x0f,0x92,0x46,0x18, 0x0f,0x94,0x46,0x19, 0x0f,0x98,0x46,0x1a,
            0x0f,0x90,0x46,0x1b, 0x0f,0x9a,0x46,0x1c, 0xc3
        };
        memcpy(code + n, capture, sizeof(capture)); n += sizeof(capture);
        r2_memory_region regions[] = {
            {.base=0x10000, .size=sizeof(data), .data=(uint8_t *)&data, .writable=true},
            {.base=0x20000, .size=sizeof(snapshot), .data=(uint8_t *)snapshot, .writable=true},
        };
        r2_interp_env env = {.regions=regions, .region_count=2, .has_initial_regs=true};
        env.initial_regs[R2_X86_RAX] = accumulator;
        env.initial_regs[R2_X86_RCX] = values[(v + 1) % 4];
        env.initial_regs[R2_X86_RDX] = values[v];
        env.initial_regs[R2_X86_RDI] = 0x10000;
        env.initial_regs[R2_X86_RSI] = 0x20000;
        char error[R2_ERROR_CAP] = {0}; r2_result result = {0};
        check(r2_decode_block(code, n, block, error, sizeof(error)) == R2_OK, error);
        check(r2_interp_block_with_env(block, &env, &result, error, sizeof(error)) == R2_OK, error);
        uint64_t value = op == 2 ? (old + source) & mask : source;
        if (op == 1 && mismatch) value = old;
        uint64_t expected_dst = memory ? (values[v] & ~mask) | value
                                      : op == 1 && mismatch ? values[v] : partial(values[v], value, width);
        check((memory ? data : snapshot[2]) == expected_dst, "destination");
        check(snapshot[1] == (op == 1 ? values[(v + 1) % 4] : partial(values[(v + 1) % 4], old, width)), "source");
        check(snapshot[0] == (op == 1 && mismatch ? partial(accumulator, old, width) : accumulator), "accumulator");
        uint8_t *flags = (uint8_t *)snapshot + 24;
        if (op == 0) check(flags[0] == 1 && flags[1] == 0 && flags[2] == 1 && flags[3] == 0 && flags[4] == 1, "xchg preserves flags");
        else {
            uint64_t lhs = op == 1 ? accumulator : old, rhs = op == 1 ? old : source;
            uint64_t answer = (op == 1 ? lhs - rhs : lhs + rhs) & mask;
            uint64_t sign = UINT64_C(1) << (width * 8 - 1);
            bool carry = op == 1 ? lhs < rhs : (__uint128_t)lhs + rhs > mask;
            bool overflow = ((op == 1 ? (lhs ^ rhs) : ~(lhs ^ rhs)) & (lhs ^ answer) & sign) != 0;
            check(flags[0] == carry && flags[1] == (answer == 0) && flags[2] == !!(answer & sign) &&
                  flags[3] == overflow && flags[4] == ((__builtin_popcount((unsigned)(answer & 255)) & 1) == 0), "arithmetic flags");
        }
    }
    const struct { uint8_t code[12]; size_t size; uint64_t expected; } special[] = {
        {{0xb8,0x12,0x34,0,0,0x86,0xc4,0xc3},8,0x1234}, /* xchg al,ah */
        {{0xb8,1,0,0,0,0x0f,0xc0,0xc0,0xc3},9,2}, /* xadd al,al */
        {{0x41,0xb8,42,0,0,0,0x49,0x90,0xc3},9,42}, /* xchg rax,r8 */
        {{0xb9,42,0,0,0,0x91,0xc3},7,42},
    };
    for (size_t i = 0; i < sizeof(special)/sizeof(*special); i++) {
        char error[R2_ERROR_CAP] = {0}; r2_result result = {0};
        check(r2_decode_block(special[i].code, special[i].size, block, error, sizeof(error)) == R2_OK, error);
        check(r2_interp_block(block, &result, error, sizeof(error)) == R2_OK, error);
        check(result.rax == special[i].expected, "alias or accumulator encoding");
    }
    const uint8_t invalid[][5] = {{0xf0,0x87,0xc8,0xc3}, {0xf0,0x90,0xc3}, {0xf0,0x89,0x08,0xc3}};
    for (size_t i = 0; i < sizeof(invalid)/sizeof(*invalid); i++) {
        char error[R2_ERROR_CAP] = {0};
        check(r2_decode_block(invalid[i], sizeof(invalid[i]), block, error, sizeof(error)) == R2_ERR_UNSUPPORTED,
              "reject invalid or unimplemented LOCK");
    }
    for (unsigned op = 0; op < 3; op++)
    for (unsigned failure = 0; failure < 3; failure++) {
        uint64_t data[2] = {1, 2};
        uint8_t code[8] = {0x48}; size_t n = 1;
        if (op != 0) code[n++] = 0x0f;
        code[n++] = op == 0 ? 0x87 : op == 1 ? 0xb1 : 0xc1;
        code[n++] = 0x0f; code[n++] = 0xc3;
        r2_memory_region region = {.base=0x10000, .size=sizeof(data), .data=(uint8_t *)data, .writable=failure != 0};
        r2_interp_env env = {.regions=&region, .region_count=1, .has_initial_regs=true};
        env.initial_regs[R2_X86_RDI] = failure == 1 ? 0x10001 : failure == 2 ? 0x20000 : 0x10000;
        env.initial_regs[R2_X86_RCX] = 42;
        char error[R2_ERROR_CAP] = {0}; r2_result result = {0};
        check(r2_decode_block(code, n, block, error, sizeof(error)) == R2_OK, error);
        check(r2_interp_block_with_env(block, &env, &result, error, sizeof(error)) == R2_ERR_UNSUPPORTED, "invalid atomic memory rejected");
        check(strstr(error, failure == 0 ? "read-only" : failure == 1 ? "unaligned atomic" : "not mapped") != NULL, error);
        check(data[0] == 1 && data[1] == 2, "failed atomic leaves memory unchanged");
    }
    free(block);
    printf("PASS xchg/cmpxchg/xadd widths, memory, flags and aliases (%u checks)\n", checks);
    return 0;
}
