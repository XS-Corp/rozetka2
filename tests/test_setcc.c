#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;

static void put_value(uint8_t *code, size_t *size, uint64_t value, unsigned width)
{
    for (unsigned i = 0; i < width; i += 1) {
        code[(*size)++] = (uint8_t)(value >> (i * 8u));
    }
}

static void mov_imm(uint8_t *code, size_t *size, unsigned reg, uint64_t value)
{
    code[(*size)++] = (uint8_t)(0x48 | (reg >= 8 ? 1 : 0));
    code[(*size)++] = (uint8_t)(0xb8 | (reg & 7));
    put_value(code, size, value, 8);
}

static void mov_reg(uint8_t *code, size_t *size, unsigned dst, unsigned src)
{
    code[(*size)++] = (uint8_t)(0x48 | (src >= 8 ? 4 : 0) | (dst >= 8 ? 1 : 0));
    code[(*size)++] = 0x89;
    code[(*size)++] = (uint8_t)(0xc0 | ((src & 7) << 3) | (dst & 7));
}

static r2_status execute(const uint8_t *code, size_t size, r2_engine engine,
                          const r2_interp_env *env, r2_result *result, char *error)
{
    r2_block block;
    r2_status status = r2_decode_block(code, size, &block, error, R2_ERROR_CAP);
    if (status != R2_OK) {
        return status;
    }
    return engine == R2_ENGINE_INTERP
        ? r2_interp_block_with_env(&block, env, result, error, R2_ERROR_CAP)
        : r2_jit_block(&block, result, error, R2_ERROR_CAP);
}

static void expect_value(const char *name, const uint8_t *code, size_t size,
                          r2_engine engine, const r2_interp_env *env, uint64_t expected)
{
    r2_result result = {0};
    char error[R2_ERROR_CAP] = {0};
    r2_status status = execute(code, size, engine, env, &result, error);
    if (status != R2_OK || result.rax != expected) {
        fprintf(stderr, "FAIL %s engine=%u: status=%s rax=%llx expected=%llx (%s)\n",
                name, engine, r2_status_name(status), (unsigned long long)result.rax,
                (unsigned long long)expected, error);
        exit(1);
    }
    checks += 1;
}

static void expect_error(const uint8_t *code, size_t size, r2_engine engine,
                          const r2_interp_env *env, r2_status expected, const char *message)
{
    r2_result result = {.rax = 99};
    char error[R2_ERROR_CAP] = {0};
    r2_status status = execute(code, size, engine, env, &result, error);
    if (status != expected || strstr(error, message) == NULL || result.rax != 99) {
        fprintf(stderr, "FAIL SETcc error: status=%s expected=%s (%s), wanted '%s'\n",
                r2_status_name(status), r2_status_name(expected), error, message);
        exit(1);
    }
    checks += 1;
}

static void check_registers(void)
{
    const uint8_t prefixes[] = {0, 0x40, 0x44, 0x48, 0x41, 0x4f};
    const uint64_t initial = UINT64_C(0x12345678abcdef88);
    for (size_t p = 0; p < sizeof(prefixes); p += 1) {
        for (unsigned rm = 0; rm < 8; rm += 1) {
            bool high = prefixes[p] == 0 && rm >= 4;
            unsigned dst = high ? rm - 4 : rm + ((prefixes[p] & 1) ? 8 : 0);
            unsigned shift = high ? 8 : 0;
            unsigned saved_sp = dst == 15 ? 14 : 15;
            for (unsigned group = 0; group < 8; group += 1) {
                for (unsigned value = 0; value < 2; value += 1) {
                    uint8_t code[64];
                    size_t size = 0;
                    mov_reg(code, &size, saved_sp, R2_X86_RSP);
                    mov_imm(code, &size, dst, initial);
                    code[size++] = 0x48; /* cmpq %rax, %rax: ZF = 1 */
                    code[size++] = 0x39;
                    code[size++] = 0xc0;
                    if (prefixes[p] != 0) {
                        code[size++] = prefixes[p];
                    }
                    code[size++] = 0x0f;
                    code[size++] = value ? 0x94 : 0x95;
                    code[size++] = (uint8_t)(0xc0 | (group << 3) | rm);
                    mov_reg(code, &size, R2_X86_RAX, dst);
                    mov_reg(code, &size, R2_X86_RSP, saved_sp);
                    code[size++] = 0xc3;
                    uint64_t expected = (initial & ~(UINT64_C(0xff) << shift)) |
                                        ((uint64_t)value << shift);
                    char name[96];
                    snprintf(name, sizeof(name), "SETcc prefix=%02x rm=%u /%u value=%u",
                             prefixes[p], rm, group, value);
                    expect_value(name, code, size, R2_ENGINE_INTERP, NULL, expected);
                    expect_value(name, code, size, R2_ENGINE_JIT, NULL, expected);
                }
            }
        }
    }
}

