#include "internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *name;
    uint8_t opcode;
    uint8_t width;
    uint64_t lhs;
    uint64_t rhs;
    bool immediate;
    bool zf;
    bool sf;
    bool of;
    bool cf;
} flag_case;

static unsigned checks;

static r2_status execute(const uint8_t *code, size_t size, r2_engine engine,
                          r2_result *result, char *error, size_t error_len)
{
    r2_block block;
    r2_status status = r2_decode_block(code, size, &block, error, error_len);
    if (status != R2_OK) {
        return status;
    }
    return engine == R2_ENGINE_INTERP
        ? r2_interp_block(&block, result, error, error_len)
        : r2_jit_block(&block, result, error, error_len);
}

static void put_value(uint8_t *code, size_t *size, uint64_t value, uint8_t width)
{
    for (uint8_t i = 0; i < width; i += 1) {
        code[(*size)++] = (uint8_t)(value >> (8u * i));
    }
}

static void mov_reg(uint8_t *code, size_t *size, uint8_t reg, uint64_t value, uint8_t width)
{
    if (width == 8) {
        code[(*size)++] = 0x48;
    }
    code[(*size)++] = (uint8_t)(0xb8 + reg);
    put_value(code, size, value, width);
}

static void check_result(const char *name, const uint8_t *code, size_t size, uint64_t expected)
{
    for (unsigned engine = R2_ENGINE_INTERP; engine <= R2_ENGINE_JIT; engine += 1) {
        r2_result result = {0};
        char error[R2_ERROR_CAP] = {0};
        r2_status status = execute(code, size, (r2_engine)engine, &result, error, sizeof(error));
        if (status != R2_OK || result.rax != expected) {
            fprintf(stderr, "FAIL %s engine=%u: status=%s rax=%llu expected=%llu (%s)\n",
                    name, engine, r2_status_name(status), (unsigned long long)result.rax,
                    (unsigned long long)expected, error);
            exit(1);
        }
    }
    checks += 1;
}

static void check_error(const char *name, const uint8_t *code, size_t size,
                        r2_status expected, const char *message)
{
    r2_result result = {.rax = 123};
    char error[R2_ERROR_CAP] = {0};
    r2_status status = execute(code, size, R2_ENGINE_JIT, &result, error, sizeof(error));
    if (status != expected || strstr(error, message) == NULL || result.rax != 123) {
        fprintf(stderr, "FAIL %s: status=%s (%s)\n", name, r2_status_name(status), error);
        exit(1);
    }
    checks += 1;
}

