#include "internal.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#endif

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

#ifndef MAP_JIT
#define MAP_JIT 0
#endif

typedef struct {
    uint32_t *words;
    size_t count;
    size_t cap;
} a64_buf;

typedef uint64_t (*r2_jit_fn)(uint64_t *runtime_error);

static uint8_t a64_reg_for_x86(uint8_t x86_reg)
{
    return x86_reg;
}

static bool emit_word(a64_buf *buf, uint32_t word)
{
    if (buf->count == buf->cap) {
        return false;
    }
    buf->words[buf->count] = word;
    buf->count += 1;
    return true;
}

static bool emit_movz(a64_buf *buf, uint8_t rd, uint16_t imm16, uint8_t hw)
{
    return emit_word(buf, 0xd2800000u | ((uint32_t)hw << 21) |
                              ((uint32_t)imm16 << 5) | rd);
}

static bool emit_movk(a64_buf *buf, uint8_t rd, uint16_t imm16, uint8_t hw)
{
    return emit_word(buf, 0xf2800000u | ((uint32_t)hw << 21) |
                              ((uint32_t)imm16 << 5) | rd);
}

static bool emit_mov_imm64(a64_buf *buf, uint8_t rd, uint64_t imm)
{
    bool emitted = false;
    for (uint8_t hw = 0; hw < 4; hw += 1) {
        uint16_t part = (uint16_t)((imm >> (hw * 16)) & 0xffffu);
        if (!emitted) {
            if (part != 0 || hw == 3) {
                if (!emit_movz(buf, rd, part, hw)) {
                    return false;
                }
                emitted = true;
            }
        } else if (part != 0) {
            if (!emit_movk(buf, rd, part, hw)) {
                return false;
            }
        }
    }

    return true;
}

/* W-register writes implement x86's zero extension of 32-bit results. */
static uint32_t operand_word(uint32_t word64, uint8_t width)
{
    return width == 4 ? word64 & ~0x80000000u : word64;
}

static bool emit_reg_op(a64_buf *buf,
                        uint32_t word64,
                        uint8_t rd,
                        uint8_t rn,
                        uint8_t rm,
                        uint8_t width)
{
    return emit_word(buf, operand_word(word64, width) | ((uint32_t)rm << 16) |
                              ((uint32_t)rn << 5) | rd);
}

static bool emit_mov_reg(a64_buf *buf, uint8_t rd, uint8_t rm, uint8_t width)
{
    return emit_reg_op(buf, 0xaa000000u, rd, 31, rm, width);
}

/* Keep C = !x86 CF at every guest instruction boundary. SUBS already has
 * this convention. ADDS and logical flag-setting instructions need C flipped;
 * the read/modify/write preserves N, Z and V and uses only scratch x16. */
static bool emit_invert_carry(a64_buf *buf)
{
    return emit_word(buf, 0xd53b4210u) && /* mrs x16, nzcv */
           emit_word(buf, 0xd2630210u) && /* eor x16, x16, #0x20000000 */
           emit_word(buf, 0xd51b4210u);   /* msr nzcv, x16 */
}

static bool emit_arith_imm(a64_buf *buf,
                           uint8_t dst,
                           uint8_t lhs,
                           int64_t imm,
                           uint8_t width,
                           bool subtract)
{
    /* Keep the signed operand intact: replacing SUB -n with ADD n would
     * produce the wrong overflow flag at the signed minimum boundary. */
    uint64_t value = width == 4 ? (uint32_t)imm : (uint64_t)imm;
    return emit_mov_imm64(buf, 16, value) &&
           emit_reg_op(buf, subtract ? 0xeb000000u : 0xab000000u,
                       dst, lhs, 16, width) &&
           (subtract || emit_invert_carry(buf));
}

static bool patch_branch(a64_buf *buf, size_t word, size_t target, bool narrow)
{
    int64_t delta = (int64_t)target - (int64_t)word;
    unsigned bits = narrow ? 19u : 26u;
    if (delta < -(INT64_C(1) << (bits - 1u)) ||
        delta >= (INT64_C(1) << (bits - 1u))) {
        return false;
    }
    uint32_t mask = (1u << bits) - 1u;
    buf->words[word] |= ((uint32_t)delta & mask) << (narrow ? 5u : 0u);
    return true;
}

