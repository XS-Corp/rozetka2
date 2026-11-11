#include "internal.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    MH_MAGIC_64 = 0xfeedfacf,
    FAT_MAGIC = 0xcafebabe,
    FAT_CIGAM = 0xbebafeca,
    CPU_TYPE_X86_64 = 0x01000007,
    CPU_TYPE_ARM64 = 0x0100000c,
    LC_SEGMENT_64 = 0x19,
    LC_LOAD_DYLIB = 0x0c,
    LC_LOAD_WEAK_DYLIB = 0x80000018u,
    LC_REEXPORT_DYLIB = 0x8000001fu,
    LC_LOAD_UPWARD_DYLIB = 0x80000023u,
    LC_MAIN = 0x80000028
};

r2_status r2_dyld_run_macho_file(const char *path,
                                 bool host_libraries,
                                 const char *const *argv,
                                 size_t argc,
                                 r2_result *result,
                                 char *error,
                                 size_t error_len);

static uint32_t read_be32(const uint8_t raw[4])
{
    return ((uint32_t)raw[0] << 24) | ((uint32_t)raw[1] << 16) |
           ((uint32_t)raw[2] << 8) | (uint32_t)raw[3];
}

static uint32_t read_le32(const uint8_t raw[4])
{
    return ((uint32_t)raw[3] << 24) | ((uint32_t)raw[2] << 16) |
           ((uint32_t)raw[1] << 8) | (uint32_t)raw[0];
}

static uint64_t read_le64(const uint8_t raw[8])
{
    return ((uint64_t)raw[0]) |
           ((uint64_t)raw[1] << 8) |
           ((uint64_t)raw[2] << 16) |
           ((uint64_t)raw[3] << 24) |
           ((uint64_t)raw[4] << 32) |
           ((uint64_t)raw[5] << 40) |
           ((uint64_t)raw[6] << 48) |
           ((uint64_t)raw[7] << 56);
}

static uint32_t read_fat32(const uint8_t raw[4], bool swapped)
{
    return swapped ? read_le32(raw) : read_be32(raw);
}

static const char *cpu_name(uint32_t cpu)
{
    if (cpu == CPU_TYPE_X86_64) {
        return "x86_64";
    }
    if (cpu == CPU_TYPE_ARM64) {
        return "arm64";
    }
    return "other";
}

static const char *filetype_name(uint32_t filetype)
{
    switch (filetype) {
    case 0x2:
        return "executable";
    case 0x6:
        return "dylib";
    case 0x8:
        return "bundle";
    default:
        return "other";
    }
}

typedef struct {
    char name[17];
    uint64_t vmaddr;
    uint64_t vmsize;
    uint64_t fileoff;
    uint64_t filesize;
    uint32_t initprot;
} macho64_segment;

typedef struct {
    bool has_entry;
    uint64_t entryoff;
    uint64_t stacksize;
    uint64_t entry_vmaddr;
    bool has_text;
    uint64_t text_addr;
    uint64_t text_offset;
    uint64_t text_size;
    uint32_t dylib_count;
    uint32_t filetype;
    macho64_segment segments[32];
    size_t segment_count;
} macho64_info;

static bool fixed_name_eq(const uint8_t raw[16], const char *name)
{
    char buf[17] = {0};
    memcpy(buf, raw, 16);
    return strcmp(buf, name) == 0;
}

static bool read_at(FILE *file,
                    uint64_t offset,
                    void *buf,
                    size_t len,
                    char *error,
                    size_t error_len)
{
    if (offset > LONG_MAX) {
        r2_set_error(error, error_len, "file offset is too large");
        return false;
    }

    if (fseek(file, (long)offset, SEEK_SET) != 0) {
        r2_set_error(error, error_len, "seek failed: %s", strerror(errno));
        return false;
    }

    if (fread(buf, 1, len, file) != len) {
        r2_set_error(error, error_len, "read failed");
        return false;
    }
    return true;
}

static bool file_size(FILE *file, uint64_t *out, char *error, size_t error_len)
{
    long current = ftell(file);
    if (current < 0) {
        r2_set_error(error, error_len, "tell failed: %s", strerror(errno));
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        r2_set_error(error, error_len, "seek failed: %s", strerror(errno));
        return false;
    }
    long end = ftell(file);
    if (end < 0) {
        r2_set_error(error, error_len, "tell failed: %s", strerror(errno));
        return false;
    }
    if (fseek(file, current, SEEK_SET) != 0) {
        r2_set_error(error, error_len, "seek failed: %s", strerror(errno));
        return false;
    }
    *out = (uint64_t)end;
    return true;
}

