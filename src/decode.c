#include "internal.h"

#include <inttypes.h>
#include <limits.h>
#include <string.h>

typedef struct {
    bool present;
    bool w;
    bool r;
    bool x;
    bool b;
    bool operand16;
    uint8_t repeat;
} rex_prefix;

typedef struct {
    uint8_t raw_group;
    uint8_t reg;
    bool rm_is_reg;
    uint8_t rm;
    r2_mem_ref mem;
} modrm_decoded;

static bool read_u8(const uint8_t *code, size_t code_len, size_t *ip, uint8_t *out)
{
    if (*ip >= code_len) {
        return false;
    }
    *out = code[*ip];
    *ip += 1;
    return true;
}

static bool read_i8(const uint8_t *code, size_t code_len, size_t *ip, int64_t *out)
{
    uint8_t raw = 0;
    if (!read_u8(code, code_len, ip, &raw)) {
        return false;
    }
    *out = (int8_t)raw;
    return true;
}

static bool read_i32(const uint8_t *code, size_t code_len, size_t *ip, int64_t *out)
{
    if (code_len - *ip < 4) {
        return false;
    }

    uint32_t raw = ((uint32_t)code[*ip]) |
                   ((uint32_t)code[*ip + 1] << 8) |
                   ((uint32_t)code[*ip + 2] << 16) |
                   ((uint32_t)code[*ip + 3] << 24);
    *ip += 4;
    *out = (int32_t)raw;
    return true;
}

static bool read_u32(const uint8_t *code, size_t code_len, size_t *ip, uint32_t *out)
{
    if (code_len - *ip < 4) {
        return false;
    }

    *out = ((uint32_t)code[*ip]) |
           ((uint32_t)code[*ip + 1] << 8) |
           ((uint32_t)code[*ip + 2] << 16) |
           ((uint32_t)code[*ip + 3] << 24);
    *ip += 4;
    return true;
}

static bool read_i64(const uint8_t *code, size_t code_len, size_t *ip, int64_t *out)
{
    if (code_len - *ip < 8) {
        return false;
    }

    uint64_t raw = ((uint64_t)code[*ip]) |
                   ((uint64_t)code[*ip + 1] << 8) |
                   ((uint64_t)code[*ip + 2] << 16) |
                   ((uint64_t)code[*ip + 3] << 24) |
                   ((uint64_t)code[*ip + 4] << 32) |
                   ((uint64_t)code[*ip + 5] << 40) |
                   ((uint64_t)code[*ip + 6] << 48) |
                   ((uint64_t)code[*ip + 7] << 56);
    *ip += 8;
    *out = (int64_t)raw;
    return true;
}

static rex_prefix read_rex(const uint8_t *code, size_t code_len, size_t *ip)
{
    rex_prefix rex = {0};

    while (*ip < code_len && code[*ip] >= 0x40 && code[*ip] <= 0x4f) {
        uint8_t raw = code[*ip];
        *ip += 1;
        rex.present = true;
        rex.w = (raw & 0x08) != 0;
        rex.r = (raw & 0x04) != 0;
        rex.x = (raw & 0x02) != 0;
        rex.b = (raw & 0x01) != 0;
    }

    return rex;
}

static r2_status append_insn(r2_block *block,
                             const r2_insn *insn,
                             char *error,
                             size_t error_len)
{
    if (block->count >= R2_MAX_INSNS) {
        r2_set_error(error, error_len, "too many instructions in one block");
        return R2_ERR_UNSUPPORTED;
    }

    block->insns[block->count] = *insn;
    block->count += 1;
    return R2_OK;
}

static r2_status require_rex_w(rex_prefix rex,
                               size_t offset,
                               char *error,
                               size_t error_len)
{
    if (rex.w) {
        return R2_OK;
    }

    r2_set_error(error,
                 error_len,
                 "only 64-bit forms are supported; missing REX.W at byte offset %zu",
                 offset);
    return R2_ERR_UNSUPPORTED;
}

static uint8_t operand_width(rex_prefix rex)
{
    return rex.w ? 8 : (rex.operand16 ? 2 : 4);
}

static r2_status decode_modrm(const uint8_t *code,
                              size_t code_len,
                              size_t *ip,
                              rex_prefix rex,
                              modrm_decoded *out,
                              char *error,
                              size_t error_len)
{
    uint8_t modrm = 0;
    if (!read_u8(code, code_len, ip, &modrm)) {
        r2_set_error(error, error_len, "truncated ModRM byte");
        return R2_ERR_DECODE;
    }

    memset(out, 0, sizeof(*out));
    out->raw_group = (uint8_t)((modrm >> 3) & 0x07);
    out->reg = (uint8_t)(out->raw_group | (rex.r ? 8 : 0));
    out->mem.base = R2_NO_REG;
    out->mem.index = R2_NO_REG;
    out->mem.scale = 1;

    uint8_t mod = (uint8_t)(modrm >> 6);
    uint8_t rm_low = (uint8_t)(modrm & 0x07);
    if (mod == 3) {
        out->rm_is_reg = true;
        out->rm = (uint8_t)(rm_low | (rex.b ? 8 : 0));
        return R2_OK;
    }

    out->mem.present = true;
    if (rm_low == 4) {
        uint8_t sib = 0;
        if (!read_u8(code, code_len, ip, &sib)) {
            r2_set_error(error, error_len, "truncated SIB byte");
            return R2_ERR_DECODE;
        }

        uint8_t scale_bits = (uint8_t)(sib >> 6);
        uint8_t index_low = (uint8_t)((sib >> 3) & 0x07);
        uint8_t base_low = (uint8_t)(sib & 0x07);
        out->mem.scale = (uint8_t)(1u << scale_bits);

        if (index_low != 4 || rex.x) {
            out->mem.index = (uint8_t)(index_low | (rex.x ? 8 : 0));
        }

        if (mod == 0 && base_low == 5) {
            int64_t disp = 0;
            if (!read_i32(code, code_len, ip, &disp)) {
                r2_set_error(error, error_len, "truncated SIB disp32");
                return R2_ERR_DECODE;
            }
            out->mem.disp = disp;
        } else {
            out->mem.base = (uint8_t)(base_low | (rex.b ? 8 : 0));
        }
    } else if (mod == 0 && rm_low == 5) {
        int64_t disp = 0;
        if (!read_i32(code, code_len, ip, &disp)) {
            r2_set_error(error, error_len, "truncated RIP-relative disp32");
            return R2_ERR_DECODE;
        }
        out->mem.rip_relative = true;
        out->mem.disp = disp;
    } else {
        out->mem.base = (uint8_t)(rm_low | (rex.b ? 8 : 0));
    }

    if (mod == 1) {
        int64_t disp = 0;
        if (!read_i8(code, code_len, ip, &disp)) {
            r2_set_error(error, error_len, "truncated disp8");
            return R2_ERR_DECODE;
        }
        out->mem.disp += disp;
    } else if (mod == 2) {
        int64_t disp = 0;
        if (!read_i32(code, code_len, ip, &disp)) {
            r2_set_error(error, error_len, "truncated disp32");
            return R2_ERR_DECODE;
        }
        out->mem.disp += disp;
    }

    return R2_OK;
}

static r2_status rel_target(size_t next_ip,
                            int64_t rel,
                            size_t code_len,
                            uint64_t guest_base,
                            bool require_in_range,
                            uint64_t *target,
                            char *error,
                            size_t error_len)
{
    if (require_in_range) {
        if (next_ip > (size_t)INT64_MAX) {
            r2_set_error(error, error_len, "code block is too large for relative branch");
            return R2_ERR_UNSUPPORTED;
        }

        int64_t signed_target = (int64_t)next_ip + rel;
        if (signed_target < 0 || (uint64_t)signed_target >= (uint64_t)code_len) {
            r2_set_error(error,
                         error_len,
                         "branch target %" PRId64 " is outside the decoded block",
                         signed_target);
            return R2_ERR_UNSUPPORTED;
        }
    }

    if ((uint64_t)next_ip > UINT64_MAX - guest_base) {
        r2_set_error(error, error_len, "relative branch address overflows guest address space");
        return R2_ERR_UNSUPPORTED;
    }
    uint64_t next_addr = guest_base + (uint64_t)next_ip;
    if (rel >= 0) {
        uint64_t amount = (uint64_t)rel;
        if (amount > UINT64_MAX - next_addr) {
            r2_set_error(error, error_len, "relative branch target overflows guest address space");
            return R2_ERR_UNSUPPORTED;
        }
        *target = next_addr + amount;
    } else {
        uint64_t amount = (uint64_t)(-(rel + 1)) + 1u;
        if (amount > next_addr) {
            r2_set_error(error, error_len, "relative branch target is below guest address zero");
            return R2_ERR_UNSUPPORTED;
        }
        *target = next_addr - amount;
    }
    return R2_OK;
}

static bool cc_from_opcode(uint8_t opcode, r2_cc *cc)
{
    switch (opcode & 0x0f) {
    case 0x0: *cc = R2_CC_O; return true;
    case 0x1: *cc = R2_CC_NO; return true;
    case 0x8: *cc = R2_CC_S; return true;
    case 0x9: *cc = R2_CC_NS; return true;
    case 0xa: *cc = R2_CC_P; return true;
    case 0xb: *cc = R2_CC_NP; return true;
    case 0x2:
        *cc = R2_CC_B;
        return true;
    case 0x3:
        *cc = R2_CC_AE;
        return true;
    case 0x4:
        *cc = R2_CC_E;
        return true;
    case 0x5:
        *cc = R2_CC_NE;
        return true;
    case 0x6:
        *cc = R2_CC_BE;
        return true;
    case 0x7:
        *cc = R2_CC_A;
        return true;
    case 0xc:
        *cc = R2_CC_L;
        return true;
    case 0xe:
        *cc = R2_CC_LE;
        return true;
    case 0xf:
        *cc = R2_CC_G;
        return true;
    case 0xd:
        *cc = R2_CC_GE;
        return true;
    default:
        return false;
    }
}

