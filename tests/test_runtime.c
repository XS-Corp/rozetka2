#include "internal.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned checks;
static void require(bool condition, const char *message)
{
    if (!condition) { fprintf(stderr, "FAIL runtime: %s\n", message); exit(1); }
    checks += 1;
}
static void instructions(void)
{
    const struct {
        const char *name;
        uint8_t code[128];
        size_t length;
        uint64_t expected;
        bool jit;
        const char *error;
    } cases[] = {
    {"rep_stos_zero_no_access", {0xb8,42,0,0,0,0xf3,0x48,0xab,0xc3}, 9, 42, false, NULL},
    {"rep_movs_zero_no_access", {0xb8,42,0,0,0,0xf3,0x48,0xa5,0xc3}, 9, 42, false, NULL},
    {"pop_memory_rsp_after_increment", {0x48,0x83,0xec,0x10,0x6a,42,0x48,0x8f,0x44,0x24,8,0x48,0x8b,0x44,0x24,8,0x48,0x83,0xc4,0x10,0xc3}, 21, 42, false, NULL},
    {"pop_memory_base", {0x48,0x83,0xec,8,0x48,0x89,0xe1,0x6a,42,0x8f,1,0x58,0xc3}, 13, 42, false, NULL},
    {"pop_rsp", {0x48,0x89,0xe1,0x51,0x5c,0xb8,42,0,0,0,0xc3}, 11, 42, false, NULL},
    {"pop_modrm_r8", {0x6a,42,0x41,0x8f,0xc0,0x4c,0x89,0xc0,0xc3}, 9, 42, false, NULL},
    {"pop_preserves_flags", {0x31,0xc0,0x83,0xf8,1,0x6a,42,0x8f,0xc1,0x0f,0x92,0xc0,0xc3}, 13, 1, false, NULL},
    {"lock_stack_barrier", {0xb8,42,0,0,0,0x50,0xf0,0x83,0x04,0x24,0x00,0x58,0xc3}, 13, 42, false, NULL},
    {"lock_inc_preserves_carry", {0x31,0xc0,0x83,0xf8,1,0x50,0xf0,0xff,0x04,0x24,0x58,0x0f,0x92,0xc4,0xc3}, 15, 257, false, NULL},
    {"lock_adc_carry", {0x31,0xc0,0x83,0xf8,1,0x50,0xf0,0x48,0x83,0x14,0x24,0xff,0x58,0x0f,0x92,0xc4,0xc3}, 17, 256, false, NULL},
    {"lock_neg", {0xb8,42,0,0,0,0x50,0xf0,0x48,0xf7,0x1c,0x24,0x58,0xc3}, 13, UINT64_C(0xffffffffffffffd6), false, NULL},
    {"lock_not", {0xb8,42,0,0,0,0x50,0xf0,0x48,0xf7,0x14,0x24,0x58,0xc3}, 13, UINT64_C(0xffffffffffffffd5), false, NULL},
    {"push_memory_rsp", {0xb8,42,0,0,0,0x50,0xff,0x34,0x24,0x58,0x59,0xc3}, 12, 42, false, NULL},
    {"push_modrm_register", {0xb8,42,0,0,0,0xff,0xf0,0x58,0xc3}, 9, 42, false, NULL},
    {"byte_alu", {0x48,0xc7,0xc0,0xff,0xff,0xff,0xff,0xb0,0x7f,0x04,0x01,0x0f,0x90,0xc4,0xc3}, 15, UINT64_C(0xffffffffffff0180), false, NULL},
    {"word_adc", {0xba,0x01,0x00,0x00,0x00,0x48,0xc7,0xc0,0xff,0xff,0xff,0xff,0x66,0xb8,0xff,0xff,0x31,0xd2,0x83,0xfa,0x01,0x66,0x83,0xd0,0x00,0x0f,0x92,0xc4,0xc3}, 29, UINT64_C(0xffffffffffff0100), false, NULL},
    {"byte_memory", {0x48,0x83,0xec,0x10,0x48,0xc7,0x04,0x24,0xff,0xff,0xff,0xff,0x80,0x64,0x24,0x01,0x7f,0x80,0x34,0x24,0x20,0x80,0x4c,0x24,0x02,0x02,0x80,0x6c,0x24,0x03,0x01,0x48,0x8b,0x04,0x24,0x48,0x83,0xc4,0x10,0xc3}, 40, UINT64_C(0xfffffffffeff7fdf), false, NULL},
    {"adc_sbb_memory", {0x48,0x83,0xec,0x10,0xc6,0x04,0x24,0x00,0x80,0x3c,0x24,0x01,0x80,0x14,0x24,0xff,0x80,0x1c,0x24,0x00,0x0f,0xb6,0x04,0x24,0x48,0x8d,0x64,0x24,0x10,0xc3}, 30, UINT64_C(0x00000000000000ff), false, NULL},
    {"cmov32_false", {0x48,0xc7,0xc0,0xff,0xff,0xff,0xff,0xb9,0x2a,0x00,0x00,0x00,0x83,0xf9,0x2a,0x0f,0x45,0xc1,0xc3}, 19, UINT64_C(0x00000000ffffffff), true, NULL},
    {"cmov16_true", {0x48,0xb8,0x88,0x77,0x66,0x55,0x44,0x33,0x22,0x11,0xb9,0x2a,0x00,0x00,0x00,0x83,0xf9,0x2a,0x66,0x0f,0x44,0xc1,0xc3}, 23, UINT64_C(0x112233445566002a), false, NULL},
    {"cmov_false_fault", {0x31,0xc0,0x48,0x0f,0x45,0x08,0xc3}, 7, UINT64_C(0x0000000000000000), false, "not mapped"},
    {"inc_cf", {0x31,0xc0,0x83,0xf8,0x01,0xff,0xc0,0x0f,0x92,0xc2,0x0f,0xb6,0xc2,0xc3}, 14, UINT64_C(0x0000000000000001), false, NULL},
    {"dec_overflow", {0xb8,0x00,0x00,0x00,0x80,0xff,0xc8,0xb8,0x00,0x00,0x00,0x00,0x0f,0x90,0xc0,0xc3}, 16, UINT64_C(0x0000000000000001), false, NULL},
    {"neg_byte", {0xb8,0x80,0x00,0x00,0x00,0xf6,0xd8,0x0f,0x90,0xc4,0xc3}, 11, UINT64_C(0x0000000000000180), false, NULL},
    {"not_word", {0x48,0xc7,0xc0,0xff,0xff,0xff,0xff,0x66,0xf7,0xd0,0xc3}, 11, UINT64_C(0xffffffffffff0000), false, NULL},
    {"rotate_full_cycle", {0x31,0xc0,0xb0,0x01,0xc0,0xc8,0x08,0x0f,0x92,0xc4,0xc3}, 11, UINT64_C(0x0000000000000001), false, NULL},
    {"rotate_carry_cycle", {0x31,0xc0,0x83,0xf8,0x01,0xb0,0x03,0xc0,0xd0,0x09,0x0f,0x92,0xc4,0xc3}, 14, UINT64_C(0x0000000000000103), false, NULL},
    {"shift_byte_full", {0xb8,0x81,0x00,0x00,0x00,0xc0,0xe0,0x08,0x0f,0x92,0xc4,0xc3}, 12, UINT64_C(0x0000000000000100), false, NULL},
    {"shift_zero_mask", {0x48,0xc7,0xc0,0xff,0xff,0xff,0xff,0xc1,0xe0,0x20,0xc3}, 11, UINT64_C(0x00000000ffffffff), false, NULL},
    {"sar64", {0x48,0xc7,0xc0,0xfe,0xff,0xff,0xff,0x48,0xc1,0xf8,0x3f,0xc3}, 12, UINT64_C(0xffffffffffffffff), false, NULL},
    {"shr_cl", {0xb8,0x80,0x00,0x00,0x00,0xb1,0x07,0xd3,0xe8,0xc3}, 10, UINT64_C(0x0000000000000001), false, NULL},
    {"imul32", {0xb8,0xeb,0xff,0xff,0xff,0xb9,0x02,0x00,0x00,0x00,0x0f,0xaf,0xc1,0xc3}, 14, UINT64_C(0x00000000ffffffd6), false, NULL},
    {"imul_imm64", {0x48,0xc7,0xc0,0xeb,0xff,0xff,0xff,0x48,0x6b,0xc0,0xfe,0xc3}, 12, UINT64_C(0x000000000000002a), false, NULL},
    {"imul_overflow", {0x48,0xb8,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x7f,0x48,0x6b,0xc0,0x02,0xb8,0x00,0x00,0x00,0x00,0x0f,0x90,0xc0,0xc3}, 23, UINT64_C(0x0000000000000001), false, NULL},
    {"mul_high", {0x48,0xc7,0xc0,0xff,0xff,0xff,0xff,0xb9,0x02,0x00,0x00,0x00,0x48,0xf7,0xe1,0x48,0x89,0xd0,0xc3}, 19, UINT64_C(0x0000000000000001), false, NULL},
    {"div64", {0xba,0x01,0x00,0x00,0x00,0x31,0xc0,0xb9,0x02,0x00,0x00,0x00,0x48,0xf7,0xf1,0xc3}, 16, UINT64_C(0x8000000000000000), false, NULL},
    {"idiv64", {0x48,0xc7,0xc0,0xd5,0xff,0xff,0xff,0x48,0x99,0xb9,0x02,0x00,0x00,0x00,0x48,0xf7,0xf9,0xc3}, 18, UINT64_C(0xffffffffffffffeb), false, NULL},
    {"div_zero", {0x31,0xc0,0x31,0xd2,0xf7,0xf0,0xc3}, 7, UINT64_C(0x0000000000000000), false, "divide by zero"},
    {"div_overflow", {0xba,0x01,0x00,0x00,0x00,0x31,0xc0,0xb9,0x01,0x00,0x00,0x00,0xf7,0xf1,0xc3}, 15, UINT64_C(0x0000000000000000), false, "quotient overflow"},
    {"cdqe", {0xb8,0x00,0x00,0x00,0x80,0x48,0x98,0xc3}, 8, UINT64_C(0xffffffff80000000), true, NULL},
    {"cdq", {0xb8,0x00,0x00,0x00,0x80,0x99,0x89,0xd0,0xc3}, 9, UINT64_C(0x00000000ffffffff), false, NULL},
    {"btr_memory", {0x48,0x83,0xec,0x10,0x48,0xc7,0x04,0x24,0xff,0xff,0xff,0xff,0x48,0x0f,0xba,0x34,0x24,0x3f,0x48,0x8b,0x04,0x24,0x48,0x83,0xd0,0x00,0x48,0x8d,0x64,0x24,0x10,0xc3}, 32, UINT64_C(0x8000000000000000), false, NULL},
    {"bt_negative", {0x48,0x83,0xec,0x20,0x48,0x8d,0x54,0x24,0x10,0x48,0xc7,0x42,0xf8,0xff,0xff,0xff,0xff,0x48,0xc7,0xc1,0xff,0xff,0xff,0xff,0x48,0x0f,0xa3,0x0a,0xb8,0x00,0x00,0x00,0x00,0x0f,0x92,0xc0,0x48,0x8d,0x64,0x24,0x20,0xc3}, 42, UINT64_C(0x0000000000000001), false, NULL},
    {"sse_zero", {0x48,0x83,0xec,0x20,0x48,0xc7,0x04,0x24,0xff,0xff,0xff,0xff,0x48,0xc7,0x44,0x24,0x08,0xff,0xff,0xff,0xff,0x0f,0x57,0xc0,0x0f,0x11,0x04,0x24,0x48,0x8b,0x04,0x24,0x48,0x0b,0x44,0x24,0x08,0x48,0x8d,0x64,0x24,0x20,0xc3}, 43, UINT64_C(0x0000000000000000), false, NULL},
    {"sse_copy", {0x48,0x83,0xec,0x20,0x48,0xc7,0x04,0x24,0x2a,0x00,0x00,0x00,0x48,0xc7,0x44,0x24,0x08,0x11,0x00,0x00,0x00,0x0f,0x10,0x04,0x24,0x0f,0x11,0x44,0x24,0x10,0x48,0x8b,0x44,0x24,0x10,0x48,0x03,0x44,0x24,0x18,0x48,0x8d,0x64,0x24,0x20,0xc3}, 46, UINT64_C(0x000000000000003b), false, NULL},
    {"sse_alignment", {0x0f,0x57,0xc0,0x0f,0x29,0x04,0x24,0xc3}, 8, UINT64_C(0x0000000000000000), false, "unaligned"},
    {"pcmpeqd_self_flags", {0x31,0xc0,0x83,0xf8,0x01,0x66,0x0f,0x76,0xc0,0x0f,0x92,0xc0,0x0f,0x98,0xc4,0xc3}, 16, UINT64_C(0x101), false, NULL},
    {"pcmpeqd_self_value", {0x48,0x83,0xec,0x10,0x66,0x0f,0x76,0xc0,0x0f,0x11,0x04,0x24,0x48,0x8b,0x04,0x24,0x48,0x83,0xc4,0x10,0xc3}, 21, UINT64_MAX, false, NULL},
    {"pcmpeqd_lanes_rex_memory", {0x48,0x83,0xec,0x28,0x48,0xc7,0x04,0x24,0x01,0,0,0,0x48,0xc7,0x44,0x24,0x08,0x02,0,0,0,0x45,0x0f,0x57,0xc9,0x66,0x44,0x0f,0x76,0x0c,0x24,0x44,0x0f,0x11,0x4c,0x24,0x10,0x48,0x8b,0x44,0x24,0x10,0x48,0x83,0xc4,0x28,0xc3}, 47, UINT64_C(0xffffffff00000000), false, NULL},
    {"pcmpeqd_alignment", {0x66,0x0f,0x76,0x04,0x24,0xc3}, 6, 0, false, "unaligned"},
    {"lea32", {0x48,0xc7,0xc1,0xff,0xff,0xff,0xff,0x8d,0x41,0x2b,0xc3}, 11, UINT64_C(0x000000000000002a), false, NULL},
    {"bswap32", {0x48,0xb8,0x78,0x56,0x34,0x12,0xff,0xff,0xff,0xff,0x0f,0xc8,0xc3}, 13, UINT64_C(0x78563412), false, NULL},
    {"bswap64_rex", {0x49,0xb8,0x88,0x77,0x66,0x55,0x44,0x33,0x22,0x11,0x49,0x0f,0xc8,0x4c,0x89,0xc0,0xc3}, 17, UINT64_C(0x8877665544332211), false, NULL},
    {"bswap_flags", {0x31,0xc0,0x83,0xf8,0x01,0x0f,0xc8,0x0f,0x92,0xc0,0x0f,0x98,0xc4,0xc3}, 14, UINT64_C(0x101), false, NULL},
    {"tzcnt_zero", {0x31,0xc0,0xf3,0x0f,0xbc,0xc0,0x0f,0x92,0xc4,0xc3}, 10, UINT64_C(0x120), false, NULL},
    {"lzcnt_word", {0x48,0xc7,0xc0,0xff,0xff,0xff,0xff,0x66,0xb8,0x01,0,0x66,0xf3,0x0f,0xbd,0xc0,0xc3}, 17, UINT64_C(0xffffffffffff000f), false, NULL},
    {"bsf_zero_preserve", {0x48,0xc7,0xc0,0xff,0xff,0xff,0xff,0x31,0xc9,0x48,0x0f,0xbc,0xc1,0x0f,0x94,0xc4,0xc3}, 17, UINT64_C(0xffffffffffff01ff), false, NULL},
    {"bsr_high", {0x48,0xb8,0,0,0,0,0,0,0,0x80,0x48,0x0f,0xbd,0xc0,0xc3}, 15, 63, false, NULL},
    };
    r2_block *block = malloc(sizeof(*block));
    require(block != NULL, "allocate test block");
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i += 1) {
        char error[R2_ERROR_CAP] = {0};
        r2_status status = r2_decode_block(cases[i].code, cases[i].length, block, error, sizeof(error));
        if (status != R2_OK) { fprintf(stderr, "%s: %s\n", cases[i].name, error); exit(1); }
        r2_result result = {0};
        status = r2_interp_block(block, &result, error, sizeof(error));
        if (cases[i].error != NULL) {
            require(status != R2_OK && strstr(error, cases[i].error) != NULL, cases[i].name);
        } else {
            if (status != R2_OK || result.rax != cases[i].expected) {
                fprintf(stderr, "%s: status=%s result=%llx expected=%llx (%s)\n", cases[i].name,
                    r2_status_name(status), (unsigned long long)result.rax,
                    (unsigned long long)cases[i].expected, error); exit(1);
            }
            checks += 1;
            if (cases[i].jit) {
                status = r2_jit_block(block, &result, error, sizeof(error));
                require(status == R2_OK && result.rax == cases[i].expected, cases[i].name);
            }
        }
    }
    free(block);
}

