#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;

typedef struct {
    uint64_t value;
    uint64_t flags;
} expected_result;

static expected_result reference(uint64_t lhs, uint64_t rhs, unsigned carry,
                                  unsigned width, bool subtract)
{
    /* Use mathematical 128-bit arithmetic, independent of the interpreter's
     * bitwise overflow and two-stage carry calculations. */
    __int128 modulus = (__int128)1 << (width * 8);
    uint64_t mask = width == 4 ? UINT32_MAX : UINT64_MAX;
    uint64_t sign = UINT64_C(1) << (width * 8 - 1);
    lhs &= mask;
    rhs &= mask;
    __int128 signed_lhs = (__int128)lhs - ((lhs & sign) ? modulus : 0);
    __int128 signed_rhs = (__int128)rhs - ((rhs & sign) ? modulus : 0);
    __int128 signed_result = subtract ? signed_lhs - signed_rhs - carry
                                      : signed_lhs + signed_rhs + carry;
    __uint128_t unsigned_result = (__uint128_t)lhs + rhs + carry;
    bool cf = subtract ? (__uint128_t)lhs < (__uint128_t)rhs + carry
                       : unsigned_result >= (__uint128_t)modulus;
    uint64_t value = (subtract ? lhs - rhs - carry : (uint64_t)unsigned_result) & mask;
    bool of = signed_result < -modulus / 2 || signed_result >= modulus / 2;
    bool sf = (value & sign) != 0;
    bool zf = value == 0;
    return (expected_result){value, (uint64_t)cf | ((uint64_t)zf << 1) |
                                   ((uint64_t)(sf != of) << 2)};
}

static void put_value(uint8_t *code, size_t *size, uint64_t value, unsigned width)
{
    for (unsigned i = 0; i < width; i += 1) {
        code[(*size)++] = (uint8_t)(value >> (8 * i));
    }
}

static void mov_imm(uint8_t *code, size_t *size, unsigned reg, uint64_t value)
{
    code[(*size)++] = (uint8_t)(0x48 | (reg >= 8 ? 1 : 0));
    code[(*size)++] = (uint8_t)(0xb8 | (reg & 7));
    put_value(code, size, value, 8);
}

static void reg_op(uint8_t *code, size_t *size, uint8_t opcode,
                    unsigned rm, unsigned reg, unsigned width)
{
    uint8_t rex = (uint8_t)(0x40 | (width == 8 ? 8 : 0) |
                             (reg >= 8 ? 4 : 0) | (rm >= 8 ? 1 : 0));
    if (rex != 0x40) {
        code[(*size)++] = rex;
    }
    code[(*size)++] = opcode;
    code[(*size)++] = (uint8_t)(0xc0 | ((reg & 7) << 3) | (rm & 7));
}

static void check(const char *name, const uint8_t *code, size_t size, uint64_t expected)
{
    r2_block block;
    char error[R2_ERROR_CAP] = {0};
    r2_status status = r2_decode_block(code, size, &block, error, sizeof(error));
    if (status != R2_OK) {
        fprintf(stderr, "FAIL %s decode: %s\n", name, error);
        exit(1);
    }
    for (unsigned engine = R2_ENGINE_INTERP; engine <= R2_ENGINE_JIT; engine += 1) {
        r2_result result = {0};
        status = engine == R2_ENGINE_INTERP
            ? r2_interp_block(&block, &result, error, sizeof(error))
            : r2_jit_block(&block, &result, error, sizeof(error));
        if (status != R2_OK || result.rax != expected) {
            fprintf(stderr, "FAIL %s engine=%u status=%s value=%llx expected=%llx (%s)\n",
                    name, engine, r2_status_name(status), (unsigned long long)result.rax,
                    (unsigned long long)expected, error);
            exit(1);
        }
    }
    checks += 1;
}

