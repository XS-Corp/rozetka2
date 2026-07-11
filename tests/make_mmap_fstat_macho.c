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
    S_ATTR_PURE_INSTRUCTIONS = 0x80000000u,
    S_ATTR_SOME_INSTRUCTIONS = 0x00000400u,
    HEADER_SIZE = 32,
    SEGMENT_COMMAND_64_SIZE = 72,
    SECTION_64_SIZE = 80,
    ENTRY_POINT_COMMAND_SIZE = 24,
    LOAD_COMMAND_SIZE = SEGMENT_COMMAND_64_SIZE + (SECTION_64_SIZE * 2) +
                        ENTRY_POINT_COMMAND_SIZE,
    CODE_OFFSET = HEADER_SIZE + LOAD_COMMAND_SIZE,
    STAT_SIZE = 144,
    STAT_ST_SIZE_OFFSET = 96
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

static void build_code(code_buf *code, uint64_t vmaddr, size_t path_offset)
{
    memset(code, 0, sizeof(*code));

    emit_mov_eax_imm32(code, 0x2000005u); /* open */
    emit_u8(code, 0x48);
    emit_u8(code, 0x8d);
    emit_u8(code, 0x3d); /* lea path(%rip), %rdi */
    size_t path_disp_at = code->len;
    emit_u32(code, 0);
    emit_u8(code, 0x31);
    emit_u8(code, 0xf6); /* xor %esi, %esi */
    emit_u8(code, 0x31);
    emit_u8(code, 0xd2); /* xor %edx, %edx */
    emit_u8(code, 0x0f);
    emit_u8(code, 0x05);

    emit_u8(code, 0x49);
    emit_u8(code, 0x89);
    emit_u8(code, 0xc0); /* mov %rax, %r8 */
    emit_u8(code, 0x48);
    emit_u8(code, 0x81);
    emit_u8(code, 0xec);
    emit_u32(code, STAT_SIZE); /* sub $STAT_SIZE, %rsp */

    emit_mov_eax_imm32(code, 0x20000bdu); /* fstat */
    emit_u8(code, 0x4c);
    emit_u8(code, 0x89);
    emit_u8(code, 0xc7); /* mov %r8, %rdi */
    emit_u8(code, 0x48);
    emit_u8(code, 0x89);
    emit_u8(code, 0xe6); /* mov %rsp, %rsi */
    emit_u8(code, 0x0f);
    emit_u8(code, 0x05);

    emit_u8(code, 0x4c);
    emit_u8(code, 0x8b);
    emit_u8(code, 0x4c);
    emit_u8(code, 0x24);
    emit_u8(code, STAT_ST_SIZE_OFFSET); /* mov 0x60(%rsp), %r9 */
    emit_u8(code, 0x4d);
    emit_u8(code, 0x89);
    emit_u8(code, 0xcc); /* mov %r9, %r12 */

    emit_mov_eax_imm32(code, 0x20000c5u); /* mmap */
    emit_u8(code, 0x31);
    emit_u8(code, 0xff); /* xor %edi, %edi */
    emit_u8(code, 0x4c);
    emit_u8(code, 0x89);
    emit_u8(code, 0xce); /* mov %r9, %rsi */
    emit_u8(code, 0xba);
    emit_u32(code, 1); /* mov $PROT_READ, %edx */
    emit_u8(code, 0x49);
    emit_u8(code, 0xc7);
    emit_u8(code, 0xc2);
    emit_u32(code, 2); /* mov $MAP_PRIVATE, %r10 */
    emit_u8(code, 0x4d);
    emit_u8(code, 0x31);
    emit_u8(code, 0xc9); /* xor %r9, %r9 */
    emit_u8(code, 0x0f);
    emit_u8(code, 0x05);

    emit_u8(code, 0x48);
    emit_u8(code, 0x89);
    emit_u8(code, 0xc6); /* mov %rax, %rsi */
    emit_u8(code, 0x4c);
    emit_u8(code, 0x89);
    emit_u8(code, 0xe2); /* mov %r12, %rdx */
    emit_mov_eax_imm32(code, 0x2000004u); /* write */
    emit_u8(code, 0xbf);
    emit_u32(code, 1); /* mov $1, %edi */
    emit_u8(code, 0x0f);
    emit_u8(code, 0x05);

    emit_mov_eax_imm32(code, 0x2000006u); /* close */
    emit_u8(code, 0x4c);
    emit_u8(code, 0x89);
    emit_u8(code, 0xc7); /* mov %r8, %rdi */
    emit_u8(code, 0x0f);
    emit_u8(code, 0x05);

    emit_u8(code, 0x48);
    emit_u8(code, 0x81);
    emit_u8(code, 0xc4);
    emit_u32(code, STAT_SIZE); /* add $STAT_SIZE, %rsp */
    emit_mov_eax_imm32(code, 42);
    emit_u8(code, 0xc3);

    uint64_t next_rip = vmaddr + CODE_OFFSET + path_disp_at + 4;
    uint64_t path_addr = vmaddr + path_offset;
    emit_i32_at(code, path_disp_at, (int32_t)(path_addr - next_rip));
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: make_mmap_fstat_macho <output> <input-path>\n");
        return 2;
    }

    const char *path = argv[2];
    size_t path_size = strlen(path) + 1;
    const uint64_t vmaddr = 0x100000000ull;

    code_buf code;
    build_code(&code, vmaddr, CODE_OFFSET);
    size_t const_offset = CODE_OFFSET + code.len;
    build_code(&code, vmaddr, const_offset);

    const size_t file_size = const_offset + path_size;
    uint8_t *buf = calloc(1, file_size);
    if (buf == NULL) {
        return 1;
    }

    put_u32(buf, 0, MH_MAGIC_64);
    put_u32(buf, 4, CPU_TYPE_X86_64);
    put_u32(buf, 8, CPU_SUBTYPE_X86_64_ALL);
    put_u32(buf, 12, MH_EXECUTE);
    put_u32(buf, 16, 2);
    put_u32(buf, 20, LOAD_COMMAND_SIZE);
    put_u32(buf, 24, 0);
    put_u32(buf, 28, 0);

    size_t off = HEADER_SIZE;
    put_u32(buf, off + 0, LC_SEGMENT_64);
    put_u32(buf, off + 4, SEGMENT_COMMAND_64_SIZE + (SECTION_64_SIZE * 2));
    put_name16(buf, off + 8, "__TEXT");
    put_u64(buf, off + 24, vmaddr);
    put_u64(buf, off + 32, file_size);
    put_u64(buf, off + 40, 0);
    put_u64(buf, off + 48, file_size);
    put_u32(buf, off + 56, VM_PROT_READ_EXEC);
    put_u32(buf, off + 60, VM_PROT_READ_EXEC);
    put_u32(buf, off + 64, 2);
    put_u32(buf, off + 68, 0);

    off += SEGMENT_COMMAND_64_SIZE;
    put_name16(buf, off + 0, "__text");
    put_name16(buf, off + 16, "__TEXT");
    put_u64(buf, off + 32, vmaddr + CODE_OFFSET);
    put_u64(buf, off + 40, code.len);
    put_u32(buf, off + 48, CODE_OFFSET);
    put_u32(buf, off + 52, 0);
    put_u32(buf, off + 56, 0);
    put_u32(buf, off + 60, 0);
    put_u32(buf, off + 64, S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS);

    off += SECTION_64_SIZE;
    put_name16(buf, off + 0, "__cstring");
    put_name16(buf, off + 16, "__TEXT");
    put_u64(buf, off + 32, vmaddr + const_offset);
    put_u64(buf, off + 40, path_size);
    put_u32(buf, off + 48, (uint32_t)const_offset);
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
    memcpy(buf + const_offset, path, path_size);

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