/* Check SHLD/SHRD against concatenated integer values, including flags,
 * masked counts, CL/immediate counts, and register/memory destinations. */
static void string_stores(void)
{
    r2_block *block = malloc(sizeof(*block));
    require(block != NULL, "allocate string store block");
    for (unsigned width = 1; width <= 8; width *= 2)
    for (unsigned backward = 0; backward < 2; backward++)
    for (unsigned repeated = 0; repeated < 2; repeated++)
    for (unsigned count = 0; count < 4; count++) {
        uint8_t memory[256], expected[256], code[32];
        memset(memory, 0xa5, sizeof(memory)); memcpy(expected, memory, sizeof(memory));
        uint64_t pattern = UINT64_C(0x8877665544332211), base = 0x100000;
        uint64_t start = base + 64, iterations = repeated ? count : 1;
        for (uint64_t i = 0; i < iterations; i++)
            memcpy(expected + 64 + (backward ? -(int64_t)(i * width) : (int64_t)(i * width)), &pattern, width);
        /* Seed CF, PF, AF and SF before the direction/store instructions. */
        const uint8_t seed[] = {0x31,0xed,0x83,0xed,1};
        size_t n = sizeof(seed); memcpy(code, seed, n);
        code[n++] = backward ? 0xfd : 0xfc;
        if (repeated) code[n++] = 0xf3;
        if (width == 2) code[n++] = 0x66;
        if (width == 8) code[n++] = 0x48;
        code[n++] = width == 1 ? 0xaa : 0xab;
        const uint8_t finish[] = {0x48,0x89,0x3b,0x48,0x89,0x4b,8,0x48,0x89,0x73,16,0x9c,0x58,0xc3};
        memcpy(code + n, finish, sizeof(finish)); n += sizeof(finish);
        uint64_t final_di = start + (backward ? -(int64_t)(iterations * width) : (int64_t)(iterations * width));
        uint64_t final_cx = repeated ? 0 : count, unchanged_si = UINT64_C(0x123456789);
        memcpy(expected + 128, &final_di, 8); memcpy(expected + 136, &final_cx, 8); memcpy(expected + 144, &unchanged_si, 8);
        r2_memory_region region = {.base=base, .size=sizeof(memory), .data=memory, .writable=true};
        r2_interp_env env = {.has_initial_regs=true, .regions=&region, .region_count=1};
        env.initial_regs[R2_X86_RAX] = pattern; env.initial_regs[R2_X86_RDI] = start;
        env.initial_regs[R2_X86_RCX] = count; env.initial_regs[R2_X86_RSI] = unchanged_si;
        env.initial_regs[R2_X86_RBX] = base + 128;
        char error[R2_ERROR_CAP] = {0}; r2_result result;
        require(r2_decode_block(code, n, block, error, sizeof(error)) == R2_OK, "decode string store");
        require(r2_interp_block_with_env(block, &env, &result, error, sizeof(error)) == R2_OK, error);
        require(memcmp(memory, expected, sizeof(memory)) == 0, "STOS widths, direction, count and unaffected registers");
        require(result.rax == (UINT64_C(0x297) | (backward ? 0x400 : 0)), "STOS preserves flags, CLD/STD set only DF");
    }
    free(block);
}

