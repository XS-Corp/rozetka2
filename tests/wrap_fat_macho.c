#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    MH_MAGIC_64 = 0xfeedfacf,
    FAT_MAGIC = 0xcafebabe,
    FAT_CIGAM = 0xbebafeca,
    CPU_TYPE_X86_64 = 0x01000007,
    CPU_SUBTYPE_X86_64_ALL = 3,
    FAT_ARCH_OFFSET = 8,
    FAT_ARCH_SIZE = 20,
    SLICE_OFFSET = 0x1000,
    SLICE_ALIGN = 12,
    COPY_BUFFER_SIZE = 64 * 1024
};

typedef void (*put_u32_fn)(uint8_t *buf, size_t offset, uint32_t value);

static uint32_t read_le32(const uint8_t *buf)
{
    return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

static void put_be32(uint8_t *buf, size_t offset, uint32_t value)
{
    buf[offset + 0] = (uint8_t)((value >> 24) & 0xffu);
    buf[offset + 1] = (uint8_t)((value >> 16) & 0xffu);
    buf[offset + 2] = (uint8_t)((value >> 8) & 0xffu);
    buf[offset + 3] = (uint8_t)(value & 0xffu);
}

static void put_le32(uint8_t *buf, size_t offset, uint32_t value)
{
    buf[offset + 0] = (uint8_t)(value & 0xffu);
    buf[offset + 1] = (uint8_t)((value >> 8) & 0xffu);
    buf[offset + 2] = (uint8_t)((value >> 16) & 0xffu);
    buf[offset + 3] = (uint8_t)((value >> 24) & 0xffu);
}

static bool get_thin_size(FILE *input, const char *path, uint32_t *size_out)
{
    if (fseek(input, 0, SEEK_END) != 0) {
        fprintf(stderr, "seek %s failed: %s\n", path, strerror(errno));
        return false;
    }

    long size = ftell(input);
    if (size < 0) {
        fprintf(stderr, "tell %s failed: %s\n", path, strerror(errno));
        return false;
    }
    if ((uint64_t)size > UINT32_MAX) {
        fprintf(stderr, "%s is too large for a 32-bit fat_arch\n", path);
        return false;
    }
    if (fseek(input, 0, SEEK_SET) != 0) {
        fprintf(stderr, "seek %s failed: %s\n", path, strerror(errno));
        return false;
    }

    *size_out = (uint32_t)size;
    return true;
}

static bool validate_x86_64_thin(FILE *input, const char *path)
{
    uint8_t header[8] = {0};
    if (fread(header, 1, sizeof(header), input) != sizeof(header)) {
        if (ferror(input)) {
            fprintf(stderr, "read %s failed: %s\n", path, strerror(errno));
        } else {
            fprintf(stderr, "%s is too small to be a thin Mach-O\n", path);
        }
        return false;
    }
    if (read_le32(header) != MH_MAGIC_64) {
        fprintf(stderr, "%s is not a little-endian 64-bit thin Mach-O\n", path);
        return false;
    }
    if (read_le32(header + 4) != CPU_TYPE_X86_64) {
        fprintf(stderr, "%s is not an x86_64 thin Mach-O\n", path);
        return false;
    }
    if (fseek(input, 0, SEEK_SET) != 0) {
        fprintf(stderr, "seek %s failed: %s\n", path, strerror(errno));
        return false;
    }
    return true;
}

static bool copy_file(FILE *input,
                      const char *input_path,
                      FILE *output,
                      const char *output_path)
{
    uint8_t buffer[COPY_BUFFER_SIZE];
    for (;;) {
        size_t count = fread(buffer, 1, sizeof(buffer), input);
        if (count != 0 && fwrite(buffer, 1, count, output) != count) {
            fprintf(stderr, "write %s failed: %s\n", output_path, strerror(errno));
            return false;
        }
        if (count < sizeof(buffer)) {
            if (ferror(input)) {
                fprintf(stderr, "read %s failed: %s\n", input_path, strerror(errno));
                return false;
            }
            return true;
        }
    }
}

static void usage(const char *program)
{
    fprintf(stderr, "usage: %s <be|le> <thin-input> <fat-output>\n", program);
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        usage(argv[0]);
        return 2;
    }

    bool little_endian = false;
    if (strcmp(argv[1], "be") == 0) {
        little_endian = false;
    } else if (strcmp(argv[1], "le") == 0) {
        little_endian = true;
    } else {
        usage(argv[0]);
        return 2;
    }
    if (strcmp(argv[2], argv[3]) == 0) {
        fprintf(stderr, "thin input and fat output must be different files\n");
        return 2;
    }

    FILE *input = fopen(argv[2], "rb");
    if (input == NULL) {
        fprintf(stderr, "open %s failed: %s\n", argv[2], strerror(errno));
        return 1;
    }

    uint32_t thin_size = 0;
    bool ok = get_thin_size(input, argv[2], &thin_size) &&
              validate_x86_64_thin(input, argv[2]);
    if (!ok) {
        fclose(input);
        return 1;
    }

    uint8_t prefix[SLICE_OFFSET] = {0};
    put_be32(prefix, 0, little_endian ? FAT_CIGAM : FAT_MAGIC);
    put_u32_fn put_field = little_endian ? put_le32 : put_be32;
    put_field(prefix, 4, 1);
    put_field(prefix, FAT_ARCH_OFFSET + 0, CPU_TYPE_X86_64);
    put_field(prefix, FAT_ARCH_OFFSET + 4, CPU_SUBTYPE_X86_64_ALL);
    put_field(prefix, FAT_ARCH_OFFSET + 8, SLICE_OFFSET);
    put_field(prefix, FAT_ARCH_OFFSET + 12, thin_size);
    put_field(prefix, FAT_ARCH_OFFSET + 16, SLICE_ALIGN);

    FILE *output = fopen(argv[3], "wb");
    if (output == NULL) {
        fprintf(stderr, "open %s failed: %s\n", argv[3], strerror(errno));
        fclose(input);
        return 1;
    }

    if (fwrite(prefix, 1, sizeof(prefix), output) != sizeof(prefix)) {
        fprintf(stderr, "write %s failed: %s\n", argv[3], strerror(errno));
        ok = false;
    } else {
        ok = copy_file(input, argv[2], output, argv[3]);
    }

    if (fclose(input) != 0) {
        fprintf(stderr, "close %s failed: %s\n", argv[2], strerror(errno));
        ok = false;
    }
    if (fclose(output) != 0) {
        fprintf(stderr, "close %s failed: %s\n", argv[3], strerror(errno));
        ok = false;
    }

    return ok ? 0 : 1;
}