static bool emit_carry_arith(a64_buf *buf, const r2_insn *insn)
{
    bool subtract = insn->op == R2_OP_SBB_IMM || insn->op == R2_OP_SBB_REG;
    bool immediate = insn->op == R2_OP_ADC_IMM || insn->op == R2_OP_SBB_IMM;
    /* ADCS consumes C = CF; SBCS consumes C = !CF. Normalize before ADCS
     * and restore the boundary convention afterward. Flip before loading an
     * immediate because carry normalization also uses scratch x16. */
    if (!subtract && !emit_invert_carry(buf)) {
        return false;
    }
    if (immediate && !emit_mov_imm64(buf, 16, insn->width == 4
                                    ? (uint32_t)insn->imm : (uint64_t)insn->imm)) {
        return false;
    }
    return emit_reg_op(buf, subtract ? 0xfa000000u : 0xba000000u,
                       insn->dst, insn->dst, immediate ? 16 : insn->src, insn->width) &&
           (subtract || emit_invert_carry(buf));
}

static bool a64_condition(r2_cc cc, uint32_t *condition)
{
    switch (cc) {
    case R2_CC_E:  *condition = 0x0; return true;
    case R2_CC_NE: *condition = 0x1; return true;
    case R2_CC_L:  *condition = 0xb; return true;
    case R2_CC_LE: *condition = 0xd; return true;
    case R2_CC_G:  *condition = 0xc; return true;
    case R2_CC_GE: *condition = 0xa; return true;
    case R2_CC_B:  *condition = 0x3; return true;
    case R2_CC_AE: *condition = 0x2; return true;
    case R2_CC_BE: *condition = 0x9; return true;
    case R2_CC_A:  *condition = 0x8; return true;
    case R2_CC_O:  *condition = 0x6; return true;
    case R2_CC_NO: *condition = 0x7; return true;
    case R2_CC_S:  *condition = 0x4; return true;
    case R2_CC_NS: *condition = 0x5; return true;
    case R2_CC_P: case R2_CC_NP: return false;
    }
    return false;
}

static bool emit_return(a64_buf *buf)
{
    return emit_word(buf, 0x910043ffu) && /* add sp, sp, #16 */
           emit_word(buf, 0xd65f03c0u);   /* ret */
}

static bool emit_runtime_error(a64_buf *buf, uint64_t code)
{
    return emit_word(buf, 0xf94003f0u) && /* ldr x16, [sp] */
           emit_mov_imm64(buf, 17, code) &&
           emit_word(buf, 0xf9000211u) && /* str x17, [x16] */
           emit_return(buf);
}