static void string_moves(void)
{
    r2_block *block = malloc(sizeof(*block));
    require(block != NULL, "allocate string move block");
    for (unsigned width = 1; width <= 8; width *= 2)
    for (unsigned backward = 0; backward < 2; backward++)
    for (unsigned repeated = 0; repeated < 2; repeated++)
    for (unsigned count = 0; count < 5; count++)
    for (int overlap = -1; overlap <= 1; overlap++) {
        uint8_t memory[256], expected[256], code[64];
        for (unsigned i = 0; i < sizeof(memory); i++) memory[i] = (uint8_t)(i * 37 + 13);
        memcpy(expected, memory, sizeof(memory));
        uint64_t base = 0x100000, si = 80, di = overlap ? si + overlap * (int)width : 144;
        unsigned iterations = repeated ? count : 1;
        int stride = backward ? -(int)width : (int)width;
        for (unsigned i = 0; i < iterations; i++) {
            uint8_t value[8];
            memcpy(value, expected + si + (int)i * stride, width);
            memcpy(expected + di + (int)i * stride, value, width);
        }
        const uint8_t seed[] = {0x31,0xed,0x83,0xed,1};
        size_t n = sizeof(seed); memcpy(code, seed, n);
        code[n++] = backward ? 0xfd : 0xfc;
        if (repeated) code[n++] = 0xf3;
        if (width == 2) code[n++] = 0x66;
        if (width == 8) code[n++] = 0x48;
        code[n++] = width == 1 ? 0xa4 : 0xa5;
        const uint8_t finish[] = {0x48,0x89,0x3b,0x48,0x89,0x4b,8,0x48,0x89,0x73,16,0x48,0x89,0x43,24,0x9c,0x58,0xc3};
        memcpy(code + n, finish, sizeof(finish)); n += sizeof(finish);
        uint64_t final_di = base + di + (int64_t)iterations * stride, final_si = base + si + (int64_t)iterations * stride;
        uint64_t final_cx = repeated ? 0 : count, original_ax = UINT64_C(0xfedcba9876543210);
        memcpy(expected + 192, &final_di, 8); memcpy(expected + 200, &final_cx, 8);
        memcpy(expected + 208, &final_si, 8); memcpy(expected + 216, &original_ax, 8);
        r2_memory_region region = {.base=base, .size=sizeof(memory), .data=memory, .writable=true};
        r2_interp_env env = {.has_initial_regs=true, .regions=&region, .region_count=1};
        env.initial_regs[R2_X86_RAX] = original_ax; env.initial_regs[R2_X86_RDI] = base + di;
        env.initial_regs[R2_X86_RCX] = count; env.initial_regs[R2_X86_RSI] = base + si;
        env.initial_regs[R2_X86_RBX] = base + 192;
        char error[R2_ERROR_CAP] = {0}; r2_result result;
        require(r2_decode_block(code, n, block, error, sizeof(error)) == R2_OK, "decode MOVS");
        require(r2_interp_block_with_env(block, &env, &result, error, sizeof(error)) == R2_OK, error);
        require(memcmp(memory, expected, sizeof(memory)) == 0, "MOVS widths, overlap, direction, count and unaffected registers");
        require(result.rax == (UINT64_C(0x297) | (backward ? 0x400 : 0)), "MOVS preserves flags");
    }
    free(block);
}

