#ifndef R2_INTERNAL_H
#define R2_INTERNAL_H

#include "rozetka2.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define R2_MAX_INSNS 4096
#define R2_NO_REG 0xffu

typedef enum {
    R2_X86_RAX = 0,
    R2_X86_RCX = 1,
    R2_X86_RDX = 2,
    R2_X86_RBX = 3,
    R2_X86_RSP = 4,
    R2_X86_RBP = 5,
    R2_X86_RSI = 6,
    R2_X86_RDI = 7,
    R2_X86_R8 = 8,
    R2_X86_R9 = 9,
    R2_X86_R10 = 10,
    R2_X86_R11 = 11,
    R2_X86_R12 = 12,
    R2_X86_R13 = 13,
    R2_X86_R14 = 14,
    R2_X86_R15 = 15
} r2_x86_reg;

typedef enum {
    R2_OP_NOP = 0,
    R2_OP_MOV_IMM,
    R2_OP_MOV_REG,
    R2_OP_MOV_LOAD,
    R2_OP_MOV_STORE,
    R2_OP_MOV_MEM_IMM,
    R2_OP_LEA,
    R2_OP_ADD_IMM,
    R2_OP_ADD_REG,
    R2_OP_SUB_IMM,
    R2_OP_SUB_REG,
    R2_OP_XOR_REG,
    R2_OP_CMP_IMM,
    R2_OP_CMP_REG,
    R2_OP_TEST_REG,
    R2_OP_PUSH_REG,
    R2_OP_PUSH_IMM,
    R2_OP_POP_REG,
    R2_OP_CALL,
    R2_OP_JMP,
    R2_OP_JCC,
    R2_OP_SYSCALL,
    R2_OP_LEAVE,
    R2_OP_RET
} r2_op;

typedef enum {
    R2_CC_E = 0,
    R2_CC_NE,
    R2_CC_L,
    R2_CC_LE,
    R2_CC_G,
    R2_CC_GE
} r2_cc;

typedef struct {
    bool present;
    bool rip_relative;
    bool gs_relative;
    uint8_t base;
    uint8_t index;
    uint8_t scale;
    int64_t disp;
} r2_mem_ref;

typedef struct {
    r2_op op;
    r2_cc cc;
    uint8_t width;
    uint8_t dst;
    uint8_t src;
    int64_t imm;
    size_t target;
    r2_mem_ref mem;
    size_t x86_offset;
    size_t x86_len;
} r2_insn;

typedef struct {
    r2_insn insns[R2_MAX_INSNS];
    size_t count;
} r2_block;

typedef struct {
    uint64_t base;
    uint64_t size;
    uint8_t *data;
    bool writable;
} r2_memory_region;

typedef struct {
    uint64_t rip_base;
    const r2_memory_region *regions;
    size_t region_count;
    bool has_initial_regs;
    uint64_t initial_regs[16];
} r2_interp_env;

r2_status r2_decode_block(const uint8_t *code,
                          size_t code_len,
                          r2_block *block,
                          char *error,
                          size_t error_len);

r2_status r2_interp_block(const r2_block *block,
                          r2_result *result,
                          char *error,
                          size_t error_len);

r2_status r2_interp_block_with_env(const r2_block *block,
                                   const r2_interp_env *env,
                                   r2_result *result,
                                   char *error,
                                   size_t error_len);

r2_status r2_jit_block(const r2_block *block,
                       r2_result *result,
                       char *error,
                       size_t error_len);

r2_status r2_inspect_macho_file(const char *path, char *error, size_t error_len);
r2_status r2_run_macho_file(const char *path,
                            const char *const *argv,
                            size_t argc,
                            r2_result *result,
                            char *error,
                            size_t error_len);
r2_status r2_launch_path(const char *path,
                         const char *const *guest_argv,
                         size_t guest_argc,
                         char *error,
                         size_t error_len);

void r2_set_error(char *error, size_t error_len, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

const char *r2_reg_name(uint8_t reg);

#endif
