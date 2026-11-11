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
        fprintf(stderr, "FAIL extension error: status=%s expected=%s (%s), wanted '%s'\n",
                r2_status_name(status), r2_status_name(expected), error, message);
        exit(1);
    }
    checks += 1;
}

typedef struct {
    uint8_t opcode;
    uint8_t src_width;
    uint8_t width;
    bool signed_value;
} extension;

static const extension forms[] = {
    {0xb6, 1, 4, false}, {0xb6, 1, 8, false},
    {0xb7, 2, 4, false}, {0xb7, 2, 8, false},
    {0xbe, 1, 4, true}, {0xbe, 1, 8, true},
    {0xbf, 2, 4, true}, {0xbf, 2, 8, true}, {0x63, 4, 8, true},
};

static uint64_t reference(extension form, uint64_t value)
{
    /* Compute the signed mathematical value before applying destination width. */
    uint64_t modulus = UINT64_C(1) << (form.src_width * 8);
    int64_t result = (int64_t)(value % modulus);
    if (form.signed_value && (uint64_t)result >= modulus / 2) {
        result -= (int64_t)modulus;
    }
    return form.width == 4 ? (uint32_t)result : (uint64_t)result;
}

static void emit_extension(uint8_t *code, size_t *size, extension form,
                            unsigned dst, unsigned encoded_src, bool high)
{
    uint8_t rex = (uint8_t)(0x40 | (form.width == 8 ? 8 : 0) |
                           (dst >= 8 ? 4 : 0) | (encoded_src >= 8 ? 1 : 0));
    if (rex != 0x40 || (!high && form.src_width == 1 && encoded_src >= 4)) {
        code[(*size)++] = rex;
    }
    if (form.opcode != 0x63) {
        code[(*size)++] = 0x0f;
    }
    code[(*size)++] = form.opcode;
    code[(*size)++] = (uint8_t)(0xc0 | ((dst & 7) << 3) | (encoded_src & 7));
}

static void seed_flags(uint8_t *code, size_t *size, unsigned scratch, unsigned state)
{
    mov_imm(code, size, scratch, state == 2 ? INT64_MAX : 0);
    /* CMP self: ZF. SUB 1 from zero: CF/SF. SUB -1 from signed maximum:
     * CF/SF/OF. These states distinguish sign from overflow. */
    uint8_t rex = (uint8_t)(0x48 | (scratch >= 8 ? 1 : 0));
    code[(*size)++] = rex;
    code[(*size)++] = 0x83;
    code[(*size)++] = (uint8_t)((state == 0 ? 0xf8 : 0xe8) | (scratch & 7));
    code[(*size)++] = state == 0 ? 0 : (state == 2 ? 0xff : 1);
}

static void flag_result(uint8_t *code, size_t *size)
{
    mov_imm(code, size, 2, 0);
    code[(*size)++] = 0x0f; code[(*size)++] = 0x9c; code[(*size)++] = 0xc2;
    mov_imm(code, size, 1, 0);
    code[(*size)++] = 0x0f; code[(*size)++] = 0x94; code[(*size)++] = 0xc1;
    mov_imm(code, size, 0, 0);
    code[(*size)++] = 0x0f; code[(*size)++] = 0x92; code[(*size)++] = 0xc0;
    const uint8_t pack[] = {0x48, 0x01, 0xc9, 0x48, 0x01, 0xd2,
        0x48, 0x01, 0xd2, 0x48, 0x01, 0xc8, 0x48, 0x01, 0xd0};
    memcpy(code + *size, pack, sizeof(pack));
    *size += sizeof(pack);
}

