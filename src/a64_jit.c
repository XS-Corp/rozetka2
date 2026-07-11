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

typedef uint64_t (*r2_jit_fn)(void);

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

static bool emit_mov_reg(a64_buf *buf, uint8_t rd, uint8_t rm)
{
    return emit_word(buf, 0xaa0003e0u | ((uint32_t)rm << 16) | rd);
}

static bool emit_add_reg(a64_buf *buf, uint8_t rd, uint8_t rn, uint8_t rm)
{
    return emit_word(buf, 0x8b000000u | ((uint32_t)rm << 16) |
                              ((uint32_t)rn << 5) | rd);
}

static bool emit_sub_reg(a64_buf *buf, uint8_t rd, uint8_t rn, uint8_t rm)
{
    return emit_word(buf, 0xcb000000u | ((uint32_t)rm << 16) |
                              ((uint32_t)rn << 5) | rd);
}

static bool emit_eor_reg(a64_buf *buf, uint8_t rd, uint8_t rn, uint8_t rm)
{
    return emit_word(buf, 0xca000000u | ((uint32_t)rm << 16) |
                              ((uint32_t)rn << 5) | rd);
}

static bool emit_ret(a64_buf *buf)
{
    return emit_word(buf, 0xd65f03c0u);
}

static bool emit_add_imm64(a64_buf *buf, uint8_t reg, int64_t imm)
{
    uint64_t magnitude = imm < 0 ? (uint64_t)(-imm) : (uint64_t)imm;
    if (!emit_mov_imm64(buf, 16, magnitude)) {
        return false;
    }
    if (imm < 0) {
        return emit_sub_reg(buf, reg, reg, 16);
    }
    return emit_add_reg(buf, reg, reg, 16);
}

static bool emit_sub_imm64(a64_buf *buf, uint8_t reg, int64_t imm)
{
    uint64_t magnitude = imm < 0 ? (uint64_t)(-imm) : (uint64_t)imm;
    if (!emit_mov_imm64(buf, 16, magnitude)) {
        return false;
    }
    if (imm < 0) {
        return emit_add_reg(buf, reg, reg, 16);
    }
    return emit_sub_reg(buf, reg, reg, 16);
}

static r2_status emit_block(const r2_block *block, a64_buf *buf, char *error, size_t error_len)
{
    for (uint8_t reg = 0; reg < 16; reg += 1) {
        if (!emit_mov_imm64(buf, reg, 0)) {
            r2_set_error(error, error_len, "jit buffer is too small for prologue");
            return R2_ERR_JIT;
        }
    }

    for (size_t i = 0; i < block->count; i += 1) {
        const r2_insn *insn = &block->insns[i];
        uint8_t dst = a64_reg_for_x86(insn->dst);
        uint8_t src = a64_reg_for_x86(insn->src);
        bool ok = true;

        if (insn->width != 8 && insn->op != R2_OP_NOP && insn->op != R2_OP_RET) {
            r2_set_error(error,
                         error_len,
                         "jit does not support %u-byte operands at instruction %zu yet",
                         insn->width,
                         i);
            return R2_ERR_UNSUPPORTED;
        }

        switch (insn->op) {
        case R2_OP_NOP:
            break;
        case R2_OP_MOV_IMM:
            ok = emit_mov_imm64(buf, dst, (uint64_t)insn->imm);
            break;
        case R2_OP_MOV_REG:
            ok = emit_mov_reg(buf, dst, src);
            break;
        case R2_OP_ADD_IMM:
            ok = emit_add_imm64(buf, dst, insn->imm);
            break;
        case R2_OP_ADD_REG:
            ok = emit_add_reg(buf, dst, dst, src);
            break;
        case R2_OP_SUB_IMM:
            ok = emit_sub_imm64(buf, dst, insn->imm);
            break;
        case R2_OP_SUB_REG:
            ok = emit_sub_reg(buf, dst, dst, src);
            break;
        case R2_OP_XOR_REG:
            ok = emit_eor_reg(buf, dst, dst, src);
            break;
        case R2_OP_RET:
            ok = emit_ret(buf);
            break;
        default:
            r2_set_error(error, error_len, "cannot jit unknown op at instruction %zu", i);
            return R2_ERR_UNSUPPORTED;
        }

        if (!ok) {
            r2_set_error(error, error_len, "jit buffer is too small at instruction %zu", i);
            return R2_ERR_JIT;
        }
    }

    return R2_OK;
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

    if (block->count > (SIZE_MAX / 64u) - 32u) {
        r2_set_error(error, error_len, "decoded block is too large to jit");
        return R2_ERR_JIT;
    }

    size_t cap_words = (block->count + 32u) * 8u;
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
        result->rax = fn();
    }

    (void)munmap(mem, code_size);
    return status;
}