static r2_status emit_block(const r2_block *block, a64_buf *buf, char *error, size_t error_len)
{
    size_t offsets[R2_MAX_INSNS + 1];
    if (!emit_word(buf, 0xf81f0fe0u)) { /* str x0, [sp, #-16]! */
        goto buffer_full;
    }
    for (uint8_t reg = 0; reg < 16; reg += 1) {
        if (!emit_mov_imm64(buf, reg, 0)) {
            goto buffer_full;
        }
    }
    /* The interpreter allows MAX_STEPS + 1 instructions. Use non-flag-setting
     * SUB/CBZ for the guard so it cannot corrupt the guest's NZCV state. */
    if (!emit_mov_imm64(buf, 17, (uint64_t)R2_MAX_STEPS + 1u) ||
        !emit_mov_imm64(buf, 16, UINT64_C(1) << 29) ||
        !emit_word(buf, 0xd51b4210u)) { /* initial SF/ZF/OF/CF = 0, C = !CF */
        goto buffer_full;
    }

    for (size_t i = 0; i < block->count; i += 1) {
        const r2_insn *insn = &block->insns[i];
        uint8_t dst = a64_reg_for_x86(insn->dst);
        uint8_t src = a64_reg_for_x86(insn->src);
        bool ok = true;
        offsets[i] = buf->count;
        if (!emit_word(buf, 0xb4000011u) || /* cbz x17, step_limit */
            !emit_word(buf, 0xd1000631u)) { /* sub x17, x17, #1 */
            goto buffer_full;
        }

        bool valid_width = insn->op == R2_OP_SETCC ? insn->width == 1
                                                 : insn->width == 4 || insn->width == 8;
        if (!valid_width || dst >= 16 || src >= 16) {
            r2_set_error(error, error_len, "invalid jit operand at instruction %zu", i);
            return R2_ERR_UNSUPPORTED;
        }

        /* N/Z/V map directly to SF/ZF/OF. Normalizing carry lets signed and
         * unsigned branches consume flags independently of their producer. */
        switch (insn->op) {
        case R2_OP_NOP:
            break;
        case R2_OP_MOV_IMM:
            ok = emit_mov_imm64(buf, dst, insn->width == 4
                               ? (uint32_t)insn->imm : (uint64_t)insn->imm);
            break;
        case R2_OP_MOV_REG:
            ok = emit_mov_reg(buf, dst, src, insn->width);
            break;
        case R2_OP_BINARY: {
            if (insn->mem.present || insn->dst_high8 || insn->src_high8) {
                r2_set_error(error, error_len, "jit does not support arithmetic memory/byte operands yet");
                return R2_ERR_UNSUPPORTED;
            }
            if (insn->alu == R2_ALU_ADC || insn->alu == R2_ALU_SBB) {
                r2_insn operation = *insn;
                operation.op = insn->alu == R2_ALU_ADC
                    ? (insn->has_imm ? R2_OP_ADC_IMM : R2_OP_ADC_REG)
                    : (insn->has_imm ? R2_OP_SBB_IMM : R2_OP_SBB_REG);
                ok = emit_carry_arith(buf, &operation);
                break;
            }
            uint8_t rhs = src;
            if (insn->has_imm) {
                ok = emit_mov_imm64(buf, 16, insn->width == 4 ? (uint32_t)insn->imm : (uint64_t)insn->imm);
                rhs = 16;
            }
            if (insn->alu == R2_ALU_ADD) {
                ok = ok && emit_reg_op(buf, 0xab000000u, dst, dst, rhs, insn->width) && emit_invert_carry(buf);
            } else if (insn->alu == R2_ALU_SUB || insn->alu == R2_ALU_CMP) {
                ok = ok && emit_reg_op(buf, 0xeb000000u, insn->alu == R2_ALU_CMP ? 31 : dst,
                                       dst, rhs, insn->width);
            } else {
                uint32_t opcode = insn->alu == R2_ALU_OR ? 0xaa000000u
                                  : (insn->alu == R2_ALU_XOR ? 0xca000000u : 0x8a000000u);
                if (insn->alu == R2_ALU_TEST) {
                    ok = ok && emit_reg_op(buf, 0xea000000u, 31, dst, rhs, insn->width);
                } else {
                    ok = ok && emit_reg_op(buf, opcode, dst, dst, rhs, insn->width) &&
                               emit_reg_op(buf, 0xea000000u, 31, dst, dst, insn->width);
                }
                ok = ok && emit_invert_carry(buf);
            }
            break;
        }
        case R2_OP_MOVZX:
        case R2_OP_MOVSX: {
            if (insn->mem.present) {
                r2_set_error(error, error_len, "jit does not support extending memory loads yet");
                return R2_ERR_UNSUPPORTED;
            }
            if ((insn->src_width != 1 && insn->src_width != 2 &&
                 !(insn->src_width == 4 && insn->width == 8 && insn->op == R2_OP_MOVSX)) ||
                (insn->src_high8 && (insn->src_width != 1 || src >= 4))) {
                r2_set_error(error, error_len, "invalid jit extension source");
                return R2_ERR_UNSUPPORTED;
            }
            uint32_t shift = insn->src_high8 ? 8u : 0u;
            uint32_t last = shift + insn->src_width * 8u - 1u;
            uint32_t word = insn->op == R2_OP_MOVSX ? 0x93400000u : 0xd3400000u;
            if (insn->width == 4) {
                word = insn->op == R2_OP_MOVSX ? 0x13000000u : 0x53000000u;
            }
            /* SBFM/UBFM preserve NZCV. W destinations zero the upper half. */
            ok = emit_word(buf, word | (shift << 16) | (last << 10) |
                                ((uint32_t)src << 5) | dst);
            break;
        }
        case R2_OP_SETCC: {
            uint32_t condition;
            if (insn->mem.present) {
                r2_set_error(error, error_len, "jit does not support SETcc memory operands yet");
                return R2_ERR_UNSUPPORTED;
            }
            if ((insn->dst_high8 && dst >= 4) || !a64_condition(insn->cc, &condition)) {
                r2_set_error(error, error_len, "invalid jit SETcc operand or condition");
                return R2_ERR_UNSUPPORTED;
            }
            /* CSET and 64-bit BFI preserve NZCV and all destination bits
             * outside the selected byte, including the upper 32 bits. */
            uint32_t immr = insn->dst_high8 ? 56u : 0u;
            ok = emit_word(buf, 0x1a9f07f0u | ((condition ^ 1u) << 12)) &&
                 emit_word(buf, 0xb3401e00u | (immr << 16) | dst);
            break;
        }
        case R2_OP_CMOV: {
            uint32_t condition;
            if (insn->mem.present || !a64_condition(insn->cc, &condition)) {
                r2_set_error(error, error_len, "unsupported jit conditional move");
                return R2_ERR_UNSUPPORTED;
            }
            ok = emit_word(buf, operand_word(0x9a800000u, insn->width) |
                                ((uint32_t)dst << 16) | (condition << 12) |
                                ((uint32_t)src << 5) | dst);
            break;
        }
        case R2_OP_ADD_IMM:
        case R2_OP_SUB_IMM:
        case R2_OP_CMP_IMM:
            ok = emit_arith_imm(buf, insn->op == R2_OP_CMP_IMM ? 31 : dst,
                               dst, insn->imm, insn->width,
                               insn->op != R2_OP_ADD_IMM);
            break;
        case R2_OP_ADD_REG:
            ok = emit_reg_op(buf, 0xab000000u, dst, dst, src, insn->width) &&
                 emit_invert_carry(buf);
            break;
        case R2_OP_SUB_REG:
        case R2_OP_CMP_REG:
            ok = emit_reg_op(buf, 0xeb000000u,
                             insn->op == R2_OP_CMP_REG ? 31 : dst,
                             dst, src, insn->width);
            break;
        case R2_OP_ADC_IMM:
        case R2_OP_ADC_REG:
        case R2_OP_SBB_IMM:
        case R2_OP_SBB_REG:
            ok = emit_carry_arith(buf, insn);
            break;
        case R2_OP_XOR_REG:
            ok = emit_reg_op(buf, 0xca000000u, dst, dst, src, insn->width) &&
                 emit_reg_op(buf, 0xea000000u, 31, dst, dst, insn->width) &&
                 emit_invert_carry(buf);
            break;
        case R2_OP_TEST_REG:
            ok = emit_reg_op(buf, 0xea000000u, 31, dst, src, insn->width) &&
                 emit_invert_carry(buf);
            break;
        case R2_OP_JCC: {
            uint32_t condition;
            if (!a64_condition(insn->cc, &condition)) {
                r2_set_error(error, error_len, "unsupported jit condition at instruction %zu", i);
                return R2_ERR_UNSUPPORTED;
            }
            ok = emit_word(buf, 0x54000000u | condition);
            break;
        }
        case R2_OP_JMP:
            ok = emit_word(buf, 0x14000000u);
            break;
        case R2_OP_RET:
            ok = emit_return(buf);
            break;
        default:
            r2_set_error(error, error_len, "jit does not support op %u at instruction %zu yet",
                         (unsigned)insn->op, i);
            return R2_ERR_UNSUPPORTED;
        }
        if (!ok) {
            goto buffer_full;
        }
    }
    offsets[block->count] = buf->count;
    if (!emit_runtime_error(buf, 2)) { /* fallthrough past decoded code */
        goto buffer_full;
    }
    size_t step_limit = buf->count;
    if (!emit_runtime_error(buf, 1)) {
        goto buffer_full;
    }

    for (size_t i = 0; i < block->count; i += 1) {
        if (!patch_branch(buf, offsets[i], step_limit, true)) {
            goto branch_range;
        }
        const r2_insn *insn = &block->insns[i];
        if (insn->op != R2_OP_JCC && insn->op != R2_OP_JMP) {
            continue;
        }
        size_t target = 0;
        while (target < block->count && block->insns[target].x86_offset != insn->target) {
            target += 1;
        }
        if (target == block->count) {
            r2_set_error(error, error_len,
                         "jit branch target is not a decoded instruction at instruction %zu", i);
            return R2_ERR_DECODE;
        }
        if (!patch_branch(buf, offsets[i + 1] - 1u, offsets[target], insn->op == R2_OP_JCC)) {
            goto branch_range;
        }
    }
    return R2_OK;

buffer_full:
    r2_set_error(error, error_len, "jit buffer is too small");
    return R2_ERR_JIT;
branch_range:
    r2_set_error(error, error_len, "jit branch displacement is out of range");
    return R2_ERR_JIT;
}