static void double_shifts(void)
{
    r2_block *block = malloc(sizeof(*block));
    require(block != NULL, "allocate double shift block");
    const uint64_t inputs[] = {0, 1, UINT64_MAX, UINT64_C(0x8123456789abcdef)};
    for (unsigned width = 2; width <= 8; width *= 2)
    for (unsigned right = 0; right < 2; right++)
    for (unsigned cl = 0; cl < 2; cl++)
    for (unsigned memory = 0; memory < 2; memory++)
    for (size_t v = 0; v < sizeof(inputs) / sizeof(inputs[0]); v++) {
        unsigned bits = width * 8;
        unsigned counts[] = {0, 1, bits - 1, bits, bits == 64 ? 64 : 32};
        for (size_t c = 0; c < sizeof(counts) / sizeof(counts[0]); c++) {
            unsigned count = counts[c] & (width == 8 ? 63 : 31);
            if (count > bits) continue; /* Architecturally undefined result. */
            uint64_t mask = width == 8 ? UINT64_MAX : (UINT64_C(1) << bits) - 1;
            uint64_t value = inputs[v] & mask, source = UINT64_C(0x76543210fedcba98) & mask;
            __uint128_t pair = right ? ((__uint128_t)source << bits) | value
                                     : ((__uint128_t)value << bits) | source;
            uint64_t result = count == 0 ? value : (uint64_t)(right ? pair >> count : pair >> (bits - count)) & mask;
            bool cf = count == 0 || ((pair >> (right ? count - 1 : bits * 2 - count)) & 1);
            bool of = count == 1 && (right ? ((value ^ result) >> (bits - 1)) & 1
                                                     : ((result >> (bits - 1)) & 1) != cf);
            bool zf = count != 0 && result == 0;
            bool sf = count == 0 || ((result >> (bits - 1)) & 1);
            bool pf = count == 0 || (__builtin_popcount((unsigned)(result & 255)) & 1) == 0;
            uint64_t expected_flags = (uint64_t)cf | ((uint64_t)of << 8) | ((uint64_t)zf << 16)
                                    | ((uint64_t)sf << 24) | ((uint64_t)pf << 32);
            uint8_t code[128] = {0x31,0xdb,0x83,0xfb,0x01,0x48,0xb8}; /* CF=SF=PF=1. */
            size_t n = 7;
            memcpy(code + n, &inputs[v], 8); n += 8;
            code[n++] = 0x48; code[n++] = 0xba;
            uint64_t full_source = UINT64_C(0x76543210fedcba98);
            memcpy(code + n, &full_source, 8); n += 8;
            code[n++] = 0xb9; memcpy(code + n, &counts[c], 4); n += 4;
            if (memory) code[n++] = 0x50;
            if (width == 2) code[n++] = 0x66;
            if (width == 8) code[n++] = 0x48;
            code[n++] = 0x0f; code[n++] = (right ? 0xac : 0xa4) + cl;
            code[n++] = memory ? 0x14 : 0xd0;
            if (memory) code[n++] = 0x24;
            if (!cl) code[n++] = (uint8_t)counts[c];
            if (memory) code[n++] = 0x58;
            size_t end = n;
            uint64_t expected = result;
            if (width == 2 || memory) expected |= inputs[v] & ~mask;
            for (unsigned flags = 0; flags < 2; flags++) {
                n = end;
                if (flags) {
                    const uint8_t capture[] = {0xb8,0,0,0,0, 0x0f,0x92,0xc0, 0x0f,0x90,0xc4,
                        0x0f,0x94,0xc3, 0x0f,0x98,0xc7, 0x0f,0x9a,0xc1,
                        0x0f,0xb7,0xd3, 0x48,0xc1,0xe2,0x10, 0x48,0x09,0xd0,
                        0x0f,0xb6,0xc9, 0x48,0xc1,0xe1,0x20, 0x48,0x09,0xc8};
                    memcpy(code + n, capture, sizeof(capture)); n += sizeof(capture);
                }
                code[n++] = 0xc3;
                char error[R2_ERROR_CAP] = {0}; r2_result actual = {0};
                require(r2_decode_block(code, n, block, error, sizeof(error)) == R2_OK, error);
                require(r2_interp_block(block, &actual, error, sizeof(error)) == R2_OK, error);
                uint64_t valid = flags && count > 1 ? ~UINT64_C(0x100) : UINT64_MAX;
                require((actual.rax & valid) == ((flags ? expected_flags : expected) & valid),
                        flags ? "double shift flags" : "double shift value and partial register");
            }
        }
    }
    free(block);
}