static void check_case(uint64_t lhs, uint64_t rhs, unsigned carry, unsigned width, bool subtract)
{
    uint64_t mask = width == 4 ? UINT32_MAX : UINT64_MAX;
    lhs &= mask;
    rhs &= mask;
    expected_result expected = reference(lhs, rhs, carry, width, subtract);
    /* Both ModRM directions, imm8, imm32, accumulator, extended registers,
     * and an aliased source/destination. */
    for (unsigned form = 0; form < 8; form += 1) {
        if (form == 2 && (((uint64_t)(int64_t)(int8_t)rhs ^ rhs) & mask) != 0) {
            continue;
        }
        if ((form == 3 || form == 4) &&
            (((uint64_t)(int64_t)(int32_t)rhs ^ rhs) & mask) != 0) {
            continue;
        }
        if (form == 7 && lhs != rhs) {
            continue;
        }
        unsigned dst = form == 5 || form == 6 ? 8 : 0;
        unsigned src = form == 7 ? dst : (dst == 8 ? 9 : 1);
        uint8_t code[256];
        size_t size = 0;
        mov_imm(code, &size, 10, 0);
        mov_imm(code, &size, 11, carry);
        reg_op(code, &size, 0x39, 10, 11, 8); /* CF = (0 < carry) */
        uint64_t dirty_upper = width == 4 ? UINT64_C(0xfeedbeef00000000) : 0;
        mov_imm(code, &size, dst, lhs | dirty_upper);
        if (form != 7) {
            mov_imm(code, &size, src, rhs | dirty_upper);
        }
        if (form == 2 || form == 3 || form == 4) {
            if (width == 8) {
                code[size++] = 0x48;
            }
            if (form == 4) {
                code[size++] = subtract ? 0x1d : 0x15;
            } else {
                code[size++] = form == 2 ? 0x83 : 0x81;
                code[size++] = subtract ? 0xd8 : 0xd0;
            }
            put_value(code, &size, rhs, form == 2 ? 1 : 4);
        } else if (form == 1 || form == 6) {
            reg_op(code, &size, subtract ? 0x1b : 0x13, src, dst, width);
        } else {
            reg_op(code, &size, subtract ? 0x19 : 0x11, dst, src, width);
        }
        size_t arithmetic_end = size;
        if (dst != 0) {
            reg_op(code, &size, 0x89, 0, dst, 8);
        }
        code[size++] = 0xc3;
        char name[160];
        snprintf(name, sizeof(name), "%s%u lhs=%llx rhs=%llx CF=%u form=%u result",
                 subtract ? "sbb" : "adc", width * 8, (unsigned long long)lhs,
                 (unsigned long long)rhs, carry, form);
        check(name, code, size, expected.value);

        size = arithmetic_end;
        /* Save CF, ZF and SF^OF before doing arithmetic to pack them into RAX.
         * Together with the checked result's sign these also determine OF. */
        mov_imm(code, &size, 2, 0);
        code[size++] = 0x0f; code[size++] = 0x9c; code[size++] = 0xc2; /* setl dl */
        mov_imm(code, &size, 1, 0);
        code[size++] = 0x0f; code[size++] = 0x94; code[size++] = 0xc1; /* sete cl */
        mov_imm(code, &size, 0, 0);
        code[size++] = 0x0f; code[size++] = 0x92; code[size++] = 0xc0; /* setb al */
        reg_op(code, &size, 0x01, 1, 1, 8);
        reg_op(code, &size, 0x01, 2, 2, 8);
        reg_op(code, &size, 0x01, 2, 2, 8);
        reg_op(code, &size, 0x01, 0, 1, 8);
        reg_op(code, &size, 0x01, 0, 2, 8);
        code[size++] = 0xc3;
        snprintf(name, sizeof(name), "%s%u lhs=%llx rhs=%llx CF=%u form=%u flags",
                 subtract ? "sbb" : "adc", width * 8, (unsigned long long)lhs,
                 (unsigned long long)rhs, carry, form);
        check(name, code, size, expected.flags);
    }
}

