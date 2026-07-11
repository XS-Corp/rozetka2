#include "internal.h"

#include <inttypes.h>
#include <limits.h>
#include <string.h>

typedef struct {
    bool w;
    bool r;
    bool x;
    bool b;
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
    return rex.w ? 8 : 4;
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
                            size_t *target,
                            char *error,
                            size_t error_len)
{
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

    *target = (size_t)signed_target;
    return R2_OK;
}

static bool cc_from_opcode(uint8_t opcode, r2_cc *cc)
{
    switch (opcode & 0x0f) {
    case 0x4:
        *cc = R2_CC_E;
        return true;
    case 0x5:
        *cc = R2_CC_NE;
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
                     " is not supported yet at byte offset %zu",
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
                     "unsupported register opcode 0x%02" PRIx8 " at byte offset %zu",
                     opcode,
                     insn->x86_offset);
        return R2_ERR_UNSUPPORTED;
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

    size_t ip = 0;
    bool saw_terminator_candidate = false;
    while (ip < code_len) {
        size_t insn_start = ip;
        bool gs_override = false;
        while (ip < code_len && code[ip] == 0x65) {
            gs_override = true;
            ip += 1;
        }
        rex_prefix rex = read_rex(code, code_len, &ip);

        uint8_t opcode = 0;
        if (!read_u8(code, code_len, &ip, &opcode)) {
            r2_set_error(error, error_len, "truncated opcode at byte offset %zu", insn_start);
            return R2_ERR_DECODE;
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
            .x86_offset = insn_start,
            .x86_len = 0,
        };

        r2_status status = R2_OK;
        modrm_decoded modrm;

        if (opcode >= 0xb8 && opcode <= 0xbf) {
            insn.op = R2_OP_MOV_IMM;
            insn.width = operand_width(rex);
            insn.dst = (uint8_t)((opcode - 0xb8) | (rex.b ? 8 : 0));
            if (rex.w) {
                if (!read_i64(code, code_len, &ip, &insn.imm)) {
                    r2_set_error(error, error_len, "truncated imm64 at byte offset %zu", ip);
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
            status = rel_target(ip, rel, code_len, &insn.target, error, error_len);
            if (status != R2_OK) {
                return status;
            }
        } else {
            switch (opcode) {
            case 0x90:
                insn.op = R2_OP_NOP;
                break;
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
                status = rel_target(ip, rel, code_len, &insn.target, error, error_len);
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
                status = rel_target(ip, rel, code_len, &insn.target, error, error_len);
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
                if (opcode2 == 0x05) {
                    insn.op = R2_OP_SYSCALL;
                    saw_terminator_candidate = true;
                } else if (opcode2 >= 0x80 && opcode2 <= 0x8f && cc_from_opcode(opcode2, &insn.cc)) {
                    int64_t rel = 0;
                    if (!read_i32(code, code_len, &ip, &rel)) {
                        r2_set_error(error, error_len, "truncated jcc rel32 at byte offset %zu", ip);
                        return R2_ERR_DECODE;
                    }
                    insn.op = R2_OP_JCC;
                    status = rel_target(ip, rel, code_len, &insn.target, error, error_len);
                    if (status != R2_OK) {
                        return status;
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
                insn.width = operand_width(rex);
                if (insn.width == 8) {
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
                if (modrm.rm_is_reg) {
                    insn.op = R2_OP_MOV_IMM;
                    insn.dst = modrm.rm;
                } else {
                    insn.op = R2_OP_MOV_MEM_IMM;
                    insn.mem = modrm.mem;
                }
                break;
            case 0x8d:
                status = require_rex_w(rex, insn_start, error, error_len);
                if (status != R2_OK) {
                    return status;
                }
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
            case 0x89:
            case 0x8b:
            case 0x01:
            case 0x03:
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
                insn.width = operand_width(rex);
                status = decode_reg_memory_opcode(opcode, &modrm, &insn, error, error_len);
                if (status != R2_OK) {
                    return status;
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
        status = append_insn(block, &insn, error, error_len);
        if (status != R2_OK) {
            return status;
        }
    }

    if (!saw_terminator_candidate) {
        r2_set_error(error, error_len, "code block does not contain ret or syscall");
        return R2_ERR_DECODE;
    }

    return R2_OK;
}