static void check_flags(const flag_case *test)
{
    static const uint8_t conditions[] = {
        0x74, 0x75, 0x7c, 0x7e, 0x7f, 0x7d, 0x72, 0x73, 0x76, 0x77,
    };
    const bool expected[] = {
        test->zf, !test->zf, test->sf != test->of,
        test->zf || test->sf != test->of,
        !test->zf && test->sf == test->of, test->sf == test->of,
        test->cf, !test->cf, test->cf || test->zf, !test->cf && !test->zf,
    };
    for (size_t cc = 0; cc < sizeof(conditions); cc += 1) {
        for (unsigned form = 0; form < (test->immediate ? 4u : 2u); form += 1) {
            unsigned near = form & 1u;
            bool imm8 = form >= 2u;
            uint64_t imm8_value = (uint64_t)(int64_t)(int8_t)test->rhs;
            uint64_t mask = test->width == 4 ? UINT32_MAX : UINT64_MAX;
            if (imm8 && ((imm8_value ^ test->rhs) & mask) != 0) {
                continue;
            }
            uint8_t code[96] = {0};
            size_t size = 0;
            /* Seed SF/OF/CF before the operation. Logical operations must
             * clear stale overflow and carry from earlier arithmetic. */
            mov_reg(code, &size, 0, INT64_MAX, 8);
            code[size++] = 0x48;
            code[size++] = 0x83;
            code[size++] = 0xe8;
            code[size++] = 0xff;
            mov_reg(code, &size, 0, test->lhs, test->width);
            mov_reg(code, &size, 1, test->rhs, test->width);
            if (test->width == 8) {
                code[size++] = 0x48;
            }
            code[size++] = test->immediate ? (imm8 ? 0x83 : 0x81) : test->opcode;
            code[size++] = test->immediate ? test->opcode : 0xc8;
            if (test->immediate) {
                put_value(code, &size, test->rhs, imm8 ? 1 : 4);
            }
            /* Neither MOV nor the JIT's instruction budget may change flags. */
            mov_reg(code, &size, 0, UINT64_MAX, 8);
            code[size++] = 0x89; /* movl %eax, %ecx */
            code[size++] = 0xc1;
            code[size++] = 0x90;
            for (unsigned high = 0; high < 2; high += 1) {
                uint8_t set_code[128];
                memcpy(set_code, code, size);
                size_t set_size = size;
                set_code[set_size++] = 0x0f;
                set_code[set_size++] = (uint8_t)(conditions[cc] + 0x20);
                set_code[set_size++] = high ? 0xc4 : 0xc0; /* AH or AL */
                set_code[set_size++] = 0xc3;
                unsigned shift = high ? 8u : 0u;
                uint64_t value = (UINT64_MAX & ~(UINT64_C(0xff) << shift)) |
                                 ((uint64_t)expected[cc] << shift);
                char set_name[128];
                snprintf(set_name, sizeof(set_name), "%s SETcc=%02x high=%u imm8=%u",
                         test->name, conditions[cc] + 0x20, high, imm8);
                check_result(set_name, set_code, set_size, value);
            }
            /* SETcc must also preserve all flags for the following Jcc. */
            code[size++] = 0x0f;
            code[size++] = (uint8_t)(conditions[cc] + 0x20);
            code[size++] = 0xc2; /* DL */
            if (near) {
                code[size++] = 0x0f;
                code[size++] = (uint8_t)(conditions[cc] + 0x10);
                put_value(code, &size, 6, 4);
            } else {
                code[size++] = conditions[cc];
                code[size++] = 6;
            }
            mov_reg(code, &size, 0, 0, 4);
            code[size++] = 0xc3;
            mov_reg(code, &size, 0, 1, 4);
            code[size++] = 0xc3;
            char name[128];
            snprintf(name, sizeof(name), "%s cc=%02x near=%u imm8=%u",
                     test->name, conditions[cc], near, imm8);
            check_result(name, code, size, expected[cc]);
        }
    }
}

