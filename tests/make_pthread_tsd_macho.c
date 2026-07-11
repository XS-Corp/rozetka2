#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SUCCESS_MESSAGE "pthread tsd isolated\n"

enum {
    MH_MAGIC_64 = 0xfeedfacf,
    CPU_TYPE_X86_64 = 0x01000007,
    CPU_SUBTYPE_X86_64_ALL = 3,
    MH_EXECUTE = 2,
    LC_SEGMENT_64 = 0x19,
    LC_MAIN = 0x80000028,
    VM_PROT_READ_EXEC = 5,
    VM_PROT_READ_WRITE = 3,
    S_ATTR_PURE_INSTRUCTIONS = 0x80000000u,
    S_ATTR_SOME_INSTRUCTIONS = 0x00000400u,
    HEADER_SIZE = 32,
    SEGMENT_COMMAND_64_SIZE = 72,
    SECTION_64_SIZE = 80,
    ENTRY_POINT_COMMAND_SIZE = 24,
    LOAD_COMMAND_SIZE = ((SEGMENT_COMMAND_64_SIZE + SECTION_64_SIZE) * 2) +
                        ENTRY_POINT_COMMAND_SIZE,
    CODE_OFFSET = HEADER_SIZE + LOAD_COMMAND_SIZE,

    READY_CV_OFFSET = 0,
    GO_CV_OFFSET = 8,
    DONE_CV_OFFSET = 16,
    REGISTRATION_DATA_OFFSET = 32,
    REGISTRATION_DATA_SIZE = 56,
    REGISTRATION_TSD_FIELD_OFFSET = REGISTRATION_DATA_OFFSET + 24,
    PTHREAD_STORAGE_SIZE = 512,
    PTHREAD_TSD_OFFSET = 256,
    PTHREAD_SELF_SLOT_OFFSET = 0,
    PTHREAD_TEST_SLOT_OFFSET = 10 * 8,
    MAIN_PTHREAD_OFFSET = 128,
    WORKER1_PTHREAD_OFFSET = MAIN_PTHREAD_OFFSET + PTHREAD_STORAGE_SIZE,
    WORKER2_PTHREAD_OFFSET = WORKER1_PTHREAD_OFFSET + PTHREAD_STORAGE_SIZE,
    CONTEXT1_OFFSET = WORKER2_PTHREAD_OFFSET + PTHREAD_STORAGE_SIZE,
    CONTEXT2_OFFSET = CONTEXT1_OFFSET + 32,
    MAIN_VALUE_OFFSET = CONTEXT2_OFFSET + 32,
    MESSAGE_OFFSET = MAIN_VALUE_OFFSET + 16,
    DATA_SIZE = 2048,

    CONTEXT_VALUE_OFFSET = 0,
    CONTEXT_PTHREAD_OFFSET = 8,
    CONTEXT_RESULT_OFFSET = 16,
    SUCCESS_MESSAGE_SIZE = sizeof(SUCCESS_MESSAGE) - 1,

    SYS_WRITE = 0x2000004u,
    SYS_PSYNCH_CVSIGNAL = 0x2000130u,
    SYS_PSYNCH_CVWAIT = 0x2000131u,
    SYS_BSDTHREAD_CREATE = 0x2000168u,
    SYS_BSDTHREAD_TERMINATE = 0x2000169u,
    SYS_BSDTHREAD_REGISTER = 0x200016eu,
    SYS_THREAD_SET_TSD_BASE = 0x03000003u,
    PTHREAD_START_CUSTOM = 0x01000000u,
    PTHREAD_START_TSD_BASE_SET = 0x10000000u,
};

typedef struct {
    uint8_t bytes[4096];
    size_t len;
} code_buf;

typedef struct {
    size_t rel_at;
    uint32_t code;
} failure_patch;

static void put_u32(uint8_t *buf, size_t off, uint32_t value)
{
    buf[off + 0] = (uint8_t)(value & 0xffu);
    buf[off + 1] = (uint8_t)((value >> 8) & 0xffu);
    buf[off + 2] = (uint8_t)((value >> 16) & 0xffu);
    buf[off + 3] = (uint8_t)((value >> 24) & 0xffu);
}

static void put_i32(uint8_t *buf, size_t off, int32_t value)
{
    put_u32(buf, off, (uint32_t)value);
}

static void put_u64(uint8_t *buf, size_t off, uint64_t value)
{
    for (size_t i = 0; i < 8; i += 1) {
        buf[off + i] = (uint8_t)((value >> (i * 8)) & 0xffu);
    }
}