static r2_status print_dylib_name(FILE *file,
                                  uint64_t cmd_offset,
                                  uint32_t cmdsize,
                                  uint32_t name_offset,
                                  char *error,
                                  size_t error_len)
{
    if (name_offset >= cmdsize) {
        printf("    dylib: <bad name offset>\n");
        return R2_OK;
    }

    size_t max_len = cmdsize - name_offset;
    if (max_len > 512) {
        max_len = 512;
    }

    char name[513] = {0};
    if (!read_at(file, cmd_offset + name_offset, name, max_len, error, error_len)) {
        return R2_ERR_IO;
    }
    name[512] = '\0';

    printf("    dylib: %s\n", name);
    return R2_OK;
}

static r2_status parse_macho64_slice(FILE *file,
                                     uint64_t slice_offset,
                                     uint64_t slice_size,
                                     bool verbose,
                                     macho64_info *info,
                                     char *error,
                                     size_t error_len)
{
    if (info != NULL) {
        memset(info, 0, sizeof(*info));
    }

    uint8_t mh[32] = {0};
    if (!read_at(file, slice_offset, mh, sizeof(mh), error, error_len)) {
        return R2_ERR_IO;
    }

    uint32_t magic = read_le32(&mh[0]);
    if (magic != MH_MAGIC_64) {
        r2_set_error(error, error_len, "x86_64 slice is not a 64-bit Mach-O");
        return R2_ERR_DECODE;
    }

    uint32_t cpu = read_le32(&mh[4]);
    uint32_t filetype = read_le32(&mh[12]);
    uint32_t ncmds = read_le32(&mh[16]);
    uint32_t sizeofcmds = read_le32(&mh[20]);

    if (info != NULL) {
        info->filetype = filetype;
    }

    if (verbose) {
        printf("  Mach-O 64-bit details: cpu=%s, filetype=%s, ncmds=%u, sizeofcmds=%u\n",
               cpu_name(cpu),
               filetype_name(filetype),
               ncmds,
               sizeofcmds);
    }

    if (ncmds > 4096) {
        r2_set_error(error, error_len, "unreasonable number of load commands");
        return R2_ERR_DECODE;
    }
    if (slice_size != UINT64_MAX && 32ull + sizeofcmds > slice_size) {
        r2_set_error(error, error_len, "load commands exceed slice size");
        return R2_ERR_DECODE;
    }

    uint64_t cmd_offset = slice_offset + 32;
    for (uint32_t i = 0; i < ncmds; i += 1) {
        uint8_t lc[72] = {0};
        if (!read_at(file, cmd_offset, lc, 8, error, error_len)) {
            return R2_ERR_IO;
        }

        uint32_t cmd = read_le32(&lc[0]);
        uint32_t cmdsize = read_le32(&lc[4]);
        if (cmdsize < 8) {
            r2_set_error(error, error_len, "invalid Mach-O load command size");
            return R2_ERR_DECODE;
        }

        if (cmd == LC_SEGMENT_64 && cmdsize >= 72) {
            if (!read_at(file, cmd_offset, lc, sizeof(lc), error, error_len)) {
                return R2_ERR_IO;
            }
            char segname[17] = {0};
            memcpy(segname, &lc[8], 16);
            uint64_t vmaddr = read_le64(&lc[24]);
            uint64_t vmsize = read_le64(&lc[32]);
            uint64_t fileoff = read_le64(&lc[40]);
            uint64_t filesize = read_le64(&lc[48]);
            uint32_t initprot = read_le32(&lc[60]);
            uint32_t nsects = read_le32(&lc[64]);
            if (verbose) {
                printf("    segment %-16s vm=0x%016llx..0x%016llx fileoff=%llu filesize=%llu\n",
                       segname,
                       (unsigned long long)vmaddr,
                       (unsigned long long)(vmaddr + vmsize),
                       (unsigned long long)fileoff,
                       (unsigned long long)filesize);
            }

            if (info != NULL && filesize > 0 && strcmp(segname, "__PAGEZERO") != 0) {
                if (info->segment_count >= sizeof(info->segments) / sizeof(info->segments[0])) {
                    r2_set_error(error, error_len, "too many Mach-O segments");
                    return R2_ERR_UNSUPPORTED;
                }
                macho64_segment *segment = &info->segments[info->segment_count];
                memset(segment, 0, sizeof(*segment));
                memcpy(segment->name, segname, sizeof(segment->name));
                segment->vmaddr = vmaddr;
                segment->vmsize = vmsize;
                segment->fileoff = fileoff;
                segment->filesize = filesize;
                segment->initprot = initprot;
                info->segment_count += 1;
            }

            uint64_t section_offset = cmd_offset + 72;
            for (uint32_t sect = 0; sect < nsects; sect += 1) {
                uint8_t section[80] = {0};
                if (72ull + ((uint64_t)sect + 1ull) * sizeof(section) > cmdsize) {
                    r2_set_error(error, error_len, "section table exceeds segment command");
                    return R2_ERR_DECODE;
                }
                if (!read_at(file,
                             section_offset + (uint64_t)sect * sizeof(section),
                             section,
                             sizeof(section),
                             error,
                             error_len)) {
                    return R2_ERR_IO;
                }
                if (fixed_name_eq(&section[0], "__text") &&
                    fixed_name_eq(&section[16], "__TEXT") && info != NULL) {
                    info->has_text = true;
                    info->text_addr = read_le64(&section[32]);
                    info->text_size = read_le64(&section[40]);
                    info->text_offset = read_le32(&section[48]);
                }
            }
        } else if (cmd == LC_MAIN && cmdsize >= 24) {
            uint8_t main_cmd[24] = {0};
            if (!read_at(file, cmd_offset, main_cmd, sizeof(main_cmd), error, error_len)) {
                return R2_ERR_IO;
            }
            uint64_t entryoff = read_le64(&main_cmd[8]);
            uint64_t stacksize = read_le64(&main_cmd[16]);
            if (info != NULL) {
                info->has_entry = true;
                info->entryoff = entryoff;
                info->stacksize = stacksize;
            }
            if (verbose) {
                printf("    LC_MAIN entryoff=%llu stacksize=%llu\n",
                       (unsigned long long)entryoff,
                       (unsigned long long)stacksize);
            }
        } else if ((cmd == LC_LOAD_DYLIB || cmd == LC_LOAD_WEAK_DYLIB ||
                    cmd == LC_REEXPORT_DYLIB || cmd == LC_LOAD_UPWARD_DYLIB) &&
                   cmdsize >= 24) {
            uint8_t dylib_cmd[24] = {0};
            if (!read_at(file, cmd_offset, dylib_cmd, sizeof(dylib_cmd), error, error_len)) {
                return R2_ERR_IO;
            }
            if (info != NULL) {
                info->dylib_count += 1;
            }
            if (verbose) {
                r2_status status = print_dylib_name(file,
                                                    cmd_offset,
                                                    cmdsize,
                                                    read_le32(&dylib_cmd[8]),
                                                    error,
                                                    error_len);
                if (status != R2_OK) {
                    return status;
                }
            }
        }

        cmd_offset += cmdsize;
    }

    if (info != NULL && info->has_entry) {
        for (size_t i = 0; i < info->segment_count; i += 1) {
            const macho64_segment *segment = &info->segments[i];
            uint64_t file_end = segment->fileoff + segment->filesize;
            if (file_end >= segment->fileoff && info->entryoff >= segment->fileoff &&
                info->entryoff < file_end) {
                info->entry_vmaddr = segment->vmaddr + (info->entryoff - segment->fileoff);
                break;
            }
        }
    }

    return R2_OK;
}

