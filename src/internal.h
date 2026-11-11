#ifndef R2_INTERNAL_H
#define R2_INTERNAL_H

#include "rozetka2.h"
#include <stdatomic.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define R2_MAX_INSNS 4096
#define R2_MAX_STEPS 1000000u
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
    R2_OP_XCHG,
    R2_OP_CMPXCHG,
    R2_OP_XADD,
    R2_OP_MOVZX,
    R2_OP_MOVSX,
    R2_OP_BINARY,
    R2_OP_INC,
    R2_OP_DEC,
    R2_OP_NEG,
    R2_OP_NOT,
    R2_OP_VECTOR,
    R2_OP_PACKED_INT,
    R2_OP_FENCE,
    R2_OP_PUSH_FLAGS,
    R2_OP_POP_FLAGS,
    R2_OP_STOS,
    R2_OP_MOVS,
    R2_OP_DIRECTION,
    R2_OP_AH_FLAGS,
    R2_OP_CPUID,
    R2_OP_RDTSC,
    R2_OP_MXCSR,
    R2_OP_FX_STATE,
    R2_OP_X87,
    R2_OP_SSE_FP,
    R2_OP_SSE_CONVERT,
    R2_OP_SSE_GP,
    R2_OP_SSE_SHUFFLE,
    R2_OP_PACKED_EXT,
    R2_OP_SHIFT,
    R2_OP_IMUL,
    R2_OP_MULDIV,
    R2_OP_SIGN_PAIR,
    R2_OP_BIT,
    R2_OP_BSWAP,
    R2_OP_DOUBLE_SHIFT,
    R2_OP_BIT_SCAN,
    R2_OP_MOV_LOAD,
    R2_OP_MOV_STORE,
    R2_OP_MOV_MEM_IMM,
    R2_OP_LEA,
    R2_OP_ADD_IMM,
    R2_OP_ADD_REG,
    R2_OP_SUB_IMM,
    R2_OP_SUB_REG,
    R2_OP_ADC_IMM,
    R2_OP_ADC_REG,
    R2_OP_SBB_IMM,
    R2_OP_SBB_REG,
    R2_OP_XOR_REG,
    R2_OP_CMP_IMM,
    R2_OP_CMP_REG,
    R2_OP_TEST_REG,
    R2_OP_PUSH_REG,
    R2_OP_PUSH_IMM,
    R2_OP_POP_REG,
    R2_OP_CALL,
    R2_OP_CALL_INDIRECT,
    R2_OP_JMP,
    R2_OP_JMP_INDIRECT,
    R2_OP_JCC,
    R2_OP_SETCC,
    R2_OP_CMOV,
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
    R2_CC_GE,
    R2_CC_B,
    R2_CC_AE,
    R2_CC_BE,
    R2_CC_A,
    R2_CC_O, R2_CC_NO, R2_CC_S, R2_CC_NS, R2_CC_P, R2_CC_NP
} r2_cc;

typedef enum {
    R2_ALU_ADD, R2_ALU_OR, R2_ALU_ADC, R2_ALU_SBB,
    R2_ALU_AND, R2_ALU_SUB, R2_ALU_XOR, R2_ALU_CMP, R2_ALU_TEST
} r2_alu;

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
    bool dst_high8;
    uint8_t src;
    uint8_t src_width;
    bool src_high8;
    bool has_imm;
    bool mem_is_dst;
    r2_alu alu;
    uint8_t subop;
    int64_t imm;
    uint64_t target;
    r2_mem_ref mem;
    uint64_t x86_offset;
    size_t x86_len;
    uint8_t encoding[15];
    bool dynamic_code;
    bool locked;
} r2_insn;

typedef struct {
    r2_insn *insns;
    r2_insn storage[R2_MAX_INSNS];
    size_t count;
    size_t capacity;
    void *runtime;
} r2_block;

typedef struct r2_native_runtime r2_native_runtime;
typedef struct { uint64_t significand; uint16_t exponent; } r2_x87_value;
typedef struct {
    r2_x87_value registers[8];
    uint16_t control, status;
    uint8_t valid;
} r2_x87_state;
enum {
    R2_X87_LOAD, R2_X87_STORE, R2_X87_LOAD_REG, R2_X87_STORE_REG,
    R2_X87_EXCHANGE, R2_X87_FREE, R2_X87_REMAINDER, R2_X87_REMAINDER_NEAREST,
    R2_X87_STATUS, R2_X87_LOAD_CONTROL, R2_X87_STORE_CONTROL, R2_X87_INIT, R2_X87_CLEAR,
    R2_X87_ZERO, R2_X87_COMPARE_FLAGS
};

typedef struct {
    uint64_t base;
    uint64_t size;
    uint8_t *data;
    bool writable;
} r2_memory_region;

typedef struct {
    enum { R2_PROCESS_RUNNING, R2_PROCESS_EXIT, R2_PROCESS_IMMEDIATE } kind;
    uint64_t code;
} r2_process_state;

typedef struct {
    uint64_t rip_base;
    bool has_initial_rip;
    uint64_t initial_rip;
    const r2_memory_region *regions;
    size_t region_count;
    const size_t *dynamic_region_count;
    r2_process_state *process;
    uint64_t stack_size;
    uint64_t borrowed_stack_base;
    bool has_initial_regs;
    uint64_t initial_regs[16];
    r2_native_runtime *native;
} r2_interp_env;

typedef struct {
    r2_block *block;
    const r2_interp_env *env;
    uint64_t stack_base, stack_size;
    uint32_t *signal_mask,*pending_signals;
    size_t *signal_frames;
} r2_native_execution;
typedef struct {
    const _Atomic bool *stopping;
    _Atomic uint32_t *thread_pending,*process_pending;
    unsigned *quantum;
} r2_native_fast_state;
void r2_native_fast_state_get(r2_native_runtime *runtime,r2_native_fast_state *state);

