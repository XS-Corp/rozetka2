#include "internal.h"

#include <errno.h>
#include <dlfcn.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
    MH_MAGIC_64 = 0xfeedfacf,
    FAT_MAGIC = 0xcafebabe,
    FAT_CIGAM = 0xbebafeca,
    CPU_TYPE_X86_64 = 0x01000007,
    MH_EXECUTE = 0x2,
    MH_DYLIB = 0x6,
    LC_SEGMENT_64 = 0x19,
    LC_SYMTAB = 0x2,
    LC_LOAD_DYLIB = 0x0c,
    LC_LOAD_WEAK_DYLIB = 0x80000018u,
    LC_REEXPORT_DYLIB = 0x8000001fu,
    LC_LOAD_UPWARD_DYLIB = 0x80000023u,
    LC_RPATH = 0x8000001cu,
    LC_DYLD_INFO = 0x22,
    LC_DYLD_INFO_ONLY = 0x80000022u,
    LC_DYLD_EXPORTS_TRIE = 0x80000033u,
    LC_DYLD_CHAINED_FIXUPS = 0x80000034u,
    LC_MAIN = 0x80000028u,
    SECTION_TYPE = 0x000000ffu,
    S_SYMBOL_STUBS = 0x8,
    S_MOD_INIT_FUNC_POINTERS = 0x9,
    S_THREAD_LOCAL_REGULAR = 0x11,
    S_THREAD_LOCAL_ZEROFILL = 0x12,
    S_THREAD_LOCAL_VARIABLES = 0x13,
    S_THREAD_LOCAL_INIT_FUNCTION_POINTERS = 0x15,
    VM_PROT_WRITE = 0x02,
    N_EXT = 0x01,
    N_TYPE = 0x0e,
    N_STAB = 0xe0,
    N_ABS = 0x02,
    N_SECT = 0x0e,
    REBASE_TYPE_POINTER = 1,
    BIND_TYPE_POINTER = 1,
    BIND_SPECIAL_DYLIB_SELF = 0,
    BIND_SPECIAL_DYLIB_MAIN_EXECUTABLE = -1,
    BIND_SPECIAL_DYLIB_FLAT_LOOKUP = -2,
    BIND_SPECIAL_DYLIB_WEAK_LOOKUP = -3,
    BIND_SYMBOL_FLAGS_WEAK_IMPORT = 0x1,
    EXPORT_SYMBOL_FLAGS_KIND_MASK = 0x03,
    EXPORT_SYMBOL_FLAGS_KIND_REGULAR = 0x00,
    EXPORT_SYMBOL_FLAGS_KIND_THREAD_LOCAL = 0x01,
    EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE = 0x02,
    EXPORT_SYMBOL_FLAGS_WEAK_DEFINITION = 0x04,
    EXPORT_SYMBOL_FLAGS_REEXPORT = 0x08,
    EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER = 0x10,
    DYLD_CHAINED_PTR_START_NONE = 0xffff,
    DYLD_CHAINED_PTR_START_MULTI = 0x8000,
    DYLD_CHAINED_PTR_START_LAST = 0x8000,
    DYLD_CHAINED_PTR_64 = 2,
    DYLD_CHAINED_PTR_64_OFFSET = 6,
    DYLD_CHAINED_IMPORT = 1,
    DYLD_CHAINED_IMPORT_ADDEND = 2,
    DYLD_CHAINED_IMPORT_ADDEND64 = 3
};

#define R2_DYLD_MAX_IMAGES 32u
#define R2_DYLD_MAX_SEGMENTS 32u
#define R2_DYLD_MAX_CODE_RANGES 32u
#define R2_DYLD_MAX_DEPENDENCIES 32u
#define R2_DYLD_MAX_RPATHS 32u
#define R2_DYLD_MAX_REGIONS 128u
#define R2_DYLD_MAX_FILE_SIZE (512u * 1024u * 1024u)
#define R2_DYLD_MAX_SEGMENT_SIZE (256u * 1024u * 1024u)
#define R2_DYLD_BASE 0x200000000ull
#define R2_DYLD_IMAGE_GAP 0x10000ull
#define R2_DYLD_PAGE_SIZE 0x1000ull
#define R2_ARGV_BASE 0x600000000000ull
#define R2_MAX_GUEST_ARGC 256u
#define R2_MAX_GUEST_ARGV_BYTES (1024u * 1024u)

typedef struct {
    char name[17];
    uint64_t vmaddr;
    uint64_t vmsize;
    uint64_t fileoff;
    uint64_t filesize;
    uint32_t initprot;
    bool mapped;
    size_t region_index;
} r2_dyld_segment;

typedef struct {
    uint64_t vmaddr;
    uint64_t size;
    uint64_t fileoff;
} r2_dyld_code_range;

typedef struct {
    char install_name[PATH_MAX];
    bool weak;
    bool reexport;
    bool missing;
    size_t image_index;
} r2_dyld_dependency;

typedef struct {
    char path[PATH_MAX];
    uint8_t *file_data;
    size_t file_size;
    uint64_t slice_offset;
    uint64_t slice_size;
    uint32_t filetype;
    r2_dyld_segment segments[R2_DYLD_MAX_SEGMENTS];
    size_t segment_count;
    r2_dyld_code_range code_ranges[R2_DYLD_MAX_CODE_RANGES];
    size_t code_range_count;
    struct { size_t segment; uint64_t offset, count; } initializers[32];
    size_t initializer_count;
    struct { size_t segment; uint64_t address, size; bool zero; } tls_parts[32];
    size_t tls_part_count, tls_range_count, tls_alignment;
    struct { size_t segment; uint64_t offset, count; } tls_ranges[32];
    bool tls_initializers;
    r2_dyld_dependency dependencies[R2_DYLD_MAX_DEPENDENCIES];
    size_t dependency_count;
    char rpaths[R2_DYLD_MAX_RPATHS][PATH_MAX];
    size_t rpath_count;
    size_t loader_image_index;
    bool dependencies_loaded;
    bool has_entry;
    uint64_t entryoff;
    uint64_t entry_vmaddr;
    uint64_t rebase_off;
    uint64_t rebase_size;
    uint64_t bind_off;
    uint64_t bind_size;
    uint64_t weak_bind_off;
    uint64_t weak_bind_size;
    uint64_t lazy_bind_off;
    uint64_t lazy_bind_size;
    uint64_t export_off;
    uint64_t export_size;
    uint64_t chained_fixups_off;
    uint64_t chained_fixups_size;
    bool has_symtab;
    uint64_t symoff;
    uint32_t nsyms;
    uint64_t stroff;
    uint64_t strsize;
    uint64_t slide;
    uint8_t *native_data;
    bool fixups_applied, globally_visible;
    size_t open_count;
} r2_dyld_image;

typedef struct {
    r2_dyld_image images[R2_DYLD_MAX_IMAGES];
    size_t image_count;
    r2_memory_region regions[R2_DYLD_MAX_REGIONS];
    size_t region_count;
    char executable_dir[PATH_MAX];
    r2_native_runtime *native;
    uint8_t *argv_data;
    uint8_t initialization_states[R2_DYLD_MAX_IMAGES];
    r2_block *active_block;
    const r2_interp_env *active_environment;
    size_t cpp_image_index;
} r2_dyld_context;

r2_status r2_decode_block_append_at(const uint8_t *code,
                                    size_t code_len,
                                    uint64_t guest_base,
                                    r2_block *block,
                                    char *error,
                                    size_t error_len);

static uint32_t read_le32(const uint8_t *raw)
{
    return ((uint32_t)raw[0]) | ((uint32_t)raw[1] << 8) | ((uint32_t)raw[2] << 16) |
           ((uint32_t)raw[3] << 24);
}

static uint16_t read_le16(const uint8_t *raw)
{
    return (uint16_t)(((uint16_t)raw[0]) | ((uint16_t)raw[1] << 8));
}

static uint32_t read_be32(const uint8_t *raw)
{
    return ((uint32_t)raw[0] << 24) | ((uint32_t)raw[1] << 16) |
           ((uint32_t)raw[2] << 8) | (uint32_t)raw[3];
}

static uint32_t read_fat32(const uint8_t *raw, bool swapped)
{
    return swapped ? read_le32(raw) : read_be32(raw);
}

static uint64_t read_le64(const uint8_t *raw)
{
    uint64_t value = 0;
    for (size_t i = 0; i < 8; i += 1) {
        value |= (uint64_t)raw[i] << (i * 8);
    }
    return value;
}

static void write_le64(uint8_t *raw, uint64_t value)
{
    for (size_t i = 0; i < 8; i += 1) {
        raw[i] = (uint8_t)((value >> (i * 8)) & 0xffu);
    }
}

static bool add_u64(uint64_t lhs, uint64_t rhs, uint64_t *out)
{
    if (lhs > UINT64_MAX - rhs) {
        return false;
    }
    *out = lhs + rhs;
    return true;
}

static bool span_in_size(uint64_t offset, uint64_t length, uint64_t size)
{
    return offset <= size && length <= size - offset;
}

static bool fixed_name_eq(const uint8_t raw[16], const char *name)
{
    char value[17] = {0};
    memcpy(value, raw, 16);
    return strcmp(value, name) == 0;
}

static bool copy_path(char *out,
                      size_t out_len,
                      const char *value,
                      char *error,
                      size_t error_len)
{
    int wrote = snprintf(out, out_len, "%s", value);
    if (wrote < 0 || (size_t)wrote >= out_len) {
        r2_set_error(error, error_len, "Mach-O path is too long");
        return false;
    }
    return true;
}

static bool path_dirname(const char *path,
                         char *out,
                         size_t out_len,
                         char *error,
                         size_t error_len)
{
    const char *slash = strrchr(path, '/');
    if (slash == NULL) {
        return copy_path(out, out_len, ".", error, error_len);
    }
    size_t len = slash == path ? 1u : (size_t)(slash - path);
    if (len >= out_len) {
        r2_set_error(error, error_len, "Mach-O directory path is too long");
        return false;
    }
    memcpy(out, path, len);
    out[len] = '\0';
    return true;
}

static bool absolute_path(const char *path,
                          char *out,
                          size_t out_len,
                          char *error,
                          size_t error_len)
{
    char resolved[PATH_MAX];
    if (realpath(path, resolved) != NULL) {
        return copy_path(out, out_len, resolved, error, error_len);
    }
    if (path[0] == '/') {
        return copy_path(out, out_len, path, error, error_len);
    }

    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        r2_set_error(error, error_len, "getcwd failed: %s", strerror(errno));
        return false;
    }
    int wrote = snprintf(out, out_len, "%s/%s", cwd, path);
    if (wrote < 0 || (size_t)wrote >= out_len) {
        r2_set_error(error, error_len, "Mach-O path is too long");
        return false;
    }
    return true;
}

static r2_status read_file(const char *path,
                           uint8_t **data_out,
                           size_t *size_out,
                           char *error,
                           size_t error_len)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        r2_set_error(error, error_len, "open %s failed: %s", path, strerror(errno));
        return R2_ERR_IO;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        r2_set_error(error, error_len, "seek %s failed: %s", path, strerror(errno));
        fclose(file);
        return R2_ERR_IO;
    }
    long end = ftell(file);
    if (end < 0 || (uint64_t)end > R2_DYLD_MAX_FILE_SIZE) {
        r2_set_error(error, error_len, "Mach-O file %s is too large", path);
        fclose(file);
        return R2_ERR_UNSUPPORTED;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        r2_set_error(error, error_len, "rewind %s failed: %s", path, strerror(errno));
        fclose(file);
        return R2_ERR_IO;
    }

    size_t size = (size_t)end;
    uint8_t *data = malloc(size == 0 ? 1u : size);
    if (data == NULL) {
        r2_set_error(error, error_len, "out of memory reading %s", path);
        fclose(file);
        return R2_ERR_NO_MEMORY;
    }
    if (size != 0 && fread(data, 1, size, file) != size) {
        r2_set_error(error, error_len, "read %s failed", path);
        free(data);
        fclose(file);
        return R2_ERR_IO;
    }
    if (fclose(file) != 0) {
        r2_set_error(error, error_len, "close %s failed: %s", path, strerror(errno));
        free(data);
        return R2_ERR_IO;
    }
    *data_out = data;
    *size_out = size;
    return R2_OK;
}

static r2_status find_x86_64_slice(const uint8_t *data,
                                   size_t file_size,
                                   uint64_t *slice_offset,
                                   uint64_t *slice_size,
                                   char *error,
                                   size_t error_len)
{
    if (file_size < 8) {
        r2_set_error(error, error_len, "file is too small to be Mach-O");
        return R2_ERR_DECODE;
    }
    if (read_le32(data) == MH_MAGIC_64) {
        if (file_size < 32) {
            r2_set_error(error, error_len, "truncated Mach-O header");
            return R2_ERR_DECODE;
        }
        if (read_le32(data + 4) != CPU_TYPE_X86_64) {
            r2_set_error(error, error_len, "thin dylib does not contain x86_64 code");
            return R2_ERR_UNSUPPORTED;
        }
        *slice_offset = 0;
        *slice_size = file_size;
        return R2_OK;
    }

    uint32_t magic = read_be32(data);
    if (magic != FAT_MAGIC && magic != FAT_CIGAM) {
        r2_set_error(error, error_len, "dependency is not a supported Mach-O file");
        return R2_ERR_DECODE;
    }
    bool swapped = magic == FAT_CIGAM;
    uint32_t count = read_fat32(data + 4, swapped);
    if (count > 1024 || !span_in_size(8, (uint64_t)count * 20u, file_size)) {
        r2_set_error(error, error_len, "invalid universal Mach-O architecture table");
        return R2_ERR_DECODE;
    }
    for (uint32_t i = 0; i < count; i += 1) {
        const uint8_t *arch = data + 8u + (size_t)i * 20u;
        if (read_fat32(arch, swapped) != CPU_TYPE_X86_64) {
            continue;
        }
        uint64_t offset = read_fat32(arch + 8, swapped);
        uint64_t size = read_fat32(arch + 12, swapped);
        if (!span_in_size(offset, size, file_size)) {
            r2_set_error(error, error_len, "x86_64 universal Mach-O slice is truncated");
            return R2_ERR_DECODE;
        }
        *slice_offset = offset;
        *slice_size = size;
        return R2_OK;
    }
    r2_set_error(error, error_len, "universal dylib has no x86_64 slice");
    return R2_ERR_UNSUPPORTED;
}