static r2_status find_x86_64_slice(FILE *file,
                                   uint64_t *slice_offset,
                                   uint64_t *slice_size,
                                   char *error,
                                   size_t error_len)
{
    uint8_t header[32] = {0};
    if (!read_at(file, 0, header, sizeof(header), error, error_len)) {
        return R2_ERR_IO;
    }

    uint32_t magic_be = read_be32(header);
    uint32_t magic_le = read_le32(header);

    if (magic_le == MH_MAGIC_64) {
        uint32_t cpu = read_le32(&header[4]);
        if (cpu != CPU_TYPE_X86_64) {
            r2_set_error(error, error_len, "thin Mach-O is %s, not x86_64", cpu_name(cpu));
            return R2_ERR_UNSUPPORTED;
        }
        *slice_offset = 0;
        if (!file_size(file, slice_size, error, error_len)) {
            return R2_ERR_IO;
        }
        return R2_OK;
    }

    if (magic_be == FAT_MAGIC || magic_be == FAT_CIGAM) {
        bool swapped = magic_be == FAT_CIGAM;
        uint32_t nfat = read_fat32(&header[4], swapped);
        if (nfat > 1024) {
            r2_set_error(error, error_len, "unreasonable number of Mach-O slices");
            return R2_ERR_DECODE;
        }

        for (uint32_t i = 0; i < nfat; i += 1) {
            uint8_t arch[20] = {0};
            if (!read_at(file, 8ull + (uint64_t)i * sizeof(arch), arch, sizeof(arch), error, error_len)) {
                return R2_ERR_IO;
            }
            uint32_t cpu = read_fat32(&arch[0], swapped);
            if (cpu == CPU_TYPE_X86_64) {
                *slice_offset = read_fat32(&arch[8], swapped);
                *slice_size = read_fat32(&arch[12], swapped);
                return R2_OK;
            }
        }

        r2_set_error(error, error_len, "universal Mach-O does not contain an x86_64 slice");
        return R2_ERR_UNSUPPORTED;
    }

    r2_set_error(error, error_len, "not a supported Mach-O header");
    return R2_ERR_DECODE;
}