int main(void)
{
    const flag_case cases[] = {
        {"cmp64 equal", 0x39, 8, 42, 42, false, true, false, false, false},
        {"cmp64 less", 0x39, 8, 2, 42, false, false, true, false, true},
        {"cmp64 greater", 0x39, 8, 42, 2, false, false, false, false, false},
        {"cmp64 min versus 1", 0x39, 8, 0x8000000000000000ull, 1, false, false, false, true, false},
        {"cmp64 max versus -1", 0x39, 8, INT64_MAX, UINT64_MAX, false, false, true, true, true},
        {"cmp32 min versus 1", 0x39, 4, 0x80000000u, 1, false, false, false, true, false},
        {"cmp32 max versus -1", 0x39, 4, INT32_MAX, UINT32_MAX, false, false, true, true, true},
        {"add64 overflow", 0x01, 8, INT64_MAX, 1, false, false, true, true, false},
        {"add32 overflow", 0x01, 4, INT32_MAX, 1, false, false, true, true, false},
        {"add64 wrap zero", 0x01, 8, UINT64_MAX, 1, false, true, false, false, true},
        {"add32 wrap zero", 0x01, 4, UINT32_MAX, 1, false, true, false, false, true},
        {"sub64 overflow", 0x29, 8, 0x8000000000000000ull, 1, false, false, false, true, false},
        {"sub32 overflow", 0x29, 4, 0x80000000u, 1, false, false, false, true, false},
        {"xor64 zero", 0x31, 8, UINT64_MAX, UINT64_MAX, false, true, false, false, false},
        {"xor32 negative", 0x31, 4, 0, 0x80000000u, false, false, true, false, false},
        {"test64 zero", 0x85, 8, 0x10, 0x20, false, true, false, false, false},
        {"test64 negative", 0x85, 8, UINT64_MAX, 0x8000000000000000ull, false, false, true, false, false},
        {"test32 negative", 0x85, 4, UINT32_MAX, 0x80000000u, false, false, true, false, false},
        {"add64 imm negative overflow", 0xc0, 8, 0x8000000000000000ull, UINT64_MAX, true, false, false, true, true},
        {"sub64 imm negative overflow", 0xe8, 8, INT64_MAX, UINT64_MAX, true, false, true, true, true},
        {"cmp64 imm sign extend", 0xf8, 8, UINT64_MAX, UINT64_MAX, true, true, false, false, false},
        {"add32 imm negative overflow", 0xc0, 4, 0x80000000u, UINT32_MAX, true, false, false, true, true},
        {"sub32 imm minimum", 0xe8, 4, 0, 0x80000000u, true, false, true, true, true},
        {"cmp32 imm sign", 0xf8, 4, 0, UINT32_MAX, true, false, false, false, true},
        {"cmp64 unsigned greater signed less", 0x39, 8, UINT64_MAX, 0, false, false, true, false, false},
        {"cmp32 unsigned greater signed less", 0x39, 4, UINT32_MAX, 0, false, false, true, false, false},
        {"cmp64 borrow from zero", 0x39, 8, 0, UINT64_MAX, false, false, false, false, true},
        {"cmp32 borrow from zero", 0x39, 4, 0, UINT32_MAX, false, false, false, false, true},
        {"add64 carry nonzero", 0x01, 8, UINT64_MAX, 2, false, false, false, false, true},
        {"add32 carry nonzero", 0x01, 4, UINT32_MAX, 2, false, false, false, false, true},
        {"add64 no carry zero", 0x01, 8, 0, 0, false, true, false, false, false},
        {"add32 no carry zero", 0x01, 4, 0, 0, false, true, false, false, false},
        {"sub64 borrow", 0x29, 8, 0, 1, false, false, true, false, true},
        {"sub32 borrow", 0x29, 4, 0, 1, false, false, true, false, true},
        {"sub64 equality", 0x29, 8, UINT64_MAX, UINT64_MAX, false, true, false, false, false},
        {"sub32 equality", 0x29, 4, UINT32_MAX, UINT32_MAX, false, true, false, false, false},
        {"add64 imm8 range carry", 0xc0, 8, UINT64_MAX, 1, true, true, false, false, true},
        {"add32 imm8 range carry", 0xc0, 4, UINT32_MAX, 1, true, true, false, false, true},
        {"sub64 imm32 borrow", 0xe8, 8, 0, 1, true, false, true, false, true},
        {"sub32 imm32 borrow", 0xe8, 4, 0, 1, true, false, true, false, true},
        {"sub64 negative immediate no borrow", 0xe8, 8, UINT64_MAX, UINT64_MAX, true, true, false, false, false},
        {"sub32 negative immediate no borrow", 0xe8, 4, UINT32_MAX, UINT32_MAX, true, true, false, false, false},
        {"cmp64 negative immediate borrow", 0xf8, 8, 0, UINT64_MAX, true, false, false, false, true},
        {"xor64 nonzero clears carry", 0x31, 8, 1, 2, false, false, false, false, false},
        {"xor32 zero clears carry", 0x31, 4, UINT32_MAX, UINT32_MAX, false, true, false, false, false},
        {"test32 zero clears carry", 0x85, 4, 1, 2, false, true, false, false, false},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i += 1) {
        check_flags(&cases[i]);
    }

    const uint8_t loop[] = {0xb8, 0, 0, 0, 0, 0xb9, 3, 0, 0, 0,
                            0x01, 0xc8, 0x83, 0xe9, 1, 0x75, 0xf9, 0xc3};
    check_result("32-bit backward loop on SUB flags", loop, sizeof(loop), 6);
    const uint8_t jump[] = {0xeb, 6, 0xb8, 42, 0, 0, 0, 0xc3,
                            0xe9, 0xf5, 0xff, 0xff, 0xff};
    check_result("forward short and backward near JMP", jump, sizeof(jump), 42);
    const uint8_t initial[] = {0x75, 6, 0xb8, 0, 0, 0, 0, 0xc3,
                               0xb8, 1, 0, 0, 0, 0xc3};
    check_result("initial ZF is clear", initial, sizeof(initial), 1);
    const uint8_t unsigned_conditions[] = {0x72, 0x73, 0x76, 0x77};
    for (size_t i = 0; i < sizeof(unsigned_conditions); i += 1) {
        for (unsigned near = 0; near < 2; near += 1) {
            uint8_t code[24] = {0};
            size_t size = 0;
            if (near) {
                code[size++] = 0x0f;
                code[size++] = (uint8_t)(unsigned_conditions[i] + 0x10);
                put_value(code, &size, 6, 4);
            } else {
                code[size++] = unsigned_conditions[i];
                code[size++] = 6;
            }
            mov_reg(code, &size, 0, 0, 4);
            code[size++] = 0xc3;
            mov_reg(code, &size, 0, 1, 4);
            code[size++] = 0xc3;
            char name[64];
            snprintf(name, sizeof(name), "initial CF clear cc=%02x near=%u", unsigned_conditions[i], near);
            check_result(name, code, size, i == 1 || i == 3);
        }
    }
    const uint8_t unsigned_loop[] = {0xb8, 0, 0, 0, 0, 0xb9, 3, 0, 0, 0,
                                    0x83, 0xc0, 1, 0x39, 0xc8, 0x72, 0xf9, 0xc3};
    check_result("unsigned backward JB loop", unsigned_loop, sizeof(unsigned_loop), 3);
    const uint8_t zero_extend[] = {0x48, 0xb8, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                   0x83, 0xc0, 2, 0xc3};
    check_result("32-bit ADD clears upper half", zero_extend, sizeof(zero_extend), 1);
    const uint8_t mov32[] = {0x48, 0xb8, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                             0xb9, 42, 0, 0, 0, 0x89, 0xc8, 0xc3};
    check_result("32-bit MOV clears upper half", mov32, sizeof(mov32), 42);
    const uint8_t test_preserve[] = {0xb8, 42, 0, 0, 0, 0xb9, 1, 0, 0, 0, 0x85, 0xc8, 0xc3};
    check_result("TEST preserves operand", test_preserve, sizeof(test_preserve), 42);
    const uint8_t cmp_preserve[] = {0xb8, 42, 0, 0, 0, 0x83, 0xf8, 1, 0xc3};
    check_result("CMP preserves operand", cmp_preserve, sizeof(cmp_preserve), 42);
    const uint8_t infinite[] = {0xeb, 0xfe, 0xc3};
    check_error("infinite loop is bounded", infinite, sizeof(infinite), R2_ERR_UNSUPPORTED, "step limit");
    const uint8_t middle[] = {0xeb, 1, 0xb8, 42, 0, 0, 0, 0xc3};
    check_error("jump into immediate is rejected", middle, sizeof(middle), R2_ERR_DECODE, "branch target");
    const uint8_t falloff[] = {0xeb, 1, 0xc3, 0x90};
    check_error("fallthrough is reported", falloff, sizeof(falloff), R2_ERR_DECODE, "ran off");
    const uint8_t unsupported[] = {0x50, 0xc3};
    check_error("stack ops still reject cleanly", unsupported, sizeof(unsupported), R2_ERR_UNSUPPORTED, "does not support");
    printf("PASS jit_flags_and_branches (%u checks against interpreter and expected results)\n", checks);
    return 0;
}