static r2_status parse_load_command_path(const uint8_t *command,
                                         uint32_t cmdsize,
                                         char *out,
                                         size_t out_len,
                                         char *error,
                                         size_t error_len)
{
    if (cmdsize < 12) {
        r2_set_error(error, error_len, "truncated Mach-O path command");
        return R2_ERR_DECODE;
    }
    uint32_t name_offset = read_le32(command + 8);
    if (name_offset >= cmdsize) {
        r2_set_error(error, error_len, "invalid Mach-O path offset");
        return R2_ERR_DECODE;
    }
    const uint8_t *name = command + name_offset;
    size_t capacity = cmdsize - name_offset;
    const uint8_t *end = memchr(name, '\0', capacity);
    if (end == NULL) {
        r2_set_error(error, error_len, "unterminated Mach-O load-command path");
        return R2_ERR_DECODE;
    }
    size_t len = (size_t)(end - name);
    if (len == 0 || len >= out_len) {
        r2_set_error(error, error_len, "Mach-O load-command path is empty or too long");
        return R2_ERR_UNSUPPORTED;
    }
    memcpy(out, name, len);
    out[len] = '\0';
    return R2_OK;
}

static r2_status parse_macho_image(r2_dyld_image *image,
                                   char *error,
                                   size_t error_len)
{
    r2_status status = find_x86_64_slice(image->file_data,
                                         image->file_size,
                                         &image->slice_offset,
                                         &image->slice_size,
                                         error,
                                         error_len);
    if (status != R2_OK) {
        return status;
    }
    if (!span_in_size(image->slice_offset, 32, image->file_size)) {
        r2_set_error(error, error_len, "truncated x86_64 Mach-O header");
        return R2_ERR_DECODE;
    }
    const uint8_t *slice = image->file_data + image->slice_offset;
    image->filetype = read_le32(slice + 12);
    uint32_t ncmds = read_le32(slice + 16);
    uint32_t sizeofcmds = read_le32(slice + 20);
    if (ncmds > 4096 || !span_in_size(32, sizeofcmds, image->slice_size)) {
        r2_set_error(error, error_len, "invalid Mach-O load-command table");
        return R2_ERR_DECODE;
    }

    uint64_t command_offset = 32;
    uint64_t command_limit = 32u + sizeofcmds;
    for (uint32_t i = 0; i < ncmds; i += 1) {
        if (!span_in_size(command_offset, 8, command_limit)) {
            r2_set_error(error, error_len, "truncated Mach-O load command");
            return R2_ERR_DECODE;
        }
        const uint8_t *command = slice + command_offset;
        uint32_t cmd = read_le32(command);
        uint32_t cmdsize = read_le32(command + 4);
        if (cmdsize < 8 || !span_in_size(command_offset, cmdsize, command_limit)) {
            r2_set_error(error, error_len, "invalid Mach-O load command size");
            return R2_ERR_DECODE;
        }

        if (cmd == LC_SEGMENT_64) {
            if (cmdsize < 72 || image->segment_count >= R2_DYLD_MAX_SEGMENTS) {
                r2_set_error(error, error_len, "invalid or excessive Mach-O segment commands");
                return R2_ERR_UNSUPPORTED;
            }
            uint32_t nsects = read_le32(command + 64);
            if ((uint64_t)nsects * 80u > cmdsize - 72u) {
                r2_set_error(error, error_len, "section table exceeds segment command");
                return R2_ERR_DECODE;
            }

            r2_dyld_segment *segment = &image->segments[image->segment_count++];
            memset(segment, 0, sizeof(*segment));
            memcpy(segment->name, command + 8, 16);
            segment->vmaddr = read_le64(command + 24);
            segment->vmsize = read_le64(command + 32);
            segment->fileoff = read_le64(command + 40);
            segment->filesize = read_le64(command + 48);
            segment->initprot = read_le32(command + 60);
            segment->region_index = SIZE_MAX;
            if (segment->filesize > segment->vmsize ||
                !span_in_size(segment->fileoff, segment->filesize, image->slice_size)) {
                r2_set_error(error, error_len, "segment %s exceeds its Mach-O slice", segment->name);
                return R2_ERR_DECODE;
            }

            for (uint32_t section_index = 0; section_index < nsects; section_index += 1) {
                const uint8_t *section = command + 72u + (size_t)section_index * 80u;
                uint32_t section_flags = read_le32(section + 64);
                uint32_t section_type = section_flags & SECTION_TYPE;
                if (section_type == S_THREAD_LOCAL_INIT_FUNCTION_POINTERS && read_le64(section + 40) != 0)
                    image->tls_initializers = true;
                if (section_type == S_THREAD_LOCAL_REGULAR || section_type == S_THREAD_LOCAL_ZEROFILL ||
                    section_type == S_THREAD_LOCAL_VARIABLES) {
                    uint64_t address = read_le64(section + 32), size = read_le64(section + 40);
                    uint32_t align = read_le32(section + 52);
                    uint64_t limit = section_type == S_THREAD_LOCAL_ZEROFILL ? segment->vmsize : segment->filesize;
                    if (address < segment->vmaddr || !span_in_size(address - segment->vmaddr, size, limit) || align > 24) {
                        r2_set_error(error, error_len, "invalid Mach-O TLS section"); return R2_ERR_DECODE;
                    }
                    if (section_type == S_THREAD_LOCAL_VARIABLES) {
                        if (size % 24 != 0 || image->tls_range_count == 32) {
                            r2_set_error(error, error_len, "invalid Mach-O TLS descriptors"); return R2_ERR_DECODE;
                        }
                        if (size != 0) {
                            size_t n = image->tls_range_count++;
                            image->tls_ranges[n].segment = image->segment_count - 1;
                            image->tls_ranges[n].offset = address - segment->vmaddr;
                            image->tls_ranges[n].count = size / 24;
                        }
                    } else if (size != 0) {
                        if (image->tls_part_count == 32) return R2_ERR_UNSUPPORTED;
                        size_t n = image->tls_part_count++;
                        image->tls_parts[n].segment = image->segment_count - 1;
                        image->tls_parts[n].address = address; image->tls_parts[n].size = size;
                        image->tls_parts[n].zero = section_type == S_THREAD_LOCAL_ZEROFILL;
                        size_t alignment = (size_t)1 << align;
                        if (alignment > image->tls_alignment) image->tls_alignment = alignment;
                    }
                }
                if ((section_flags & SECTION_TYPE) == S_MOD_INIT_FUNC_POINTERS) {
                    uint64_t address = read_le64(section + 32), size = read_le64(section + 40);
                    if (image->initializer_count == 32 || size % 8 != 0 ||
                        address < segment->vmaddr ||
                        !span_in_size(address - segment->vmaddr, size, segment->filesize)) {
                        r2_set_error(error, error_len, "invalid Mach-O initializer section");
                        return R2_ERR_DECODE;
                    }
                    size_t i = image->initializer_count++;
                    image->initializers[i].segment = image->segment_count - 1;
                    image->initializers[i].offset = address - segment->vmaddr;
                    image->initializers[i].count = size / 8;
                }
                bool is_text = fixed_name_eq(section, "__text") &&
                               fixed_name_eq(section + 16, "__TEXT");
                bool is_stubs = (section_flags & SECTION_TYPE) == S_SYMBOL_STUBS;
                if (!is_text && !is_stubs) {
                    continue;
                }
                if (image->code_range_count >= R2_DYLD_MAX_CODE_RANGES) {
                    r2_set_error(error, error_len, "too many executable Mach-O sections");
                    return R2_ERR_UNSUPPORTED;
                }
                r2_dyld_code_range *code = &image->code_ranges[image->code_range_count++];
                code->vmaddr = read_le64(section + 32);
                code->size = read_le64(section + 40);
                code->fileoff = read_le32(section + 48);
                if (code->size == 0 || !span_in_size(code->fileoff, code->size, image->slice_size)) {
                    r2_set_error(error, error_len, "executable section exceeds its Mach-O slice");
                    return R2_ERR_DECODE;
                }
            }
        } else if (cmd == LC_MAIN) {
            if (cmdsize < 24) {
                r2_set_error(error, error_len, "truncated LC_MAIN command");
                return R2_ERR_DECODE;
            }
            image->has_entry = true;
            image->entryoff = read_le64(command + 8);
        } else if (cmd == LC_LOAD_DYLIB || cmd == LC_LOAD_WEAK_DYLIB ||
                   cmd == LC_REEXPORT_DYLIB || cmd == LC_LOAD_UPWARD_DYLIB) {
            if (cmdsize < 24) {
                r2_set_error(error, error_len, "truncated dylib load command");
                return R2_ERR_DECODE;
            }
            if (image->dependency_count >= R2_DYLD_MAX_DEPENDENCIES) {
                r2_set_error(error, error_len, "too many dylib dependencies");
                return R2_ERR_UNSUPPORTED;
            }
            r2_dyld_dependency *dependency =
                &image->dependencies[image->dependency_count++];
            memset(dependency, 0, sizeof(*dependency));
            dependency->weak = cmd == LC_LOAD_WEAK_DYLIB;
            dependency->reexport = cmd == LC_REEXPORT_DYLIB;
            dependency->image_index = SIZE_MAX;
            status = parse_load_command_path(command,
                                             cmdsize,
                                             dependency->install_name,
                                             sizeof(dependency->install_name),
                                             error,
                                             error_len);
            if (status != R2_OK) {
                return status;
            }
        } else if (cmd == LC_RPATH) {
            if (image->rpath_count >= R2_DYLD_MAX_RPATHS) {
                r2_set_error(error, error_len, "too many LC_RPATH commands");
                return R2_ERR_UNSUPPORTED;
            }
            status = parse_load_command_path(command,
                                             cmdsize,
                                             image->rpaths[image->rpath_count],
                                             sizeof(image->rpaths[image->rpath_count]),
                                             error,
                                             error_len);
            if (status != R2_OK) {
                return status;
            }
            image->rpath_count += 1;
        } else if (cmd == LC_DYLD_INFO || cmd == LC_DYLD_INFO_ONLY) {
            if (cmdsize < 48) {
                r2_set_error(error, error_len, "truncated LC_DYLD_INFO command");
                return R2_ERR_DECODE;
            }
            image->rebase_off = read_le32(command + 8);
            image->rebase_size = read_le32(command + 12);
            image->bind_off = read_le32(command + 16);
            image->bind_size = read_le32(command + 20);
            image->weak_bind_off = read_le32(command + 24);
            image->weak_bind_size = read_le32(command + 28);
            image->lazy_bind_off = read_le32(command + 32);
            image->lazy_bind_size = read_le32(command + 36);
            image->export_off = read_le32(command + 40);
            image->export_size = read_le32(command + 44);
        } else if (cmd == LC_DYLD_EXPORTS_TRIE || cmd == LC_DYLD_CHAINED_FIXUPS) {
            if (cmdsize < 16) {
                r2_set_error(error, error_len, "truncated dyld linkedit-data command");
                return R2_ERR_DECODE;
            }
            if (cmd == LC_DYLD_EXPORTS_TRIE) {
                image->export_off = read_le32(command + 8);
                image->export_size = read_le32(command + 12);
            } else {
                image->chained_fixups_off = read_le32(command + 8);
                image->chained_fixups_size = read_le32(command + 12);
            }
        } else if (cmd == LC_SYMTAB) {
            if (cmdsize < 24) {
                r2_set_error(error, error_len, "truncated LC_SYMTAB command");
                return R2_ERR_DECODE;
            }
            image->has_symtab = true;
            image->symoff = read_le32(command + 8);
            image->nsyms = read_le32(command + 12);
            image->stroff = read_le32(command + 16);
            image->strsize = read_le32(command + 20);
        }
        command_offset += cmdsize;
    }

    if (image->has_entry) {
        for (size_t i = 0; i < image->segment_count; i += 1) {
            const r2_dyld_segment *segment = &image->segments[i];
            if (image->entryoff >= segment->fileoff &&
                image->entryoff - segment->fileoff < segment->filesize) {
                image->entry_vmaddr = segment->vmaddr + (image->entryoff - segment->fileoff);
                break;
            }
        }
    }
    return R2_OK;
}