static void put_name16(uint8_t *buf, size_t off, const char *value)
{
    size_t len = strlen(value);
    if (len > 16) {
        len = 16;
    }
    memcpy(buf + off, value, len);
}

static void emit_u8(code_buf *code, uint8_t value)
{
    code->bytes[code->len] = value;
    code->len += 1;
}

static void emit_u32(code_buf *code, uint32_t value)
{
    put_u32(code->bytes, code->len, value);
    code->len += 4;
}

static void emit_mov_eax_imm32(code_buf *code, uint32_t value)
{
    emit_u8(code, 0xb8);
    emit_u32(code, value);
}

static void emit_syscall(code_buf *code)
{
    emit_u8(code, 0x0f);
    emit_u8(code, 0x05);
}

static size_t emit_jcc_rel32(code_buf *code, uint8_t opcode2)
{
    emit_u8(code, 0x0f);
    emit_u8(code, opcode2);
    size_t rel_at = code->len;
    emit_u32(code, 0);
    return rel_at;
}

static size_t emit_jmp_rel32(code_buf *code)
{
    emit_u8(code, 0xe9);
    size_t rel_at = code->len;
    emit_u32(code, 0);
    return rel_at;
}

static size_t emit_call_rel32(code_buf *code)
{
    emit_u8(code, 0xe8);
    size_t rel_at = code->len;
    emit_u32(code, 0);
    return rel_at;
}

static void patch_rel32(code_buf *code, size_t rel_at, size_t target_offset)
{
    put_i32(code->bytes, rel_at, (int32_t)(target_offset - (rel_at + 4)));
}

static void patch_rip_disp(code_buf *code,
                           uint64_t text_vmaddr,
                           size_t disp_at,
                           uint64_t target_addr)
{
    put_i32(code->bytes,
            disp_at,
            (int32_t)(target_addr - (text_vmaddr + disp_at + 4)));
}

static size_t emit_lea_rip_rax(code_buf *code)
{
    emit_u8(code, 0x48);
    emit_u8(code, 0x8d);
    emit_u8(code, 0x05);
    size_t disp_at = code->len;
    emit_u32(code, 0);
    return disp_at;
}

static size_t emit_lea_rip_rcx(code_buf *code)
{
    emit_u8(code, 0x48);
    emit_u8(code, 0x8d);
    emit_u8(code, 0x0d);
    size_t disp_at = code->len;
    emit_u32(code, 0);
    return disp_at;
}

static size_t emit_lea_rip_rsi(code_buf *code)
{
    emit_u8(code, 0x48);
    emit_u8(code, 0x8d);
    emit_u8(code, 0x35);
    size_t disp_at = code->len;
    emit_u32(code, 0);
    return disp_at;
}

static size_t emit_lea_rip_rdi(code_buf *code)
{
    emit_u8(code, 0x48);
    emit_u8(code, 0x8d);
    emit_u8(code, 0x3d);
    size_t disp_at = code->len;
    emit_u32(code, 0);
    return disp_at;
}

static size_t emit_lea_rip_r10(code_buf *code)
{
    emit_u8(code, 0x4c);
    emit_u8(code, 0x8d);
    emit_u8(code, 0x15);
    size_t disp_at = code->len;
    emit_u32(code, 0);
    return disp_at;
}

static size_t emit_mov_rip_rax(code_buf *code)
{
    emit_u8(code, 0x48);
    emit_u8(code, 0x8b);
    emit_u8(code, 0x05);
    size_t disp_at = code->len;
    emit_u32(code, 0);
    return disp_at;
}

static size_t emit_mov_rip_rcx(code_buf *code)
{
    emit_u8(code, 0x48);
    emit_u8(code, 0x8b);
    emit_u8(code, 0x0d);
    size_t disp_at = code->len;
    emit_u32(code, 0);
    return disp_at;
}

static size_t emit_mov_rip_eax(code_buf *code)
{
    emit_u8(code, 0x8b);
    emit_u8(code, 0x05);
    size_t disp_at = code->len;
    emit_u32(code, 0);
    return disp_at;
}

static void emit_gs_load_rax(code_buf *code, uint32_t offset)
{
    emit_u8(code, 0x65);
    emit_u8(code, 0x48);
    emit_u8(code, 0x8b);
    emit_u8(code, 0x04);
    emit_u8(code, 0x25);
    emit_u32(code, offset);
}