static void check_memory(void)
{
    const uint64_t base = UINT64_C(0x200000000);
    const uint64_t rip_base = base + 0x1000;
    /* base, disp8, disp32, SIB index, RIP, absolute SIB, GS, extended SIB */
    for (unsigned mode = 0; mode < 8; mode += 1) {
        for (unsigned value = 0; value < 2; value += 1) {
            uint8_t data[16];
            memset(data, 0xa5, sizeof(data));
            r2_memory_region region = {.base = base, .size = sizeof(data), .data = data, .writable = true};
            r2_interp_env env = {.rip_base = rip_base, .regions = &region, .region_count = 1};
            uint8_t code[128];
            size_t size = 0;
            mov_imm(code, &size, R2_X86_RBX, mode == 0 ? base + 7 : base);
            mov_imm(code, &size, R2_X86_RCX, 2);
            if (mode == 5) {
                /* Absolute disp32 sign extends; use a low mapped address. */
                region.base = 0x2000;
            } else if (mode == 6) {
                mov_imm(code, &size, R2_X86_RDI, base);
                mov_imm(code, &size, R2_X86_RSI, 0);
                mov_imm(code, &size, R2_X86_RAX, 0x3000003); /* thread_set_tsd_base */
                code[size++] = 0x0f;
                code[size++] = 0x05;
            } else if (mode == 7) {
                mov_imm(code, &size, R2_X86_R12, base);
                mov_imm(code, &size, R2_X86_R9, 2);
            }
            code[size++] = 0x31; /* xor eax, eax: ZF = 1, CF = 0 */
            code[size++] = 0xc0;
            if (mode == 6) {
                code[size++] = 0x65;
            } else if (mode == 7) {
                code[size++] = 0x43; /* REX.X/B */
            }
            code[size++] = 0x0f;
            code[size++] = value ? 0x94 : 0x95;
            switch (mode) {
            case 0: code[size++] = 0x03; break; /* [rbx] */
            case 1: code[size++] = 0x43; code[size++] = 7; break;
            case 2: code[size++] = 0x83; put_value(code, &size, 7, 4); break;
            case 3: /* [rbx + rcx*4 - 1] */
                code[size++] = 0x44; code[size++] = 0x8b; code[size++] = 0xff; break;
            case 4:
                code[size++] = 0x05;
                put_value(code, &size, base + 7 - (rip_base + size + 4), 4);
                break;
            case 5: case 6:
                code[size++] = 0x04; code[size++] = 0x25;
                put_value(code, &size, mode == 5 ? region.base + 7 : 7, 4);
                break;
            case 7: /* [r12 + r9*4 - 1] */
                code[size++] = 0x44; code[size++] = 0x8c; code[size++] = 0xff; break;
            }
            code[size++] = 0x0f; code[size++] = 0x94; code[size++] = 0xc0; /* sete al */
            code[size++] = 0xc3;
            expect_value("SETcc memory and flags", code, size, R2_ENGINE_INTERP, &env, 1);
            for (size_t i = 0; i < sizeof(data); i += 1) {
                if (data[i] != (i == 7 ? value : 0xa5)) {
                    fprintf(stderr, "FAIL SETcc memory mode=%u value=%u byte=%zu\n", mode, value, i);
                    exit(1);
                }
            }
            /* Exactly one mapped byte is sufficient; neighbours stay untouched. */
            region.base += 7;
            region.data += 7;
            region.size = 1;
            expect_value("SETcc single-byte mapping", code, size, R2_ENGINE_INTERP, &env, 1);
            region.writable = false;
            expect_error(code, size, R2_ENGINE_INTERP, &env, R2_ERR_UNSUPPORTED, "read-only");
            region.base += 1;
            region.writable = true;
            expect_error(code, size, R2_ENGINE_INTERP, &env, R2_ERR_UNSUPPORTED, "not mapped");
        }
    }
    const uint8_t stack[] = {0x48, 0x83, 0xec, 0x10,
        0x48, 0xc7, 0x04, 0x24, 0x88, 0x88, 0x88, 0x88,
        0x31, 0xc0, 0x0f, 0x94, 0x44, 0x24, 1,
        0x48, 0x8b, 0x04, 0x24, 0x48, 0x83, 0xc4, 0x10, 0xc3};
    expect_value("SETcc stack byte", stack, sizeof(stack), R2_ENGINE_INTERP, NULL,
                 UINT64_C(0xffffffff88880188));
}

int main(void)
{
    check_registers();
    check_memory();
    const uint8_t parity[] = {0x31, 0xc0, 0x0f, 0x9a, 0xc0, 0xc3};
    expect_value("parity after zero result", parity, sizeof(parity), R2_ENGINE_INTERP, NULL, 1);
    expect_error(parity, sizeof(parity), R2_ENGINE_JIT, NULL, R2_ERR_UNSUPPORTED, "condition");
    const uint8_t modrm[] = {0x0f, 0x94};
    const uint8_t sib[] = {0x0f, 0x94, 0x04};
    const uint8_t disp[] = {0x0f, 0x94, 0x05, 0};
    const uint8_t disp8[] = {0x0f, 0x94, 0x43};
    const uint8_t unsupported[] = {0x0f, 0x0b, 0xc3}; /* UD2 remains unsupported */
    const uint8_t memory[] = {0x0f, 0x94, 0x00, 0xc3};
    expect_error(modrm, sizeof(modrm), R2_ENGINE_INTERP, NULL, R2_ERR_DECODE, "ModRM");
    expect_error(sib, sizeof(sib), R2_ENGINE_INTERP, NULL, R2_ERR_DECODE, "SIB");
    expect_error(disp, sizeof(disp), R2_ENGINE_INTERP, NULL, R2_ERR_DECODE, "disp32");
    expect_error(disp8, sizeof(disp8), R2_ENGINE_INTERP, NULL, R2_ERR_DECODE, "disp8");
    expect_error(unsupported, sizeof(unsupported), R2_ENGINE_INTERP, NULL, R2_ERR_UNSUPPORTED, "unsupported 0f");
    expect_error(memory, sizeof(memory), R2_ENGINE_JIT, NULL, R2_ERR_UNSUPPORTED, "SETcc memory");
    printf("PASS setcc_registers_and_memory (%u checks)\n", checks);
    return 0;
}