static void bridge_machine(r2_native_runtime *runtime, const char *symbol,
                           uint64_t registers[16], uint8_t xmm[16][16])
{
    char error[R2_ERROR_CAP] = {0};
    uint64_t address = 0;
    require(r2_native_symbol(runtime, symbol, &address, error, sizeof(error)) == R2_OK, error);
    uint32_t number;
    memcpy(&number, (void *)(uintptr_t)(address + 1), 4);
    registers[R2_X86_RAX] = number;
    bool exited = false;
    require(r2_native_call(runtime, registers, xmm, &exited, error, sizeof(error)) == R2_OK, error);
}

static uint64_t bridge(r2_native_runtime *runtime, const char *symbol, uint64_t *values, size_t count)
{
    uint64_t registers[16] = {0}, stack[32] = {0};
    unsigned indices[] = {R2_X86_RDI, R2_X86_RSI, R2_X86_RDX, R2_X86_RCX, R2_X86_R8, R2_X86_R9};
    for (size_t i = 0; i < count; i += 1) {
        if (i < 6) registers[indices[i]] = values[i];
        else stack[i - 5] = values[i];
    }
    registers[R2_X86_RSP] = (uintptr_t)stack;
    uint8_t xmm[16][16] = {{0}};
    bridge_machine(runtime, symbol, registers, xmm);
    return registers[R2_X86_RAX];
}

