#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    DATA_SIZE = 128,
    PTHREAD_OFFSET = 16,
    PTHREAD_TSD_OFFSET = 16,
    PTHREAD_MACH_THREAD_SELF_OFFSET = 24,
    REGISTRATION_DATA_OFFSET = 64,
    REGISTRATION_DATA_SIZE = 56,
    REGISTRATION_TSD_OFFSET = REGISTRATION_DATA_OFFSET + 24,
    REGISTRATION_MACH_THREAD_SELF_OFFSET = REGISTRATION_DATA_OFFSET + 32,
    REGISTRATION_STACK_HINT_OFFSET = REGISTRATION_DATA_OFFSET + 36
};

typedef struct {
    uint8_t bytes[768];
    size_t len;
} code_buf;

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

static void emit_i32_at(code_buf *code, size_t offset, int32_t value)
{
    put_i32(code->bytes, offset, value);
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

static size_t emit_lea_rip_rax(code_buf *code)
{
    emit_u8(code, 0x48);
    emit_u8(code, 0x8d);
    emit_u8(code, 0x05);
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

static size_t emit_movl_imm32_rip(code_buf *code, uint32_t value)
{
    emit_u8(code, 0xc7);
    emit_u8(code, 0x05);
    size_t disp_at = code->len;
    emit_u32(code, 0);
    emit_u32(code, value);
    return disp_at;
}

static void patch_rel32(code_buf *code, size_t rel_at, size_t target_offset)
{
    emit_i32_at(code, rel_at, (int32_t)(target_offset - (rel_at + 4)));
}

static void patch_rip_disp(code_buf *code, size_t disp_at, uint64_t target_addr)
{
    const uint64_t text_vmaddr = 0x100000000ull;
    emit_i32_at(code, disp_at, (int32_t)(target_addr - (text_vmaddr + disp_at + 4)));
}

static void patch_rip_disp_after_imm(code_buf *code, size_t disp_at, uint64_t target_addr)
{
    const uint64_t text_vmaddr = 0x100000000ull;
    emit_i32_at(code, disp_at, (int32_t)(target_addr - (text_vmaddr + disp_at + 8)));
}

static void emit_signal_and_terminate(code_buf *code, size_t *cv_disp_at)
{
    emit_mov_eax_imm32(code, 0x2000130u); /* psynch_cvsignal */
    *cv_disp_at = emit_lea_rip_rdi(code);
    emit_syscall(code);

    emit_u8(code, 0x45);
    emit_u8(code, 0x31);
    emit_u8(code, 0xd2); /* xor %r10d, %r10d */
    emit_mov_eax_imm32(code, 0x2000169u); /* bsdthread_terminate */
    emit_syscall(code);
    emit_u8(code, 0xc3);
}

static void build_code(code_buf *code, uint64_t data_vmaddr)
{
    memset(code, 0, sizeof(*code));

    size_t fail_rels[16];
    size_t fail_rel_count = 0;

    size_t register_start_disp_at = emit_lea_rip_rdi(code);
    emit_u8(code, 0x31);
    emit_u8(code, 0xf6); /* xor %esi, %esi */
    emit_u8(code, 0xba);
    emit_u32(code, 4096); /* mov $4096, %edx */
    size_t register_data_disp_at = emit_lea_rip_r10(code);
    emit_u8(code, 0x41);
    emit_u8(code, 0xb8);
    emit_u32(code, REGISTRATION_DATA_SIZE); /* mov $sizeof(regdata), %r8d */
    emit_u8(code, 0x45);
    emit_u8(code, 0x31);
    emit_u8(code, 0xc9); /* xor %r9d, %r9d */
    emit_mov_eax_imm32(code, 0x200016eu); /* bsdthread_register */
    emit_syscall(code);
    emit_u8(code, 0x48);
    emit_u8(code, 0x83);
    emit_u8(code, 0xf8);
    emit_u8(code, 0); /* cmp $0, %rax */
    size_t fail_register_return_rel_at = emit_jcc_rel32(code, 0x85); /* jne fail */
    emit_u8(code, 0x48);
    emit_u8(code, 0x8b);
    emit_u8(code, 0x05); /* mov stack_hint(%rip), %rax */
    size_t stack_hint_disp_at = code->len;
    emit_u32(code, 0);
    emit_u8(code, 0x48);
    emit_u8(code, 0x85);
    emit_u8(code, 0xc0); /* test %rax, %rax */
    size_t fail_register_stack_rel_at = emit_jcc_rel32(code, 0x84); /* je fail */

    size_t create_worker_disp_at = emit_lea_rip_rdi(code);
    emit_u8(code, 0xbe);
    emit_u32(code, 77); /* mov $77, %esi */
    emit_u8(code, 0x31);
    emit_u8(code, 0xd2); /* xor %edx, %edx */
    size_t create_pthread_disp_at = emit_lea_rip_r10(code);
    emit_u8(code, 0x41);
    emit_u8(code, 0xb8);
    emit_u32(code, 0x01000000u); /* mov $PTHREAD_START_CUSTOM, %r8d */
    emit_mov_eax_imm32(code, 0x2000168u); /* bsdthread_create */
    emit_syscall(code);
    emit_u8(code, 0x4c);
    emit_u8(code, 0x39);
    emit_u8(code, 0xd0); /* cmp %r10, %rax */
    fail_rels[fail_rel_count++] = emit_jcc_rel32(code, 0x85); /* jne fail */

    emit_mov_eax_imm32(code, 0x2000131u); /* psynch_cvwait */
    size_t wait_cv_disp_at = emit_lea_rip_rdi(code);
    emit_u8(code, 0x45);
    emit_u8(code, 0x31);
    emit_u8(code, 0xd2); /* xor %r10d, %r10d */
    emit_syscall(code);

    emit_u8(code, 0x8b);
    emit_u8(code, 0x05); /* mov result(%rip), %eax */
    size_t result_load_disp_at = code->len;
    emit_u32(code, 0);
    emit_u8(code, 0x83);
    emit_u8(code, 0xf8);
    emit_u8(code, 42); /* cmp $42, %eax */
    emit_u8(code, 0x0f);
    emit_u8(code, 0x85);
    size_t fail_result_rel_at = code->len;
    emit_u32(code, 0);
    emit_mov_eax_imm32(code, 42);
    emit_u8(code, 0xc3);

    size_t fail_result_offset = code->len;
    emit_mov_eax_imm32(code, 98);
    emit_u8(code, 0xc3);

    size_t fail_register_offset = code->len;
    emit_mov_eax_imm32(code, 97);
    emit_u8(code, 0xc3);

    size_t thread_start_offset = code->len;
    size_t check_pthread_disp_at = emit_lea_rip_rax(code);
    emit_u8(code, 0x48);
    emit_u8(code, 0x39);
    emit_u8(code, 0xc7); /* cmp %rax, %rdi */
    fail_rels[fail_rel_count++] = emit_jcc_rel32(code, 0x85); /* jne fail */

    emit_u8(code, 0x65);
    emit_u8(code, 0x48);
    emit_u8(code, 0x8b);
    emit_u8(code, 0x04);
    emit_u8(code, 0x25);
    emit_u32(code, 0); /* mov %gs:0, %rax */
    emit_u8(code, 0x48);
    emit_u8(code, 0x39);
    emit_u8(code, 0xf8); /* cmp %rdi, %rax */
    fail_rels[fail_rel_count++] = emit_jcc_rel32(code, 0x85); /* jne fail */

    emit_u8(code, 0x65);
    emit_u8(code, 0x48);
    emit_u8(code, 0x8b);
    emit_u8(code, 0x04);
    emit_u8(code, 0x25);
    emit_u32(code, PTHREAD_MACH_THREAD_SELF_OFFSET); /* mov %gs:24, %rax */
    emit_u8(code, 0x48);
    emit_u8(code, 0x39);
    emit_u8(code, 0xf0); /* cmp %rsi, %rax */
    fail_rels[fail_rel_count++] = emit_jcc_rel32(code, 0x85); /* jne fail */

    emit_u8(code, 0x48);
    emit_u8(code, 0x85);
    emit_u8(code, 0xf6); /* test %rsi, %rsi */
    fail_rels[fail_rel_count++] = emit_jcc_rel32(code, 0x84); /* je fail */

    size_t check_worker_disp_at = emit_lea_rip_rax(code);
    emit_u8(code, 0x48);
    emit_u8(code, 0x39);
    emit_u8(code, 0xc2); /* cmp %rax, %rdx */
    fail_rels[fail_rel_count++] = emit_jcc_rel32(code, 0x85); /* jne fail */

    emit_u8(code, 0x48);
    emit_u8(code, 0x83);
    emit_u8(code, 0xf9);
    emit_u8(code, 77); /* cmp $77, %rcx */
    fail_rels[fail_rel_count++] = emit_jcc_rel32(code, 0x85); /* jne fail */

    emit_u8(code, 0x4d);
    emit_u8(code, 0x85);
    emit_u8(code, 0xc0); /* test %r8, %r8 */
    fail_rels[fail_rel_count++] = emit_jcc_rel32(code, 0x84); /* je fail */

    emit_u8(code, 0x4c);
    emit_u8(code, 0x39);
    emit_u8(code, 0xc4); /* cmp %r8, %rsp */
    fail_rels[fail_rel_count++] = emit_jcc_rel32(code, 0x85); /* jne fail */

    emit_u8(code, 0x49);
    emit_u8(code, 0x81);
    emit_u8(code, 0xf9);
    emit_u32(code, 0x11000000u); /* cmp $PTHREAD_START_CUSTOM|TSD_BASE_SET, %r9 */
    fail_rels[fail_rel_count++] = emit_jcc_rel32(code, 0x85); /* jne fail */

    emit_u8(code, 0x65);
    emit_u8(code, 0x48);
    emit_u8(code, 0x89);
    emit_u8(code, 0x0c);
    emit_u8(code, 0x25);
    emit_u32(code, 8); /* mov %rcx, %gs:8 */

    emit_u8(code, 0x48);
    emit_u8(code, 0x89);
    emit_u8(code, 0xcf); /* mov %rcx, %rdi */
    size_t call_worker_rel_at = emit_call_rel32(code);
    size_t unexpected_return_rel_at = emit_jmp_rel32(code);

    size_t worker_offset = code->len;
    emit_u8(code, 0x48);
    emit_u8(code, 0x83);
    emit_u8(code, 0xff);
    emit_u8(code, 77); /* cmp $77, %rdi */
    fail_rels[fail_rel_count++] = emit_jcc_rel32(code, 0x85); /* jne fail */
    emit_u8(code, 0x65);
    emit_u8(code, 0x48);
    emit_u8(code, 0x8b);
    emit_u8(code, 0x04);
    emit_u8(code, 0x25);
    emit_u32(code, 8); /* mov %gs:8, %rax */
    emit_u8(code, 0x48);
    emit_u8(code, 0x83);
    emit_u8(code, 0xf8);
    emit_u8(code, 77); /* cmp $77, %rax */
    fail_rels[fail_rel_count++] = emit_jcc_rel32(code, 0x85); /* jne fail */
    size_t worker_result_disp_at = emit_movl_imm32_rip(code, 42);
    size_t worker_signal_cv_disp_at = 0;
    emit_signal_and_terminate(code, &worker_signal_cv_disp_at);

    size_t fail_offset = code->len;
    size_t fail_store_disp_at = emit_movl_imm32_rip(code, 90);
    size_t fail_signal_cv_disp_at = 0;
    emit_signal_and_terminate(code, &fail_signal_cv_disp_at);

    uint64_t text_vmaddr = 0x100000000ull;
    uint64_t thread_start_addr = text_vmaddr + thread_start_offset;
    uint64_t worker_addr = text_vmaddr + worker_offset;
    uint64_t result_addr = data_vmaddr;
    uint64_t cv_addr = data_vmaddr + 8;
    uint64_t pthread_addr = data_vmaddr + PTHREAD_OFFSET;
    uint64_t register_data_addr = data_vmaddr + REGISTRATION_DATA_OFFSET;
    uint64_t stack_hint_addr = data_vmaddr + REGISTRATION_STACK_HINT_OFFSET;

    patch_rip_disp(code, register_start_disp_at, thread_start_addr);
    patch_rip_disp(code, register_data_disp_at, register_data_addr);
    patch_rel32(code, fail_register_return_rel_at, fail_register_offset);
    patch_rip_disp(code, stack_hint_disp_at, stack_hint_addr);
    patch_rel32(code, fail_register_stack_rel_at, fail_register_offset);
    patch_rip_disp(code, create_worker_disp_at, worker_addr);
    patch_rip_disp(code, create_pthread_disp_at, pthread_addr);
    patch_rip_disp(code, wait_cv_disp_at, cv_addr);
    patch_rip_disp(code, result_load_disp_at, result_addr);
    patch_rel32(code, fail_result_rel_at, fail_result_offset);
    patch_rip_disp(code, check_pthread_disp_at, pthread_addr);
    patch_rip_disp(code, check_worker_disp_at, worker_addr);
    patch_rel32(code, call_worker_rel_at, worker_offset);
    patch_rel32(code, unexpected_return_rel_at, fail_offset);
    patch_rip_disp_after_imm(code, worker_result_disp_at, result_addr);
    patch_rip_disp(code, worker_signal_cv_disp_at, cv_addr);
    patch_rip_disp_after_imm(code, fail_store_disp_at, result_addr);
    patch_rip_disp(code, fail_signal_cv_disp_at, cv_addr);

    for (size_t i = 0; i < fail_rel_count; i += 1) {
        patch_rel32(code, fail_rels[i], fail_offset);
    }
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: make_bsdthread_register_macho <output>\n");
        return 2;
    }

    const uint64_t text_vmaddr = 0x100000000ull;
    const uint64_t data_vmaddr = 0x100010000ull;

    code_buf code;
    build_code(&code, data_vmaddr);

    const size_t data_offset = CODE_OFFSET + code.len;
    const size_t file_size = data_offset + DATA_SIZE;
    uint8_t *buf = calloc(1, file_size);
    if (buf == NULL) {
        return 1;
    }
    put_u64(buf, data_offset + REGISTRATION_DATA_OFFSET, REGISTRATION_DATA_SIZE);
    put_u32(buf, data_offset + REGISTRATION_TSD_OFFSET, PTHREAD_TSD_OFFSET);
    put_u32(buf,
            data_offset + REGISTRATION_MACH_THREAD_SELF_OFFSET,
            PTHREAD_MACH_THREAD_SELF_OFFSET);
    put_u64(buf,
            data_offset + PTHREAD_OFFSET + PTHREAD_TSD_OFFSET,
            data_vmaddr + PTHREAD_OFFSET);

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
    put_u32(buf, off + 52, 3);

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
    if (fwrite(buf, 1, file_size, out) != file_size) {
        fclose(out);
        free(buf);
        return 1;
    }
    fclose(out);
    free(buf);
    return 0;
}