static r2_status decode_reg_memory_opcode(uint8_t opcode,
                                          const modrm_decoded *modrm,
                                          r2_insn *insn,
                                          char *error,
                                          size_t error_len)
{
    if (!modrm->rm_is_reg) {
        if (opcode == 0x89) {
            insn->op = R2_OP_MOV_STORE;
            insn->src = modrm->reg;
            insn->mem = modrm->mem;
            return R2_OK;
        }
        if (opcode == 0x8b) {
            insn->op = R2_OP_MOV_LOAD;
            insn->dst = modrm->reg;
            insn->mem = modrm->mem;
            return R2_OK;
        }

        r2_set_error(error,
                     error_len,
                     "memory operand for opcode 0x%02" PRIx8
                     " is not supported yet at byte offset %" PRIu64,
                     opcode,
                     insn->x86_offset);
        return R2_ERR_UNSUPPORTED;
    }

    if (opcode == 0x89) {
        insn->op = R2_OP_MOV_REG;
        insn->dst = modrm->rm;
        insn->src = modrm->reg;
    } else if (opcode == 0x8b) {
        insn->op = R2_OP_MOV_REG;
        insn->dst = modrm->reg;
        insn->src = modrm->rm;
    } else if (opcode == 0x01) {
        insn->op = R2_OP_ADD_REG;
        insn->dst = modrm->rm;
        insn->src = modrm->reg;
    } else if (opcode == 0x03) {
        insn->op = R2_OP_ADD_REG;
        insn->dst = modrm->reg;
        insn->src = modrm->rm;
    } else if (opcode == 0x11 || opcode == 0x19) {
        insn->op = opcode == 0x11 ? R2_OP_ADC_REG : R2_OP_SBB_REG;
        insn->dst = modrm->rm;
        insn->src = modrm->reg;
    } else if (opcode == 0x13 || opcode == 0x1b) {
        insn->op = opcode == 0x13 ? R2_OP_ADC_REG : R2_OP_SBB_REG;
        insn->dst = modrm->reg;
        insn->src = modrm->rm;
    } else if (opcode == 0x29) {
        insn->op = R2_OP_SUB_REG;
        insn->dst = modrm->rm;
        insn->src = modrm->reg;
    } else if (opcode == 0x2b) {
        insn->op = R2_OP_SUB_REG;
        insn->dst = modrm->reg;
        insn->src = modrm->rm;
    } else if (opcode == 0x31) {
        insn->op = R2_OP_XOR_REG;
        insn->dst = modrm->rm;
        insn->src = modrm->reg;
    } else if (opcode == 0x33) {
        insn->op = R2_OP_XOR_REG;
        insn->dst = modrm->reg;
        insn->src = modrm->rm;
    } else if (opcode == 0x39) {
        insn->op = R2_OP_CMP_REG;
        insn->dst = modrm->rm;
        insn->src = modrm->reg;
    } else if (opcode == 0x3b) {
        insn->op = R2_OP_CMP_REG;
        insn->dst = modrm->reg;
        insn->src = modrm->rm;
    } else if (opcode == 0x85) {
        insn->op = R2_OP_TEST_REG;
        insn->dst = modrm->rm;
        insn->src = modrm->reg;
    } else {
        r2_set_error(error,
                     error_len,
                     "unsupported register opcode 0x%02" PRIx8
                     " at byte offset %" PRIu64,
                     opcode,
                     insn->x86_offset);
        return R2_ERR_UNSUPPORTED;
    }

    return R2_OK;
}

static void decode_extend_operand(const modrm_decoded *modrm,
                                   rex_prefix rex,
                                   uint8_t source_width,
                                   r2_insn *insn)
{
    insn->width = operand_width(rex);
    insn->src_width = source_width;
    insn->dst = modrm->reg;
    if (modrm->rm_is_reg) {
        insn->src_high8 = source_width == 1 && !rex.present &&
                          modrm->rm >= 4 && modrm->rm <= 7;
        insn->src = insn->src_high8 ? (uint8_t)(modrm->rm - 4) : modrm->rm;
    } else {
        insn->mem = modrm->mem;
    }
}

static void byte_register(uint8_t encoded, rex_prefix rex, uint8_t *reg, bool *high)
{
    *high = !rex.present && encoded >= 4 && encoded <= 7;
    *reg = *high ? (uint8_t)(encoded - 4) : encoded;
}

static bool read_immediate(const uint8_t *code, size_t length, size_t *ip,
                           uint8_t width, int64_t *value)
{
    if (width == 1) return read_i8(code, length, ip, value);
    if (width == 2) {
        uint8_t lo, hi;
        if (!read_u8(code, length, ip, &lo) || !read_u8(code, length, ip, &hi)) return false;
        *value = (int16_t)((uint16_t)lo | ((uint16_t)hi << 8));
        return true;
    }
    return read_i32(code, length, ip, value);
}

static r2_status decode_binary(const uint8_t *code, size_t length, size_t *ip,
                                uint8_t opcode, rex_prefix rex, r2_insn *insn,
                                char *error, size_t error_len)
{
    insn->op = R2_OP_BINARY;
    bool group = opcode == 0x80 || opcode == 0x81 || opcode == 0x83;
    bool test = opcode == 0x84 || opcode == 0x85;
    bool accumulator = !group && !test && (opcode & 7) >= 4;
    insn->width = (opcode == 0x80 || opcode == 0x84 || (!group && !test && !(opcode & 1)))
                    ? 1 : operand_width(rex);
    insn->alu = test ? R2_ALU_TEST : (r2_alu)((opcode >> 3) & 7);
    if (accumulator) {
        insn->dst = R2_X86_RAX;
        insn->has_imm = true;
    } else {
        modrm_decoded modrm;
        r2_status status = decode_modrm(code, length, ip, rex, &modrm, error, error_len);
        if (status != R2_OK) return status;
        if (group) {
            insn->alu = (r2_alu)modrm.raw_group;
            insn->has_imm = true;
            insn->dst = modrm.rm;
            insn->mem_is_dst = !modrm.rm_is_reg;
            if (insn->width == 1 && modrm.rm_is_reg)
                byte_register(modrm.rm, rex, &insn->dst, &insn->dst_high8);
        } else {
            bool reverse = !test && (opcode & 2) != 0;
            uint8_t rm = modrm.rm, reg = modrm.reg;
            bool rm_high = false, reg_high = false;
            if (insn->width == 1) {
                if (modrm.rm_is_reg) byte_register(rm, rex, &rm, &rm_high);
                byte_register(reg, rex, &reg, &reg_high);
            }
            insn->dst = reverse ? reg : rm;
            insn->src = reverse ? rm : reg;
            insn->dst_high8 = reverse ? reg_high : rm_high;
            insn->src_high8 = reverse ? rm_high : reg_high;
            insn->mem_is_dst = !reverse && !modrm.rm_is_reg;
        }
        if (!modrm.rm_is_reg) insn->mem = modrm.mem;
    }
    if (insn->has_imm && !read_immediate(code, length, ip,
                                         opcode == 0x83 ? 1 : insn->width, &insn->imm)) {
        r2_set_error(error, error_len, "truncated arithmetic immediate");
        return R2_ERR_DECODE;
    }
    return R2_OK;
}