static void *alloc_exec(size_t size)
{
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    if (MAP_JIT != 0) {
        flags |= MAP_JIT;
    }

    void *mem = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, flags, -1, 0);
    if (mem == MAP_FAILED && (flags & MAP_JIT) != 0) {
        flags &= ~MAP_JIT;
        mem = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, flags, -1, 0);
    }

    if (mem == MAP_FAILED) {
        return NULL;
    }
    return mem;
}

static void flush_icache(void *mem, size_t size)
{
#if defined(__APPLE__)
    sys_icache_invalidate(mem, size);
#else
    __builtin___clear_cache((char *)mem, (char *)mem + size);
#endif
}

static void set_jit_write_protect(bool enabled)
{
#if defined(__APPLE__) && defined(__arm64__)
    pthread_jit_write_protect_np(enabled ? 1 : 0);
#else
    (void)enabled;
#endif
}

r2_status r2_jit_block(const r2_block *block,
                       r2_result *result,
                       char *error,
                       size_t error_len)
{
    if (block == NULL || result == NULL) {
        r2_set_error(error, error_len, "null argument");
        return R2_ERR_BAD_ARGUMENT;
    }

    if (block->count == 0 || block->count > R2_MAX_INSNS) {
        r2_set_error(error, error_len, "invalid decoded instruction count for jit");
        return R2_ERR_JIT;
    }

    size_t cap_words = (block->count + 32u) * 16u;
    size_t code_size = cap_words * sizeof(uint32_t);
    long page_size_raw = sysconf(_SC_PAGESIZE);
    size_t page_size = page_size_raw > 0 ? (size_t)page_size_raw : 16384u;
    if (code_size < page_size) {
        code_size = page_size;
    }

    void *mem = alloc_exec(code_size);
    if (mem == NULL) {
        r2_set_error(error, error_len, "mmap executable memory failed: %s", strerror(errno));
        return R2_ERR_NO_MEMORY;
    }

    a64_buf buf = {
        .words = (uint32_t *)mem,
        .count = 0,
        .cap = code_size / sizeof(uint32_t),
    };

    set_jit_write_protect(false);
    r2_status status = emit_block(block, &buf, error, error_len);
    set_jit_write_protect(true);
    if (status == R2_OK) {
        flush_icache(mem, buf.count * sizeof(uint32_t));
        r2_jit_fn fn = (r2_jit_fn)mem;
        uint64_t runtime_error = 0;
        uint64_t rax = fn(&runtime_error);
        if (runtime_error == 1) {
            r2_set_error(error, error_len, "jit step limit reached");
            status = R2_ERR_UNSUPPORTED;
        } else if (runtime_error != 0) {
            r2_set_error(error, error_len, "jit ran off the decoded block");
            status = R2_ERR_DECODE;
        } else {
            result->rax = rax;
        }
    }

    (void)munmap(mem, code_size);
    return status;
}