static void emit_gs_store_rax(code_buf *code, uint32_t offset)
{
    emit_u8(code, 0x65);
    emit_u8(code, 0x48);
    emit_u8(code, 0x89);
    emit_u8(code, 0x04);
    emit_u8(code, 0x25);
    emit_u32(code, offset);
}

static void emit_store_result(code_buf *code, uint32_t value)
{
    emit_u8(code, 0xc7);
    emit_u8(code, 0x43);
    emit_u8(code, CONTEXT_RESULT_OFFSET);
    emit_u32(code, value);
}

static void add_failure(failure_patch *failures,
                        size_t *failure_count,
                        size_t rel_at,
                        uint32_t code)
{
    failures[*failure_count].rel_at = rel_at;
    failures[*failure_count].code = code;
    *failure_count += 1;
}

static void emit_cv_wait(code_buf *code,
                         uint64_t text_vmaddr,
                         uint64_t cv_addr)
{
    emit_mov_eax_imm32(code, SYS_PSYNCH_CVWAIT);
    size_t disp_at = emit_lea_rip_rdi(code);
    patch_rip_disp(code, text_vmaddr, disp_at, cv_addr);
    emit_u8(code, 0x45);
    emit_u8(code, 0x31);
    emit_u8(code, 0xd2); /* xor %r10d, %r10d */
    emit_syscall(code);
}

static void emit_cv_signal(code_buf *code,
                           uint64_t text_vmaddr,
                           uint64_t cv_addr)
{
    emit_mov_eax_imm32(code, SYS_PSYNCH_CVSIGNAL);
    size_t disp_at = emit_lea_rip_rdi(code);
    patch_rip_disp(code, text_vmaddr, disp_at, cv_addr);
    emit_syscall(code);
}

static void emit_terminate(code_buf *code)
{
    emit_u8(code, 0x45);
    emit_u8(code, 0x31);
    emit_u8(code, 0xd2); /* xor %r10d, %r10d */
    emit_mov_eax_imm32(code, SYS_BSDTHREAD_TERMINATE);
    emit_syscall(code);
    emit_u8(code, 0xc3);
}

static void emit_create(code_buf *code,
                        uint64_t text_vmaddr,
                        uint64_t context_addr,
                        uint64_t pthread_addr,
                        size_t *worker_disp_at,
                        failure_patch *failures,
                        size_t *failure_count,
                        uint32_t failure_code)
{
    *worker_disp_at = emit_lea_rip_rdi(code);
    size_t context_disp_at = emit_lea_rip_rsi(code);
    patch_rip_disp(code, text_vmaddr, context_disp_at, context_addr);
    emit_u8(code, 0x31);
    emit_u8(code, 0xd2); /* xor %edx, %edx */
    size_t pthread_disp_at = emit_lea_rip_r10(code);
    patch_rip_disp(code, text_vmaddr, pthread_disp_at, pthread_addr);
    emit_u8(code, 0x41);
    emit_u8(code, 0xb8);
    emit_u32(code, PTHREAD_START_CUSTOM);
    emit_mov_eax_imm32(code, SYS_BSDTHREAD_CREATE);
    emit_syscall(code);
    emit_u8(code, 0x48);
    emit_u8(code, 0x85);
    emit_u8(code, 0xc0); /* test %rax, %rax */
    size_t fail_rel_at = emit_jcc_rel32(code, 0x84); /* je fail */
    add_failure(failures, failure_count, fail_rel_at, failure_code);
}

