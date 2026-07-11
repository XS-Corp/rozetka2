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
    CODE_OFFSET = HEADER_SIZE + SEGMENT_COMMAND_64_SIZE + SECTION_64_SIZE +
                  ENTRY_POINT_COMMAND_SIZE,
    ARG_MESSAGE_SIZE = 18
};

static void put_u32(uint8_t *buf, size_t off, uint32_t value)
{
    buf[off + 0] = (uint8_t)(value & 0xffu);
    buf[off + 1] = (uint8_t)((value >> 8) & 0xffu);
    buf[off + 2] = (uint8_t)((value >> 16) & 0xffu);
    buf[off + 3] = (uint8_t)((value >> 24) & 0xffu);
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

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: make_argv_macho <output>\n");
        return 2;
    }

    const uint8_t code[] = {
        0x48, 0x8b, 0x76, 0x08,
        0xb8, 0x04, 0x00, 0x00, 0x02,
        0xbf, 0x01, 0x00, 0x00, 0x00,
        0xba, ARG_MESSAGE_SIZE, 0x00, 0x00, 0x00,
        0x0f, 0x05,
        0xb8, 0x2a, 0x00, 0x00, 0x00,
        0xc3,
    };
    const size_t code_size = sizeof(code);
    const size_t file_size = CODE_OFFSET + code_size;
    const uint64_t vmaddr = 0x100000000ull;

    uint8_t *buf = calloc(1, file_size);
    if (buf == NULL) {
        return 1;
    }

    put_u32(buf, 0, MH_MAGIC_64);
    put_u32(buf, 4, CPU_TYPE_X86_64);
    put_u32(buf, 8, CPU_SUBTYPE_X86_64_ALL);
    put_u32(buf, 12, MH_EXECUTE);
    put_u32(buf, 16, 2);
    put_u32(buf, 20, SEGMENT_COMMAND_64_SIZE + SECTION_64_SIZE + ENTRY_POINT_COMMAND_SIZE);
    put_u32(buf, 24, 0);
    put_u32(buf, 28, 0);

    size_t off = HEADER_SIZE;
    put_u32(buf, off + 0, LC_SEGMENT_64);
    put_u32(buf, off + 4, SEGMENT_COMMAND_64_SIZE + SECTION_64_SIZE);
    put_name16(buf, off + 8, "__TEXT");
    put_u64(buf, off + 24, vmaddr);
    put_u64(buf, off + 32, file_size);
    put_u64(buf, off + 40, 0);
    put_u64(buf, off + 48, file_size);
    put_u32(buf, off + 56, VM_PROT_READ_EXEC);
    put_u32(buf, off + 60, VM_PROT_READ_EXEC);
    put_u32(buf, off + 64, 1);
    put_u32(buf, off + 68, 0);

    off += SEGMENT_COMMAND_64_SIZE;
    put_name16(buf, off + 0, "__text");
    put_name16(buf, off + 16, "__TEXT");
    put_u64(buf, off + 32, vmaddr + CODE_OFFSET);
    put_u64(buf, off + 40, code_size);
    put_u32(buf, off + 48, CODE_OFFSET);
    put_u32(buf, off + 52, 0);
    put_u32(buf, off + 56, 0);
    put_u32(buf, off + 60, 0);
    put_u32(buf, off + 64, S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS);

    off += SECTION_64_SIZE;
    put_u32(buf, off + 0, LC_MAIN);
    put_u32(buf, off + 4, ENTRY_POINT_COMMAND_SIZE);
    put_u64(buf, off + 8, CODE_OFFSET);
    put_u64(buf, off + 16, 0);

    memcpy(buf + CODE_OFFSET, code, code_size);

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