static void check_chains(void)
{
    /* Two-limb arithmetic; MOV and SETcc between limbs must preserve carry. */
    const uint8_t adc[] = {0x48, 0xb8, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0x48, 0x83, 0xc0, 1, 0x0f, 0x92, 0xc2, 0xb8, 41, 0, 0, 0,
        0x48, 0x83, 0xd0, 0, 0xc3};
    check("ADD to ADC across SETcc/MOV", adc, sizeof(adc), 42);
    const uint8_t sbb[] = {0x31, 0xc0, 0x48, 0x83, 0xe8, 1,
        0x0f, 0x92, 0xc2, 0xb8, 43, 0, 0, 0, 0x48, 0x83, 0xd8, 0, 0xc3};
    check("SUB to SBB across SETcc/MOV", sbb, sizeof(sbb), 42);
    const uint8_t adc_repeat[] = {0xb8, 0, 0, 0, 0, 0x48, 0x83, 0xe8, 1,
        0x48, 0x83, 0xd0, 0, 0x48, 0x83, 0xd0, 0, 0xc3};
    check("ADC carry propagates then clears", adc_repeat, sizeof(adc_repeat), 1);
    const uint8_t sbb_repeat[] = {0x31, 0xc0, 0x48, 0x83, 0xe8, 1,
        0xb8, 0, 0, 0, 0, 0x48, 0x83, 0xd8, 0, 0xb8, 43, 0, 0, 0,
        0x48, 0x83, 0xd8, 0, 0xc3};
    check("SBB borrow propagates", sbb_repeat, sizeof(sbb_repeat), 42);
    const uint8_t mixed[] = {0x31, 0xc0, 0x48, 0x83, 0xe8, 1,
        0xb8, 0, 0, 0, 0, 0x48, 0x83, 0xd8, 0,
        0x48, 0x83, 0xd0, 0, 0x48, 0x83, 0xd8, 0, 0xc3};
    check("mixed SBB ADC SBB carry convention", mixed, sizeof(mixed), UINT64_MAX);
    const uint8_t initial_adc[] = {0xb8, 42, 0, 0, 0, 0x83, 0xd0, 0, 0xc3};
    check("ADC initial carry is clear", initial_adc, sizeof(initial_adc), 42);
    const uint8_t initial_sbb[] = {0xb8, 42, 0, 0, 0, 0x83, 0xd8, 0, 0xc3};
    check("SBB initial carry is clear", initial_sbb, sizeof(initial_sbb), 42);
}

static void check_rejected_encodings(void)
{
    const struct {
        uint8_t bytes[8];
        size_t size;
        r2_status expected;
    } cases[] = {
        {{0x11}, 1, R2_ERR_DECODE},
        {{0x1b}, 1, R2_ERR_DECODE},
        {{0x48, 0x83, 0xd0}, 3, R2_ERR_DECODE},
        {{0x48, 0x81, 0xd8, 1}, 4, R2_ERR_DECODE},
        {{0x48, 0x15, 1}, 3, R2_ERR_DECODE},
        {{0x1d, 1}, 2, R2_ERR_DECODE},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i += 1) {
        r2_block block;
        char error[R2_ERROR_CAP] = {0};
        if (r2_decode_block(cases[i].bytes, cases[i].size, &block, error, sizeof(error)) != cases[i].expected) {
            fprintf(stderr, "FAIL ADC/SBB rejected encoding %zu: %s\n", i, error);
            exit(1);
        }
        checks += 1;
    }
}

int main(void)
{
    const uint64_t values[] = {0, 1, 2, 0x7f, 0x80, 0x7fffffff, 0x80000000,
        0x7ffffffffffffffeull, INT64_MAX, 0x8000000000000000ull,
        0xffffffff80000000ull, UINT64_MAX - 1, UINT64_MAX};
    for (unsigned width = 4; width <= 8; width += 4) {
        for (unsigned subtract = 0; subtract < 2; subtract += 1) {
            for (unsigned carry = 0; carry < 2; carry += 1) {
                for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i += 1) {
                    for (size_t j = 0; j < sizeof(values) / sizeof(values[0]); j += 1) {
                        check_case(values[i], values[j], carry, width, subtract);
                    }
                }
            }
        }
    }
    check_chains();
    check_rejected_encodings();
    printf("PASS adc_sbb (%u checks against interpreter, JIT and 128-bit reference)\n", checks);
    return 0;
}