static void build_code(code_buf *code, uint64_t text_vmaddr, uint64_t data_vmaddr)
{
    memset(code, 0, sizeof(*code));

    const uint64_t ready_cv_addr = data_vmaddr + READY_CV_OFFSET;
    const uint64_t go_cv_addr = data_vmaddr + GO_CV_OFFSET;
    const uint64_t done_cv_addr = data_vmaddr + DONE_CV_OFFSET;
    const uint64_t registration_addr = data_vmaddr + REGISTRATION_DATA_OFFSET;
    const uint64_t main_pthread_addr = data_vmaddr + MAIN_PTHREAD_OFFSET;
    const uint64_t worker1_pthread_addr = data_vmaddr + WORKER1_PTHREAD_OFFSET;
    const uint64_t worker2_pthread_addr = data_vmaddr + WORKER2_PTHREAD_OFFSET;
    const uint64_t context1_addr = data_vmaddr + CONTEXT1_OFFSET;
    const uint64_t context2_addr = data_vmaddr + CONTEXT2_OFFSET;
    const uint64_t main_value_addr = data_vmaddr + MAIN_VALUE_OFFSET;
    const uint64_t message_addr = data_vmaddr + MESSAGE_OFFSET;

    failure_patch failures[24];
    size_t failure_count = 0;

    size_t register_start_disp_at = emit_lea_rip_rdi(code);
    emit_u8(code, 0x31);
    emit_u8(code, 0xf6); /* xor %esi, %esi */
    emit_u8(code, 0xba);
    emit_u32(code, PTHREAD_STORAGE_SIZE);
    size_t register_data_disp_at = emit_lea_rip_r10(code);
    patch_rip_disp(code, text_vmaddr, register_data_disp_at, registration_addr);
    emit_u8(code, 0x41);
    emit_u8(code, 0xb8);
    emit_u32(code, REGISTRATION_DATA_SIZE);
    emit_u8(code, 0x45);
    emit_u8(code, 0x31);
    emit_u8(code, 0xc9); /* xor %r9d, %r9d */
    emit_mov_eax_imm32(code, SYS_BSDTHREAD_REGISTER);
    emit_syscall(code);
    emit_u8(code, 0x48);
    emit_u8(code, 0x83);
    emit_u8(code, 0xf8);
    emit_u8(code, 0); /* cmp $0, %rax */
    add_failure(failures,
                &failure_count,
                emit_jcc_rel32(code, 0x85),
                90); /* jne fail */

    size_t main_tsd_base_disp_at = emit_lea_rip_rdi(code);
    patch_rip_disp(code,
                   text_vmaddr,
                   main_tsd_base_disp_at,
                   main_pthread_addr + PTHREAD_TSD_OFFSET);
    emit_u8(code, 0x31);
    emit_u8(code, 0xf6); /* xor %esi, %esi */
    emit_mov_eax_imm32(code, SYS_THREAD_SET_TSD_BASE);
    emit_syscall(code);
    emit_u8(code, 0x48);
    emit_u8(code, 0x83);
    emit_u8(code, 0xf8);
    emit_u8(code, 15); /* cmp $USER_CTHREAD selector, %rax */
    add_failure(failures,
                &failure_count,
                emit_jcc_rel32(code, 0x85),
                91); /* jne fail */

    emit_gs_load_rax(code, PTHREAD_SELF_SLOT_OFFSET);
    size_t main_pthread_disp_at = emit_lea_rip_rcx(code);
    patch_rip_disp(code, text_vmaddr, main_pthread_disp_at, main_pthread_addr);
    emit_u8(code, 0x48);
    emit_u8(code, 0x39);
    emit_u8(code, 0xc8); /* cmp %rcx, %rax */
    add_failure(failures,
                &failure_count,
                emit_jcc_rel32(code, 0x85),
                92); /* jne fail */

    size_t main_value_load_disp_at = emit_mov_rip_rax(code);
    patch_rip_disp(code, text_vmaddr, main_value_load_disp_at, main_value_addr);
    emit_gs_store_rax(code, PTHREAD_TEST_SLOT_OFFSET);

    size_t create_worker1_disp_at = 0;
    emit_create(code,
                text_vmaddr,
                context1_addr,
                worker1_pthread_addr,
                &create_worker1_disp_at,
                failures,
                &failure_count,
                93);
    size_t create_worker2_disp_at = 0;
    emit_create(code,
                text_vmaddr,
                context2_addr,
                worker2_pthread_addr,
                &create_worker2_disp_at,
                failures,
                &failure_count,
                94);

    emit_cv_wait(code, text_vmaddr, ready_cv_addr);
    emit_cv_wait(code, text_vmaddr, ready_cv_addr);

    /* Two queued/wakeup tokens avoid losing a broadcast before both workers wait. */
    emit_cv_signal(code, text_vmaddr, go_cv_addr);
    emit_cv_signal(code, text_vmaddr, go_cv_addr);

    emit_cv_wait(code, text_vmaddr, done_cv_addr);
    emit_cv_wait(code, text_vmaddr, done_cv_addr);

    size_t result1_load_disp_at = emit_mov_rip_eax(code);
    patch_rip_disp(code,
                   text_vmaddr,
                   result1_load_disp_at,
                   context1_addr + CONTEXT_RESULT_OFFSET);
    emit_u8(code, 0x83);
    emit_u8(code, 0xf8);
    emit_u8(code, 42); /* cmp $42, %eax */
    size_t return_result1_rel_at = emit_jcc_rel32(code, 0x85); /* jne return eax */

    size_t result2_load_disp_at = emit_mov_rip_eax(code);
    patch_rip_disp(code,
                   text_vmaddr,
                   result2_load_disp_at,
                   context2_addr + CONTEXT_RESULT_OFFSET);
    emit_u8(code, 0x83);
    emit_u8(code, 0xf8);
    emit_u8(code, 42); /* cmp $42, %eax */
    size_t return_result2_rel_at = emit_jcc_rel32(code, 0x85); /* jne return eax */

    emit_gs_load_rax(code, PTHREAD_SELF_SLOT_OFFSET);
    main_pthread_disp_at = emit_lea_rip_rcx(code);
    patch_rip_disp(code, text_vmaddr, main_pthread_disp_at, main_pthread_addr);
    emit_u8(code, 0x48);
    emit_u8(code, 0x39);
    emit_u8(code, 0xc8); /* cmp %rcx, %rax */
    add_failure(failures,
                &failure_count,
                emit_jcc_rel32(code, 0x85),
                95); /* jne fail */

    emit_gs_load_rax(code, PTHREAD_TEST_SLOT_OFFSET);
    size_t main_value_cmp_disp_at = emit_mov_rip_rcx(code);
    patch_rip_disp(code, text_vmaddr, main_value_cmp_disp_at, main_value_addr);
    emit_u8(code, 0x48);
    emit_u8(code, 0x39);
    emit_u8(code, 0xc8); /* cmp %rcx, %rax */
    add_failure(failures,
                &failure_count,
                emit_jcc_rel32(code, 0x85),
                96); /* jne fail */

    size_t worker1_slot_disp_at = emit_mov_rip_rax(code);
    patch_rip_disp(code,
                   text_vmaddr,
                   worker1_slot_disp_at,
                   worker1_pthread_addr + PTHREAD_TSD_OFFSET + PTHREAD_TEST_SLOT_OFFSET);
    size_t worker1_value_disp_at = emit_mov_rip_rcx(code);
    patch_rip_disp(code,
                   text_vmaddr,
                   worker1_value_disp_at,
                   context1_addr + CONTEXT_VALUE_OFFSET);
    emit_u8(code, 0x48);
    emit_u8(code, 0x39);
    emit_u8(code, 0xc8); /* cmp %rcx, %rax */
    add_failure(failures,
                &failure_count,
                emit_jcc_rel32(code, 0x85),
                97); /* jne fail */

    size_t worker2_slot_disp_at = emit_mov_rip_rax(code);
    patch_rip_disp(code,
                   text_vmaddr,
                   worker2_slot_disp_at,
                   worker2_pthread_addr + PTHREAD_TSD_OFFSET + PTHREAD_TEST_SLOT_OFFSET);
    size_t worker2_value_disp_at = emit_mov_rip_rcx(code);
    patch_rip_disp(code,
                   text_vmaddr,
                   worker2_value_disp_at,
                   context2_addr + CONTEXT_VALUE_OFFSET);
    emit_u8(code, 0x48);
    emit_u8(code, 0x39);
    emit_u8(code, 0xc8); /* cmp %rcx, %rax */
    add_failure(failures,
                &failure_count,
                emit_jcc_rel32(code, 0x85),
                98); /* jne fail */

    emit_mov_eax_imm32(code, SYS_WRITE);
    emit_u8(code, 0xbf);
    emit_u32(code, 1); /* stdout */
    size_t message_disp_at = emit_lea_rip_rsi(code);
    patch_rip_disp(code, text_vmaddr, message_disp_at, message_addr);
    emit_u8(code, 0xba);
    emit_u32(code, SUCCESS_MESSAGE_SIZE);
    emit_syscall(code);
    emit_u8(code, 0x48);
    emit_u8(code, 0x83);
    emit_u8(code, 0xf8);
    emit_u8(code, SUCCESS_MESSAGE_SIZE); /* cmp $message_size, %rax */
    add_failure(failures,
                &failure_count,
                emit_jcc_rel32(code, 0x85),
                99); /* jne fail */
    emit_mov_eax_imm32(code, 42);
    emit_u8(code, 0xc3);

    size_t thread_start_offset = code->len;
    emit_u8(code, 0x48);
    emit_u8(code, 0x89);
    emit_u8(code, 0xcb); /* mov %rcx, %rbx */

    emit_gs_load_rax(code, PTHREAD_SELF_SLOT_OFFSET);
    emit_u8(code, 0x48);
    emit_u8(code, 0x39);
    emit_u8(code, 0xc7); /* cmp %rax, %rdi */
    size_t start_fail_self_rel_at = emit_jcc_rel32(code, 0x85); /* jne fail */

    emit_u8(code, 0x49);
    emit_u8(code, 0x81);
    emit_u8(code, 0xf9);
    emit_u32(code, PTHREAD_START_CUSTOM | PTHREAD_START_TSD_BASE_SET);
    size_t start_fail_flags_rel_at = emit_jcc_rel32(code, 0x85); /* jne fail */

    size_t start_worker_disp_at = emit_lea_rip_rax(code);
    emit_u8(code, 0x48);
    emit_u8(code, 0x39);
    emit_u8(code, 0xc2); /* cmp %rax, %rdx */
    size_t start_fail_func_rel_at = emit_jcc_rel32(code, 0x85); /* jne fail */

    emit_u8(code, 0x48);
    emit_u8(code, 0x8b);
    emit_u8(code, 0x4b);
    emit_u8(code, CONTEXT_PTHREAD_OFFSET); /* mov 8(%rbx), %rcx */
    emit_u8(code, 0x48);
    emit_u8(code, 0x39);
    emit_u8(code, 0xcf); /* cmp %rcx, %rdi */
    size_t start_fail_context_rel_at = emit_jcc_rel32(code, 0x85); /* jne fail */

    emit_u8(code, 0x48);
    emit_u8(code, 0x89);
    emit_u8(code, 0xdf); /* mov %rbx, %rdi */
    size_t call_worker_rel_at = emit_call_rel32(code);
    size_t unexpected_return_rel_at = emit_jmp_rel32(code);

    size_t worker_offset = code->len;
    emit_u8(code, 0x48);
    emit_u8(code, 0x8b);
    emit_u8(code, 0x03); /* mov (%rbx), %rax */
    emit_gs_store_rax(code, PTHREAD_TEST_SLOT_OFFSET);
    emit_cv_signal(code, text_vmaddr, ready_cv_addr);
    emit_cv_wait(code, text_vmaddr, go_cv_addr);

    emit_gs_load_rax(code, PTHREAD_SELF_SLOT_OFFSET);
    emit_u8(code, 0x48);
    emit_u8(code, 0x8b);
    emit_u8(code, 0x4b);
    emit_u8(code, CONTEXT_PTHREAD_OFFSET); /* mov 8(%rbx), %rcx */
    emit_u8(code, 0x48);
    emit_u8(code, 0x39);
    emit_u8(code, 0xc8); /* cmp %rcx, %rax */
    size_t worker_fail_self_rel_at = emit_jcc_rel32(code, 0x85); /* jne fail */

    emit_gs_load_rax(code, PTHREAD_TEST_SLOT_OFFSET);
    emit_u8(code, 0x48);
    emit_u8(code, 0x8b);
    emit_u8(code, 0x0b); /* mov (%rbx), %rcx */
    emit_u8(code, 0x48);
    emit_u8(code, 0x39);
    emit_u8(code, 0xc8); /* cmp %rcx, %rax */
    size_t worker_fail_value_rel_at = emit_jcc_rel32(code, 0x85); /* jne fail */

    emit_store_result(code, 42);
    size_t worker_success_finish_rel_at = emit_jmp_rel32(code);

    size_t worker_fail_self_offset = code->len;
    emit_store_result(code, 85);
    size_t worker_fail_self_finish_rel_at = emit_jmp_rel32(code);

    size_t worker_fail_value_offset = code->len;
    emit_store_result(code, 86);
    size_t worker_fail_value_finish_rel_at = emit_jmp_rel32(code);

    size_t finish_offset = code->len;
    emit_cv_signal(code, text_vmaddr, done_cv_addr);
    emit_terminate(code);

    size_t start_fail_self_offset = code->len;
    emit_store_result(code, 81);
    size_t start_fail_self_arrive_rel_at = emit_jmp_rel32(code);

    size_t start_fail_flags_offset = code->len;
    emit_store_result(code, 82);
    size_t start_fail_flags_arrive_rel_at = emit_jmp_rel32(code);

    size_t start_fail_func_offset = code->len;
    emit_store_result(code, 83);
    size_t start_fail_func_arrive_rel_at = emit_jmp_rel32(code);

    size_t start_fail_context_offset = code->len;
    emit_store_result(code, 84);
    size_t start_fail_context_arrive_rel_at = emit_jmp_rel32(code);

    size_t pre_barrier_failure_arrive_offset = code->len;
    emit_cv_signal(code, text_vmaddr, ready_cv_addr);
    emit_cv_wait(code, text_vmaddr, go_cv_addr);
    size_t pre_barrier_failure_finish_rel_at = emit_jmp_rel32(code);

    size_t unexpected_return_offset = code->len;
    emit_store_result(code, 87);
    size_t unexpected_return_finish_rel_at = emit_jmp_rel32(code);

    patch_rip_disp(code,
                   text_vmaddr,
                   register_start_disp_at,
                   text_vmaddr + thread_start_offset);
    patch_rip_disp(code,
                   text_vmaddr,
                   create_worker1_disp_at,
                   text_vmaddr + worker_offset);
    patch_rip_disp(code,
                   text_vmaddr,
                   create_worker2_disp_at,
                   text_vmaddr + worker_offset);
    patch_rip_disp(code,
                   text_vmaddr,
                   start_worker_disp_at,
                   text_vmaddr + worker_offset);
    patch_rel32(code, call_worker_rel_at, worker_offset);
    patch_rel32(code, unexpected_return_rel_at, unexpected_return_offset);

    patch_rel32(code, start_fail_self_rel_at, start_fail_self_offset);
    patch_rel32(code, start_fail_flags_rel_at, start_fail_flags_offset);
    patch_rel32(code, start_fail_func_rel_at, start_fail_func_offset);
    patch_rel32(code, start_fail_context_rel_at, start_fail_context_offset);
    patch_rel32(code, worker_fail_self_rel_at, worker_fail_self_offset);
    patch_rel32(code, worker_fail_value_rel_at, worker_fail_value_offset);

    patch_rel32(code, worker_success_finish_rel_at, finish_offset);
    patch_rel32(code, worker_fail_self_finish_rel_at, finish_offset);
    patch_rel32(code, worker_fail_value_finish_rel_at, finish_offset);
    patch_rel32(code, start_fail_self_arrive_rel_at, pre_barrier_failure_arrive_offset);
    patch_rel32(code, start_fail_flags_arrive_rel_at, pre_barrier_failure_arrive_offset);
    patch_rel32(code, start_fail_func_arrive_rel_at, pre_barrier_failure_arrive_offset);
    patch_rel32(code, start_fail_context_arrive_rel_at, pre_barrier_failure_arrive_offset);
    patch_rel32(code, pre_barrier_failure_finish_rel_at, finish_offset);
    patch_rel32(code, unexpected_return_finish_rel_at, finish_offset);

    size_t return_eax_offset = code->len;
    emit_u8(code, 0xc3);
    patch_rel32(code, return_result1_rel_at, return_eax_offset);
    patch_rel32(code, return_result2_rel_at, return_eax_offset);

    for (size_t i = 0; i < failure_count; i += 1) {
        size_t failure_offset = code->len;
        patch_rel32(code, failures[i].rel_at, failure_offset);
        emit_mov_eax_imm32(code, failures[i].code);
        emit_u8(code, 0xc3);
    }
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: make_pthread_tsd_macho <output>\n");
        return 2;
    }

    const uint64_t text_vmaddr = 0x100000000ull;
    const uint64_t data_vmaddr = 0x100010000ull;

    code_buf code;
    build_code(&code, text_vmaddr, data_vmaddr);

    const size_t data_offset = CODE_OFFSET + code.len;
    const size_t file_size = data_offset + DATA_SIZE;
    uint8_t *buf = calloc(1, file_size);
    if (buf == NULL) {
        return 1;
    }

    put_u64(buf, data_offset + REGISTRATION_DATA_OFFSET, REGISTRATION_DATA_SIZE);
    put_u32(buf, data_offset + REGISTRATION_TSD_FIELD_OFFSET, PTHREAD_TSD_OFFSET);

    put_u64(buf,
            data_offset + MAIN_PTHREAD_OFFSET + PTHREAD_TSD_OFFSET,
            data_vmaddr + MAIN_PTHREAD_OFFSET);
    put_u64(buf,
            data_offset + WORKER1_PTHREAD_OFFSET + PTHREAD_TSD_OFFSET,
            data_vmaddr + WORKER1_PTHREAD_OFFSET);
    put_u64(buf,
            data_offset + WORKER2_PTHREAD_OFFSET + PTHREAD_TSD_OFFSET,
            data_vmaddr + WORKER2_PTHREAD_OFFSET);

    put_u64(buf,
            data_offset + CONTEXT1_OFFSET + CONTEXT_VALUE_OFFSET,
            0x1111111111111111ull);
    put_u64(buf,
            data_offset + CONTEXT1_OFFSET + CONTEXT_PTHREAD_OFFSET,
            data_vmaddr + WORKER1_PTHREAD_OFFSET);
    put_u64(buf,
            data_offset + CONTEXT2_OFFSET + CONTEXT_VALUE_OFFSET,
            0x2222222222222222ull);
    put_u64(buf,
            data_offset + CONTEXT2_OFFSET + CONTEXT_PTHREAD_OFFSET,
            data_vmaddr + WORKER2_PTHREAD_OFFSET);
    put_u64(buf, data_offset + MAIN_VALUE_OFFSET, 0x3333333333333333ull);
    memcpy(buf + data_offset + MESSAGE_OFFSET, SUCCESS_MESSAGE, SUCCESS_MESSAGE_SIZE);

    put_u32(buf, 0, MH_MAGIC_64);
    put_u32(buf, 4, CPU_TYPE_X86_64);
    put_u32(buf, 8, CPU_SUBTYPE_X86_64_ALL);
    put_u32(buf, 12, MH_EXECUTE);
    put_u32(buf, 16, 3);
    put_u32(buf, 20, LOAD_COMMAND_SIZE);
    put_u32(buf, 24, 0);
    put_u32(buf, 28, 0);

    size_t off = HEADER_SIZE;
    put_u32(buf, off + 0, LC_SEGMENT_64);
    put_u32(buf, off + 4, SEGMENT_COMMAND_64_SIZE + SECTION_64_SIZE);
    put_name16(buf, off + 8, "__TEXT");
    put_u64(buf, off + 24, text_vmaddr);
    put_u64(buf, off + 32, code.len);
    put_u64(buf, off + 40, CODE_OFFSET);
    put_u64(buf, off + 48, code.len);
    put_u32(buf, off + 56, VM_PROT_READ_EXEC);
    put_u32(buf, off + 60, VM_PROT_READ_EXEC);
    put_u32(buf, off + 64, 1);
    put_u32(buf, off + 68, 0);

    off += SEGMENT_COMMAND_64_SIZE;
    put_name16(buf, off + 0, "__text");
    put_name16(buf, off + 16, "__TEXT");
    put_u64(buf, off + 32, text_vmaddr);
    put_u64(buf, off + 40, code.len);
    put_u32(buf, off + 48, CODE_OFFSET);
    put_u32(buf, off + 52, 4);
    put_u32(buf, off + 64, S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS);

    off += SECTION_64_SIZE;
    put_u32(buf, off + 0, LC_SEGMENT_64);
    put_u32(buf, off + 4, SEGMENT_COMMAND_64_SIZE + SECTION_64_SIZE);
    put_name16(buf, off + 8, "__DATA");
    put_u64(buf, off + 24, data_vmaddr);
    put_u64(buf, off + 32, DATA_SIZE);
    put_u64(buf, off + 40, data_offset);
    put_u64(buf, off + 48, DATA_SIZE);
    put_u32(buf, off + 56, VM_PROT_READ_WRITE);
    put_u32(buf, off + 60, VM_PROT_READ_WRITE);
    put_u32(buf, off + 64, 1);
    put_u32(buf, off + 68, 0);

    off += SEGMENT_COMMAND_64_SIZE;
    put_name16(buf, off + 0, "__data");
    put_name16(buf, off + 16, "__DATA");
    put_u64(buf, off + 32, data_vmaddr);
    put_u64(buf, off + 40, DATA_SIZE);
    put_u32(buf, off + 48, (uint32_t)data_offset);
    put_u32(buf, off + 52, 4);

    off += SECTION_64_SIZE;
    put_u32(buf, off + 0, LC_MAIN);
    put_u32(buf, off + 4, ENTRY_POINT_COMMAND_SIZE);
    put_u64(buf, off + 8, CODE_OFFSET);
    put_u64(buf, off + 16, 0);

    memcpy(buf + CODE_OFFSET, code.bytes, code.len);

    FILE *out = fopen(argv[1], "wb");
    if (out == NULL) {
        free(buf);
        return 1;
    }
    int ok = fwrite(buf, 1, file_size, out) == file_size ? 0 : 1;
    if (fclose(out) != 0) {
        ok = 1;
    }
    free(buf);
    return ok;
}