static size_t find_loaded_image(const r2_dyld_context *context, const char *path)
{
    for (size_t i = 0; i < context->image_count; i += 1) {
        if (strcmp(context->images[i].path, path) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

static r2_status join_paths(const char *base,
                            const char *suffix,
                            char *out,
                            size_t out_len,
                            char *error,
                            size_t error_len)
{
    int wrote = suffix[0] == '\0' ? snprintf(out, out_len, "%s", base)
                                  : snprintf(out, out_len, "%s/%s", base, suffix);
    if (wrote < 0 || (size_t)wrote >= out_len) {
        r2_set_error(error, error_len, "resolved Mach-O path is too long");
        return R2_ERR_BAD_ARGUMENT;
    }
    return R2_OK;
}

static r2_status expand_image_path(const r2_dyld_context *context,
                                   const r2_dyld_image *owner,
                                   const char *value,
                                   char *out,
                                   size_t out_len,
                                   char *error,
                                   size_t error_len)
{
    char loader_dir[PATH_MAX];
    if (!path_dirname(owner->path,
                      loader_dir,
                      sizeof(loader_dir),
                      error,
                      error_len)) {
        return R2_ERR_BAD_ARGUMENT;
    }

    const char *suffix = NULL;
    const char *base = NULL;
    if (strcmp(value, "@loader_path") == 0) {
        base = loader_dir;
        suffix = "";
    } else if (strncmp(value, "@loader_path/", 13) == 0) {
        base = loader_dir;
        suffix = value + 13;
    } else if (strcmp(value, "@executable_path") == 0) {
        base = context->executable_dir;
        suffix = "";
    } else if (strncmp(value, "@executable_path/", 17) == 0) {
        base = context->executable_dir;
        suffix = value + 17;
    } else if (value[0] == '@') {
        r2_set_error(error, error_len, "unsupported Mach-O path token in %s", value);
        return R2_ERR_UNSUPPORTED;
    } else if (value[0] == '/') {
        if (!copy_path(out, out_len, value, error, error_len)) {
            return R2_ERR_BAD_ARGUMENT;
        }
    } else {
        base = loader_dir;
        suffix = value;
    }

    if (base != NULL) {
        r2_status status = join_paths(base, suffix, out, out_len, error, error_len);
        if (status != R2_OK) {
            return status;
        }
    }

    char canonical[PATH_MAX];
    if (realpath(out, canonical) != NULL &&
        !copy_path(out, out_len, canonical, error, error_len)) {
        return R2_ERR_BAD_ARGUMENT;
    }
    return R2_OK;
}

static r2_status resolve_dependency_path(const r2_dyld_context *context,
                                         size_t loader_index,
                                         const char *install_name,
                                         char *out,
                                         size_t out_len,
                                         char *error,
                                         size_t error_len)
{
    if (loader_index >= context->image_count) {
        r2_set_error(error, error_len, "invalid loader image index");
        return R2_ERR_DECODE;
    }
    const r2_dyld_image *loader = &context->images[loader_index];
    const char *suffix = NULL;
    if (strcmp(install_name, "@rpath") == 0) {
        suffix = "";
    } else if (strncmp(install_name, "@rpath/", 7) == 0) {
        suffix = install_name + 7;
    }
    if (suffix == NULL) {
        return expand_image_path(context,
                                 loader,
                                 install_name,
                                 out,
                                 out_len,
                                 error,
                                 error_len);
    }

    bool have_candidate = false;
    char first_candidate[PATH_MAX] = {0};
    size_t chain_index = loader_index;
    for (size_t depth = 0; depth < context->image_count; depth += 1) {
        if (chain_index >= context->image_count) {
            break;
        }
        const r2_dyld_image *owner = &context->images[chain_index];
        for (size_t i = 0; i < owner->rpath_count; i += 1) {
            char base[PATH_MAX];
            r2_status status = expand_image_path(context,
                                                 owner,
                                                 owner->rpaths[i],
                                                 base,
                                                 sizeof(base),
                                                 error,
                                                 error_len);
            if (status != R2_OK) {
                return status;
            }
            char candidate[PATH_MAX];
            status = join_paths(base,
                                suffix,
                                candidate,
                                sizeof(candidate),
                                error,
                                error_len);
            if (status != R2_OK) {
                return status;
            }
            if (!have_candidate) {
                if (!copy_path(first_candidate,
                               sizeof(first_candidate),
                               candidate,
                               error,
                               error_len)) {
                    return R2_ERR_BAD_ARGUMENT;
                }
                have_candidate = true;
            }
            struct stat st;
            if (stat(candidate, &st) == 0) {
                return expand_image_path(context,
                                         owner,
                                         candidate,
                                         out,
                                         out_len,
                                         error,
                                         error_len);
            }
        }
        if (owner->loader_image_index == SIZE_MAX ||
            owner->loader_image_index == chain_index) {
            break;
        }
        chain_index = owner->loader_image_index;
    }

    if (!have_candidate) {
        r2_set_error(error,
                     error_len,
                     "dependency %s has no LC_RPATH search path",
                     install_name);
        return R2_ERR_UNSUPPORTED;
    }
    if (!copy_path(out, out_len, first_candidate, error, error_len)) {
        return R2_ERR_BAD_ARGUMENT;
    }
    return R2_OK;
}

static r2_status load_image(r2_dyld_context *context,
                            const char *path,
                            bool main_executable,
                            size_t loader_image_index,
                            size_t *index_out,
                            char *error,
                            size_t error_len)
{
    char canonical[PATH_MAX];
    if (realpath(path, canonical) != NULL) path = canonical;
    size_t existing = find_loaded_image(context, path);
    if (existing != SIZE_MAX) {
        *index_out = existing;
        return R2_OK;
    }
    if (context->image_count >= R2_DYLD_MAX_IMAGES) {
        r2_set_error(error, error_len, "too many loaded Mach-O images");
        return R2_ERR_UNSUPPORTED;
    }

    size_t index = context->image_count;
    r2_dyld_image *image = &context->images[index];
    memset(image, 0, sizeof(*image));
    image->globally_visible = true;
    image->loader_image_index = loader_image_index;
    if (!copy_path(image->path, sizeof(image->path), path, error, error_len)) {
        return R2_ERR_BAD_ARGUMENT;
    }
    r2_status status =
        read_file(image->path, &image->file_data, &image->file_size, error, error_len);
    if (status != R2_OK) {
        return status;
    }
    status = parse_macho_image(image, error, error_len);
    if (status != R2_OK) {
        free(image->file_data);
        image->file_data = NULL;
        return status;
    }
    if ((main_executable && image->filetype != MH_EXECUTE) ||
        (!main_executable && image->filetype != MH_DYLIB)) {
        r2_set_error(error,
                     error_len,
                     "%s is a Mach-O filetype %u, expected %s",
                     image->path,
                     image->filetype,
                     main_executable ? "executable" : "dylib");
        free(image->file_data);
        image->file_data = NULL;
        return R2_ERR_UNSUPPORTED;
    }
    context->image_count += 1;
    *index_out = index;
    return R2_OK;
}

static r2_status load_image_dependencies(r2_dyld_context *context,
                                         size_t image_index,
                                         char *error,
                                         size_t error_len)
{
    r2_dyld_image *image = &context->images[image_index];
    if (image->dependencies_loaded) {
        return R2_OK;
    }
    image->dependencies_loaded = true;

    for (size_t i = 0; i < image->dependency_count; i += 1) {
        image = &context->images[image_index];
        r2_dyld_dependency *dependency = &image->dependencies[i];
        if (context->native != NULL && r2_native_library(dependency->install_name)) {
            dependency->image_index = SIZE_MAX;
            continue;
        }
        char resolved[PATH_MAX];
        r2_status status = resolve_dependency_path(context,
                                                   image_index,
                                                   dependency->install_name,
                                                   resolved,
                                                   sizeof(resolved),
                                                   error,
                                                   error_len);
        if (status != R2_OK) {
            return status;
        }

        struct stat st;
        if (stat(resolved, &st) != 0) {
            if (dependency->weak && errno == ENOENT) {
                dependency->missing = true;
                dependency->image_index = SIZE_MAX;
                continue;
            }
            r2_set_error(error,
                         error_len,
                         "dependency %s resolved to %s but cannot be opened: %s",
                         dependency->install_name,
                         resolved,
                         strerror(errno));
            return R2_ERR_IO;
        }

        size_t loaded_index = SIZE_MAX;
        status = load_image(context,
                            resolved,
                            false,
                            image_index,
                            &loaded_index,
                            error,
                            error_len);
        if (status != R2_OK) {
            return status;
        }
        image = &context->images[image_index];
        image->dependencies[i].image_index = loaded_index;
        status = load_image_dependencies(context, loaded_index, error, error_len);
        if (status != R2_OK) {
            return status;
        }
    }
    return R2_OK;
}

static bool align_up_u64(uint64_t value, uint64_t alignment, uint64_t *out)
{
    uint64_t mask = alignment - 1u;
    if (value > UINT64_MAX - mask) {
        return false;
    }
    *out = (value + mask) & ~mask;
    return true;
}

static bool mapped_image_span(const r2_dyld_image *image,
                              uint64_t *minimum,
                              uint64_t *maximum)
{
    bool found = false;
    uint64_t min_value = UINT64_MAX;
    uint64_t max_value = 0;
    for (size_t i = 0; i < image->segment_count; i += 1) {
        const r2_dyld_segment *segment = &image->segments[i];
        if (segment->vmsize == 0 || strcmp(segment->name, "__PAGEZERO") == 0) {
            continue;
        }
        uint64_t end = 0;
        if (!add_u64(segment->vmaddr, segment->vmsize, &end)) {
            return false;
        }
        if (segment->vmaddr < min_value) {
            min_value = segment->vmaddr;
        }
        if (end > max_value) {
            max_value = end;
        }
        found = true;
    }
    if (found) {
        *minimum = min_value;
        *maximum = max_value;
    }
    return found;
}

static r2_status assign_image_slides(r2_dyld_context *context,
                                     char *error,
                                     size_t error_len)
{
    if (context->native != NULL) {
        for (size_t i = 0; i < context->image_count; i += 1) {
            uint64_t minimum = 0, maximum = 0;
            r2_dyld_image *image = &context->images[i];
            if (image->native_data != NULL) continue;
            if (!mapped_image_span(image, &minimum, &maximum) ||
                maximum - minimum > R2_DYLD_MAX_SEGMENT_SIZE) return R2_ERR_UNSUPPORTED;
            image->native_data = calloc(1, (size_t)(maximum - minimum));
            if (image->native_data == NULL) return R2_ERR_NO_MEMORY;
            image->slide = (uintptr_t)image->native_data - minimum;
        }
        return R2_OK;
    }
    uint64_t main_minimum = 0;
    uint64_t main_maximum = 0;
    if (!mapped_image_span(&context->images[0], &main_minimum, &main_maximum)) {
        r2_set_error(error, error_len, "executable has no mappable segments");
        return R2_ERR_DECODE;
    }
    (void)main_minimum;
    context->images[0].slide = 0;

    uint64_t next_base = R2_DYLD_BASE;
    uint64_t main_after_gap = 0;
    if (!add_u64(main_maximum, R2_DYLD_IMAGE_GAP, &main_after_gap) ||
        !align_up_u64(main_after_gap, R2_DYLD_PAGE_SIZE, &main_after_gap)) {
        r2_set_error(error, error_len, "executable VM range overflows");
        return R2_ERR_DECODE;
    }
    if (main_after_gap > next_base) {
        next_base = main_after_gap;
    }

    for (size_t i = 1; i < context->image_count; i += 1) {
        r2_dyld_image *image = &context->images[i];
        uint64_t minimum = 0;
        uint64_t maximum = 0;
        if (!mapped_image_span(image, &minimum, &maximum)) {
            r2_set_error(error, error_len, "dylib %s has no mappable segments", image->path);
            return R2_ERR_DECODE;
        }
        uint64_t target = 0;
        if (!align_up_u64(next_base, R2_DYLD_PAGE_SIZE, &target)) {
            r2_set_error(error, error_len, "dylib VM allocation overflows");
            return R2_ERR_NO_MEMORY;
        }
        if (target < minimum) {
            if (!align_up_u64(minimum, R2_DYLD_PAGE_SIZE, &target)) {
                r2_set_error(error, error_len, "dylib preferred VM address overflows");
                return R2_ERR_NO_MEMORY;
            }
        }
        image->slide = target - minimum;
        uint64_t span = maximum - minimum;
        if (!add_u64(target, span, &next_base) ||
            !add_u64(next_base, R2_DYLD_IMAGE_GAP, &next_base)) {
            r2_set_error(error, error_len, "dylib VM allocation overflows");
            return R2_ERR_NO_MEMORY;
        }
    }
    return R2_OK;
}

static bool ranges_overlap(uint64_t lhs_base,
                           uint64_t lhs_size,
                           uint64_t rhs_base,
                           uint64_t rhs_size)
{
    uint64_t lhs_end = lhs_base + lhs_size;
    uint64_t rhs_end = rhs_base + rhs_size;
    return lhs_end >= lhs_base && rhs_end >= rhs_base && lhs_base < rhs_end &&
           rhs_base < lhs_end;
}

static r2_status map_images(r2_dyld_context *context, char *error, size_t error_len)
{
    for (size_t image_index = 0; image_index < context->image_count; image_index += 1) {
        r2_dyld_image *image = &context->images[image_index];
        for (size_t segment_index = 0; segment_index < image->segment_count; segment_index += 1) {
            r2_dyld_segment *segment = &image->segments[segment_index];
            if (segment->mapped) continue;
            if (segment->vmsize == 0 || strcmp(segment->name, "__PAGEZERO") == 0) {
                continue;
            }
            if (segment->vmsize > R2_DYLD_MAX_SEGMENT_SIZE || segment->vmsize > SIZE_MAX) {
                r2_set_error(error,
                             error_len,
                             "segment %s in %s is too large",
                             segment->name,
                             image->path);
                return R2_ERR_UNSUPPORTED;
            }
            if (context->region_count >= R2_DYLD_MAX_REGIONS) {
                r2_set_error(error, error_len, "too many mapped dylib segments");
                return R2_ERR_UNSUPPORTED;
            }
            uint64_t base = 0;
            if (!add_u64(segment->vmaddr, image->slide, &base)) {
                r2_set_error(error, error_len, "segment address overflows in %s", image->path);
                return R2_ERR_DECODE;
            }
            for (size_t existing = 0; existing < context->region_count; existing += 1) {
                const r2_memory_region *region = &context->regions[existing];
                if (ranges_overlap(base, segment->vmsize, region->base, region->size)) {
                    r2_set_error(error,
                                 error_len,
                                 "mapped segment %s in %s overlaps another Mach-O image",
                                 segment->name,
                                 image->path);
                    return R2_ERR_UNSUPPORTED;
                }
            }

            uint8_t *data = context->native != NULL ? (uint8_t *)(uintptr_t)base
                                                  : calloc(1, (size_t)segment->vmsize);
            if (data == NULL) {
                r2_set_error(error, error_len, "out of memory mapping %s", image->path);
                return R2_ERR_NO_MEMORY;
            }
            if (segment->filesize != 0) {
                memcpy(data,
                       image->file_data + image->slice_offset + segment->fileoff,
                       (size_t)segment->filesize);
            }
            size_t region_index = context->region_count++;
            context->regions[region_index].base = base;
            context->regions[region_index].size = segment->vmsize;
            context->regions[region_index].data = data;
            context->regions[region_index].writable =
                (segment->initprot & VM_PROT_WRITE) != 0;
            segment->mapped = true;
            segment->region_index = region_index;
        }
    }
    return R2_OK;
}

static r2_status image_stream(const r2_dyld_image *image,
                              uint64_t offset,
                              uint64_t size,
                              const uint8_t **stream,
                              char *error,
                              size_t error_len)
{
    if (!span_in_size(offset, size, image->slice_size) || size > SIZE_MAX) {
        r2_set_error(error, error_len, "dyld linkedit data exceeds %s", image->path);
        return R2_ERR_DECODE;
    }
    *stream = image->file_data + image->slice_offset + offset;
    return R2_OK;
}

static r2_status read_uleb128(const uint8_t *stream,
                              size_t stream_size,
                              size_t *cursor,
                              uint64_t *value,
                              char *error,
                              size_t error_len)
{
    uint64_t result = 0;
    unsigned shift = 0;
    while (*cursor < stream_size && shift < 64) {
        uint8_t byte = stream[(*cursor)++];
        uint64_t payload = byte & 0x7fu;
        if (shift == 63 && payload > 1) {
            break;
        }
        result |= payload << shift;
        if ((byte & 0x80u) == 0) {
            *value = result;
            return R2_OK;
        }
        shift += 7;
    }
    r2_set_error(error, error_len, "invalid ULEB128 in dyld opcode stream");
    return R2_ERR_DECODE;
}

static r2_status read_sleb128(const uint8_t *stream,
                              size_t stream_size,
                              size_t *cursor,
                              int64_t *value,
                              char *error,
                              size_t error_len)
{
    uint64_t result = 0;
    unsigned shift = 0;
    uint8_t byte = 0;
    do {
        if (*cursor >= stream_size || shift >= 64) {
            r2_set_error(error, error_len, "invalid SLEB128 in dyld opcode stream");
            return R2_ERR_DECODE;
        }
        byte = stream[(*cursor)++];
        result |= (uint64_t)(byte & 0x7fu) << shift;
        shift += 7;
    } while ((byte & 0x80u) != 0);
    if (shift < 64 && (byte & 0x40u) != 0) {
        result |= UINT64_MAX << shift;
    }
    *value = (int64_t)result;
    return R2_OK;
}

static r2_status segment_pointer(r2_dyld_context *context,
                                 r2_dyld_image *image,
                                 size_t segment_index,
                                 uint64_t offset,
                                 uint8_t **pointer,
                                 char *error,
                                 size_t error_len)
{
    if (segment_index >= image->segment_count) {
        r2_set_error(error, error_len, "dyld opcode references an invalid segment index");
        return R2_ERR_DECODE;
    }
    r2_dyld_segment *segment = &image->segments[segment_index];
    if (!segment->mapped || !span_in_size(offset, 8, segment->vmsize) ||
        segment->region_index >= context->region_count) {
        r2_set_error(error,
                     error_len,
                     "dyld pointer is outside segment %s in %s",
                     segment->name,
                     image->path);
        return R2_ERR_DECODE;
    }
    *pointer = context->regions[segment->region_index].data + (size_t)offset;
    return R2_OK;
}

static r2_status advance_offset(uint64_t *offset,
                                uint64_t amount,
                                char *error,
                                size_t error_len)
{
    if (!add_u64(*offset, amount, offset)) {
        r2_set_error(error, error_len, "dyld segment offset overflows");
        return R2_ERR_DECODE;
    }
    return R2_OK;
}

static r2_status bind_address_delta(r2_dyld_image *image,
                                    size_t segment_index,
                                    uint64_t *offset,
                                    uint64_t amount,
                                    char *error,
                                    size_t error_len)
{
    /* dyld's ADD_ADDR_ULEB uses unsigned address arithmetic. Apple's linker
     * also emits negative deltas encoded as full-width ULEBs. Check the
     * resulting segment offset, rather than rejecting a valid wrap. */
    uint64_t target = *offset + amount;
    if (segment_index >= image->segment_count ||
        !image->segments[segment_index].mapped ||
        target > image->segments[segment_index].vmsize) {
        r2_set_error(error, error_len, "dyld bind address is outside segment");
        return R2_ERR_DECODE;
    }
    *offset = target;
    return R2_OK;
}

static r2_status rebase_pointer(r2_dyld_context *context,
                                r2_dyld_image *image,
                                size_t segment_index,
                                uint64_t segment_offset,
                                uint8_t type,
                                char *error,
                                size_t error_len)
{
    if (type != REBASE_TYPE_POINTER) {
        r2_set_error(error,
                     error_len,
                     "unsupported rebase type %u in %s",
                     type,
                     image->path);
        return R2_ERR_UNSUPPORTED;
    }
    uint8_t *pointer = NULL;
    r2_status status = segment_pointer(context,
                                       image,
                                       segment_index,
                                       segment_offset,
                                       &pointer,
                                       error,
                                       error_len);
    if (status != R2_OK) {
        return status;
    }
    uint64_t value = read_le64(pointer);
    if (!add_u64(value, image->slide, &value)) {
        r2_set_error(error, error_len, "rebased pointer overflows in %s", image->path);
        return R2_ERR_DECODE;
    }
    write_le64(pointer, value);
    return R2_OK;
}

static r2_status apply_rebase_stream(r2_dyld_context *context,
                                     r2_dyld_image *image,
                                     char *error,
                                     size_t error_len)
{
    if (image->rebase_size == 0) {
        return R2_OK;
    }
    const uint8_t *stream = NULL;
    r2_status status = image_stream(image,
                                    image->rebase_off,
                                    image->rebase_size,
                                    &stream,
                                    error,
                                    error_len);
    if (status != R2_OK) {
        return status;
    }

    size_t cursor = 0;
    size_t stream_size = (size_t)image->rebase_size;
    size_t segment_index = SIZE_MAX;
    uint64_t segment_offset = 0;
    uint8_t type = REBASE_TYPE_POINTER;
    while (cursor < stream_size) {
        uint8_t byte = stream[cursor++];
        uint8_t opcode = byte & 0xf0u;
        uint8_t immediate = byte & 0x0fu;
        uint64_t value = 0;
        uint64_t count = 0;
        uint64_t skip = 0;

        switch (opcode) {
        case 0x00:
            return R2_OK;
        case 0x10:
            type = immediate;
            break;
        case 0x20:
            segment_index = immediate;
            status = read_uleb128(stream,
                                  stream_size,
                                  &cursor,
                                  &segment_offset,
                                  error,
                                  error_len);
            if (status != R2_OK) {
                return status;
            }
            break;
        case 0x30:
            status = read_uleb128(stream, stream_size, &cursor, &value, error, error_len);
            if (status != R2_OK) {
                return status;
            }
            status = advance_offset(&segment_offset, value, error, error_len);
            if (status != R2_OK) {
                return status;
            }
            break;
        case 0x40:
            status = advance_offset(&segment_offset,
                                    (uint64_t)immediate * 8u,
                                    error,
                                    error_len);
            if (status != R2_OK) {
                return status;
            }
            break;
        case 0x50:
            count = immediate;
            for (uint64_t i = 0; i < count; i += 1) {
                status = rebase_pointer(context,
                                        image,
                                        segment_index,
                                        segment_offset,
                                        type,
                                        error,
                                        error_len);
                if (status != R2_OK) {
                    return status;
                }
                status = advance_offset(&segment_offset, 8, error, error_len);
                if (status != R2_OK) {
                    return status;
                }
            }
            break;
        case 0x60:
            status = read_uleb128(stream, stream_size, &cursor, &count, error, error_len);
            if (status != R2_OK || count > 1000000u) {
                if (status == R2_OK) {
                    r2_set_error(error, error_len, "unreasonable rebase repeat count");
                    status = R2_ERR_UNSUPPORTED;
                }
                return status;
            }
            for (uint64_t i = 0; i < count; i += 1) {
                status = rebase_pointer(context,
                                        image,
                                        segment_index,
                                        segment_offset,
                                        type,
                                        error,
                                        error_len);
                if (status != R2_OK) {
                    return status;
                }
                status = advance_offset(&segment_offset, 8, error, error_len);
                if (status != R2_OK) {
                    return status;
                }
            }
            break;
        case 0x70:
            status = read_uleb128(stream, stream_size, &cursor, &value, error, error_len);
            if (status != R2_OK) {
                return status;
            }
            status = rebase_pointer(context,
                                    image,
                                    segment_index,
                                    segment_offset,
                                    type,
                                    error,
                                    error_len);
            if (status != R2_OK) {
                return status;
            }
            if (!add_u64(value, 8, &value)) {
                r2_set_error(error, error_len, "rebase offset increment overflows");
                return R2_ERR_DECODE;
            }
            status = advance_offset(&segment_offset, value, error, error_len);
            if (status != R2_OK) {
                return status;
            }
            break;
        case 0x80:
            status = read_uleb128(stream, stream_size, &cursor, &count, error, error_len);
            if (status == R2_OK) {
                status = read_uleb128(stream, stream_size, &cursor, &skip, error, error_len);
            }
            if (status != R2_OK || count > 1000000u) {
                if (status == R2_OK) {
                    r2_set_error(error, error_len, "unreasonable rebase repeat count");
                    status = R2_ERR_UNSUPPORTED;
                }
                return status;
            }
            if (!add_u64(skip, 8, &skip)) {
                r2_set_error(error, error_len, "rebase skip overflows");
                return R2_ERR_DECODE;
            }
            for (uint64_t i = 0; i < count; i += 1) {
                status = rebase_pointer(context,
                                        image,
                                        segment_index,
                                        segment_offset,
                                        type,
                                        error,
                                        error_len);
                if (status != R2_OK) {
                    return status;
                }
                status = advance_offset(&segment_offset, skip, error, error_len);
                if (status != R2_OK) {
                    return status;
                }
            }
            break;
        default:
            r2_set_error(error,
                         error_len,
                         "unsupported rebase opcode 0x%02x in %s",
                         opcode,
                         image->path);
            return R2_ERR_UNSUPPORTED;
        }
    }
    return R2_OK;
}

static bool lookup_symbol_in_symtab(const r2_dyld_image *image,
                                    const char *symbol,
                                    uint64_t *address,
                                    bool *weak)
{
    if (!image->has_symtab ||
        !span_in_size(image->symoff, (uint64_t)image->nsyms * 16u, image->slice_size) ||
        !span_in_size(image->stroff, image->strsize, image->slice_size)) {
        return false;
    }
    const uint8_t *symbols = image->file_data + image->slice_offset + image->symoff;
    const uint8_t *strings = image->file_data + image->slice_offset + image->stroff;
    for (uint32_t i = 0; i < image->nsyms; i += 1) {
        const uint8_t *entry = symbols + (size_t)i * 16u;
        uint32_t string_index = read_le32(entry);
        uint8_t type = entry[4];
        if ((type & N_STAB) != 0 || (type & N_EXT) == 0 ||
            ((type & N_TYPE) != N_SECT && (type & N_TYPE) != N_ABS) ||
            string_index >= image->strsize) {
            continue;
        }
        const char *name = (const char *)(strings + string_index);
        size_t remaining = (size_t)(image->strsize - string_index);
        if (memchr(name, '\0', remaining) == NULL || strcmp(name, symbol) != 0) {
            continue;
        }
        uint64_t value = read_le64(entry + 8);
        if ((type & N_TYPE) == N_SECT && !add_u64(value, image->slide, &value)) {
            return false;
        }
        *address = value;
        *weak = (entry[6] & 0x80) != 0; /* N_WEAK_DEF */
        return true;
    }
    return false;
}

static bool image_preferred_base(const r2_dyld_image *image, uint64_t *base)
{
    uint64_t minimum = UINT64_MAX;
    for (size_t i = 0; i < image->segment_count; i += 1) {
        const r2_dyld_segment *segment = &image->segments[i];
        if (segment->vmsize != 0 && strcmp(segment->name, "__PAGEZERO") != 0 &&
            segment->vmaddr < minimum) {
            minimum = segment->vmaddr;
        }
    }
    if (minimum == UINT64_MAX) {
        return false;
    }
    *base = minimum;
    return true;
}

static r2_status lookup_definition_from_image(const r2_dyld_context *context,
                                          size_t image_index,
                                          const char *symbol,
                                          uint64_t visited,
                                          unsigned depth,
                                          uint64_t *address,
                                          bool *found,
                                          bool *weak,
                                          char *error,
                                          size_t error_len);

static r2_status lookup_symbol_in_export_node(const r2_dyld_context *context,
                                              size_t image_index,
                                              const uint8_t *trie,
                                              size_t trie_size,
                                              uint64_t node_offset,
                                              const char *symbol,
                                              size_t symbol_offset,
                                              uint64_t visited,
                                              unsigned image_depth,
                                              unsigned node_depth,
                                              uint64_t *address,
                                              bool *found,
                                              bool *weak,
                                              char *error,
                                              size_t error_len)
{
    *found = false;
    *weak = false;
    if (node_depth > 1024 || node_offset >= trie_size) {
        r2_set_error(error, error_len, "invalid export trie node offset or cycle");
        return R2_ERR_DECODE;
    }
    size_t cursor = (size_t)node_offset;
    uint64_t terminal_size = 0;
    r2_status status =
        read_uleb128(trie, trie_size, &cursor, &terminal_size, error, error_len);
    if (status != R2_OK || terminal_size > trie_size - cursor) {
        if (status == R2_OK) {
            r2_set_error(error, error_len, "export trie terminal exceeds its payload");
            status = R2_ERR_DECODE;
        }
        return status;
    }
    size_t terminal_end = cursor + (size_t)terminal_size;
    if (symbol[symbol_offset] == '\0' && terminal_size != 0) {
        size_t terminal_cursor = cursor;
        uint64_t flags = 0;
        status = read_uleb128(trie,
                              terminal_end,
                              &terminal_cursor,
                              &flags,
                              error,
                              error_len);
        if (status != R2_OK) {
            return status;
        }
        if ((flags & EXPORT_SYMBOL_FLAGS_REEXPORT) != 0) {
            uint64_t ordinal = 0;
            status = read_uleb128(trie,
                                  terminal_end,
                                  &terminal_cursor,
                                  &ordinal,
                                  error,
                                  error_len);
            if (status != R2_OK) {
                return status;
            }
            const uint8_t *name_end =
                memchr(trie + terminal_cursor, '\0', terminal_end - terminal_cursor);
            if (name_end == NULL || ordinal == 0 || ordinal > SIZE_MAX) {
                r2_set_error(error, error_len, "invalid re-export terminal in export trie");
                return R2_ERR_DECODE;
            }
            const r2_dyld_image *image = &context->images[image_index];
            size_t dependency_index = (size_t)ordinal - 1u;
            if (dependency_index >= image->dependency_count) {
                r2_set_error(error, error_len, "export trie re-export ordinal is out of range");
                return R2_ERR_DECODE;
            }
            const r2_dyld_dependency *dependency = &image->dependencies[dependency_index];
            if (dependency->missing || dependency->image_index >= context->image_count) {
                return R2_OK;
            }
            const char *import_name = (const char *)(trie + terminal_cursor);
            if (import_name[0] == '\0') {
                import_name = symbol;
            }
            return lookup_definition_from_image(context,
                                            dependency->image_index,
                                            import_name,
                                            visited,
                                            image_depth + 1u,
                                            address,
                                            found,
                                            weak,
                                            error,
                                            error_len);
        }

        uint64_t value = 0;
        status = read_uleb128(trie,
                              terminal_end,
                              &terminal_cursor,
                              &value,
                              error,
                              error_len);
        if (status != R2_OK) {
            return status;
        }
        if ((flags & EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER) != 0) {
            uint64_t resolver = 0;
            status = read_uleb128(trie,
                                  terminal_end,
                                  &terminal_cursor,
                                  &resolver,
                                  error,
                                  error_len);
            if (status != R2_OK) {
                return status;
            }
            (void)resolver;
        }
        uint64_t kind = flags & EXPORT_SYMBOL_FLAGS_KIND_MASK;
        if (kind == EXPORT_SYMBOL_FLAGS_KIND_THREAD_LOCAL) {
            r2_set_error(error, error_len, "thread-local Mach-O exports are not supported yet");
            return R2_ERR_UNSUPPORTED;
        }
        if (kind == EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE) {
            *address = value;
        } else if (kind == EXPORT_SYMBOL_FLAGS_KIND_REGULAR) {
            uint64_t base = 0;
            if (!image_preferred_base(&context->images[image_index], &base) ||
                !add_u64(base, context->images[image_index].slide, &base) ||
                !add_u64(base, value, address)) {
                r2_set_error(error, error_len, "exported symbol address overflows");
                return R2_ERR_DECODE;
            }
        } else {
            r2_set_error(error, error_len, "unsupported Mach-O export kind");
            return R2_ERR_UNSUPPORTED;
        }
        *weak = (flags & EXPORT_SYMBOL_FLAGS_WEAK_DEFINITION) != 0;
        *found = true;
        return R2_OK;
    }

    cursor = terminal_end;
    if (cursor >= trie_size) {
        r2_set_error(error, error_len, "export trie node has no child count");
        return R2_ERR_DECODE;
    }
    uint8_t child_count = trie[cursor++];
    size_t symbol_remaining = strlen(symbol + symbol_offset);
    for (uint8_t i = 0; i < child_count; i += 1) {
        const uint8_t *edge_end = memchr(trie + cursor, '\0', trie_size - cursor);
        if (edge_end == NULL) {
            r2_set_error(error, error_len, "unterminated export trie edge");
            return R2_ERR_DECODE;
        }
        size_t edge_size = (size_t)(edge_end - (trie + cursor));
        if (edge_size == 0) {
            r2_set_error(error, error_len, "empty export trie edge");
            return R2_ERR_DECODE;
        }
        const uint8_t *edge = trie + cursor;
        cursor += edge_size + 1u;
        uint64_t child_offset = 0;
        status = read_uleb128(trie,
                              trie_size,
                              &cursor,
                              &child_offset,
                              error,
                              error_len);
        if (status != R2_OK) {
            return status;
        }
        if (edge_size <= symbol_remaining &&
            memcmp(edge, symbol + symbol_offset, edge_size) == 0) {
            return lookup_symbol_in_export_node(context,
                                                image_index,
                                                trie,
                                                trie_size,
                                                child_offset,
                                                symbol,
                                                symbol_offset + edge_size,
                                                visited,
                                                image_depth,
                                                node_depth + 1u,
                                                address,
                                                found,
                                                weak,
                                                error,
                                                error_len);
        }
    }
    return R2_OK;
}

static r2_status lookup_definition_from_image(const r2_dyld_context *context,
                                          size_t image_index,
                                          const char *symbol,
                                          uint64_t visited,
                                          unsigned depth,
                                          uint64_t *address,
                                          bool *found,
                                          bool *weak,
                                          char *error,
                                          size_t error_len)
{
    *found = false;
    *weak = false;
    if (image_index >= context->image_count || image_index >= 64u ||
        depth > R2_DYLD_MAX_IMAGES) {
        r2_set_error(error, error_len, "invalid or cyclic Mach-O re-export graph");
        return R2_ERR_DECODE;
    }
    uint64_t image_bit = 1ull << image_index;
    if ((visited & image_bit) != 0) {
        return R2_OK;
    }
    visited |= image_bit;
    const r2_dyld_image *image = &context->images[image_index];
    if (image->export_size != 0) {
        const uint8_t *trie = NULL;
        r2_status status = image_stream(image,
                                        image->export_off,
                                        image->export_size,
                                        &trie,
                                        error,
                                        error_len);
        if (status != R2_OK) {
            return status;
        }
        status = lookup_symbol_in_export_node(context,
                                              image_index,
                                              trie,
                                              (size_t)image->export_size,
                                              0,
                                              symbol,
                                              0,
                                              visited,
                                              depth,
                                              0,
                                              address,
                                              found,
                                              weak,
                                              error,
                                              error_len);
        if (status != R2_OK || *found) {
            return status;
        }
    } else if (lookup_symbol_in_symtab(image, symbol, address, weak)) {
        *found = true;
        return R2_OK;
    }

    for (size_t i = 0; i < image->dependency_count; i += 1) {
        const r2_dyld_dependency *dependency = &image->dependencies[i];
        if (!dependency->reexport || dependency->missing ||
            dependency->image_index >= context->image_count) {
            continue;
        }
        r2_status status = lookup_definition_from_image(context,
                                                    dependency->image_index,
                                                    symbol,
                                                    visited,
                                                    depth + 1u,
                                                    address,
                                                    found,
                                                    weak,
                                                    error,
                                                    error_len);
        if (status != R2_OK || *found) {
            return status;
        }
    }
    return R2_OK;
}

static r2_status lookup_symbol_from_image(const r2_dyld_context *context,
                                          size_t image_index, const char *symbol,
                                          uint64_t visited, unsigned depth,
                                          uint64_t *address, bool *found,
                                          char *error, size_t error_len)
{
    bool weak;
    return lookup_definition_from_image(context, image_index, symbol, visited, depth,
                                         address, found, &weak, error, error_len);
}

/* Opcode weak binding is a second pass over normal rebases/binds. Missing
 * definitions leave that earlier fixup intact, as in Apple's dyld Loader.cpp.
 * Hidden (RTLD_LOCAL) images do not participate in global coalescing. */
static r2_status lookup_coalesced_symbol(const r2_dyld_context *context,
                                         const char *symbol, uint64_t *address,
                                         bool *found, char *error, size_t error_len)
{
    *found = false;
    for (size_t i = 0; i < context->image_count; i++) {
        if (!context->images[i].globally_visible) continue;
        bool candidate_found, weak; uint64_t candidate;
        r2_status status = lookup_definition_from_image(context, i, symbol, 0, 0,
            &candidate, &candidate_found, &weak, error, error_len);
        if (status != R2_OK) return status;
        if (!candidate_found) continue;
        if (!*found || !weak) { *address = candidate; *found = true; }
        if (!weak) return R2_OK;
    }
    return R2_OK;
}

static r2_status lookup_symbol_flat(const r2_dyld_context *context,
                                    const char *symbol,
                                    uint64_t *address,
                                    bool *found,
                                    char *error,
                                    size_t error_len)
{
    *found = false;
    for (size_t i = 0; i < context->image_count; i += 1) {
        if (!context->images[i].globally_visible) continue;
        r2_status status = lookup_symbol_from_image(context,
                                                    i,
                                                    symbol,
                                                    0,
                                                    0,
                                                    address,
                                                    found,
                                                    error,
                                                    error_len);
        if (status != R2_OK || *found) {
            return status;
        }
    }
    return R2_OK;
}

static r2_status native_lookup_symbol(void *opaque, uint64_t handle, const char *symbol,
                                       uint64_t *address, bool *found, char *error, size_t error_len)
{
    r2_dyld_context *context = opaque;
    if (handle == (uintptr_t)RTLD_DEFAULT) return lookup_symbol_flat(context, symbol, address, found, error, error_len);
    bool first = (handle & 1) != 0;
    handle &= ~UINT64_C(1);
    size_t queue[R2_DYLD_MAX_IMAGES], count = 0; uint32_t visited = 0;
    for (size_t i = 0; i < context->image_count; i++) {
        if (handle == (uintptr_t)&context->images[i] && context->images[i].open_count != 0) queue[count++] = i;
    }
    if (count == 0) { r2_set_error(error, error_len, "invalid guest dlsym handle"); return R2_ERR_UNSUPPORTED; }
    visited = UINT32_C(1) << queue[0];
    *found = false;
    for (size_t cursor = 0; cursor < count; cursor++) {
        size_t index = queue[cursor];
        r2_status status = lookup_symbol_from_image(context, index, symbol, 0, 0, address, found, error, error_len);
        if (status != R2_OK || *found || first) return status;
        const r2_dyld_image *image = &context->images[index];
        for (size_t j = 0; j < image->dependency_count; j++) {
            size_t next = image->dependencies[j].image_index;
            if (image->dependencies[j].missing || next >= context->image_count || (visited & (UINT32_C(1) << next))) continue;
            visited |= UINT32_C(1) << next; queue[count++] = next;
        }
    }
    return R2_OK;
}

static bool native_lookup_address(void *opaque, uint64_t address, r2_dl_info *info)
{
    const r2_dyld_context *context = opaque;
    for (size_t i = 0; i < context->image_count; i++) {
        const r2_dyld_image *image = &context->images[i];
        for (size_t j = 0; j < image->segment_count; j++) {
            const r2_dyld_segment *segment = &image->segments[j];
            uint64_t base = segment->vmaddr + image->slide;
            if (!segment->mapped || address < base || address - base >= segment->vmsize) continue;
            uint64_t preferred;
            if (!image_preferred_base(image, &preferred)) return false;
            info->filename = image->path;
            info->base = (void *)(uintptr_t)(preferred + image->slide);
            /* dladdr permits NULL symbol fields when no symbol is available. */
            info->symbol = NULL; info->address = NULL;
            return true;
        }
    }
    return false;
}

static r2_status resolve_bind_symbol(const r2_dyld_context *context,
                                     const r2_dyld_image *requester,
                                     int ordinal,
                                     const char *symbol,
                                     bool weak_import,
                                     uint64_t *address,
                                     char *error,
                                     size_t error_len)
{
    bool found = false;
    r2_status status = R2_OK;
    if (ordinal > 0) {
        size_t dependency_index = (size_t)(ordinal - 1);
        if (dependency_index >= requester->dependency_count) {
            r2_set_error(error,
                         error_len,
                         "bind ordinal %d is outside dependency list for %s",
                         ordinal,
                         requester->path);
            return R2_ERR_DECODE;
        }
        const r2_dyld_dependency *dependency = &requester->dependencies[dependency_index];
        if (context->native != NULL && r2_native_library(dependency->install_name)) {
            if ((strcmp(dependency->install_name, "/usr/lib/libc++.1.dylib") == 0 ||
                 (strcmp(dependency->install_name, "/usr/lib/libSystem.B.dylib") == 0 && strcmp(symbol, "_qsort") == 0)) &&
                context->cpp_image_index < context->image_count) {
                status = lookup_symbol_from_image(context, context->cpp_image_index, symbol, 0, 0,
                                                  address, &found, error, error_len);
                if (status != R2_OK || found) return status;
            }
            return r2_native_symbol(context->native, symbol, address, error, error_len);
        }
        if (!dependency->missing && dependency->image_index < context->image_count) {
            status = lookup_symbol_from_image(context,
                                              dependency->image_index,
                                              symbol,
                                              0,
                                              0,
                                              address,
                                              &found,
                                              error,
                                              error_len);
        }
    } else if (ordinal == BIND_SPECIAL_DYLIB_SELF) {
        size_t requester_index = (size_t)(requester - context->images);
        status = lookup_symbol_from_image(context,
                                          requester_index,
                                          symbol,
                                          0,
                                          0,
                                          address,
                                          &found,
                                          error,
                                          error_len);
    } else if (ordinal == BIND_SPECIAL_DYLIB_MAIN_EXECUTABLE) {
        status = lookup_symbol_from_image(context,
                                          0,
                                          symbol,
                                          0,
                                          0,
                                          address,
                                          &found,
                                          error,
                                          error_len);
    } else if (ordinal == BIND_SPECIAL_DYLIB_WEAK_LOOKUP) {
        status = lookup_coalesced_symbol(context, symbol, address, &found, error, error_len);
    } else if (ordinal == BIND_SPECIAL_DYLIB_FLAT_LOOKUP) {
        status = lookup_symbol_flat(context,
                                    symbol,
                                    address,
                                    &found,
                                    error,
                                    error_len);
    } else {
        r2_set_error(error, error_len, "unsupported special dylib ordinal %d", ordinal);
        return R2_ERR_UNSUPPORTED;
    }

    if (status != R2_OK) {
        return status;
    }
    if (found) {
        return R2_OK;
    }
    if (weak_import || ordinal == BIND_SPECIAL_DYLIB_WEAK_LOOKUP) {
        *address = 0;
        return R2_OK;
    }
    r2_set_error(error,
                 error_len,
                 "unresolved symbol %s required by %s",
                 symbol,
                 requester->path);
    return R2_ERR_UNSUPPORTED;
}

static r2_status add_signed_value(uint64_t value,
                                  int64_t addend,
                                  uint64_t *result,
                                  char *error,
                                  size_t error_len)
{
    if (addend >= 0) {
        if (!add_u64(value, (uint64_t)addend, result)) {
            r2_set_error(error, error_len, "bound symbol plus addend overflows");
            return R2_ERR_DECODE;
        }
    } else {
        uint64_t magnitude = (uint64_t)(-(addend + 1)) + 1u;
        if (value < magnitude) {
            r2_set_error(error, error_len, "bound symbol plus addend underflows");
            return R2_ERR_DECODE;
        }
        *result = value - magnitude;
    }
    return R2_OK;
}

static r2_status bind_pointer(r2_dyld_context *context,
                              r2_dyld_image *image,
                              size_t segment_index,
                              uint64_t segment_offset,
                              uint8_t type,
                              int ordinal,
                              const char *symbol,
                              uint8_t symbol_flags,
                              int64_t addend,
                              bool coalescing,
                              char *error,
                              size_t error_len)
{
    if (type != BIND_TYPE_POINTER) {
        r2_set_error(error,
                     error_len,
                     "unsupported bind type %u in %s",
                     type,
                     image->path);
        return R2_ERR_UNSUPPORTED;
    }
    uint8_t *pointer = NULL;
    r2_status status = segment_pointer(context, image, segment_index, segment_offset,
                                        &pointer, error, error_len);
    if (status != R2_OK) return status;
    uint64_t address = 0;
    if (coalescing) {
        bool found;
        status = lookup_coalesced_symbol(context, symbol, &address, &found, error, error_len);
        if (status != R2_OK || !found) return status;
    } else {
        status = resolve_bind_symbol(context, image, ordinal, symbol,
            (symbol_flags & BIND_SYMBOL_FLAGS_WEAK_IMPORT) != 0, &address, error, error_len);
        if (status != R2_OK) return status;
    }
    status = add_signed_value(address, addend, &address, error, error_len);
    if (status != R2_OK) return status;
    write_le64(pointer, address);
    return R2_OK;
}

static r2_status apply_bind_stream(r2_dyld_context *context,
                                   r2_dyld_image *image,
                                   uint64_t stream_offset,
                                   uint64_t stream_length,
                                   int initial_ordinal,
                                   bool multiple_programs,
                                   bool coalescing,
                                   char *error,
                                   size_t error_len)
{
    if (stream_length == 0) {
        return R2_OK;
    }
    const uint8_t *stream = NULL;
    r2_status status = image_stream(image,
                                    stream_offset,
                                    stream_length,
                                    &stream,
                                    error,
                                    error_len);
    if (status != R2_OK) {
        return status;
    }

    size_t cursor = 0;
    size_t stream_size = (size_t)stream_length;
    size_t segment_index = SIZE_MAX;
    uint64_t segment_offset = 0;
    uint8_t type = BIND_TYPE_POINTER;
    int ordinal = initial_ordinal;
    char symbol[512] = {0};
    bool has_symbol = false;
    uint8_t symbol_flags = 0;
    int64_t addend = 0;

    while (cursor < stream_size) {
        uint8_t byte = stream[cursor++];
        uint8_t opcode = byte & 0xf0u;
        uint8_t immediate = byte & 0x0fu;
        uint64_t value = 0;
        uint64_t count = 0;
        uint64_t skip = 0;

        if (coalescing && (opcode == 0x10 || opcode == 0x20 || opcode == 0x30)) {
            r2_set_error(error, error_len, "dylib ordinal opcode in weak bind stream");
            return R2_ERR_DECODE;
        }
        switch (opcode) {
        case 0x00:
            if (!multiple_programs || cursor >= stream_size) {
                return R2_OK;
            }
            segment_index = SIZE_MAX;
            segment_offset = 0;
            type = BIND_TYPE_POINTER;
            ordinal = initial_ordinal;
            symbol[0] = '\0';
            has_symbol = false;
            symbol_flags = 0;
            addend = 0;
            break;
        case 0x10:
            ordinal = immediate;
            break;
        case 0x20:
            status = read_uleb128(stream, stream_size, &cursor, &value, error, error_len);
            if (status != R2_OK || value > INT_MAX) {
                if (status == R2_OK) {
                    r2_set_error(error, error_len, "dylib ordinal is too large");
                    status = R2_ERR_DECODE;
                }
                return status;
            }
            ordinal = (int)value;
            break;
        case 0x30:
            ordinal = immediate == 0 ? 0 : (int)(int8_t)(immediate | 0xf0u);
            break;
        case 0x40: {
            const uint8_t *end = memchr(stream + cursor, '\0', stream_size - cursor);
            if (end == NULL) {
                r2_set_error(error, error_len, "unterminated symbol in bind stream");
                return R2_ERR_DECODE;
            }
            size_t length = (size_t)(end - (stream + cursor));
            if (length == 0 || length >= sizeof(symbol)) {
                r2_set_error(error, error_len, "bind symbol is empty or too long");
                return R2_ERR_UNSUPPORTED;
            }
            memcpy(symbol, stream + cursor, length);
            symbol[length] = '\0';
            cursor += length + 1u;
            symbol_flags = immediate;
            has_symbol = true;
            break;
        }
        case 0x50:
            type = immediate;
            break;
        case 0x60:
            status = read_sleb128(stream, stream_size, &cursor, &addend, error, error_len);
            if (status != R2_OK) {
                return status;
            }
            break;
        case 0x70:
            segment_index = immediate;
            status = read_uleb128(stream,
                                  stream_size,
                                  &cursor,
                                  &segment_offset,
                                  error,
                                  error_len);
            if (status != R2_OK) {
                return status;
            }
            break;
        case 0x80:
            status = read_uleb128(stream, stream_size, &cursor, &value, error, error_len);
            if (status != R2_OK) {
                return status;
            }
            status = bind_address_delta(image, segment_index, &segment_offset,
                                        value, error, error_len);
            if (status != R2_OK) {
                return status;
            }
            break;
        case 0x90:
            if (!has_symbol) {
                r2_set_error(error, error_len, "bind opcode has no symbol");
                return R2_ERR_DECODE;
            }
            status = bind_pointer(context,
                                  image,
                                  segment_index,
                                  segment_offset,
                                  type,
                                  ordinal,
                                  symbol,
                                  symbol_flags,
                                  addend,
                                  coalescing,
                                  error,
                                  error_len);
            if (status != R2_OK) {
                return status;
            }
            status = advance_offset(&segment_offset, 8, error, error_len);
            if (status != R2_OK) {
                return status;
            }
            break;
        case 0xa0:
            status = read_uleb128(stream, stream_size, &cursor, &value, error, error_len);
            if (status != R2_OK || !has_symbol) {
                if (status == R2_OK) {
                    r2_set_error(error, error_len, "bind opcode has no symbol");
                    status = R2_ERR_DECODE;
                }
                return status;
            }
            status = bind_pointer(context,
                                  image,
                                  segment_index,
                                  segment_offset,
                                  type,
                                  ordinal,
                                  symbol,
                                  symbol_flags,
                                  addend,
                                  coalescing,
                                  error,
                                  error_len);
            if (status != R2_OK) {
                return status;
            }
            if (!add_u64(value, 8, &value)) {
                r2_set_error(error, error_len, "bind offset increment overflows");
                return R2_ERR_DECODE;
            }
            status = advance_offset(&segment_offset, value, error, error_len);
            if (status != R2_OK) {
                return status;
            }
            break;
        case 0xb0:
            if (!has_symbol) {
                r2_set_error(error, error_len, "bind opcode has no symbol");
                return R2_ERR_DECODE;
            }
            status = bind_pointer(context,
                                  image,
                                  segment_index,
                                  segment_offset,
                                  type,
                                  ordinal,
                                  symbol,
                                  symbol_flags,
                                  addend,
                                  coalescing,
                                  error,
                                  error_len);
            if (status != R2_OK) {
                return status;
            }
            value = 8u + (uint64_t)immediate * 8u;
            status = advance_offset(&segment_offset, value, error, error_len);
            if (status != R2_OK) {
                return status;
            }
            break;
        case 0xc0:
            status = read_uleb128(stream, stream_size, &cursor, &count, error, error_len);
            if (status == R2_OK) {
                status = read_uleb128(stream, stream_size, &cursor, &skip, error, error_len);
            }
            if (status != R2_OK || count > 1000000u || !has_symbol) {
                if (status == R2_OK) {
                    r2_set_error(error, error_len, "invalid bind repeat state");
                    status = R2_ERR_UNSUPPORTED;
                }
                return status;
            }
            if (!add_u64(skip, 8, &skip)) {
                r2_set_error(error, error_len, "bind skip overflows");
                return R2_ERR_DECODE;
            }
            for (uint64_t i = 0; i < count; i += 1) {
                status = bind_pointer(context,
                                      image,
                                      segment_index,
                                      segment_offset,
                                      type,
                                      ordinal,
                                      symbol,
                                      symbol_flags,
                                      addend,
                                      coalescing,
                                      error,
                                      error_len);
                if (status != R2_OK) {
                    return status;
                }
                status = advance_offset(&segment_offset, skip, error, error_len);
                if (status != R2_OK) {
                    return status;
                }
            }
            break;
        case 0xd0:
            r2_set_error(error,
                         error_len,
                         "threaded bind opcodes are not supported yet in %s",
                         image->path);
            return R2_ERR_UNSUPPORTED;
        default:
            r2_set_error(error,
                         error_len,
                         "unsupported bind opcode 0x%02x in %s",
                         opcode,
                         image->path);
            return R2_ERR_UNSUPPORTED;
        }
    }
    return R2_OK;
}

typedef struct {
    int ordinal;
    bool weak_import;
    const char *symbol;
    int64_t addend;
} r2_chained_import_info;

static int64_t sign_extend_u64(uint64_t value, unsigned bits)
{
    uint64_t sign = 1ull << (bits - 1u);
    return (int64_t)((value ^ sign) - sign);
}

static int64_t bits_to_i64(uint64_t value)
{
    if (value <= INT64_MAX) {
        return (int64_t)value;
    }
    return -1 - (int64_t)(UINT64_MAX - value);
}

static r2_status parse_chained_import(const uint8_t *payload,
                                      size_t payload_size,
                                      uint32_t imports_offset,
                                      uint32_t imports_count,
                                      uint32_t imports_format,
                                      uint32_t symbols_offset,
                                      uint32_t import_index,
                                      r2_chained_import_info *info,
                                      char *error,
                                      size_t error_len)
{
    if (import_index >= imports_count) {
        r2_set_error(error, error_len, "chained bind import ordinal is out of range");
        return R2_ERR_DECODE;
    }
    size_t entry_size = 0;
    if (imports_format == DYLD_CHAINED_IMPORT) {
        entry_size = 4;
    } else if (imports_format == DYLD_CHAINED_IMPORT_ADDEND) {
        entry_size = 8;
    } else if (imports_format == DYLD_CHAINED_IMPORT_ADDEND64) {
        entry_size = 16;
    } else {
        r2_set_error(error,
                     error_len,
                     "unsupported chained-import format %u",
                     imports_format);
        return R2_ERR_UNSUPPORTED;
    }
    uint64_t entry_offset = (uint64_t)imports_offset + (uint64_t)import_index * entry_size;
    if (!span_in_size(entry_offset, entry_size, payload_size)) {
        r2_set_error(error, error_len, "chained import table exceeds fixups payload");
        return R2_ERR_DECODE;
    }
    const uint8_t *entry = payload + (size_t)entry_offset;
    uint64_t name_offset = 0;
    memset(info, 0, sizeof(*info));
    if (imports_format == DYLD_CHAINED_IMPORT ||
        imports_format == DYLD_CHAINED_IMPORT_ADDEND) {
        uint32_t raw = read_le32(entry);
        info->ordinal = (int)sign_extend_u64(raw & 0xffu, 8);
        info->weak_import = ((raw >> 8) & 1u) != 0;
        name_offset = raw >> 9;
        if (imports_format == DYLD_CHAINED_IMPORT_ADDEND) {
            info->addend = sign_extend_u64(read_le32(entry + 4), 32);
        }
    } else {
        uint64_t raw = read_le64(entry);
        info->ordinal = (int)sign_extend_u64(raw & 0xffffu, 16);
        info->weak_import = ((raw >> 16) & 1u) != 0;
        name_offset = raw >> 32;
        info->addend = bits_to_i64(read_le64(entry + 8));
    }
    uint64_t symbol_offset = 0;
    if (!add_u64(symbols_offset, name_offset, &symbol_offset) ||
        symbol_offset >= payload_size) {
        r2_set_error(error, error_len, "chained import symbol offset is out of range");
        return R2_ERR_DECODE;
    }
    const uint8_t *symbol = payload + (size_t)symbol_offset;
    if (memchr(symbol, '\0', payload_size - (size_t)symbol_offset) == NULL ||
        symbol[0] == '\0') {
        r2_set_error(error, error_len, "invalid chained import symbol name");
        return R2_ERR_DECODE;
    }
    info->symbol = (const char *)symbol;
    return R2_OK;
}

static r2_status apply_chained_pointer_chain(r2_dyld_context *context,
                                             r2_dyld_image *image,
                                             size_t segment_index,
                                             uint64_t start_offset,
                                             uint16_t pointer_format,
                                             const uint8_t *payload,
                                             size_t payload_size,
                                             uint32_t imports_offset,
                                             uint32_t imports_count,
                                             uint32_t imports_format,
                                             uint32_t symbols_offset,
                                             uint64_t preferred_base,
                                             char *error,
                                             size_t error_len)
{
    if (segment_index >= image->segment_count) {
        r2_set_error(error, error_len, "chained fixup references an invalid segment");
        return R2_ERR_DECODE;
    }
    const r2_dyld_segment *segment = &image->segments[segment_index];
    uint64_t segment_offset = start_offset;
    uint64_t max_steps = segment->vmsize / 4u + 1u;
    for (uint64_t step = 0; step < max_steps; step += 1) {
        uint8_t *pointer = NULL;
        r2_status status = segment_pointer(context,
                                           image,
                                           segment_index,
                                           segment_offset,
                                           &pointer,
                                           error,
                                           error_len);
        if (status != R2_OK) {
            return status;
        }
        uint64_t raw = read_le64(pointer);
        uint64_t next = (raw >> 51) & 0xfffu;
        uint64_t value = 0;
        if ((raw >> 63) != 0) {
            if (((raw >> 32) & 0x7ffffu) != 0) {
                r2_set_error(error, error_len, "chained bind pointer has nonzero reserved bits");
                return R2_ERR_DECODE;
            }
            uint32_t import_index = (uint32_t)(raw & 0xffffffu);
            r2_chained_import_info import;
            status = parse_chained_import(payload,
                                          payload_size,
                                          imports_offset,
                                          imports_count,
                                          imports_format,
                                          symbols_offset,
                                          import_index,
                                          &import,
                                          error,
                                          error_len);
            if (status != R2_OK) {
                return status;
            }
            status = resolve_bind_symbol(context,
                                         image,
                                         import.ordinal,
                                         import.symbol,
                                         import.weak_import,
                                         &value,
                                         error,
                                         error_len);
            if (status == R2_OK) {
                status = add_signed_value(value, import.addend, &value, error, error_len);
            }
            if (status == R2_OK) {
                status = add_signed_value(value,
                                          (int64_t)((raw >> 24) & 0xffu),
                                          &value,
                                          error,
                                          error_len);
            }
            if (status != R2_OK) {
                return status;
            }
        } else {
            if (((raw >> 44) & 0x7fu) != 0) {
                r2_set_error(error, error_len, "chained rebase pointer has nonzero reserved bits");
                return R2_ERR_DECODE;
            }
            value = (raw & 0xfffffffffull) | (((raw >> 36) & 0xffu) << 56);
            if (pointer_format == DYLD_CHAINED_PTR_64_OFFSET) {
                uint64_t loaded_base = 0;
                if (!add_u64(preferred_base, image->slide, &loaded_base) ||
                    !add_u64(loaded_base, value, &value)) {
                    r2_set_error(error, error_len, "chained offset rebase overflows");
                    return R2_ERR_DECODE;
                }
            } else if (!add_u64(value, image->slide, &value)) {
                r2_set_error(error, error_len, "chained pointer rebase overflows");
                return R2_ERR_DECODE;
            }
        }
        write_le64(pointer, value);
        if (next == 0) {
            return R2_OK;
        }
        uint64_t advance = next * 4u;
        if (!add_u64(segment_offset, advance, &segment_offset)) {
            r2_set_error(error, error_len, "chained pointer offset overflows");
            return R2_ERR_DECODE;
        }
    }
    r2_set_error(error, error_len, "chained pointer list contains a cycle");
    return R2_ERR_DECODE;
}

static r2_status apply_chained_fixups(r2_dyld_context *context,
                                      r2_dyld_image *image,
                                      char *error,
                                      size_t error_len)
{
    if (image->chained_fixups_size == 0) {
        return R2_OK;
    }
    const uint8_t *payload = NULL;
    r2_status status = image_stream(image,
                                    image->chained_fixups_off,
                                    image->chained_fixups_size,
                                    &payload,
                                    error,
                                    error_len);
    if (status != R2_OK) {
        return status;
    }
    size_t payload_size = (size_t)image->chained_fixups_size;
    if (payload_size < 28) {
        r2_set_error(error, error_len, "truncated chained-fixups header");
        return R2_ERR_DECODE;
    }
    uint32_t version = read_le32(payload);
    uint32_t starts_offset = read_le32(payload + 4);
    uint32_t imports_offset = read_le32(payload + 8);
    uint32_t symbols_offset = read_le32(payload + 12);
    uint32_t imports_count = read_le32(payload + 16);
    uint32_t imports_format = read_le32(payload + 20);
    uint32_t symbols_format = read_le32(payload + 24);
    if (version != 0) {
        r2_set_error(error, error_len, "unsupported chained-fixups version %u", version);
        return R2_ERR_UNSUPPORTED;
    }
    if (symbols_format != 0) {
        r2_set_error(error, error_len, "compressed chained-fixup symbols are not supported");
        return R2_ERR_UNSUPPORTED;
    }
    if (imports_count > 1000000u || starts_offset >= payload_size ||
        imports_offset > payload_size || symbols_offset > payload_size ||
        !span_in_size(starts_offset, 4, payload_size)) {
        r2_set_error(error, error_len, "invalid chained-fixups header offsets");
        return R2_ERR_DECODE;
    }
    size_t import_size = imports_format == DYLD_CHAINED_IMPORT
                             ? 4u
                             : (imports_format == DYLD_CHAINED_IMPORT_ADDEND
                                    ? 8u
                                    : (imports_format == DYLD_CHAINED_IMPORT_ADDEND64 ? 16u : 0u));
    if (import_size == 0) {
        r2_set_error(error,
                     error_len,
                     "unsupported chained-import format %u",
                     imports_format);
        return R2_ERR_UNSUPPORTED;
    }
    if (!span_in_size(imports_offset,
                      (uint64_t)imports_count * import_size,
                      payload_size)) {
        r2_set_error(error, error_len, "chained import table exceeds fixups payload");
        return R2_ERR_DECODE;
    }

    uint64_t preferred_base = 0;
    if (!image_preferred_base(image, &preferred_base)) {
        r2_set_error(error, error_len, "chained-fixup image has no preferred base");
        return R2_ERR_DECODE;
    }
    const uint8_t *starts = payload + starts_offset;
    uint32_t segment_count = read_le32(starts);
    if (segment_count > image->segment_count ||
        !span_in_size((uint64_t)starts_offset + 4u,
                      (uint64_t)segment_count * 4u,
                      payload_size)) {
        r2_set_error(error, error_len, "invalid chained-fixup segment table");
        return R2_ERR_DECODE;
    }
    for (uint32_t segment_index = 0; segment_index < segment_count; segment_index += 1) {
        uint32_t info_relative = read_le32(starts + 4u + (size_t)segment_index * 4u);
        if (info_relative == 0) {
            continue;
        }
        uint64_t info_offset = (uint64_t)starts_offset + info_relative;
        if (!span_in_size(info_offset, 22, payload_size)) {
            r2_set_error(error, error_len, "truncated chained segment starts");
            return R2_ERR_DECODE;
        }
        const uint8_t *info = payload + (size_t)info_offset;
        uint32_t info_size = read_le32(info);
        uint16_t page_size = read_le16(info + 4);
        uint16_t pointer_format = read_le16(info + 6);
        uint64_t memory_segment_offset = read_le64(info + 8);
        uint16_t page_count = read_le16(info + 20);
        if (info_size < 22u + (uint32_t)page_count * 2u ||
            !span_in_size(info_offset, info_size, payload_size)) {
            r2_set_error(error, error_len, "invalid chained segment-starts size");
            return R2_ERR_DECODE;
        }
        if (page_size == 0 || (pointer_format != DYLD_CHAINED_PTR_64 &&
                               pointer_format != DYLD_CHAINED_PTR_64_OFFSET)) {
            r2_set_error(error,
                         error_len,
                         "unsupported chained pointer format %u",
                         pointer_format);
            return R2_ERR_UNSUPPORTED;
        }
        const r2_dyld_segment *segment = &image->segments[segment_index];
        if (segment->vmaddr < preferred_base ||
            segment->vmaddr - preferred_base != memory_segment_offset) {
            r2_set_error(error, error_len, "chained segment offset does not match Mach-O layout");
            return R2_ERR_DECODE;
        }
        size_t start_entry_count = (info_size - 22u) / 2u;
        for (uint16_t page_index = 0; page_index < page_count; page_index += 1) {
            uint16_t page_start = read_le16(info + 22u + (size_t)page_index * 2u);
            if (page_start == DYLD_CHAINED_PTR_START_NONE) {
                continue;
            }
            size_t list_index = 0;
            bool multiple = (page_start & DYLD_CHAINED_PTR_START_MULTI) != 0;
            if (multiple) {
                list_index = page_start & ~DYLD_CHAINED_PTR_START_MULTI;
            }
            for (;;) {
                uint16_t chain_start = page_start;
                bool last = true;
                if (multiple) {
                    if (list_index >= start_entry_count) {
                        r2_set_error(error, error_len, "chained multi-start index is out of range");
                        return R2_ERR_DECODE;
                    }
                    uint16_t entry = read_le16(info + 22u + list_index * 2u);
                    list_index += 1u;
                    last = (entry & DYLD_CHAINED_PTR_START_LAST) != 0;
                    chain_start = entry & ~DYLD_CHAINED_PTR_START_LAST;
                }
                uint64_t page_offset = (uint64_t)page_index * page_size;
                uint64_t start_offset = 0;
                if (chain_start >= page_size ||
                    !add_u64(page_offset, chain_start, &start_offset)) {
                    r2_set_error(error, error_len, "invalid chained pointer page start");
                    return R2_ERR_DECODE;
                }
                status = apply_chained_pointer_chain(context,
                                                     image,
                                                     segment_index,
                                                     start_offset,
                                                     pointer_format,
                                                     payload,
                                                     payload_size,
                                                     imports_offset,
                                                     imports_count,
                                                     imports_format,
                                                     symbols_offset,
                                                     preferred_base,
                                                     error,
                                                     error_len);
                if (status != R2_OK) {
                    return status;
                }
                if (last) {
                    break;
                }
            }
        }
    }
    return R2_OK;
}

static r2_status configure_tls(r2_dyld_context *context, r2_dyld_image *image, char *error, size_t error_len)
{
    if (image->tls_range_count == 0) return R2_OK;
    if (context->native == NULL || image->tls_initializers) {
        r2_set_error(error, error_len, "guest TLS initialization functions are not supported yet"); return R2_ERR_UNSUPPORTED;
    }
    uint64_t start = UINT64_MAX, end = 0;
    for (size_t i = 0; i < image->tls_part_count; i++) {
        uint64_t address = image->tls_parts[i].address, limit;
        if (!add_u64(address, image->tls_parts[i].size, &limit)) return R2_ERR_DECODE;
        if (address < start) start = address;
        if (limit > end) end = limit;
    }
    if (end <= start || end - start > 16 * 1024 * 1024) {
        r2_set_error(error, error_len, "invalid Mach-O TLS template span"); return R2_ERR_DECODE;
    }
    uint8_t *data = calloc(1, (size_t)(end - start));
    if (data == NULL) return R2_ERR_NO_MEMORY;
    for (size_t i = 0; i < image->tls_part_count; i++) {
        if (image->tls_parts[i].zero) continue;
        const r2_dyld_segment *segment = &image->segments[image->tls_parts[i].segment];
        if (!segment->mapped || segment->region_index >= context->region_count) {
            free(data); r2_set_error(error, error_len, "TLS template is not mapped"); return R2_ERR_DECODE;
        }
        memcpy(data + image->tls_parts[i].address - start,
               context->regions[segment->region_index].data + image->tls_parts[i].address - segment->vmaddr,
               (size_t)image->tls_parts[i].size);
    }
    r2_tls_range ranges[32];
    for (size_t i = 0; i < image->tls_range_count; i++) {
        const r2_dyld_segment *segment = &image->segments[image->tls_ranges[i].segment];
        if (!segment->mapped) { free(data); return R2_ERR_DECODE; }
        ranges[i].address = segment->vmaddr + image->slide + image->tls_ranges[i].offset;
        ranges[i].count = image->tls_ranges[i].count;
    }
    size_t alignment = image->tls_alignment < 16 ? 16 : image->tls_alignment;
    r2_status status = r2_native_add_tls(context->native, ranges, image->tls_range_count,
                                        data, (size_t)(end - start), alignment, error, error_len);
    free(data); return status;
}

static r2_status apply_fixups(r2_dyld_context *context, char *error, size_t error_len)
{
    for (size_t i = 0; i < context->image_count; i += 1) {
        if (context->images[i].fixups_applied) continue;
        r2_status status =
            apply_chained_fixups(context, &context->images[i], error, error_len);
        if (status != R2_OK) {
            return status;
        }
    }
    for (size_t i = 0; i < context->image_count; i += 1) {
        if (context->images[i].fixups_applied) continue;
        r2_status status = apply_rebase_stream(context, &context->images[i], error, error_len);
        if (status != R2_OK) {
            return status;
        }
    }
    for (size_t i = 0; i < context->image_count; i += 1) {
        r2_dyld_image *image = &context->images[i];
        if (image->fixups_applied) continue;
        r2_status status = apply_bind_stream(context,
                                             image,
                                             image->bind_off,
                                             image->bind_size,
                                             BIND_SPECIAL_DYLIB_SELF,
                                             false,
                                             false,
                                             error,
                                             error_len);
        if (status == R2_OK) {
            status = apply_bind_stream(context,
                                       image,
                                       image->lazy_bind_off,
                                       image->lazy_bind_size,
                                       BIND_SPECIAL_DYLIB_SELF,
                                       true,
                                       false,
                                       error,
                                       error_len);
        }
        if (status != R2_OK) {
            return status;
        }
    }
    for (size_t i = 0; i < context->image_count; i++) {
        r2_dyld_image *image = &context->images[i];
        if (image->fixups_applied) continue;
        r2_status status = apply_bind_stream(context, image, image->weak_bind_off, image->weak_bind_size,
                                             BIND_SPECIAL_DYLIB_WEAK_LOOKUP, false, true, error, error_len);
        if (status != R2_OK) return status;
        status = configure_tls(context, image, error, error_len);
        if (status != R2_OK) return status;
        image->fixups_applied = true;
    }
    return R2_OK;
}

static r2_status build_argv_region(const char *path,
                                   const char *const *argv,
                                   size_t argc,
                                   r2_memory_region *region,
                                   char *error,
                                   size_t error_len)
{
    const char *default_argv[1] = {path};
    if (argc == 0 || argv == NULL) {
        argv = default_argv;
        argc = 1;
    }
    if (argc > R2_MAX_GUEST_ARGC) {
        r2_set_error(error, error_len, "too many guest argv entries");
        return R2_ERR_UNSUPPORTED;
    }

    size_t pointer_bytes = (argc + 1u) * sizeof(uint64_t);
    size_t total = pointer_bytes;
    for (size_t i = 0; i < argc; i += 1) {
        if (argv[i] == NULL) {
            r2_set_error(error, error_len, "guest argv contains a null string");
            return R2_ERR_BAD_ARGUMENT;
        }
        size_t length = strlen(argv[i]) + 1u;
        if (length > R2_MAX_GUEST_ARGV_BYTES || total > R2_MAX_GUEST_ARGV_BYTES - length) {
            r2_set_error(error, error_len, "guest argv data is too large");
            return R2_ERR_UNSUPPORTED;
        }
        total += length;
    }

    uint8_t *data = calloc(1, total);
    if (data == NULL) {
        r2_set_error(error, error_len, "out of memory building guest argv");
        return R2_ERR_NO_MEMORY;
    }
    size_t string_offset = pointer_bytes;
    for (size_t i = 0; i < argc; i += 1) {
        write_le64(data + i * sizeof(uint64_t), R2_ARGV_BASE + string_offset);
        size_t length = strlen(argv[i]) + 1u;
        memcpy(data + string_offset, argv[i], length);
        string_offset += length;
    }
    write_le64(data + argc * sizeof(uint64_t), 0);
    region->base = R2_ARGV_BASE;
    region->size = total;
    region->data = data;
    region->writable = false;
    return R2_OK;
}

static r2_status decode_images(const r2_dyld_context *context,
                               r2_block *block,
                               char *error,
                               size_t error_len)
{
    memset(block, 0, sizeof(*block));
    const r2_dyld_image *main_image = &context->images[0];
    bool entry_in_code = false;
    for (size_t i = 0; i < main_image->code_range_count; i += 1) {
        const r2_dyld_code_range *code = &main_image->code_ranges[i];
        if (main_image->entry_vmaddr < code->vmaddr ||
            main_image->entry_vmaddr - code->vmaddr >= code->size) {
            continue;
        }
        entry_in_code = true;
        break;
    }
    if (!entry_in_code) {
        r2_set_error(error, error_len, "LC_MAIN entry point is outside __TEXT,__text");
        return R2_ERR_DECODE;
    }

    for (size_t image_index = 0; image_index < context->image_count; image_index += 1) {
        const r2_dyld_image *image = &context->images[image_index];
        for (size_t code_index = 0; code_index < image->code_range_count; code_index += 1) {
            const r2_dyld_code_range *code = &image->code_ranges[code_index];
            if (code->size > SIZE_MAX) {
                r2_set_error(error, error_len, "Mach-O code section is too large");
                return R2_ERR_UNSUPPORTED;
            }
            uint64_t guest_base = 0;
            if (!add_u64(code->vmaddr, image->slide, &guest_base)) {
                r2_set_error(error, error_len, "Mach-O code address overflows");
                return R2_ERR_DECODE;
            }
            r2_status status = r2_decode_block_append_at(
                image->file_data + image->slice_offset + code->fileoff,
                (size_t)code->size,
                guest_base,
                block,
                error,
                error_len);
            if (status != R2_OK) {
                return status;
            }
        }
    }
    return R2_OK;
}

static r2_status initialize_image(r2_dyld_context *context, size_t index,
                                   uint8_t states[R2_DYLD_MAX_IMAGES],
                                   r2_block *block, const r2_interp_env *environment,
                                   char *error, size_t error_len)
{
    if (states[index] != 0) return R2_OK; /* Includes dependency cycles. */
    states[index] = 1;
    r2_dyld_image *image = &context->images[index];
    for (size_t i = 0; i < image->dependency_count; i++) {
        const r2_dyld_dependency *dependency = &image->dependencies[i];
        if (dependency->missing || dependency->image_index == SIZE_MAX) continue;
        r2_status status = initialize_image(context, dependency->image_index, states,
                                            block, environment, error, error_len);
        if (status != R2_OK) return status;
        if (environment->process != NULL && environment->process->kind != R2_PROCESS_RUNNING) return R2_OK;
    }
    for (size_t i = 0; i < image->initializer_count; i++) {
        for (uint64_t j = 0; j < image->initializers[i].count; j++) {
            uint8_t *pointer;
            r2_status status = segment_pointer(context, image, image->initializers[i].segment,
                image->initializers[i].offset + j * 8, &pointer, error, error_len);
            if (status != R2_OK) return status;
            r2_interp_env env = *environment;
            env.initial_rip = read_le64(pointer);
            r2_result ignored = {0};
            status = r2_interp_block_with_env(block, &env, &ignored, error, error_len);
            if (status != R2_OK) {
                char detail[R2_ERROR_CAP];
                snprintf(detail, sizeof(detail), "%s", error);
                r2_set_error(error, error_len, "initializer in %s: %s", image->path, detail);
                return status;
            }
            if (environment->process != NULL && environment->process->kind != R2_PROCESS_RUNNING) return R2_OK;
        }
    }
    states[index] = 2;
    return R2_OK;
}

static bool dlopen_search_list(const char *list, const char *leaf, char out[PATH_MAX])
{
    if (!list) return false;
    for (const char *part = list;;) {
        const char *end = strchr(part, ':'); size_t length = end ? (size_t)(end - part) : strlen(part);
        char candidate[PATH_MAX];
        int count = length ? snprintf(candidate, sizeof(candidate), "%.*s/%s", (int)length, part, leaf)
                           : snprintf(candidate, sizeof(candidate), "./%s", leaf);
        if (count > 0 && (size_t)count < sizeof(candidate) && realpath(candidate, out)) return true;
        if (!end) return false;
        part = end + 1;
    }
}

static r2_status resolve_dlopen_path(const r2_dyld_context *context, size_t loader,
                                      const char *path, char out[PATH_MAX], char *error, size_t error_len)
{
    if (path[0] == '@') {
        char expanded[PATH_MAX];
        r2_status status = resolve_dependency_path(context, loader, path, expanded, sizeof(expanded), error, error_len);
        if (status != R2_OK) return status;
        if (realpath(expanded, out)) return R2_OK;
    } else {
        const char *slash = strrchr(path, '/'), *leaf = slash ? slash + 1 : path;
        if ((!slash && dlopen_search_list(getenv("LD_LIBRARY_PATH"), leaf, out)) ||
            dlopen_search_list(getenv("DYLD_LIBRARY_PATH"), leaf, out) || realpath(path, out)) return R2_OK;
        const char *fallback = getenv("DYLD_FALLBACK_LIBRARY_PATH");
        if (fallback) {
            if (dlopen_search_list(fallback, leaf, out)) return R2_OK;
        } else {
            const char *home = getenv("HOME"); char home_lib[PATH_MAX];
            int length = home ? snprintf(home_lib, sizeof(home_lib), "%s/lib", home) : -1;
            if ((length > 0 && (size_t)length < sizeof(home_lib) && dlopen_search_list(home_lib, leaf, out)) ||
                dlopen_search_list("/usr/local/lib:/usr/lib", leaf, out)) return R2_OK;
        }
    }
    r2_set_error(error, error_len, "dlopen image not found: %s", path); return R2_ERR_IO;
}

static r2_status native_open_image(void *opaque, const char *path, int flags, uint64_t caller,
                                    uint64_t *handle, char *error, size_t error_len)
{
    r2_dyld_context *context = opaque;
    r2_native_execution execution;
    bool active = r2_native_current_execution(context->native, &execution);
    int supported = RTLD_LAZY | RTLD_NOW | RTLD_LOCAL | RTLD_GLOBAL | RTLD_NODELETE | RTLD_NOLOAD | RTLD_FIRST;
    if ((flags & ~supported) != 0 || !active) {
        r2_set_error(error, error_len, "unsupported guest dlopen flags or context"); return R2_ERR_UNSUPPORTED;
    }
    size_t index = 0, old_count = context->image_count, old_regions = context->region_count;
    r2_status status = R2_OK;
    bool global = (flags & RTLD_LOCAL) == 0 || (flags & RTLD_GLOBAL) != 0;
    if (path[0] != 0) {
        size_t loader = 0;
        r2_dl_info caller_info;
        if (native_lookup_address(context, caller, &caller_info))
            loader = find_loaded_image(context, caller_info.filename);
        char canonical[PATH_MAX];
        status = resolve_dlopen_path(context, loader, path, canonical, error, error_len);
        if (status != R2_OK) return status;
        index = find_loaded_image(context, canonical);
        if (index == SIZE_MAX && (flags & RTLD_NOLOAD)) {
            r2_set_error(error, error_len, "dlopen image is not loaded: %s", canonical); return R2_ERR_IO;
        }
        if (index == SIZE_MAX) {
            status = load_image(context, canonical, false, loader, &index, error, error_len);
            if (status == R2_OK) status = load_image_dependencies(context, index, error, error_len);
            for (size_t i = old_count; i < context->image_count; i++) context->images[i].globally_visible = global;
            if (status == R2_OK) status = assign_image_slides(context, error, error_len);
            if (status == R2_OK) status = map_images(context, error, error_len);
            if (status == R2_OK) status = apply_fixups(context, error, error_len);
            if (status != R2_OK) {
                /* No guest code has run and no code ranges were published. */
                for (size_t i = old_count; i < context->image_count; i++) {
                    free(context->images[i].file_data); free(context->images[i].native_data);
                    memset(&context->images[i], 0, sizeof(context->images[i]));
                }
                context->image_count = old_count; context->region_count = old_regions;
                return status;
            }
            for (size_t i = old_count; i < context->image_count; i++) {
                const r2_dyld_image *image = &context->images[i];
                for (size_t j = 0; j < image->code_range_count; j++)
                    r2_native_add_code(context->native, image->code_ranges[j].vmaddr + image->slide, image->code_ranges[j].size);
            }
            status = initialize_image(context, index, context->initialization_states, execution.block,
                                        execution.env, error, error_len);
            if (status != R2_OK) return status;
        }
    }
    if (global) {
        uint32_t pending = UINT32_C(1) << index, visited = 0;
        while (pending) {
            unsigned current = (unsigned)__builtin_ctz(pending);
            pending &= pending - 1; visited |= UINT32_C(1) << current;
            context->images[current].globally_visible = true;
            const r2_dyld_image *image = &context->images[current];
            for (size_t j = 0; j < image->dependency_count; j++) {
                size_t next = image->dependencies[j].image_index;
                if (!image->dependencies[j].missing && next < context->image_count && !(visited & (UINT32_C(1) << next)))
                    pending |= UINT32_C(1) << next;
            }
        }
    }
    context->images[index].open_count++;
    *handle = (uintptr_t)&context->images[index] | ((flags & RTLD_FIRST) ? UINT64_C(1) : 0);
    return R2_OK;
}

static bool native_close_image(void *opaque, uint64_t handle)
{
    r2_dyld_context *context = opaque;
    handle &= ~UINT64_C(1);
    for (size_t i = 0; i < context->image_count; i++) {
        if (handle == (uintptr_t)&context->images[i] && context->images[i].open_count != 0) {
            context->images[i].open_count--;
            /* Images remain mapped until process exit. Guest callbacks, TLS,
             * cached instructions and native handles must not outlive code. */
            return true;
        }
    }
    return false;
}

static size_t native_image_count(void *opaque)
{
    return ((const r2_dyld_context *)opaque)->image_count;
}
static const char *native_image_name(void *opaque, size_t index)
{
    const r2_dyld_context *context = opaque;
    return index < context->image_count ? context->images[index].path : NULL;
}

static const void *native_image_header(void *opaque,size_t index)
{
    const r2_dyld_context *context=opaque;if(index>=context->image_count)return NULL;
    const r2_dyld_image *image=&context->images[index];
    for(size_t i=0;i<image->segment_count;i++) {
        const r2_dyld_segment *segment=&image->segments[i];
        if(segment->mapped&&segment->fileoff==0&&segment->filesize>=32)
            return (const void *)(uintptr_t)(segment->vmaddr+image->slide);
    }
    return NULL;
}

static void destroy_context(r2_dyld_context *context)
{
    r2_native_stop_and_join(context->native);
    for (size_t i = 0; i < context->region_count; i += 1) {
        if (context->native == NULL) free(context->regions[i].data);
    }
    for (size_t i = 0; i < context->image_count; i += 1) {
        free(context->images[i].file_data);
        free(context->images[i].native_data);
    }
    r2_native_destroy(context->native);
    if (context->native != NULL) free(context->argv_data);
    free(context);
}

r2_status r2_dyld_run_macho_file(const char *path,
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

    r2_dyld_context *context = calloc(1, sizeof(*context));
    if (context == NULL) {
        r2_set_error(error, error_len, "out of memory creating dyld context");
        return R2_ERR_NO_MEMORY;
    }
    context->cpp_image_index = SIZE_MAX;
    if (host_libraries) {
        context->native = r2_native_create(error, error_len);
        if (context->native == NULL) { free(context); return R2_ERR_UNSUPPORTED; }
    }
    char executable_path[PATH_MAX];
    if (!absolute_path(path,
                       executable_path,
                       sizeof(executable_path),
                       error,
                       error_len) ||
        !path_dirname(executable_path,
                      context->executable_dir,
                      sizeof(context->executable_dir),
                      error,
                      error_len)) {
        destroy_context(context);
        return R2_ERR_BAD_ARGUMENT;
    }

    size_t main_index = SIZE_MAX;
    r2_status status = load_image(context,
                                  executable_path,
                                  true,
                                  SIZE_MAX,
                                  &main_index,
                                  error,
                                  error_len);
    if (status != R2_OK) {
        destroy_context(context);
        return status;
    }
    if (main_index != 0 || !context->images[0].has_entry ||
        context->images[0].entry_vmaddr == 0) {
        r2_set_error(error, error_len, "x86_64 executable has no usable LC_MAIN entry point");
        destroy_context(context);
        return R2_ERR_DECODE;
    }
    status = load_image_dependencies(context, 0, error, error_len);
    const char *cpp_path = getenv("R2_GUEST_CXX_ABI");
    if (status == R2_OK && context->native != NULL && cpp_path != NULL) {
        status = load_image(context, cpp_path, false, 0, &context->cpp_image_index, error, error_len);
        if (status == R2_OK) status = load_image_dependencies(context, context->cpp_image_index, error, error_len);
    }
    if (status == R2_OK) {
        status = assign_image_slides(context, error, error_len);
    }
    if (status == R2_OK) {
        status = map_images(context, error, error_len);
    }
    if (status == R2_OK) {
        status = apply_fixups(context, error, error_len);
    }
    if (status != R2_OK) {
        destroy_context(context);
        return status;
    }

    if (context->region_count >= R2_DYLD_MAX_REGIONS) {
        r2_set_error(error, error_len, "too many mapped Mach-O regions");
        destroy_context(context);
        return R2_ERR_UNSUPPORTED;
    }
    const char *default_argv[1] = {path};
    const char *const *guest_argv = argv;
    size_t guest_argc = argc;
    if (guest_argv == NULL || guest_argc == 0) {
        guest_argv = default_argv;
        guest_argc = 1;
    }
    status = build_argv_region(path,
                               guest_argv,
                               guest_argc,
                               &context->regions[context->region_count],
                               error,
                               error_len);
    if (status != R2_OK) {
        destroy_context(context);
        return status;
    }
    context->region_count += 1;
    context->argv_data = context->regions[context->region_count - 1u].data;
    if (context->native != NULL) {
        r2_memory_region *arguments = &context->regions[context->region_count - 1u];
        uint64_t new_base = (uintptr_t)arguments->data;
        for (size_t i = 0; i < guest_argc; i += 1) {
            uint64_t old = read_le64(arguments->data + i * 8u);
            write_le64(arguments->data + i * 8u, old - arguments->base + new_base);
        }
        arguments->base = new_base;
        for (size_t i = 0; i < context->image_count; i += 1) {
            r2_dyld_image *image = &context->images[i];
            for (size_t j = 0; j < image->code_range_count; j += 1) {
                r2_native_add_code(context->native, image->code_ranges[j].vmaddr + image->slide,
                                   image->code_ranges[j].size);
            }
        }
    }

    r2_block *block = malloc(sizeof(*block));
    if (block == NULL) {
        r2_set_error(error, error_len, "out of memory decoding loaded images");
        destroy_context(context);
        return R2_ERR_NO_MEMORY;
    }
    if (context->native != NULL) {
        memset(block, 0, sizeof(*block));
        block->insns = block->storage;
        block->capacity = R2_MAX_INSNS;
        block->runtime = context->native;
        r2_native_set_images(context->native, native_image_count, native_image_name,native_image_header);
        r2_native_set_regions(context->native, context->regions, &context->region_count);
        r2_native_set_lookup(context->native, context, native_lookup_symbol, native_lookup_address,
                              native_open_image, native_close_image);
    } else {
        status = decode_images(context, block, error, error_len);
    }
    r2_process_state process = {0}, callback_process = {0};
    if (status == R2_OK) {
        uint64_t initial_rip = 0;
        if (!add_u64(context->images[0].entry_vmaddr,
                     context->images[0].slide,
                     &initial_rip)) {
            r2_set_error(error, error_len, "executable entry address overflows");
            status = R2_ERR_DECODE;
        }
        r2_interp_env env = {
            .rip_base = 0,
            .has_initial_rip = true,
            .initial_rip = initial_rip,
            .regions = context->regions,
            .region_count = context->region_count,
            .dynamic_region_count = &context->region_count,
            .process = &process,
            .has_initial_regs = true,
            .native = context->native,
        };
        if (status == R2_OK) {
            env.initial_regs[R2_X86_RDI] = guest_argc;
            env.initial_regs[R2_X86_RSI] = context->regions[context->region_count - 1u].base;
            env.initial_regs[R2_X86_RDX] = 0;
            context->active_block = block; context->active_environment = &env;
            status = initialize_image(context, 0, context->initialization_states, block, &env, error, error_len);
            if (status == R2_OK && process.kind == R2_PROCESS_RUNNING)
                status = r2_interp_block_with_env(block, &env, result, error, error_len);
            if (process.kind != R2_PROCESS_RUNNING) result->rax = process.code;
            uint64_t function, argument;
            while (status == R2_OK && process.kind != R2_PROCESS_IMMEDIATE &&
                   r2_native_pop_exit(context->native, &function, &argument)) {
                r2_interp_env callback = env;
                callback_process = (r2_process_state){0};
                callback.process = &callback_process;
                memset(callback.initial_regs, 0, sizeof(callback.initial_regs));
                callback.initial_regs[R2_X86_RDI] = argument;
                callback.initial_rip = function;
                r2_result ignored = {0};
                status = r2_interp_block_with_env(block, &callback, &ignored, error, error_len);
                if (callback_process.kind != R2_PROCESS_RUNNING) {
                    process = callback_process; result->rax = process.code;
                }
            }
        }
    }
    if (block->insns != NULL && block->insns != block->storage) free(block->insns);
    free(block);
    destroy_context(context);
    return status;
}