static r2_status decode_block_at(const uint8_t *code,
                                 size_t code_len,
                                 uint64_t guest_base,
                                 bool require_targets_in_range,
                                 bool require_terminator,
                                 bool single_instruction,
                                 r2_block *block,
                                 r2_insn *single_output,
                                 char *error,
                                 size_t error_len)
{
    if ((uint64_t)code_len > UINT64_MAX - guest_base) {
        r2_set_error(error, error_len, "decoded code range overflows guest address space");
        return R2_ERR_UNSUPPORTED;
    }

    size_t ip = 0;
    bool saw_terminator_candidate = false;
    while (ip < code_len) {
        size_t insn_start = ip;
        bool gs_override = false;
        bool operand16 = false;
        bool locked = false;
        uint8_t repeat = 0;
        while (ip < code_len && (code[ip] == 0x65 || code[ip] == 0x66 || code[ip] == 0x2e ||
                                code[ip] == 0x3e || code[ip] == 0xf2 || code[ip] == 0xf3 || code[ip] == 0xf0)) {
            if (code[ip] == 0xf0) locked = true;
            if (code[ip] == 0x65) gs_override = true;
            if (code[ip] == 0x66) operand16 = true;
            if (code[ip] == 0xf2 || code[ip] == 0xf3) repeat = code[ip];
            ip += 1;
        }
        rex_prefix rex = read_rex(code, code_len, &ip);
        rex.operand16 = operand16;
        rex.repeat = repeat;

        uint8_t opcode = 0;
        if (!read_u8(code, code_len, &ip, &opcode)) {
            r2_set_error(error, error_len, "truncated opcode at byte offset %zu", insn_start);
            return R2_ERR_DECODE;
        }
        if (operand16 && ((opcode >= 0x50 && opcode <= 0x5f) || opcode == 0x68 || opcode == 0x6a)) {
            r2_set_error(error, error_len, "16-bit stack operands are not supported yet");
            return R2_ERR_UNSUPPORTED;
        }

        r2_insn insn = {
            .op = R2_OP_NOP,
            .cc = R2_CC_E,
            .width = 8,
            .dst = 0,
            .src = 0,
            .imm = 0,
            .target = 0,
            .mem = {.present = false, .base = R2_NO_REG, .index = R2_NO_REG, .scale = 1},
            .x86_offset = guest_base + (uint64_t)insn_start,
            .x86_len = 0,
        };

        r2_status status = R2_OK;
        modrm_decoded modrm;

        if (opcode >= 0x90 && opcode <= 0x97 && (opcode != 0x90 || rex.b)) {
            insn.op = R2_OP_XCHG;
            insn.width = operand_width(rex);
            insn.dst = R2_X86_RAX;
            insn.src = (opcode & 7) | (rex.b ? 8 : 0);
        } else if ((opcode <= 0x3d && (opcode & 7) <= 5) || opcode == 0x84 || opcode == 0x85 ||
            opcode == 0x80 || opcode == 0x81 || opcode == 0x83) {
            status = decode_binary(code, code_len, &ip, opcode, rex, &insn, error, error_len);
            if (status != R2_OK) return status;
        } else if (opcode >= 0xb0 && opcode <= 0xb7) {
            insn.op = R2_OP_MOV_IMM;
            insn.width = 1;
            byte_register((uint8_t)((opcode - 0xb0) | (rex.b ? 8 : 0)), rex, &insn.dst, &insn.dst_high8);
            if (!read_i8(code, code_len, &ip, &insn.imm)) return R2_ERR_DECODE;
        } else if (opcode >= 0xb8 && opcode <= 0xbf) {
            insn.op = R2_OP_MOV_IMM;
            insn.width = operand_width(rex);
            insn.dst = (uint8_t)((opcode - 0xb8) | (rex.b ? 8 : 0));
            if (rex.w) {
                if (!read_i64(code, code_len, &ip, &insn.imm)) {
                    r2_set_error(error, error_len, "truncated imm64 at byte offset %zu", ip);
                    return R2_ERR_DECODE;
                }
            } else if (insn.width == 2) {
                if (!read_immediate(code, code_len, &ip, 2, &insn.imm)) return R2_ERR_DECODE;
            } else {
                uint32_t imm = 0;
                if (!read_u32(code, code_len, &ip, &imm)) {
                    r2_set_error(error, error_len, "truncated imm32 at byte offset %zu", ip);
                    return R2_ERR_DECODE;
                }
                insn.imm = imm;
            }
        } else if (opcode >= 0x50 && opcode <= 0x57) {
            insn.op = R2_OP_PUSH_REG;
            insn.src = (uint8_t)((opcode - 0x50) | (rex.b ? 8 : 0));
        } else if (opcode >= 0x58 && opcode <= 0x5f) {
            insn.op = R2_OP_POP_REG;
            insn.dst = (uint8_t)((opcode - 0x58) | (rex.b ? 8 : 0));
        } else if (opcode >= 0x70 && opcode <= 0x7f && cc_from_opcode(opcode, &insn.cc)) {
            int64_t rel = 0;
            if (!read_i8(code, code_len, &ip, &rel)) {
                r2_set_error(error, error_len, "truncated rel8 at byte offset %zu", ip);
                return R2_ERR_DECODE;
            }
            insn.op = R2_OP_JCC;
            status = rel_target(ip,
                                rel,
                                code_len,
                                guest_base,
                                require_targets_in_range,
                                &insn.target,
                                error,
                                error_len);
            if (status != R2_OK) {
                return status;
            }
        } else {
            switch (opcode) {
            case 0xa4:
            case 0xa5:
                if (repeat == 0xf2) { r2_set_error(error, error_len, "REPNE MOVS is unsupported"); return R2_ERR_UNSUPPORTED; }
                insn.op = R2_OP_MOVS; insn.width = opcode == 0xa4 ? 1 : operand_width(rex);
                insn.has_imm = repeat == 0xf3;
                insn.mem.present = true; insn.mem.base = R2_X86_RSI;
                /* Segment overrides affect only the implicit source. */
                break;
            case 0xaa:
            case 0xab:
                if (repeat == 0xf2) { r2_set_error(error, error_len, "REPNE STOS is unsupported"); return R2_ERR_UNSUPPORTED; }
                insn.op = R2_OP_STOS; insn.width = opcode == 0xaa ? 1 : operand_width(rex);
                insn.has_imm = repeat == 0xf3;
                /* Segment overrides do not affect the implicit ES destination. */
                gs_override = false;
                break;
            case 0xfc:
            case 0xfd:
                insn.op = R2_OP_DIRECTION; insn.subop = opcode == 0xfd;
                break;
            case 0x8f:
                if (operand16) { r2_set_error(error, error_len, "16-bit stack operands are not supported yet"); return R2_ERR_UNSUPPORTED; }
                status = decode_modrm(code, code_len, &ip, rex, &modrm, error, error_len);
                if (status != R2_OK) return status;
                if (modrm.raw_group != 0) { r2_set_error(error, error_len, "unsupported POP opcode group /%u", modrm.raw_group); return R2_ERR_UNSUPPORTED; }
                insn.op = R2_OP_POP_REG;
                if (modrm.rm_is_reg) insn.dst = modrm.rm;
                else insn.mem = modrm.mem;
                break;
            case 0x90:
                insn.op = R2_OP_NOP;
                break;
            case 0x9b:
                insn.op = R2_OP_NOP; /* All supported x87 exceptions are masked. */
                break;
            case 0xd9: case 0xdb: case 0xdd: case 0xdf: {
                uint8_t extension = ip < code_len ? code[ip] : 0;
                modrm_decoded modrm;
                status = decode_modrm(code, code_len, &ip, rex, &modrm, error, error_len);
                if (status != R2_OK) return status;
                insn.op = R2_OP_X87; insn.mem = modrm.mem; insn.src = modrm.rm & 7;
                insn.width = opcode == 0xd9 ? 4 : opcode == 0xdd ? 8 : 10;
                if (!modrm.rm_is_reg && ((opcode == 0xd9 || opcode == 0xdd) && modrm.raw_group == 0)) insn.subop = R2_X87_LOAD;
                else if (!modrm.rm_is_reg && (opcode == 0xd9 || opcode == 0xdd) && (modrm.raw_group == 2 || modrm.raw_group == 3)) {
                    insn.subop = R2_X87_STORE; insn.has_imm = modrm.raw_group == 3;
                } else if (!modrm.rm_is_reg && opcode == 0xdb && (modrm.raw_group == 5 || modrm.raw_group == 7)) {
                    insn.subop = modrm.raw_group == 5 ? R2_X87_LOAD : R2_X87_STORE; insn.has_imm = modrm.raw_group == 7;
                } else if (!modrm.rm_is_reg && opcode == 0xd9 && (modrm.raw_group == 5 || modrm.raw_group == 7)) {
                    insn.subop = modrm.raw_group == 5 ? R2_X87_LOAD_CONTROL : R2_X87_STORE_CONTROL; insn.width = 2;
                } else if (!modrm.rm_is_reg && opcode == 0xdd && modrm.raw_group == 7) {
                    insn.subop = R2_X87_STATUS; insn.width = 2;
                } else if (modrm.rm_is_reg && opcode == 0xdf && extension == 0xe0) insn.subop = R2_X87_STATUS;
                else if (modrm.rm_is_reg && opcode == 0xd9 && (extension == 0xf8 || extension == 0xf5)) insn.subop = extension == 0xf8 ? R2_X87_REMAINDER : R2_X87_REMAINDER_NEAREST;
                else if (modrm.rm_is_reg && opcode == 0xd9 && extension >= 0xc0 && extension <= 0xc7) insn.subop = R2_X87_LOAD_REG;
                else if (modrm.rm_is_reg && opcode == 0xd9 && extension >= 0xc8 && extension <= 0xcf) insn.subop = R2_X87_EXCHANGE;
                else if (modrm.rm_is_reg && opcode == 0xdd && extension >= 0xc0 && extension <= 0xc7) insn.subop = R2_X87_FREE;
                else if (modrm.rm_is_reg && opcode == 0xdd && extension >= 0xd0 && extension <= 0xdf) {
                    insn.subop = R2_X87_STORE_REG; insn.has_imm = extension >= 0xd8;
                } else if (modrm.rm_is_reg && opcode == 0xdb && (extension == 0xe2 || extension == 0xe3)) insn.subop = extension == 0xe2 ? R2_X87_CLEAR : R2_X87_INIT;
                else if (modrm.rm_is_reg && opcode == 0xd9 && extension == 0xee) insn.subop = R2_X87_ZERO;
                else if (modrm.rm_is_reg && (opcode == 0xdb || opcode == 0xdf) && extension >= 0xe8 && extension <= 0xf7) {
                    insn.subop = R2_X87_COMPARE_FLAGS; insn.has_imm = opcode == 0xdf;
                    insn.imm = extension >= 0xf0; /* Ordered comparisons reject quiet NaNs too. */
                }
                else if (modrm.rm_is_reg && opcode == 0xd9 && extension == 0xd0) insn.op = R2_OP_NOP;
                else { r2_set_error(error, error_len, "unsupported x87 encoding %02x %02x", opcode, extension); return R2_ERR_UNSUPPORTED; }
                break;
            }
            case 0xc3:
                insn.op = R2_OP_RET;
                saw_terminator_candidate = true;
                break;
            case 0xc9:
                insn.op = R2_OP_LEAVE;
                break;
            case 0x68:
                insn.op = R2_OP_PUSH_IMM;
                if (!read_i32(code, code_len, &ip, &insn.imm)) {
                    r2_set_error(error, error_len, "truncated push imm32 at byte offset %zu", ip);
                    return R2_ERR_DECODE;
                }
                break;
            case 0x6a:
                insn.op = R2_OP_PUSH_IMM;
                if (!read_i8(code, code_len, &ip, &insn.imm)) {
                    r2_set_error(error, error_len, "truncated push imm8 at byte offset %zu", ip);
                    return R2_ERR_DECODE;
                }
                break;
            case 0xe8: {
                int64_t rel = 0;
                if (!read_i32(code, code_len, &ip, &rel)) {
                    r2_set_error(error, error_len, "truncated call rel32 at byte offset %zu", ip);
                    return R2_ERR_DECODE;
                }
                insn.op = R2_OP_CALL;
                status = rel_target(ip,
                                    rel,
                                    code_len,
                                    guest_base,
                                    require_targets_in_range,
                                    &insn.target,
                                    error,
                                    error_len);
                if (status != R2_OK) {
                    return status;
                }
                break;
            }
            case 0xe9:
            case 0xeb: {
                int64_t rel = 0;
                bool ok = opcode == 0xe9 ? read_i32(code, code_len, &ip, &rel)
                                         : read_i8(code, code_len, &ip, &rel);
                if (!ok) {
                    r2_set_error(error, error_len, "truncated jmp relative at byte offset %zu", ip);
                    return R2_ERR_DECODE;
                }
                insn.op = R2_OP_JMP;
                status = rel_target(ip,
                                    rel,
                                    code_len,
                                    guest_base,
                                    require_targets_in_range,
                                    &insn.target,
                                    error,
                                    error_len);
                if (status != R2_OK) {
                    return status;
                }
                break;
            }
            case 0x0f: {
                uint8_t opcode2 = 0;
                if (!read_u8(code, code_len, &ip, &opcode2)) {
                    r2_set_error(error, error_len, "truncated 0f escape at byte offset %zu", ip);
                    return R2_ERR_DECODE;
                }
                if (opcode2 >= 0xc8 && opcode2 <= 0xcf) {
                    insn.op = R2_OP_BSWAP;
                    insn.width = rex.w ? 8 : 4;
                    insn.dst = (uint8_t)((opcode2 - 0xc8) | (rex.b ? 8 : 0));
                } else if (opcode2 == 0x18) {
                    status = decode_modrm(code, code_len, &ip, rex, &modrm, error, error_len);
                    if (status != R2_OK) return status;
                    if (modrm.rm_is_reg || modrm.raw_group > 3) {
                        r2_set_error(error, error_len, "unsupported prefetch encoding"); return R2_ERR_UNSUPPORTED;
                    }
                    /* PREFETCH is a non-faulting memory hint. */
                    insn.op = R2_OP_NOP;
                } else if (opcode2 == 0xc4 || opcode2 == 0xc5) {
                    if (!operand16 || repeat) { r2_set_error(error, error_len, "MMX word transfer unsupported"); return R2_ERR_UNSUPPORTED; }
                    status = decode_modrm(code, code_len, &ip, rex, &modrm, error, error_len);
                    if (status != R2_OK) return status;
                    uint8_t control; if (!read_u8(code, code_len, &ip, &control)) return R2_ERR_DECODE;
                    insn.op = R2_OP_SSE_GP; insn.width = 2; insn.imm = control;
                    if (opcode2 == 0xc4) {
                        insn.subop = 0xc4; insn.dst = modrm.reg; insn.src = modrm.rm;
                        if (!modrm.rm_is_reg) insn.mem = modrm.mem;
                    } else {
                        if (!modrm.rm_is_reg) { r2_set_error(error, error_len, "invalid PEXTRW memory operand"); return R2_ERR_UNSUPPORTED; }
                        insn.subop = 0x15; insn.dst = modrm.rm; insn.src = modrm.reg;
                    }
                } else if (opcode2 == 0x63 || opcode2 == 0x67 || opcode2 == 0x6b) {
                    if (!operand16 || repeat) { r2_set_error(error, error_len, "MMX pack unsupported"); return R2_ERR_UNSUPPORTED; }
                    status = decode_modrm(code, code_len, &ip, rex, &modrm, error, error_len);
                    if (status != R2_OK) return status;
                    insn.op = R2_OP_PACKED_INT; insn.subop = opcode2 == 0x67 ? 16 : 15;
                    insn.src_width = opcode2 == 0x6b ? 4 : 2; insn.width = insn.src_width / 2;
                    insn.dst = modrm.reg; insn.src = modrm.rm;
                    if (!modrm.rm_is_reg) insn.mem = modrm.mem;
                } else if (opcode2 == 0xd7 && operand16) {
                    status = decode_modrm(code, code_len, &ip, rex, &modrm, error, error_len);
                    if (status != R2_OK) return status;
                    if (!modrm.rm_is_reg) { r2_set_error(error, error_len, "invalid PMOVMSKB memory operand"); return R2_ERR_UNSUPPORTED; }
                    insn.op = R2_OP_SSE_GP; insn.subop = 0xd7; insn.dst = modrm.reg; insn.src = modrm.rm;
                } else if (opcode2 == 0x71 || opcode2 == 0x72 || opcode2 == 0x73 ||
                           opcode2 == 0xd1 || opcode2 == 0xd2 || opcode2 == 0xd3 ||
                           opcode2 == 0xe1 || opcode2 == 0xe2 || opcode2 == 0xf1 || opcode2 == 0xf2 || opcode2 == 0xf3) {
                    if (!operand16 || repeat) { r2_set_error(error, error_len, "MMX shift unsupported"); return R2_ERR_UNSUPPORTED; }
                    status = decode_modrm(code, code_len, &ip, rex, &modrm, error, error_len);
                    if (status != R2_OK) return status;
                    insn.op = R2_OP_PACKED_INT; insn.has_imm = opcode2 < 0x80;
                    insn.src_width = (opcode2 & 3) == 1 ? 2 : (opcode2 & 3) == 2 ? 4 : 8;
                    if (insn.has_imm) {
                        uint8_t control;
                        if (!modrm.rm_is_reg || !read_u8(code, code_len, &ip, &control)) return R2_ERR_DECODE;
                        insn.imm = control; insn.dst = modrm.rm;
                        if (modrm.raw_group == 2) insn.subop = 10;
                        else if (modrm.raw_group == 4 && opcode2 != 0x73) insn.subop = 11;
                        else if (modrm.raw_group == 6) insn.subop = 12;
                        else if (opcode2 == 0x73 && (modrm.raw_group == 3 || modrm.raw_group == 7)) insn.subop = modrm.raw_group == 3 ? 13 : 14;
                        else { r2_set_error(error, error_len, "invalid packed shift group"); return R2_ERR_UNSUPPORTED; }
                    } else {
                        insn.subop = opcode2 < 0xe0 ? 10 : opcode2 < 0xf0 ? 11 : 12;
                        insn.dst = modrm.reg; insn.src = modrm.rm;
                        if (!modrm.rm_is_reg) insn.mem = modrm.mem;
                    }
                } else if(opcode2==0xd5||opcode2==0xe4||opcode2==0xe5||opcode2==0xf5||opcode2==0xe0||opcode2==0xe3||opcode2==0xd8||opcode2==0xd9||opcode2==0xdc||opcode2==0xdd||opcode2==0xe8||opcode2==0xe9||opcode2==0xec||opcode2==0xed) {
                    if(!operand16||repeat){r2_set_error(error,error_len,"unsupported packed multiply prefix or MMX form");return R2_ERR_UNSUPPORTED;}
                    status=decode_modrm(code,code_len,&ip,rex,&modrm,error,error_len);if(status!=R2_OK)return status;
                    insn.op=R2_OP_PACKED_EXT;insn.width=16;insn.subop=opcode2;insn.dst=modrm.reg;insn.src=modrm.rm;
                    if(!modrm.rm_is_reg)insn.mem=modrm.mem;
                } else if (opcode2 == 0x31) {
                    insn.op = R2_OP_RDTSC;
                } else if (opcode2 == 0x38) {
                    uint8_t opcode3;
                    if (!read_u8(code, code_len, &ip, &opcode3)) return R2_ERR_DECODE;
                    if((opcode3>=1&&opcode3<=0x0b)||(opcode3>=0x1c&&opcode3<=0x1e)) {
                        if(!operand16||repeat){r2_set_error(error,error_len,"unsupported SSSE3 prefix or MMX form");return R2_ERR_UNSUPPORTED;}
                        status=decode_modrm(code,code_len,&ip,rex,&modrm,error,error_len);if(status!=R2_OK)return status;
                        insn.op=R2_OP_PACKED_EXT;insn.width=16;insn.subop=opcode3;insn.dst=modrm.reg;insn.src=modrm.rm;
                        if(!modrm.rm_is_reg)insn.mem=modrm.mem;
                        break;
                    }
                    bool extend = (opcode3 >= 0x20 && opcode3 <= 0x25) || (opcode3 >= 0x30 && opcode3 <= 0x35);
                    bool minmax = opcode3 >= 0x38 && opcode3 <= 0x3f;
                    if (!operand16 || (!extend && !minmax && opcode3 != 0x00 && opcode3 != 0x10 && opcode3 != 0x14 && opcode3 != 0x15 &&
                                       opcode3 != 0x17 && opcode3 != 0x28 && opcode3 != 0x29 && opcode3 != 0x37 && opcode3 != 0x40)) {
                        r2_set_error(error, error_len, "unsupported 0f 38 opcode 0x%02x", opcode3); return R2_ERR_UNSUPPORTED;
                    }
                    status = decode_modrm(code, code_len, &ip, rex, &modrm, error, error_len);
                    if (status != R2_OK) return status;
                    insn.op = R2_OP_PACKED_INT; insn.width = 16; insn.subop = 4;
                    insn.src_width = opcode3 == 0x10 ? 1 : opcode3 == 0x14 ? 4 : 8;
                    if (minmax) {
                        unsigned form = opcode3 & 3;
                        insn.subop = 18 + (opcode3 >= 0x3c) + (form >= 2 ? 2 : 0);
                        insn.src_width = form == 0 ? 1 : form == 2 ? 2 : 4;
                    }
                    if (opcode3 == 0x17) { insn.subop = 17; insn.src_width = 8; }
                    if (opcode3 == 0x28) { insn.subop = 24; insn.src_width = 8; }
                    if (opcode3 == 0x40) { insn.subop = 5; insn.src_width = 4; }
                    if (opcode3 == 0x00) { insn.subop = 6; insn.src_width = 1; }
                    if (opcode3 == 0x29 || opcode3 == 0x37) { insn.subop = opcode3 == 0x29 ? 1 : 0; insn.src_width = 8; }
                    if (extend) {
                        unsigned form = opcode3 & 15;
                        insn.subop = opcode3 < 0x30 ? 7 : 8;
                        insn.src_width = form < 3 ? 1 : form < 5 ? 2 : 4;
                        insn.width = form == 0 ? 2 : form == 1 || form == 3 ? 4 : 8;
                    }
                    insn.dst = modrm.reg; insn.src = modrm.rm;
                    if (!modrm.rm_is_reg) insn.mem = modrm.mem;
                } else if (opcode2 == 0xa2) {
                    insn.op = R2_OP_CPUID;
                } else if (opcode2 == 0xae) {
                    status = decode_modrm(code, code_len, &ip, rex, &modrm, error, error_len);
                    if (status != R2_OK) return status;
                    if (!modrm.rm_is_reg && modrm.raw_group <= 1) {
                        insn.op = R2_OP_FX_STATE; insn.subop = modrm.raw_group; insn.mem = modrm.mem;
                    } else if (!modrm.rm_is_reg && (modrm.raw_group == 2 || modrm.raw_group == 3)) {
                        insn.op = R2_OP_MXCSR; insn.subop = modrm.raw_group; insn.mem = modrm.mem;
                    } else if (modrm.rm_is_reg && modrm.rm == 0 && modrm.raw_group >= 5) {
                        insn.op = R2_OP_FENCE;
                    } else if (!modrm.rm_is_reg && (modrm.raw_group == 7 || (operand16 && modrm.raw_group == 6))) {
                        insn.op = R2_OP_FENCE; insn.mem = modrm.mem;
                    } else {
                        r2_set_error(error, error_len, "unsupported 0f ae group /%u", modrm.raw_group); return R2_ERR_UNSUPPORTED;
                    }
                } else if (opcode2 == 0x3a) {
                    uint8_t opcode3, control;
                    if (!read_u8(code, code_len, &ip, &opcode3)) return R2_ERR_DECODE;
                    if(opcode3==0x0f) {
                        if(!operand16||repeat){r2_set_error(error,error_len,"unsupported PALIGNR prefix or MMX form");return R2_ERR_UNSUPPORTED;}
                        status=decode_modrm(code,code_len,&ip,rex,&modrm,error,error_len);if(status!=R2_OK)return status;
                        if(!read_u8(code,code_len,&ip,&control))return R2_ERR_DECODE;
                        insn.op=R2_OP_PACKED_EXT;insn.width=16;insn.subop=opcode3;insn.imm=control;insn.dst=modrm.reg;insn.src=modrm.rm;
                        if(!modrm.rm_is_reg)insn.mem=modrm.mem;
                        break;
                    }
                    bool extract = opcode3 >= 0x14 && opcode3 <= 0x17;
                    bool blend = opcode3 >= 0x0c && opcode3 <= 0x0e;
                    bool vector_insert = opcode3 == 0x21;
                    bool insert = opcode3 == 0x20 || opcode3 == 0x22;
                    if (!operand16 || (!extract && !insert && !vector_insert && !blend && (opcode3 < 8 || opcode3 > 11))) {
                        r2_set_error(error, error_len, "unsupported 0f 3a opcode 0x%02x", opcode3); return R2_ERR_UNSUPPORTED;
                    }
                    status = decode_modrm(code, code_len, &ip, rex, &modrm, error, error_len);
                    if (status != R2_OK) return status;
                    if (!read_u8(code, code_len, &ip, &control)) return R2_ERR_DECODE;
                    if (vector_insert) {
                        insn.op = R2_OP_SSE_SHUFFLE; insn.subop = 0x21; insn.width = 4;
                        insn.imm = control; insn.dst = modrm.reg; insn.src = modrm.rm;
                        if (!modrm.rm_is_reg) insn.mem = modrm.mem;
                        break;
                    }
                    if (blend) {
                        insn.op = R2_OP_PACKED_INT; insn.subop = 9;
                        insn.src_width = opcode3 == 0x0e ? 2 : opcode3 == 0x0d ? 8 : 4;
                        insn.imm = control; insn.dst = modrm.reg; insn.src = modrm.rm;
                        if (!modrm.rm_is_reg) insn.mem = modrm.mem;
                        break;
                    }
                    if (extract || insert) {
                        insn.op = R2_OP_SSE_GP; insn.subop = opcode3;
                        insn.width = insert ? (opcode3 == 0x20 ? 1 : rex.w ? 8 : 4) : opcode3 == 0x14 ? 1 : opcode3 == 0x15 ? 2 : opcode3 == 0x16 && rex.w ? 8 : 4;
                        insn.imm = control; insn.dst = modrm.reg; insn.src = modrm.rm;
                        if (!modrm.rm_is_reg) { insn.mem = modrm.mem; insn.mem_is_dst = extract; }
                        break;
                    }
                    insn.op = R2_OP_SSE_FP; insn.subop = 0x08;
                    insn.width = opcode3 & 1 ? 8 : 4; insn.has_imm = opcode3 >= 10;
                    insn.imm = control; insn.dst = modrm.reg; insn.src = modrm.rm;
                    if (!modrm.rm_is_reg) insn.mem = modrm.mem;
                } else if (opcode2 == 0x52 || opcode2 == 0x53) {
                    if (operand16 || (repeat && repeat != 0xf3)) { r2_set_error(error, error_len, "invalid reciprocal SSE prefix"); return R2_ERR_UNSUPPORTED; }
                    status = decode_modrm(code, code_len, &ip, rex, &modrm, error, error_len);
                    if (status != R2_OK) return status;
                    insn.op = R2_OP_SSE_FP; insn.subop = opcode2; insn.width = 4; insn.has_imm = repeat == 0xf3;
                    insn.dst = modrm.reg; insn.src = modrm.rm;
                    if (!modrm.rm_is_reg) insn.mem = modrm.mem;
                } else if (opcode2 == 0x7c || opcode2 == 0x7d || opcode2 == 0xd0) {
                    if ((!operand16 && repeat != 0xf2) || repeat == 0xf3) {
                        r2_set_error(error, error_len, "invalid SSE horizontal arithmetic prefix"); return R2_ERR_UNSUPPORTED;
                    }
                    status = decode_modrm(code, code_len, &ip, rex, &modrm, error, error_len);
                    if (status != R2_OK) return status;
                    insn.op = R2_OP_SSE_FP; insn.subop = opcode2; insn.width = operand16 ? 8 : 4;
                    insn.dst = modrm.reg; insn.src = modrm.rm;
                    if (!modrm.rm_is_reg) insn.mem = modrm.mem;
                } else if (opcode2 == 0x58 || opcode2 == 0x59 || opcode2 == 0x5c || opcode2 == 0x5e ||
                           opcode2 == 0x51 || opcode2 == 0x5d || opcode2 == 0x5f || opcode2 == 0x2e || opcode2 == 0x2f ||
                           opcode2 == 0xc2 || opcode2 == 0x50) {
                    status = decode_modrm(code, code_len, &ip, rex, &modrm, error, error_len);
                    if (status != R2_OK) return status;
                    insn.op = R2_OP_SSE_FP; insn.subop = opcode2;
                    insn.width = repeat == 0xf2 || operand16 ? 8 : 4;
                    insn.has_imm = repeat != 0 || opcode2 == 0x2e || opcode2 == 0x2f;
                    insn.dst = modrm.reg; insn.src = modrm.rm;
                    if (!modrm.rm_is_reg) insn.mem = modrm.mem;
                    if (opcode2 == 0xc2) {
                        uint8_t predicate;
                        if (!read_u8(code, code_len, &ip, &predicate)) return R2_ERR_DECODE;
                        insn.imm = predicate & 7;
                    }
                    if (opcode2 == 0x50 && !modrm.rm_is_reg) return R2_ERR_DECODE;
                } else if (opcode2 == 0xe6 || opcode2 == 0x5b || (opcode2 == 0x5a && repeat == 0)) {
                    status = decode_modrm(code, code_len, &ip, rex, &modrm, error, error_len);
                    if (status != R2_OK) return status;
                    insn.op = R2_OP_SSE_CONVERT; insn.has_imm = true;
                    if (opcode2 == 0xe6) {
                        if (!operand16 && repeat != 0xf2 && repeat != 0xf3) return R2_ERR_UNSUPPORTED;
                        insn.subop = repeat == 0xf3 ? 0xe8 : repeat == 0xf2 ? 0xe7 : 0xe6;
                        insn.width = repeat == 0xf3 ? 8 : 4; insn.src_width = repeat == 0xf3 ? 4 : 8;
                        insn.imm = 2;
                    } else if (opcode2 == 0x5b) {
                        insn.subop = operand16 ? 0x5c : repeat == 0xf3 ? 0x5d : 0x5b;
                        insn.width = insn.src_width = 4; insn.imm = 4;
                    } else {
                        insn.subop = operand16 ? 0x5f : 0x5e;
                        insn.width = operand16 ? 4 : 8; insn.src_width = operand16 ? 8 : 4; insn.imm = 2;
                    }
                    insn.dst = modrm.reg; insn.src = modrm.rm;
                    if (!modrm.rm_is_reg) insn.mem = modrm.mem;
                } else if (opcode2 == 0x2a || opcode2 == 0x2c || opcode2 == 0x2d || opcode2 == 0x5a) {
                    if (repeat != 0xf2 && repeat != 0xf3) {
                        r2_set_error(error, error_len, "packed/MMX SSE conversion is unsupported"); return R2_ERR_UNSUPPORTED;
                    }
                    status = decode_modrm(code, code_len, &ip, rex, &modrm, error, error_len);
                    if (status != R2_OK) return status;
                    insn.op = R2_OP_SSE_CONVERT; insn.subop = opcode2;
                    insn.width = repeat == 0xf2 ? 8 : 4;
                    insn.src_width = rex.w ? 8 : 4;
                    if (opcode2 == 0x2c || opcode2 == 0x2d) {
                        insn.src_width = insn.width; insn.width = rex.w ? 8 : 4;
                    } else if (opcode2 == 0x5a) {
                        insn.src_width = insn.width; insn.width = insn.width == 8 ? 4 : 8;
                    }
                    insn.dst = modrm.reg; insn.src = modrm.rm;
                    if (!modrm.rm_is_reg) insn.mem = modrm.mem;
                } else if ((operand16 && (opcode2 == 0x6e || opcode2 == 0x7e || opcode2 == 0xd6)) ||
                           (repeat == 0xf3 && opcode2 == 0x7e)) {
                    status = decode_modrm(code, code_len, &ip, rex, &modrm, error, error_len);
                    if (status != R2_OK) return status;
                    insn.op = R2_OP_SSE_GP; insn.subop = opcode2;
                    insn.width = rex.w || opcode2 == 0xd6 || repeat == 0xf3 ? 8 : 4;
                    insn.dst = modrm.reg; insn.src = modrm.rm;
                    insn.has_imm = repeat == 0xf3 || opcode2 == 0xd6;
                    if (!modrm.rm_is_reg) insn.mem = modrm.mem;
                } else if (opcode2 == 0x13 || opcode2 == 0x17) {
                    status = decode_modrm(code, code_len, &ip, rex, &modrm, error, error_len);
                    if (status != R2_OK) return status;
                    if (modrm.rm_is_reg || repeat) { r2_set_error(error, error_len, "invalid scalar SSE half store"); return R2_ERR_UNSUPPORTED; }
                    insn.op = R2_OP_SSE_GP; insn.subop = 0x16; insn.width = 8;
                    insn.imm = opcode2 == 0x17; insn.dst = modrm.reg; insn.src = modrm.rm;
                    insn.mem = modrm.mem; insn.mem_is_dst = true;
                } else if (opcode2 == 0x70 || opcode2 == 0xc6 || opcode2 == 0x14 || opcode2 == 0x15 || opcode2 == 0x12 || opcode2 == 0x16 ||
                           (opcode2 >= 0x60 && opcode2 <= 0x62) || (opcode2 >= 0x68 && opcode2 <= 0x6a) || opcode2 == 0x6c || opcode2 == 0x6d) {
                    bool packed_unpack = (opcode2 >= 0x60 && opcode2 <= 0x62) || (opcode2 >= 0x68 && opcode2 <= 0x6a) || opcode2 == 0x6c || opcode2 == 0x6d;
                    if (packed_unpack && !operand16) { r2_set_error(error, error_len, "MMX unpack is unsupported"); return R2_ERR_UNSUPPORTED; }
                    if (opcode2 == 0x70 && !operand16 && repeat != 0xf2 && repeat != 0xf3) {
                        r2_set_error(error, error_len, "MMX shuffle is unsupported"); return R2_ERR_UNSUPPORTED;
                    }
                    status = decode_modrm(code, code_len, &ip, rex, &modrm, error, error_len);
                    if (status != R2_OK) return status;
                    insn.op = R2_OP_SSE_SHUFFLE; insn.subop = opcode2;
                    insn.width = operand16 || repeat == 0xf2 ? 8 : 4;
                    if (opcode2 == 0x70) insn.width = operand16 ? 4 : 2;
                    if (packed_unpack) {
                        insn.subop = opcode2 >= 0x68 && opcode2 != 0x6c ? 0x15 : 0x14;
                        insn.width = opcode2 >= 0x6c ? 8 : (uint8_t)(1u << (opcode2 & 7));
                    }
                    insn.src_width = repeat;
                    insn.dst = modrm.reg; insn.src = modrm.rm;
                    if (!modrm.rm_is_reg) insn.mem = modrm.mem;
                    if (opcode2 == 0xc6 || opcode2 == 0x70) {
                        uint8_t control;
                        if (!read_u8(code, code_len, &ip, &control)) return R2_ERR_DECODE;
                        insn.imm = control;
                    }
                } else if (opcode2 == 0xda || opcode2 == 0xde || opcode2 == 0xea || opcode2 == 0xee || opcode2 == 0xf4 || opcode2 == 0xf6) {
                    if (!operand16 || repeat) { r2_set_error(error, error_len, "MMX integer operands unsupported"); return R2_ERR_UNSUPPORTED; }
                    status = decode_modrm(code, code_len, &ip, rex, &modrm, error, error_len);
                    if (status != R2_OK) return status;
                    insn.op = R2_OP_PACKED_INT; insn.width = 16;
                    insn.subop = opcode2 == 0xf4 ? 23 : opcode2 == 0xf6 ? 22 :
                        (opcode2 >= 0xe0 ? 18 : 20) + (opcode2 == 0xde || opcode2 == 0xee);
                    insn.src_width = opcode2 >= 0xf0 ? 8 : opcode2 >= 0xe0 ? 2 : 1;
                    insn.dst = modrm.reg; insn.src = modrm.rm;
                    if (!modrm.rm_is_reg) insn.mem = modrm.mem;
                } else if ((opcode2 >= 0x64 && opcode2 <= 0x66) || (opcode2 >= 0x74 && opcode2 <= 0x76) ||
                           (opcode2 >= 0xfc && opcode2 <= 0xfe) || opcode2 == 0xd4 ||
                           (opcode2 >= 0xf8 && opcode2 <= 0xfb)) {
                    if (!operand16) { r2_set_error(error, error_len, "MMX integer operands are unsupported"); return R2_ERR_UNSUPPORTED; }
                    status = decode_modrm(code, code_len, &ip, rex, &modrm, error, error_len);
                    if (status != R2_OK) return status;
                    insn.op = R2_OP_PACKED_INT; insn.width = 16;
                    insn.subop = opcode2 <= 0x66 ? 0 : opcode2 <= 0x76 ? 1 :
                                  (opcode2 >= 0xfc || opcode2 == 0xd4) ? 2 : 3;
                    insn.src_width = opcode2 == 0xd4 ? 8 :
                        (uint8_t)(1u << (opcode2 - (insn.subop == 0 ? 0x64 : insn.subop == 1 ? 0x74 : insn.subop == 2 ? 0xfc : 0xf8)));
                    insn.dst = modrm.reg; insn.src = modrm.rm;
                    if (!modrm.rm_is_reg) insn.mem = modrm.mem;
                } else if (opcode2 == 0x57 || opcode2 == 0xef || opcode2 == 0x54 || opcode2 == 0x55 || opcode2 == 0x56 ||
                    opcode2 == 0xdb || opcode2 == 0xdf || opcode2 == 0xeb || opcode2 == 0x76 || opcode2 == 0x10 || opcode2 == 0x11 ||
                    opcode2 == 0x28 || opcode2 == 0x29 || opcode2 == 0x6f || opcode2 == 0x7f) {
                    if (((opcode2 == 0xef || opcode2 == 0x76 || opcode2 == 0xdb || opcode2 == 0xdf || opcode2 == 0xeb) && !operand16) ||
                        ((opcode2 == 0x6f || opcode2 == 0x7f) && !operand16 && repeat != 0xf3)) {
                        r2_set_error(error, error_len, "MMX operands are not supported");
                        return R2_ERR_UNSUPPORTED;
                    }
                    status = decode_modrm(code, code_len, &ip, rex, &modrm, error, error_len);
                    if (status != R2_OK) return status;
                    insn.op = R2_OP_VECTOR;
                    insn.width = 16;
                    if (opcode2 == 0x10 || opcode2 == 0x11) {
                        if (repeat == 0xf2) insn.width = 8;
                        if (repeat == 0xf3) insn.width = 4;
                    }
                    bool store = opcode2 == 0x11 || opcode2 == 0x29 || opcode2 == 0x7f;
                    insn.imm = opcode2 == 0x76 ? 2 : opcode2 == 0x57 || opcode2 == 0xef ? 1 :
                               opcode2 == 0x54 || opcode2 == 0xdb ? 3 : opcode2 == 0x55 || opcode2 == 0xdf ? 4 :
                               opcode2 == 0x56 || opcode2 == 0xeb ? 5 : 0;
                    insn.subop = (opcode2 == 0x28 || opcode2 == 0x29 || opcode2 == 0x76 ||
                                  ((opcode2 == 0x6f || opcode2 == 0x7f) && operand16)) ? 1 : 0;
                    insn.dst = store && modrm.rm_is_reg ? modrm.rm : modrm.reg;
                    insn.src = store ? modrm.reg : modrm.rm;
                    if (!modrm.rm_is_reg) { insn.mem = modrm.mem; insn.mem_is_dst = store; }
                } else if (opcode2 == 0xb0 || opcode2 == 0xb1 || opcode2 == 0xc0 || opcode2 == 0xc1) {
                    status = decode_modrm(code, code_len, &ip, rex, &modrm, error, error_len);
                    if (status != R2_OK) return status;
                    insn.op = opcode2 < 0xc0 ? R2_OP_CMPXCHG : R2_OP_XADD;
                    insn.width = (opcode2 & 1) ? operand_width(rex) : 1;
                    insn.dst = modrm.rm;
                    insn.src = modrm.reg;
                    if (!modrm.rm_is_reg) { insn.mem = modrm.mem; insn.mem_is_dst = true; }
                    if (insn.width == 1) {
                        byte_register(insn.src, rex, &insn.src, &insn.src_high8);
                        if (modrm.rm_is_reg) byte_register(insn.dst, rex, &insn.dst, &insn.dst_high8);
                    }
                } else if (opcode2 == 0x1f) {
                    status = decode_modrm(code, code_len, &ip, rex, &modrm, error, error_len);
                    if (status != R2_OK) return status;
                    insn.op = R2_OP_NOP;
                } else if (opcode2 == 0x05) {
                    insn.op = R2_OP_SYSCALL;
                    saw_terminator_candidate = true;
                } else if (opcode2 >= 0x80 && opcode2 <= 0x8f && cc_from_opcode(opcode2, &insn.cc)) {
                    int64_t rel = 0;
                    if (!read_i32(code, code_len, &ip, &rel)) {
                        r2_set_error(error, error_len, "truncated jcc rel32 at byte offset %zu", ip);
                        return R2_ERR_DECODE;
                    }
                    insn.op = R2_OP_JCC;
                    status = rel_target(ip,
                                        rel,
                                        code_len,
                                        guest_base,
                                        require_targets_in_range,
                                        &insn.target,
                                        error,
                                        error_len);
                    if (status != R2_OK) {
                        return status;
                    }
                } else if (opcode2 == 0xa4 || opcode2 == 0xa5 || opcode2 == 0xac || opcode2 == 0xad) {
                    status = decode_modrm(code, code_len, &ip, rex, &modrm, error, error_len);
                    if (status != R2_OK) return status;
                    insn.op = R2_OP_DOUBLE_SHIFT; insn.width = operand_width(rex);
                    insn.dst = modrm.rm; insn.src = modrm.reg;
                    insn.subop = opcode2 >= 0xac;
                    insn.has_imm = !(opcode2 & 1);
                    if (!modrm.rm_is_reg) insn.mem = modrm.mem;
                    if (insn.has_imm) {
                        uint8_t count;
                        if (!read_u8(code, code_len, &ip, &count)) return R2_ERR_DECODE;
                        insn.imm = count;
                    }
                } else if (opcode2 == 0xbc || opcode2 == 0xbd) {
                    status = decode_modrm(code, code_len, &ip, rex, &modrm, error, error_len);
                    if (status != R2_OK) return status;
                    insn.op = R2_OP_BIT_SCAN; insn.width = operand_width(rex);
                    insn.subop = (opcode2 == 0xbd) | (repeat == 0xf3 ? 2 : 0);
                    insn.dst = modrm.reg; insn.src = modrm.rm;
                    if (!modrm.rm_is_reg) insn.mem = modrm.mem;
                } else if (opcode2 == 0xba || opcode2 == 0xa3 || opcode2 == 0xab ||
                           opcode2 == 0xb3 || opcode2 == 0xbb) {
                    status = decode_modrm(code, code_len, &ip, rex, &modrm, error, error_len);
                    if (status != R2_OK) return status;
                    insn.op = R2_OP_BIT; insn.width = operand_width(rex); insn.dst = modrm.rm;
                    insn.src = modrm.reg; insn.has_imm = opcode2 == 0xba;
                    if (!modrm.rm_is_reg) insn.mem = modrm.mem;
                    if (insn.has_imm) {
                        uint8_t bit;
                        if (modrm.raw_group < 4 || !read_u8(code, code_len, &ip, &bit)) return R2_ERR_DECODE;
                        insn.imm = bit; insn.subop = (uint8_t)(modrm.raw_group - 4);
                    } else insn.subop = (uint8_t)((opcode2 - 0xa3) / 8);
                } else if (opcode2 == 0xaf) {
                    status = decode_modrm(code, code_len, &ip, rex, &modrm, error, error_len);
                    if (status != R2_OK) return status;
                    insn.op = R2_OP_IMUL; insn.width = operand_width(rex); insn.dst = modrm.reg;
                    if (modrm.rm_is_reg) insn.src = modrm.rm;
                    else insn.mem = modrm.mem;
                } else if (opcode2 >= 0x40 && opcode2 <= 0x4f && cc_from_opcode(opcode2, &insn.cc)) {
                    status = decode_modrm(code, code_len, &ip, rex, &modrm, error, error_len);
                    if (status != R2_OK) return status;
                    insn.op = R2_OP_CMOV;
                    insn.width = operand_width(rex);
                    insn.dst = modrm.reg;
                    if (modrm.rm_is_reg) insn.src = modrm.rm;
                    else insn.mem = modrm.mem;
                } else if (opcode2 == 0xb6 || opcode2 == 0xb7 ||
                           opcode2 == 0xbe || opcode2 == 0xbf) {
                    status = decode_modrm(code, code_len, &ip, rex, &modrm, error, error_len);
                    if (status != R2_OK) {
                        return status;
                    }
                    insn.op = opcode2 == 0xb6 || opcode2 == 0xb7 ? R2_OP_MOVZX : R2_OP_MOVSX;
                    decode_extend_operand(&modrm, rex, (opcode2 & 1) ? 2 : 1, &insn);
                    if (insn.width == 2) {
                        r2_set_error(error, error_len, "16-bit extension destination opcode is not supported yet");
                        return R2_ERR_UNSUPPORTED;
                    }
                } else if (opcode2 >= 0x90 && opcode2 <= 0x9f && cc_from_opcode(opcode2, &insn.cc)) {
                    status = decode_modrm(code, code_len, &ip, rex, &modrm, error, error_len);
                    if (status != R2_OK) {
                        return status;
                    }
                    insn.op = R2_OP_SETCC;
                    insn.width = 1;
                    /* ModRM.reg is ignored. REX.W does not widen SETcc, and
                     * any REX prefix selects SPL/BPL/SIL/DIL over AH/CH/DH/BH. */
                    if (modrm.rm_is_reg) {
                        insn.dst_high8 = !rex.present && modrm.rm >= 4 && modrm.rm <= 7;
                        insn.dst = insn.dst_high8 ? (uint8_t)(modrm.rm - 4) : modrm.rm;
                    } else {
                        insn.mem = modrm.mem;
                    }
                } else {
                    r2_set_error(error,
                                 error_len,
                                 "unsupported 0f opcode 0x%02" PRIx8 " at byte offset %zu",
                                 opcode2,
                                 insn_start);
                    return R2_ERR_UNSUPPORTED;
                }
                break;
            }
            case 0x63:
                status = require_rex_w(rex, insn_start, error, error_len);
                if (status != R2_OK) {
                    return status;
                }
                status = decode_modrm(code, code_len, &ip, rex, &modrm, error, error_len);
                if (status != R2_OK) {
                    return status;
                }
                insn.op = R2_OP_MOVSX;
                decode_extend_operand(&modrm, rex, 4, &insn);
                break;
            case 0xa8:
            case 0xa9:
                insn.op = R2_OP_BINARY;
                insn.alu = R2_ALU_TEST;
                insn.has_imm = true;
                insn.dst = R2_X86_RAX;
                insn.width = opcode == 0xa8 ? 1 : operand_width(rex);
                if (!read_immediate(code, code_len, &ip, insn.width, &insn.imm)) return R2_ERR_DECODE;
                break;
            case 0xf6:
            case 0xf7:
                status = decode_modrm(code, code_len, &ip, rex, &modrm, error, error_len);
                if (status != R2_OK) return status;
                insn.width = opcode == 0xf6 ? 1 : operand_width(rex);
                if (modrm.rm_is_reg) {
                    insn.dst = modrm.rm;
                    if (insn.width == 1) byte_register(modrm.rm, rex, &insn.dst, &insn.dst_high8);
                } else { insn.mem = modrm.mem; insn.mem_is_dst = true; }
                if (modrm.raw_group == 0) {
                    insn.op = R2_OP_BINARY; insn.alu = R2_ALU_TEST; insn.has_imm = true;
                    if (!read_immediate(code, code_len, &ip, insn.width, &insn.imm)) return R2_ERR_DECODE;
                } else if (modrm.raw_group == 2 || modrm.raw_group == 3) {
                    insn.op = modrm.raw_group == 2 ? R2_OP_NOT : R2_OP_NEG;
                } else if (modrm.raw_group >= 4) {
                    insn.op = R2_OP_MULDIV;
                    insn.subop = modrm.raw_group;
                } else {
                    r2_set_error(error, error_len, "unsupported F6/F7 opcode group /%u", modrm.raw_group);
                    return R2_ERR_UNSUPPORTED;
                }
                break;
            case 0xc0: case 0xc1: case 0xd0: case 0xd1: case 0xd2: case 0xd3:
                status = decode_modrm(code, code_len, &ip, rex, &modrm, error, error_len);
                if (status != R2_OK) return status;
                insn.op = R2_OP_SHIFT;
                insn.width = (opcode & 1) ? operand_width(rex) : 1;
                insn.subop = modrm.raw_group;
                if (modrm.rm_is_reg) {
                    insn.dst = modrm.rm;
                    if (insn.width == 1) byte_register(modrm.rm, rex, &insn.dst, &insn.dst_high8);
                } else insn.mem = modrm.mem;
                insn.has_imm = opcode < 0xd2;
                insn.imm = 1;
                if (opcode == 0xc0 || opcode == 0xc1) {
                    uint8_t count;
                    if (!read_u8(code, code_len, &ip, &count)) return R2_ERR_DECODE;
                    insn.imm = count;
                }
                break;
            case 0x69: case 0x6b:
                status = decode_modrm(code, code_len, &ip, rex, &modrm, error, error_len);
                if (status != R2_OK) return status;
                insn.op = R2_OP_IMUL; insn.width = operand_width(rex); insn.dst = modrm.reg;
                insn.has_imm = true;
                if (modrm.rm_is_reg) insn.src = modrm.rm;
                else insn.mem = modrm.mem;
                if (!read_immediate(code, code_len, &ip, opcode == 0x6b ? 1 : insn.width, &insn.imm)) return R2_ERR_DECODE;
                break;
            case 0x98:
                insn.op = R2_OP_MOVSX; insn.width = operand_width(rex);
                insn.src_width = (uint8_t)(insn.width / 2); insn.src = insn.dst = R2_X86_RAX;
                break;
            case 0x99:
                insn.op = R2_OP_SIGN_PAIR; insn.width = operand_width(rex);
                break;
            case 0xc6:
            case 0xc7:
                status = decode_modrm(code, code_len, &ip, rex, &modrm, error, error_len);
                if (status != R2_OK) {
                    return status;
                }
                if (modrm.raw_group != 0) {
                    r2_set_error(error,
                                 error_len,
                                 "unsupported C7 opcode group /%u at byte offset %zu",
                                 modrm.raw_group,
                                 insn_start);
                    return R2_ERR_UNSUPPORTED;
                }
                insn.width = opcode == 0xc6 ? 1 : operand_width(rex);
                if (!read_immediate(code, code_len, &ip, insn.width, &insn.imm)) return R2_ERR_DECODE;
                if (modrm.rm_is_reg) {
                    insn.op = R2_OP_MOV_IMM;
                    insn.dst = modrm.rm;
                    if (insn.width == 1) byte_register(modrm.rm, rex, &insn.dst, &insn.dst_high8);
                } else {
                    insn.op = R2_OP_MOV_MEM_IMM;
                    insn.mem = modrm.mem;
                }
                break;
            case 0x8d:
                insn.width = operand_width(rex);
                status = decode_modrm(code, code_len, &ip, rex, &modrm, error, error_len);
                if (status != R2_OK) {
                    return status;
                }
                if (modrm.rm_is_reg) {
                    r2_set_error(error, error_len, "lea requires a memory addressing operand");
                    return R2_ERR_UNSUPPORTED;
                }
                insn.op = R2_OP_LEA;
                insn.dst = modrm.reg;
                insn.mem = modrm.mem;
                break;
            case 0x15:
            case 0x1d:
                insn.op = opcode == 0x15 ? R2_OP_ADC_IMM : R2_OP_SBB_IMM;
                insn.dst = R2_X86_RAX;
                insn.width = operand_width(rex);
                if (!read_i32(code, code_len, &ip, &insn.imm)) {
                    r2_set_error(error, error_len, "truncated accumulator imm32 at byte offset %zu", ip);
                    return R2_ERR_DECODE;
                }
                break;
            case 0x9c:
            case 0x9d:
                if (operand16) { r2_set_error(error, error_len, "16-bit flags stack operands are unsupported"); return R2_ERR_UNSUPPORTED; }
                insn.op = opcode == 0x9c ? R2_OP_PUSH_FLAGS : R2_OP_POP_FLAGS;
                break;
            case 0x9e:
            case 0x9f:
                insn.op = R2_OP_AH_FLAGS; insn.subop = opcode;
                break;
            case 0x86:
            case 0x87:
                status = decode_modrm(code, code_len, &ip, rex, &modrm, error, error_len);
                if (status != R2_OK) return status;
                insn.op = R2_OP_XCHG;
                insn.width = opcode == 0x86 ? 1 : operand_width(rex);
                insn.dst = modrm.rm;
                insn.src = modrm.reg;
                if (!modrm.rm_is_reg) { insn.mem = modrm.mem; insn.mem_is_dst = true; }
                if (insn.width == 1) {
                    byte_register(insn.src, rex, &insn.src, &insn.src_high8);
                    if (modrm.rm_is_reg) byte_register(insn.dst, rex, &insn.dst, &insn.dst_high8);
                }
                break;
            case 0x88:
            case 0x8a:
            case 0x89:
            case 0x8b:
            case 0x01:
            case 0x03:
            case 0x11:
            case 0x13:
            case 0x19:
            case 0x1b:
            case 0x29:
            case 0x2b:
            case 0x31:
            case 0x33:
            case 0x39:
            case 0x3b:
            case 0x85:
                status = decode_modrm(code, code_len, &ip, rex, &modrm, error, error_len);
                if (status != R2_OK) {
                    return status;
                }
                insn.width = opcode == 0x88 || opcode == 0x8a ? 1 : operand_width(rex);
                status = decode_reg_memory_opcode(insn.width == 1 ? (uint8_t)(opcode | 1) : opcode,
                                                  &modrm, &insn, error, error_len);
                if (status != R2_OK) {
                    return status;
                }
                if (insn.width == 1) {
                    if (insn.op == R2_OP_MOV_REG || insn.op == R2_OP_MOV_LOAD)
                        byte_register(insn.dst, rex, &insn.dst, &insn.dst_high8);
                    if (insn.op == R2_OP_MOV_REG || insn.op == R2_OP_MOV_STORE)
                        byte_register(insn.src, rex, &insn.src, &insn.src_high8);
                }
                break;
            case 0xfe:
            case 0xff:
                status = decode_modrm(code, code_len, &ip, rex, &modrm, error, error_len);
                if (status != R2_OK) {
                    return status;
                }
                if (modrm.raw_group == 0 || modrm.raw_group == 1) {
                    insn.op = modrm.raw_group == 0 ? R2_OP_INC : R2_OP_DEC;
                    insn.width = opcode == 0xfe ? 1 : operand_width(rex);
                    if (modrm.rm_is_reg) {
                        insn.dst = modrm.rm;
                        if (insn.width == 1) byte_register(modrm.rm, rex, &insn.dst, &insn.dst_high8);
                    } else insn.mem = modrm.mem;
                    break;
                } else if (opcode == 0xfe) {
                    r2_set_error(error, error_len, "unsupported FE opcode group");
                    return R2_ERR_UNSUPPORTED;
                } else if (modrm.raw_group == 2) {
                    insn.op = R2_OP_CALL_INDIRECT;
                } else if (modrm.raw_group == 4) {
                    insn.op = R2_OP_JMP_INDIRECT;
                } else if (modrm.raw_group == 6) {
                    if (operand16) {
                        r2_set_error(error, error_len, "16-bit stack operands are not supported yet");
                        return R2_ERR_UNSUPPORTED;
                    }
                    insn.op = R2_OP_PUSH_REG;
                } else {
                    r2_set_error(error,
                                 error_len,
                                 "unsupported FF opcode group /%u at byte offset %zu",
                                 modrm.raw_group,
                                 insn_start);
                    return R2_ERR_UNSUPPORTED;
                }
                insn.width = 8;
                if (modrm.rm_is_reg) {
                    insn.src = modrm.rm;
                } else {
                    insn.mem = modrm.mem;
                }
                break;
            case 0x81:
            case 0x83:
                status = decode_modrm(code, code_len, &ip, rex, &modrm, error, error_len);
                if (status != R2_OK) {
                    return status;
                }
                if (!modrm.rm_is_reg) {
                    r2_set_error(error,
                                 error_len,
                                 "immediate arithmetic on memory is not supported yet");
                    return R2_ERR_UNSUPPORTED;
                }
                insn.width = operand_width(rex);
                if (modrm.raw_group == 0) {
                    insn.op = R2_OP_ADD_IMM;
                } else if (modrm.raw_group == 2) {
                    insn.op = R2_OP_ADC_IMM;
                } else if (modrm.raw_group == 3) {
                    insn.op = R2_OP_SBB_IMM;
                } else if (modrm.raw_group == 5) {
                    insn.op = R2_OP_SUB_IMM;
                } else if (modrm.raw_group == 7) {
                    insn.op = R2_OP_CMP_IMM;
                } else {
                    r2_set_error(error,
                                 error_len,
                                 "unsupported arithmetic opcode group /%u at byte offset %zu",
                                 modrm.raw_group,
                                 insn_start);
                    return R2_ERR_UNSUPPORTED;
                }
                insn.dst = modrm.rm;
                if (opcode == 0x83) {
                    if (!read_i8(code, code_len, &ip, &insn.imm)) {
                        r2_set_error(error, error_len, "truncated imm8 at byte offset %zu", ip);
                        return R2_ERR_DECODE;
                    }
                } else if (insn.width == 8) {
                    if (!read_i32(code, code_len, &ip, &insn.imm)) {
                        r2_set_error(error, error_len, "truncated imm32 at byte offset %zu", ip);
                        return R2_ERR_DECODE;
                    }
                } else {
                    uint32_t imm = 0;
                    if (!read_u32(code, code_len, &ip, &imm)) {
                        r2_set_error(error, error_len, "truncated imm32 at byte offset %zu", ip);
                        return R2_ERR_DECODE;
                    }
                    insn.imm = imm;
                }
                break;
            default:
                r2_set_error(error,
                             error_len,
                             "unsupported opcode 0x%02" PRIx8 " at byte offset %zu",
                             opcode,
                             insn_start);
                return R2_ERR_UNSUPPORTED;
            }
        }

        bool locked_alu = insn.op == R2_OP_BINARY && insn.mem_is_dst && insn.alu != R2_ALU_CMP && insn.alu != R2_ALU_TEST;
        bool locked_unary = insn.op == R2_OP_INC || insn.op == R2_OP_DEC || insn.op == R2_OP_NEG || insn.op == R2_OP_NOT;
        if (locked && (!insn.mem.present ||
            (!locked_alu && !locked_unary && insn.op != R2_OP_XCHG && insn.op != R2_OP_CMPXCHG && insn.op != R2_OP_XADD))) {
            r2_set_error(error, error_len, "unsupported LOCK instruction at byte offset %zu", insn_start);
            return R2_ERR_UNSUPPORTED;
        }
        insn.locked = locked;
        if (gs_override) {
            if (!insn.mem.present) {
                r2_set_error(error,
                             error_len,
                             "GS segment override requires a memory operand at byte offset %zu",
                             insn_start);
                return R2_ERR_UNSUPPORTED;
            }
            if (insn.op != R2_OP_LEA) {
                insn.mem.gs_relative = true;
            }
        }

        insn.x86_len = ip - insn_start;
        if(single_instruction) {
            if(insn.x86_len>15)return R2_ERR_DECODE;
            *single_output=insn;return R2_OK;
        }
        status = append_insn(block, &insn, error, error_len);
        if (status != R2_OK) {
            return status;
        }
    }

    if (require_terminator && !saw_terminator_candidate) {
        r2_set_error(error, error_len, "code block does not contain ret or syscall");
        return R2_ERR_DECODE;
    }

    return R2_OK;
}