static void check_register_case(extension form, unsigned dst, unsigned src,
                                 bool high, uint64_t value, unsigned state)
{
    unsigned saved_sp = 15;
    while (saved_sp == dst || saved_sp == src) {
        saved_sp -= 1;
    }
    unsigned scratch = 12;
    while (scratch == dst || scratch == src || scratch == saved_sp) {
        scratch -= 1;
    }
    uint8_t code[256];
    size_t size = 0;
    mov_reg(code, &size, saved_sp, R2_X86_RSP);
    mov_imm(code, &size, dst, UINT64_C(0x8877665544332211));
    uint64_t mask = (UINT64_C(1) << (form.src_width * 8)) - 1;
    uint64_t source = high ? (UINT64_C(0xfedcba98765400a5) | ((value & 0xff) << 8))
                           : (UINT64_C(0xfedcba9876543210) & ~mask) | (value & mask);
    mov_imm(code, &size, src, source);
    seed_flags(code, &size, scratch, state);
    emit_extension(code, &size, form, dst, high ? src + 4 : src, high);
    size_t extension_end = size;
    mov_reg(code, &size, R2_X86_RAX, dst);
    mov_reg(code, &size, R2_X86_RSP, saved_sp);
    code[size++] = 0xc3;
    char name[128];
    snprintf(name, sizeof(name), "extend op=%02x width=%u dst=%u src=%u high=%u value=%llx",
             form.opcode, form.width, dst, src, high, (unsigned long long)value);
    uint64_t expected = reference(form, value);
    expect_value(name, code, size, R2_ENGINE_INTERP, NULL, expected);
    expect_value(name, code, size, R2_ENGINE_JIT, NULL, expected);

    /* Restore RSP before reusing registers to capture flags. */
    size = extension_end;
    mov_reg(code, &size, R2_X86_RSP, saved_sp);
    flag_result(code, &size);
    code[size++] = 0xc3;
    const uint64_t flag_values[] = {2, 5, 1}; /* CF | ZF<<1 | (SF^OF)<<2 */
    expect_value("extension preserves flags", code, size, R2_ENGINE_INTERP, NULL, flag_values[state]);
    expect_value("extension preserves flags", code, size, R2_ENGINE_JIT, NULL, flag_values[state]);
}

static void check_registers(void)
{
    const uint64_t values[] = {0, 1, 0x7f, 0x80, 0xff, 0x7fff, 0x8000,
                               0xffff, 0x7fffffff, 0x80000000, UINT32_MAX};
    for (size_t f = 0; f < sizeof(forms) / sizeof(forms[0]); f += 1) {
        for (unsigned dst = 0; dst < 16; dst += 1) {
            for (unsigned src = 0; src < 16; src += 1) {
                for (size_t v = 0; v < sizeof(values) / sizeof(values[0]); v += 1) {
                    check_register_case(forms[f], dst, src, false, values[v], (dst + src + v) % 3);
                }
            }
        }
    }
    for (unsigned sign = 0; sign < 2; sign += 1) {
        extension form = {sign ? 0xbe : 0xb6, 1, 4, sign != 0};
        for (unsigned dst = 0; dst < 8; dst += 1) {
            for (unsigned src = 0; src < 4; src += 1) {
                for (size_t v = 0; v < sizeof(values) / sizeof(values[0]); v += 1) {
                    check_register_case(form, dst, src, true, values[v], v % 3);
                }
            }
        }
    }
}