static void native_runtime(void)
{
    char error[R2_ERROR_CAP] = {0};
    r2_native_runtime *runtime = r2_native_create(error, sizeof(error));
    require(runtime != NULL, error);
    char output[128];
    uint64_t arguments[] = {(uintptr_t)output, sizeof(output), (uintptr_t)"%d:%s:%ld:%u:%s:%d:%s",
        (uint64_t)-123, (uintptr_t)"alpha", (uint64_t)-456, 789, (uintptr_t)"beta", 42, (uintptr_t)"tail"};
    uint64_t length = bridge(runtime, "_snprintf", arguments, sizeof(arguments) / sizeof(arguments[0]));
    require(strcmp(output, "-123:alpha:-456:789:beta:42:tail") == 0 && length == strlen(output),
            "Darwin variadic register/stack ABI");

    uint64_t saved_registers[6] = {0, 0, 0, (uint64_t)-7, (uintptr_t)"list", 99};
    uint64_t overflow[] = {1234};
    struct { uint32_t gp, fp; uint64_t overflow, registers; } guest_va = {
        24, 48, (uintptr_t)overflow, (uintptr_t)saved_registers};
    uint64_t va_arguments[] = {(uintptr_t)output, sizeof(output), (uintptr_t)"%d:%s:%u:%ld", (uintptr_t)&guest_va};
    length = bridge(runtime, "_vsnprintf", va_arguments, 4);
    require(strcmp(output, "-7:list:99:1234") == 0 && length == strlen(output), "x86_64 va_list conversion");

    /* Independent GP/XMM register banks and interleaved stack overflow. */
    uint64_t regs[16] = {0}, fp_stack[32] = {0};
    uint8_t xmm[16][16] = {{0}};
    regs[R2_X86_RDI] = (uintptr_t)output; regs[R2_X86_RSI] = sizeof(output);
    regs[R2_X86_RDX] = (uintptr_t)"%d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %.1f %.1f %.1f %.1f";
    regs[R2_X86_RCX] = 11; regs[R2_X86_R8] = 22; regs[R2_X86_R9] = 33;
    fp_stack[1] = 44; fp_stack[2] = 55;
    for (unsigned i = 0; i < 9; i++) {
        double d = i + 0.5;
        if (i < 8) memcpy(xmm[i], &d, 8); else memcpy(&fp_stack[3], &d, 8);
    }
    regs[R2_X86_RSP] = (uintptr_t)fp_stack;
    bridge_machine(runtime, "_snprintf", regs, xmm);
    require(strcmp(output, "11 0.5 22 1.5 33 2.5 44 3.5 55 4.5 5.5 6.5 7.5 8.5") == 0,
            "variadic mixed GP/FP arguments and both register bank overflows");

    uint8_t saved_fp[176] = {0};
    uint64_t saved_int = 77; double saved_double = 1.25, spilled_double = 2.5;
    memcpy(saved_fp + 24, &saved_int, 8); memcpy(saved_fp + 160, &saved_double, 8);
    guest_va.gp = 24; guest_va.fp = 160; guest_va.registers = (uintptr_t)saved_fp;
    guest_va.overflow = (uintptr_t)&spilled_double;
    va_arguments[2] = (uintptr_t)"%d:%.2f:%.1f";
    bridge(runtime, "_vsnprintf", va_arguments, 4);
    require(strcmp(output, "77:1.25:2.5") == 0, "va_list FP save area and overflow area");

    uint64_t atof_arguments[] = {(uintptr_t)"-12.75"};
    memset(regs, 0, sizeof(regs)); memset(xmm, 0, sizeof(xmm));
    regs[R2_X86_RDI] = atof_arguments[0]; regs[R2_X86_RSP] = (uintptr_t)fp_stack;
    bridge_machine(runtime, "_atof", regs, xmm);
    double converted; memcpy(&converted, xmm[0], 8);
    require(converted == -12.75, "native double result returned through guest XMM0");

    int scanned = 0; double scanned_double = 0; char scanned_text[8] = {0};
    uint64_t scanf_arguments[] = {(uintptr_t)"skip 42 1.25 abc", (uintptr_t)"%*s %d %lf %3[a-z]",
        (uintptr_t)&scanned, (uintptr_t)&scanned_double, (uintptr_t)scanned_text};
    require(bridge(runtime, "_sscanf", scanf_arguments, 5) == 3 && scanned == 42 && scanned_double == 1.25
            && strcmp(scanned_text, "abc") == 0, "scanf suppression, double pointer, bounded scanset");

    char path[] = "/tmp/rozetka2-bridge-XXXXXX";
    int placeholder = mkstemp(path);
    require(placeholder >= 0, "create bridge test name");
    close(placeholder); unlink(path);
    uint64_t open_arguments[] = {(uintptr_t)path, O_WRONLY | O_CREAT | O_EXCL, 0600};
    int fd = (int)bridge(runtime, "_open", open_arguments, 3);
    require(fd >= 0, "bridged open");
    struct stat st;
    require(fstat(fd, &st) == 0 && (st.st_mode & 0777) == 0600, "open variadic creation mode");
    close(fd); unlink(path);

    uint64_t value = 42;
    require(r2_native_pointer(runtime, (uintptr_t)&value, sizeof(value), true, error, sizeof(error)) == &value,
            "checked host-backed writable memory");
    require(r2_native_pointer(runtime, 0, 8, false, error, sizeof(error)) == NULL, "null guest pointer rejected");
    require(r2_native_pointer(runtime, UINT64_MAX, 8, false, error, sizeof(error)) == NULL, "wrapped guest span rejected");
    uint64_t unknown;
    require(r2_native_symbol(runtime, "_no_such_declared_bridge_function", &unknown, error, sizeof(error)) == R2_ERR_UNSUPPORTED,
            "undeclared native function rejected");

    uint8_t *code = malloc(5001);
    require(code != NULL, "allocate large instruction stream");
    memset(code, 0x90, 5000); code[5000] = 0xc3;
    r2_native_add_code(runtime, (uintptr_t)code, 5001);
    r2_block *block = calloc(1, sizeof(*block));
    require(block != NULL, "allocate on-demand block");
    block->insns = block->storage; block->capacity = R2_MAX_INSNS; block->runtime = runtime;
    for (size_t i = 0; i < 5001; i += 1) {
        size_t pc;
        require(r2_native_decode(block, (uintptr_t)(code + i), &pc, error, sizeof(error)) == R2_OK && pc == i,
                "on-demand cache growth");
    }
    size_t count = block->count, pc;
    require(r2_native_decode(block, (uintptr_t)code, &pc, error, sizeof(error)) == R2_OK && pc == 0 && block->count == count,
            "cached instruction reused after growth");
    require(r2_native_decode(block, (uintptr_t)code - 1, &pc, error, sizeof(error)) == R2_ERR_DECODE,
            "instruction fetch outside guest code rejected");
    if (block->insns != block->storage) free(block->insns);
    free(block); free(code);
    r2_native_destroy(runtime);
    runtime = r2_native_create(error, sizeof(error));
    require(runtime != NULL, "fresh runtime for signal execution");

    struct { uint64_t handler; uint32_t mask, flags; } action = {0x1234, 0, SA_RESTART};
    uint64_t signal_arguments[] = {SIGUSR1, (uintptr_t)&action, 0};
    require(bridge(runtime, "_sigaction", signal_arguments, 3) == 0, "guest signal action install");
    raise(SIGUSR1);
    require(r2_native_pending(runtime) == (UINT32_C(1) << (SIGUSR1 - 1)), "host signal queued for interpreter");
    require(r2_native_pending(runtime) == 0, "pending host signal consumed once");
    uint64_t handler; uint32_t mask, flags;
    r2_native_signal_action(runtime, 32, &handler, &mask, &flags);
    require(handler == 0 && mask == 0 && flags == 0, "out-of-range Darwin signal slot is empty");
    r2_native_signal_action(runtime, SIGUSR1, &handler, &mask, &flags);
    require(handler == action.handler && flags == action.flags, "guest signal callback remains guest code");
    uint32_t delivered = 0;
    uint8_t handler_code[] = {0x48, 0xb8, 0,0,0,0,0,0,0,0, 0xc7,0x00,42,0,0,0,0xc3};
    uint8_t main_code[] = {0x48, 0xb8, 0,0,0,0,0,0,0,0, 0x8b,0x00,0xc3};
    uint64_t destination = (uintptr_t)&delivered;
    memcpy(handler_code + 2, &destination, 8);
    memcpy(main_code + 2, &destination, 8);
    r2_native_add_code(runtime, (uintptr_t)handler_code, sizeof(handler_code));
    r2_native_add_code(runtime, (uintptr_t)main_code, sizeof(main_code));
    action.handler = (uintptr_t)handler_code;
    require(bridge(runtime, "_sigaction", signal_arguments, 3) == 0, "install executable guest callback");
    raise(SIGUSR1);
    block = calloc(1, sizeof(*block));
    require(block != NULL, "allocate callback execution block");
    block->insns = block->storage; block->capacity = R2_MAX_INSNS; block->runtime = runtime;
    r2_interp_env environment = {.native = runtime, .has_initial_rip = true, .initial_rip = (uintptr_t)main_code};
    r2_result result = {0};
    require(r2_interp_block_with_env(block, &environment, &result, error, sizeof(error)) == R2_OK,
            "execute queued guest signal handler");
    require(delivered == 42 && result.rax == 42, "signal handler interpreted and caller resumed");
    if (block->insns != block->storage) free(block->insns);
    free(block);
    r2_native_destroy(runtime);
}

int main(void)
{
    instructions();
    string_stores();
    string_moves();
    double_shifts();
    native_runtime();
    printf("PASS runtime instructions and host ABI (%u checks)\n", checks);
    return 0;
}