r2_status r2_decode_block(const uint8_t *code,
                          size_t code_len,
                          r2_block *block,
                          char *error,
                          size_t error_len)
{
    if (code == NULL || block == NULL) {
        r2_set_error(error, error_len, "null argument");
        return R2_ERR_BAD_ARGUMENT;
    }

    memset(block, 0, sizeof(*block));
    block->insns = block->storage;
    block->capacity = R2_MAX_INSNS;
    return decode_block_at(code, code_len, 0, true, true, false, block, NULL, error, error_len);
}

r2_status r2_decode_block_append_at(const uint8_t *code,
                                    size_t code_len,
                                    uint64_t guest_base,
                                    r2_block *block,
                                    char *error,
                                    size_t error_len)
{
    if (code == NULL || block == NULL) {
        r2_set_error(error, error_len, "null argument");
        return R2_ERR_BAD_ARGUMENT;
    }
    if (block->count > R2_MAX_INSNS) {
        r2_set_error(error, error_len, "invalid existing instruction count");
        return R2_ERR_BAD_ARGUMENT;
    }

    size_t original_count = block->count;
    if (block->insns == NULL) {
        block->insns = block->storage;
        block->capacity = R2_MAX_INSNS;
    }
    r2_status status =
        decode_block_at(code, code_len, guest_base, false, false, false, block, NULL, error, error_len);
    if (status != R2_OK) {
        block->count = original_count;
    }
    return status;
}

r2_status r2_decode_insn_at(const uint8_t *code, size_t length, uint64_t address,
                            r2_insn *insn, char *error, size_t error_len)
{
    if(!code||!insn)return R2_ERR_BAD_ARGUMENT;
    if(!length)return R2_ERR_DECODE;
    return decode_block_at(code,length,address,false,false,true,NULL,insn,error,error_len);
}