static void check_memory(void)
{
    const uint64_t base = UINT64_C(0x200000000);
    const uint64_t rip_base = base + 0x1000;
    const uint64_t values[] = {0, 0x7f, 0x80, 0x8000, 0x80000000, UINT32_MAX};
    for (size_t f = 0; f < sizeof(forms) / sizeof(forms[0]); f += 1) {
        extension form = forms[f];
        for (unsigned mode = 0; mode < 8; mode += 1) {
            for (size_t v = 0; v < sizeof(values) / sizeof(values[0]); v += 1) {
                uint8_t data[16];
                memset(data, 0xa5, sizeof(data));
                for (unsigned i = 0; i < form.src_width; i += 1) {
                    data[7 + i] = (uint8_t)(values[v] >> (i * 8));
                }
                uint8_t original[16];
                memcpy(original, data, sizeof(data));
                r2_memory_region region = {.base = base, .size = sizeof(data), .data = data, .writable = false};
                r2_interp_env env = {.rip_base = rip_base, .regions = &region, .region_count = 1};
                uint8_t code[256];
                size_t size = 0;
                mov_imm(code, &size, R2_X86_RBX, mode == 0 ? base + 7 : base);
                mov_imm(code, &size, R2_X86_RCX, 2);
                if (mode == 5) {
                    region.base = 0x2000;
                } else if (mode == 6) {
                    mov_imm(code, &size, R2_X86_RDI, base);
                    mov_imm(code, &size, R2_X86_RSI, 0);
                    mov_imm(code, &size, R2_X86_RAX, 0x3000003);
                    code[size++] = 0x0f; code[size++] = 0x05;
                } else if (mode == 7) {
                    mov_imm(code, &size, R2_X86_R12, base);
                    mov_imm(code, &size, R2_X86_R9, 2);
                }
                seed_flags(code, &size, 10, v % 3);
                if (mode == 6) {
                    code[size++] = 0x65;
                }
                uint8_t rex = (uint8_t)(0x40 | (form.width == 8 ? 8 : 0) | (mode == 7 ? 3 : 0));
                if (rex != 0x40) {
                    code[size++] = rex;
                }
                if (form.opcode != 0x63) {
                    code[size++] = 0x0f;
                }
                code[size++] = form.opcode;
                /* Alias the destination with RBX in the base-only case. */
                unsigned dst = mode == 0 ? 3 : 0;
                unsigned reg_bits = dst << 3;
                switch (mode) {
                case 0: code[size++] = (uint8_t)(reg_bits | 0x03); break;
                case 1: code[size++] = 0x43; code[size++] = 7; break;
                case 2: code[size++] = 0x83; put_value(code, &size, 7, 4); break;
                case 3: code[size++] = 0x44; code[size++] = 0x8b; code[size++] = 0xff; break;
                case 4:
                    code[size++] = 0x05;
                    put_value(code, &size, base + 7 - (rip_base + size + 4), 4);
                    break;
                case 5: case 6:
                    code[size++] = 0x04; code[size++] = 0x25;
                    put_value(code, &size, mode == 5 ? region.base + 7 : 7, 4);
                    break;
                case 7: code[size++] = 0x44; code[size++] = 0x8c; code[size++] = 0xff; break;
                }
                size_t extension_end = size;
                mov_reg(code, &size, R2_X86_RAX, dst);
                code[size++] = 0xc3;
                expect_value("extending memory load", code, size, R2_ENGINE_INTERP, &env,
                             reference(form, values[v]));
                region.base += 7;
                region.data += 7;
                region.size = form.src_width;
                expect_value("exact-width memory load", code, size, R2_ENGINE_INTERP, &env,
                             reference(form, values[v]));
                region.size -= 1;
                expect_error(code, size, R2_ENGINE_INTERP, &env, R2_ERR_UNSUPPORTED, "not mapped");
                region.size += 1;
                size = extension_end;
                flag_result(code, &size);
                code[size++] = 0xc3;
                const uint64_t flag_values[] = {2, 5, 1};
                expect_value("memory extension preserves flags", code, size, R2_ENGINE_INTERP,
                             &env, flag_values[v % 3]);
                if (memcmp(original, data, sizeof(data)) != 0) {
                    fprintf(stderr, "FAIL extension changed source memory\n");
                    exit(1);
                }
            }
        }
    }
}

int main(void)
{
    check_registers();
    check_memory();
    const uint8_t modrm[] = {0x0f, 0xbe};
    const uint8_t sib[] = {0x0f, 0xb7, 0x04};
    const uint8_t disp[] = {0x48, 0x63, 0x05, 0};
    const uint8_t no_w[] = {0x63, 0xc0, 0xc3};
    const uint8_t word_dst[] = {0x66, 0x0f, 0xbe, 0xc0, 0xc3};
    const uint8_t memory[] = {0x48, 0x0f, 0xbe, 0x00, 0xc3};
    expect_error(modrm, sizeof(modrm), R2_ENGINE_INTERP, NULL, R2_ERR_DECODE, "ModRM");
    expect_error(sib, sizeof(sib), R2_ENGINE_INTERP, NULL, R2_ERR_DECODE, "SIB");
    expect_error(disp, sizeof(disp), R2_ENGINE_INTERP, NULL, R2_ERR_DECODE, "disp32");
    expect_error(no_w, sizeof(no_w), R2_ENGINE_INTERP, NULL, R2_ERR_UNSUPPORTED, "REX.W");
    expect_error(word_dst, sizeof(word_dst), R2_ENGINE_INTERP, NULL, R2_ERR_UNSUPPORTED, "opcode");
    expect_error(memory, sizeof(memory), R2_ENGINE_JIT, NULL, R2_ERR_UNSUPPORTED, "extending memory loads");
    printf("PASS movzx_movsx_movsxd (%u checks)\n", checks);
    return 0;
}