r2_status r2_inspect_macho_file(const char *path, char *error, size_t error_len)
{
    if (path == NULL) {
        r2_set_error(error, error_len, "null path");
        return R2_ERR_BAD_ARGUMENT;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        r2_set_error(error, error_len, "open failed: %s", strerror(errno));
        return R2_ERR_IO;
    }

    uint8_t header[32] = {0};
    size_t got = fread(header, 1, sizeof(header), file);
    if (got < 8) {
        fclose(file);
        r2_set_error(error, error_len, "file is too small to be Mach-O");
        return R2_ERR_DECODE;
    }

    uint32_t magic_be = read_be32(header);
    uint32_t magic_le = read_le32(header);

    if (magic_le == MH_MAGIC_64 && got >= 8) {
        uint32_t cpu = read_le32(&header[4]);
        printf("Mach-O 64-bit: cpu=%s (0x%08x)\n", cpu_name(cpu), cpu);
        r2_status status = parse_macho64_slice(file, 0, UINT64_MAX, true, NULL, error, error_len);
        if (status != R2_OK) {
            fclose(file);
            return status;
        }
        if (cpu == CPU_TYPE_X86_64) {
            printf("x86_64 Mach-O detected.\n");
        }
        fclose(file);
        return R2_OK;
    }

    if ((magic_be == FAT_MAGIC || magic_be == FAT_CIGAM) && got >= 8) {
        bool swapped = magic_be == FAT_CIGAM;
        uint32_t nfat = read_fat32(&header[4], swapped);
        printf("Universal Mach-O: %u slice(s)\n", nfat);

        if (nfat > 1024) {
            fclose(file);
            r2_set_error(error, error_len, "unreasonable number of Mach-O slices");
            return R2_ERR_DECODE;
        }

        if (fseek(file, 8, SEEK_SET) != 0) {
            fclose(file);
            r2_set_error(error, error_len, "seek failed: %s", strerror(errno));
            return R2_ERR_IO;
        }

        bool has_x86_64 = false;
        uint32_t x86_offset = 0;
        uint32_t x86_size = 0;
        for (uint32_t i = 0; i < nfat; i += 1) {
            uint8_t arch[20] = {0};
            if (fread(arch, 1, sizeof(arch), file) != sizeof(arch)) {
                fclose(file);
                r2_set_error(error, error_len, "truncated fat_arch table");
                return R2_ERR_DECODE;
            }

            uint32_t cpu = read_fat32(&arch[0], swapped);
            uint32_t offset = read_fat32(&arch[8], swapped);
            uint32_t size = read_fat32(&arch[12], swapped);
            printf("  slice %u: cpu=%s (0x%08x), offset=%u, size=%u\n",
                   i,
                   cpu_name(cpu),
                   cpu,
                   offset,
                   size);
            if (cpu == CPU_TYPE_X86_64) {
                has_x86_64 = true;
                x86_offset = offset;
                x86_size = size;
            }
        }

        if (has_x86_64) {
            r2_status status =
                parse_macho64_slice(file, x86_offset, x86_size, true, NULL, error, error_len);
            if (status != R2_OK) {
                fclose(file);
                return status;
            }
            printf("x86_64 slice detected.\n");
        }

        fclose(file);
        return R2_OK;
    }

    fclose(file);
    r2_set_error(error, error_len, "not a supported Mach-O header");
    return R2_ERR_DECODE;
}

r2_status r2_run_macho_file(const char *path,
                            bool host_libraries,
                            const char *const *argv,
                            size_t argc,
                            r2_result *result,
                            char *error,
                            size_t error_len)
{
    if (path == NULL || result == NULL) {
        r2_set_error(error, error_len, "null argument");
        return R2_ERR_BAD_ARGUMENT;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        r2_set_error(error, error_len, "open failed: %s", strerror(errno));
        return R2_ERR_IO;
    }

    uint64_t slice_offset = 0;
    uint64_t slice_size = 0;
    r2_status status = find_x86_64_slice(file, &slice_offset, &slice_size, error, error_len);
    if (status != R2_OK) {
        fclose(file);
        return status;
    }

    macho64_info info;
    status = parse_macho64_slice(file, slice_offset, slice_size, false, &info, error, error_len);
    if (status != R2_OK) {
        fclose(file);
        return status;
    }

    if (info.filetype != 0x2) {
        fclose(file);
        r2_set_error(error, error_len, "only executable Mach-O files can be launched");
        return R2_ERR_UNSUPPORTED;
    }
    fclose(file);
    return r2_dyld_run_macho_file(path, host_libraries, argv, argc, result, error, error_len);
}
