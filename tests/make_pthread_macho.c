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
    DATA_SIZE = 24
};

typedef struct {
    uint8_t bytes[512];
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

static void build_code(code_buf *code, uint64_t text_vmaddr, uint64_t data_vmaddr)
{
    memset(code, 0, sizeof(*code));

    emit_u8(code, 0x48);
    emit_u8(code, 0x8d);
    emit_u8(code, 0x3d); /* lea mutex(%rip), %rdi */
    size_t main_mutex_lock_disp_at = code->len;
    emit_u32(code, 0);
    emit_mov_eax_imm32(code, 0x200012du); /* psynch_mutexwait */
    emit_syscall(code);

    emit_u8(code, 0x48);
    emit_u8(code, 0x8d);
    emit_u8(code, 0x3d); /* lea thread_main(%rip), %rdi */
    size_t thread_disp_at = code->len;
    emit_u32(code, 0);

    emit_u8(code, 0x31);
    emit_u8(code, 0xf6); /* xor %esi, %esi */
    emit_u8(code, 0x31);
    emit_u8(code, 0xd2); /* xor %edx, %edx */
    emit_u8(code, 0x45);
    emit_u8(code, 0x31);
    emit_u8(code, 0xd2); /* xor %r10d, %r10d */
    emit_u8(code, 0x45);
    emit_u8(code, 0x31);
    emit_u8(code, 0xc0); /* xor %r8d, %r8d */
    emit_mov_eax_imm32(code, 0x2000168u); /* bsdthread_create */
    emit_syscall(code);

    emit_mov_eax_imm32(code, 0x2000131u); /* psynch_cvwait */
    emit_u8(code, 0x48);
    emit_u8(code, 0x8d);
    emit_u8(code, 0x3d); /* lea cv(%rip), %rdi */
    size_t wait_cv_disp_at = code->len;
    emit_u32(code, 0);
    emit_u8(code, 0x4c);
    emit_u8(code, 0x8d);
    emit_u8(code, 0x15); /* lea mutex(%rip), %r10 */
    size_t wait_mutex_disp_at = code->len;
    emit_u32(code, 0);
    emit_syscall(code);

    emit_u8(code, 0x8b);
    emit_u8(code, 0x05); /* mov data(%rip), %eax */
    size_t result_disp_at = code->len;
    emit_u32(code, 0);
    emit_u8(code, 0x83);
    emit_u8(code, 0xf8);
    emit_u8(code, 42); /* cmp $42, %eax */
    emit_u8(code, 0x0f);
    emit_u8(code, 0x85);
    size_t fail_result_rel_at = code->len;
    emit_u32(code, 0);

    emit_u8(code, 0x8b);
    emit_u8(code, 0x05); /* mov tid(%rip), %eax */
    size_t tid_disp_at = code->len;
    emit_u32(code, 0);
    emit_u8(code, 0x83);
    emit_u8(code, 0xf8);
    emit_u8(code, 2); /* cmp $2, %eax */
    emit_u8(code, 0x0f);
    emit_u8(code, 0x85);
    size_t fail_tid_rel_at = code->len;
    emit_u32(code, 0);

    emit_mov_eax_imm32(code, 0x200012eu); /* psynch_mutexdrop */
    emit_u8(code, 0x48);
    emit_u8(code, 0x8d);
    emit_u8(code, 0x3d); /* lea mutex(%rip), %rdi */
    size_t main_mutex_drop_disp_at = code->len;
    emit_u32(code, 0);
    emit_syscall(code);

    emit_mov_eax_imm32(code, 42);
    emit_u8(code, 0xc3);

    size_t fail_result_offset = code->len;
    emit_mov_eax_imm32(code, 98);
    emit_u8(code, 0xc3);

    size_t fail_tid_offset = code->len;
    emit_mov_eax_imm32(code, 99);
    emit_u8(code, 0xc3);

    size_t thread_offset = code->len;
    emit_u8(code, 0x48);
    emit_u8(code, 0x8d);
    emit_u8(code, 0x3d); /* lea mutex(%rip), %rdi */
    size_t worker_mutex_lock_disp_at = code->len;
    emit_u32(code, 0);
    emit_mov_eax_imm32(code, 0x200012du); /* psynch_mutexwait */
    emit_syscall(code);

    emit_u8(code, 0xc7);
    emit_u8(code, 0x05); /* movl $42, result(%rip) */
    size_t worker_result_disp_at = code->len;
    emit_u32(code, 0);
    emit_u32(code, 42);

    emit_mov_eax_imm32(code, 0x2000174u); /* thread_selfid */
    emit_syscall(code);
    emit_u8(code, 0x89);
    emit_u8(code, 0x05); /* mov %eax, tid(%rip) */
    size_t worker_tid_disp_at = code->len;
    emit_u32(code, 0);

    emit_mov_eax_imm32(code, 0x2000130u); /* psynch_cvsignal */
    emit_u8(code, 0x48);
    emit_u8(code, 0x8d);
    emit_u8(code, 0x3d); /* lea cv(%rip), %rdi */
    size_t signal_cv_disp_at = code->len;
    emit_u32(code, 0);
    emit_syscall(code);

    emit_mov_eax_imm32(code, 0x200012eu); /* psynch_mutexdrop */
    emit_u8(code, 0x48);
    emit_u8(code, 0x8d);
    emit_u8(code, 0x3d); /* lea mutex(%rip), %rdi */
    size_t worker_mutex_drop_disp_at = code->len;
    emit_u32(code, 0);
    emit_syscall(code);

    emit_mov_eax_imm32(code, 0x2000169u); /* bsdthread_terminate */
    emit_syscall(code);
    emit_u8(code, 0xc3);

    uint64_t thread_addr = text_vmaddr + thread_offset;
    uint64_t result_addr = data_vmaddr;
    uint64_t tid_addr = data_vmaddr + 4;
    uint64_t cv_addr = data_vmaddr + 8;
    uint64_t mutex_addr = data_vmaddr + 16;
    emit_i32_at(code, main_mutex_lock_disp_at,
                (int32_t)(mutex_addr - (text_vmaddr + main_mutex_lock_disp_at + 4)));
    emit_i32_at(code, thread_disp_at,
                (int32_t)(thread_addr - (text_vmaddr + thread_disp_at + 4)));
    emit_i32_at(code, wait_cv_disp_at,
                (int32_t)(cv_addr - (text_vmaddr + wait_cv_disp_at + 4)));
    emit_i32_at(code, wait_mutex_disp_at,
                (int32_t)(mutex_addr - (text_vmaddr + wait_mutex_disp_at + 4)));
    emit_i32_at(code, result_disp_at,
                (int32_t)(result_addr - (text_vmaddr + result_disp_at + 4)));
    emit_i32_at(code, tid_disp_at,
                (int32_t)(tid_addr - (text_vmaddr + tid_disp_at + 4)));
    emit_i32_at(code, main_mutex_drop_disp_at,
                (int32_t)(mutex_addr - (text_vmaddr + main_mutex_drop_disp_at + 4)));
    emit_i32_at(code, worker_mutex_lock_disp_at,
                (int32_t)(mutex_addr - (text_vmaddr + worker_mutex_lock_disp_at + 4)));
    emit_i32_at(code, worker_result_disp_at,
                (int32_t)(result_addr - (text_vmaddr + worker_result_disp_at + 8)));
    emit_i32_at(code, worker_tid_disp_at,
                (int32_t)(tid_addr - (text_vmaddr + worker_tid_disp_at + 4)));
    emit_i32_at(code, signal_cv_disp_at,
                (int32_t)(cv_addr - (text_vmaddr + signal_cv_disp_at + 4)));
    emit_i32_at(code, worker_mutex_drop_disp_at,
                (int32_t)(mutex_addr - (text_vmaddr + worker_mutex_drop_disp_at + 4)));
    emit_i32_at(code, fail_result_rel_at,
                (int32_t)(fail_result_offset - (fail_result_rel_at + 4)));
    emit_i32_at(code, fail_tid_rel_at, (int32_t)(fail_tid_offset - (fail_tid_rel_at + 4)));
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: make_pthread_macho <output>\n");
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
    put_u32(buf, off + 52, 0);
    put_u32(buf, off + 56, 0);
    put_u32(buf, off + 60, 0);
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
    put_u32(buf, off + 52, 0);
    put_u32(buf, off + 56, 0);
    put_u32(buf, off + 60, 0);
    put_u32(buf, off + 64, 0);

    off += SECTION_64_SIZE;
    put_u32(buf, off + 0, LC_MAIN);
    put_u32(buf, off + 4, ENTRY_POINT_COMMAND_SIZE);
    put_u64(buf, off + 8, CODE_OFFSET);
    put_u64(buf, off + 16, 0);

    memcpy(buf + CODE_OFFSET, code.bytes, code.len);

    FILE *file = fopen(argv[1], "wb");
    if (file == NULL) {
        free(buf);
        return 1;
    }
    int ok = fwrite(buf, 1, file_size, file) == file_size ? 0 : 1;
    if (fclose(file) != 0) {
        ok = 1;
    }

    free(buf);
    return ok;
}