r2_status r2_decode_insn_at(const uint8_t *code, size_t length, uint64_t address,
                            r2_insn *insn, char *error, size_t error_len);
r2_status r2_native_decode(r2_block *block, uint64_t address, size_t *pc,
                           char *error, size_t error_len);
r2_status r2_native_decode_next(r2_block *block,uint64_t address,size_t hint,size_t *pc,char *error,size_t error_len);
r2_native_runtime *r2_native_create(char *error, size_t error_len);
void r2_native_execution_enter(r2_native_runtime *runtime, r2_block *block,
                               const r2_interp_env *env,uint32_t *signal_mask,uint32_t *pending_signals,size_t *signal_frames,r2_native_execution *previous);
void r2_native_execution_leave(r2_native_runtime *runtime, const r2_native_execution *previous);
r2_status r2_native_execution_poll(r2_native_runtime *runtime, char *error, size_t error_len);
void r2_native_request_stop(r2_native_runtime *runtime, r2_status status, const char *error);
bool r2_native_stopping(r2_native_runtime *runtime);
void r2_native_get_x87(r2_native_runtime *runtime, uint16_t *control, uint16_t *status);
void r2_native_set_x87(r2_native_runtime *runtime, uint16_t control, uint16_t status);
r2_x87_state *r2_native_x87_state(r2_native_runtime *runtime);
uint32_t r2_native_get_mxcsr(r2_native_runtime *runtime);
static inline bool r2_mxcsr_supported(uint32_t value) {
    /* Masked exceptions, nearest-even rounding; independent DAZ and FTZ. */
    return (value & UINT32_C(0xffff7f80)) == 0x1f80;
}
void r2_native_set_mxcsr(r2_native_runtime *runtime, uint32_t value);
bool r2_native_thread_returning(r2_native_runtime *runtime);
bool r2_native_current_execution(r2_native_runtime *runtime, r2_native_execution *execution);
void r2_native_stop_and_join(r2_native_runtime *runtime);
void r2_native_destroy(r2_native_runtime *runtime);
bool r2_native_library(const char *path);
r2_status r2_native_symbol(r2_native_runtime *runtime, const char *name,
                           uint64_t *address, char *error, size_t error_len);
void r2_native_add_code(r2_native_runtime *runtime, uint64_t address, uint64_t size);
bool r2_native_pop_exit(r2_native_runtime *runtime, uint64_t *function, uint64_t *argument);
void r2_native_set_regions(r2_native_runtime *runtime, const r2_memory_region *regions, const size_t *count);
void r2_native_set_stack(r2_native_runtime *runtime, uint64_t base, uint64_t size);
void *r2_native_stack_allocate(r2_native_runtime *runtime, size_t size);
void r2_native_stack_release(r2_native_runtime *runtime, void *address, size_t size);
typedef struct { uint64_t address, count; } r2_tls_range;
r2_status r2_native_add_tls(r2_native_runtime *runtime, const r2_tls_range *ranges, size_t count,
                            const void *data, size_t size, size_t alignment, char *error, size_t error_len);
typedef r2_status (*r2_symbol_lookup)(void *context, uint64_t handle, const char *symbol,
                                      uint64_t *address, bool *found, char *error, size_t error_len);
typedef struct { const char *filename; void *base; const char *symbol; void *address; } r2_dl_info;
typedef bool (*r2_address_lookup)(void *context, uint64_t address, r2_dl_info *info);
typedef r2_status (*r2_image_open)(void *context, const char *path, int flags, uint64_t caller,
                                   uint64_t *handle, char *error, size_t error_len);
typedef bool (*r2_image_close)(void *context, uint64_t handle);
typedef size_t (*r2_image_count)(void *context);
typedef const char *(*r2_image_name)(void *context, size_t index);
typedef const void *(*r2_image_header)(void *context,size_t index);
void r2_native_set_images(r2_native_runtime *runtime, r2_image_count count, r2_image_name name,r2_image_header header);
void r2_native_set_lookup(r2_native_runtime *runtime, void *context,
                          r2_symbol_lookup lookup, r2_address_lookup address_lookup,
                          r2_image_open open_image, r2_image_close close_image);
void *r2_native_pointer(r2_native_runtime *runtime, uint64_t address, size_t size,
                        bool write, char *error, size_t error_len);
bool r2_native_is_call(uint64_t number);
int r2_native_fault_code(r2_native_runtime *runtime, uint64_t address);
void r2_native_reset_signal_action(r2_native_runtime *runtime, unsigned signum);
uint32_t r2_native_pending(r2_native_runtime *runtime);
void r2_native_signal_action(r2_native_runtime *runtime, unsigned signum,
                              uint64_t *handler, uint32_t *mask, uint32_t *flags);
r2_status r2_native_call(r2_native_runtime *runtime, uint64_t regs[16],
                         uint8_t xmm[16][16],
                         bool *exited, char *error, size_t error_len);

r2_status r2_decode_block(const uint8_t *code,
                          size_t code_len,
                          r2_block *block,
                          char *error,
                          size_t error_len);

r2_status r2_decode_block_append_at(const uint8_t *code,
                                    size_t code_len,
                                    uint64_t guest_base,
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
                            bool host_libraries,
                            const char *const *argv,
                            size_t argc,
                            r2_result *result,
                            char *error,
                            size_t error_len);
r2_status r2_launch_path(const char *path,
                         bool host_libraries,
                         const char *const *guest_argv,
                         size_t guest_argc,
                         char *error,
                         size_t error_len);

void r2_set_error(char *error, size_t error_len, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

const char *r2_reg_name(uint8_t reg);

#endif
