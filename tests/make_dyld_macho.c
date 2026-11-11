#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    MH_MAGIC_64 = 0xfeedfacf,
    CPU_TYPE_X86_64 = 0x01000007,
    CPU_SUBTYPE_X86_64_ALL = 3,
    MH_EXECUTE = 2,
    MH_DYLIB = 6,
    MH_DYLDLINK = 0x4,
    MH_TWOLEVEL = 0x80,
    LC_SEGMENT_64 = 0x19,
    LC_SYMTAB = 0x2,
    LC_LOAD_DYLIB = 0x0c,
    LC_ID_DYLIB = 0x0d,
    LC_LOAD_WEAK_DYLIB = 0x80000018u,
    LC_DYLD_INFO_ONLY = 0x80000022u,
    LC_MAIN = 0x80000028u,
    VM_PROT_READ = 1,
    VM_PROT_WRITE = 2,
    VM_PROT_EXECUTE = 4,
    S_ATTR_PURE_INSTRUCTIONS = 0x80000000u,
    S_ATTR_SOME_INSTRUCTIONS = 0x00000400u,
    N_SECT = 0x0e,
    N_EXT = 0x01,
    HEADER_SIZE = 32,
    SEGMENT_COMMAND_64_SIZE = 72,
    SECTION_64_SIZE = 80,
    ENTRY_POINT_COMMAND_SIZE = 24,
    SYMTAB_COMMAND_SIZE = 24,
    DYLD_INFO_COMMAND_SIZE = 48,
    TEXT_FILE_SIZE = 0x1000,
    CODE_FILE_OFFSET = 0x800,
    DATA_FILE_OFFSET = 0x1000,
    DATA_FILE_SIZE = 0x1000,
    LINKEDIT_FILE_OFFSET = 0x2000,
    PAGE_SIZE = 0x1000,
};

typedef struct {
    const char *output;
    uint32_t filetype;
    uint64_t vmaddr;
    const uint8_t *code;
    size_t code_size;
    size_t entry_offset;
    const char *id_name;
    uint32_t dependency_command;
    const char *dependency_name;
    const char *bind_symbol;
    const char *export_symbol;
    const uint8_t *data;
    size_t data_size;
    bool rebase_data_pointer;
    unsigned bind_backwards; /* 1: valid negative delta, 2: outside segment */
} image_spec;

static void put_u16(uint8_t *buf, size_t off, uint16_t value)
{
    buf[off + 0] = (uint8_t)(value & 0xffu);
    buf[off + 1] = (uint8_t)((value >> 8) & 0xffu);
}

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

static size_t align_u64(size_t value)
{
    return (value + 7u) & ~(size_t)7u;
}

static uint32_t dylib_command_size(const char *name)
{
    return (uint32_t)align_u64(24u + strlen(name) + 1u);
}

static void put_dylib_command(uint8_t *buf,
                              size_t off,
                              uint32_t command,
                              const char *name)
{
    uint32_t command_size = dylib_command_size(name);
    put_u32(buf, off + 0, command);
    put_u32(buf, off + 4, command_size);
    put_u32(buf, off + 8, 24);
    put_u32(buf, off + 12, 0);
    put_u32(buf, off + 16, 0x00010000u);
    put_u32(buf, off + 20, 0x00010000u);
    memcpy(buf + off + 24, name, strlen(name) + 1u);
}

static bool write_file(const char *path, const uint8_t *buf, size_t len)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        fprintf(stderr, "could not open %s for writing\n", path);
        return false;
    }

    bool ok = fwrite(buf, 1, len, file) == len;
    if (fclose(file) != 0) {
        ok = false;
    }
    if (!ok) {
        fprintf(stderr, "could not write %s\n", path);
    }
    return ok;
}

static bool build_image(const image_spec *spec)
{
    bool has_data = spec->bind_symbol != NULL || spec->data_size != 0;
    bool has_symbols = spec->export_symbol != NULL;
    bool has_dyld_info = spec->bind_symbol != NULL || spec->rebase_data_pointer;
    bool has_linkedit = has_symbols || has_dyld_info;

    uint8_t bind_stream[256] = {0};
    size_t bind_size = 0;
    if (spec->bind_symbol != NULL) {
        size_t symbol_size = strlen(spec->bind_symbol) + 1u;
        if (symbol_size > sizeof(bind_stream) - 24u) {
            fprintf(stderr, "bind symbol is too long\n");
            return false;
        }
        bind_stream[bind_size++] = 0x11; /* SET_DYLIB_ORDINAL_IMM(1) */
        bind_stream[bind_size++] = 0x40; /* SET_SYMBOL_TRAILING_FLAGS_IMM(0) */
        memcpy(bind_stream + bind_size, spec->bind_symbol, symbol_size);
        bind_size += symbol_size;
        bind_stream[bind_size++] = 0x71; /* SET_SEGMENT_AND_OFFSET_ULEB(__DATA) */
        bind_stream[bind_size++] = spec->bind_backwards ? 0x08 : 0x00;
        bind_stream[bind_size++] = 0x90; /* DO_BIND */
        if (spec->bind_backwards) {
            bind_stream[bind_size++] = 0x80; /* ADD_ADDR_ULEB, from offset 16 */
            uint64_t delta = spec->bind_backwards == 1 ? UINT64_MAX - 15 : UINT64_MAX - 8191;
            do {
                uint8_t byte = (uint8_t)(delta & 0x7f);
                delta >>= 7;
                bind_stream[bind_size++] = byte | (delta ? 0x80 : 0);
            } while (delta);
            bind_stream[bind_size++] = 0x90; /* DO_BIND at offset zero */
        }
        bind_stream[bind_size++] = 0x00; /* DONE */
    }

    const uint8_t rebase_stream[] = {
        0x21, /* SET_SEGMENT_AND_OFFSET_ULEB(__DATA) */
        0x00, /* __DATA offset zero */
        0x51, /* DO_REBASE_IMM_TIMES(1) */
        0x00, /* DONE */
    };

    uint32_t symoff = 0;
    uint32_t stroff = 0;
    uint32_t strsize = 0;
    uint32_t rebase_off = 0;
    uint32_t bind_off = 0;
    size_t linkedit_cursor = LINKEDIT_FILE_OFFSET;
    if (has_symbols) {
        symoff = (uint32_t)linkedit_cursor;
        linkedit_cursor += 16;
        stroff = (uint32_t)linkedit_cursor;
        strsize = (uint32_t)(1u + strlen(spec->export_symbol) + 1u);
        linkedit_cursor += strsize;
        linkedit_cursor = align_u64(linkedit_cursor);
    }
    if (spec->rebase_data_pointer) {
        rebase_off = (uint32_t)linkedit_cursor;
        linkedit_cursor += sizeof(rebase_stream);
    }
    if (spec->bind_symbol != NULL) {
        bind_off = (uint32_t)linkedit_cursor;
        linkedit_cursor += bind_size;
    }

    size_t file_size = TEXT_FILE_SIZE;
    if (has_data) {
        file_size = DATA_FILE_OFFSET + DATA_FILE_SIZE;
    }
    if (has_linkedit) {
        file_size = linkedit_cursor;
    }

    uint32_t ncmds = 1; /* __TEXT */
    uint32_t sizeofcmds = SEGMENT_COMMAND_64_SIZE + SECTION_64_SIZE;
    if (has_data) {
        ncmds += 1;
        sizeofcmds += SEGMENT_COMMAND_64_SIZE + SECTION_64_SIZE;
    }
    if (has_linkedit) {
        ncmds += 1;
        sizeofcmds += SEGMENT_COMMAND_64_SIZE;
    }
    if (spec->filetype == MH_EXECUTE) {
        ncmds += 1;
        sizeofcmds += ENTRY_POINT_COMMAND_SIZE;
    } else {
        ncmds += 1;
        sizeofcmds += dylib_command_size(spec->id_name);
    }
    if (spec->dependency_name != NULL) {
        ncmds += 1;
        sizeofcmds += dylib_command_size(spec->dependency_name);
    }
    if (has_symbols) {
        ncmds += 1;
        sizeofcmds += SYMTAB_COMMAND_SIZE;
    }
    if (has_dyld_info) {
        ncmds += 1;
        sizeofcmds += DYLD_INFO_COMMAND_SIZE;
    }
    if ((uint64_t)HEADER_SIZE + sizeofcmds > CODE_FILE_OFFSET) {
        fprintf(stderr, "load commands do not fit before synthetic __text\n");
        return false;
    }
    if (spec->code_size == 0 || CODE_FILE_OFFSET + spec->code_size > TEXT_FILE_SIZE) {
        fprintf(stderr, "synthetic __text is too large\n");
        return false;
    }
    if (spec->filetype == MH_EXECUTE && spec->entry_offset >= spec->code_size) {
        fprintf(stderr, "synthetic entry point is outside __text\n");
        return false;
    }
    if (spec->data_size > DATA_FILE_SIZE) {
        fprintf(stderr, "synthetic __DATA is too large\n");
        return false;
    }

    uint8_t *buf = calloc(1, file_size);
    if (buf == NULL) {
        return false;
    }

    put_u32(buf, 0, MH_MAGIC_64);
    put_u32(buf, 4, CPU_TYPE_X86_64);
    put_u32(buf, 8, CPU_SUBTYPE_X86_64_ALL);
    put_u32(buf, 12, spec->filetype);
    put_u32(buf, 16, ncmds);
    put_u32(buf, 20, sizeofcmds);
    put_u32(buf, 24, MH_DYLDLINK | MH_TWOLEVEL);
    put_u32(buf, 28, 0);

    size_t off = HEADER_SIZE;
    put_u32(buf, off + 0, LC_SEGMENT_64);
    put_u32(buf, off + 4, SEGMENT_COMMAND_64_SIZE + SECTION_64_SIZE);
    put_name16(buf, off + 8, "__TEXT");
    put_u64(buf, off + 24, spec->vmaddr);
    put_u64(buf, off + 32, PAGE_SIZE);
    put_u64(buf, off + 40, 0);
    put_u64(buf, off + 48, TEXT_FILE_SIZE);
    put_u32(buf, off + 56, VM_PROT_READ | VM_PROT_EXECUTE);
    put_u32(buf, off + 60, VM_PROT_READ | VM_PROT_EXECUTE);
    put_u32(buf, off + 64, 1);
    put_u32(buf, off + 68, 0);

    off += SEGMENT_COMMAND_64_SIZE;
    put_name16(buf, off + 0, "__text");
    put_name16(buf, off + 16, "__TEXT");
    put_u64(buf, off + 32, spec->vmaddr + CODE_FILE_OFFSET);
    put_u64(buf, off + 40, spec->code_size);
    put_u32(buf, off + 48, CODE_FILE_OFFSET);
    put_u32(buf, off + 52, 0);
    put_u32(buf, off + 56, 0);
    put_u32(buf, off + 60, 0);
    put_u32(buf, off + 64, S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS);
    put_u32(buf, off + 68, 0);

    off += SECTION_64_SIZE;
    if (has_data) {
        size_t data_section_size = spec->data_size != 0 ? spec->data_size : 8u;
        put_u32(buf, off + 0, LC_SEGMENT_64);
        put_u32(buf, off + 4, SEGMENT_COMMAND_64_SIZE + SECTION_64_SIZE);
        put_name16(buf, off + 8, "__DATA");
        put_u64(buf, off + 24, spec->vmaddr + DATA_FILE_OFFSET);
        put_u64(buf, off + 32, PAGE_SIZE);
        put_u64(buf, off + 40, DATA_FILE_OFFSET);
        put_u64(buf, off + 48, DATA_FILE_SIZE);
        put_u32(buf, off + 56, VM_PROT_READ | VM_PROT_WRITE);
        put_u32(buf, off + 60, VM_PROT_READ | VM_PROT_WRITE);
        put_u32(buf, off + 64, 1);
        put_u32(buf, off + 68, 0);

        off += SEGMENT_COMMAND_64_SIZE;
        put_name16(buf, off + 0, spec->bind_symbol != NULL ? "__got" : "__data");
        put_name16(buf, off + 16, "__DATA");
        put_u64(buf, off + 32, spec->vmaddr + DATA_FILE_OFFSET);
        put_u64(buf, off + 40, data_section_size);
        put_u32(buf, off + 48, DATA_FILE_OFFSET);
        put_u32(buf, off + 52, 3);
        put_u32(buf, off + 56, 0);
        put_u32(buf, off + 60, 0);
        put_u32(buf, off + 64, 0);
        put_u32(buf, off + 68, 0);
        off += SECTION_64_SIZE;
    }

    if (has_linkedit) {
        put_u32(buf, off + 0, LC_SEGMENT_64);
        put_u32(buf, off + 4, SEGMENT_COMMAND_64_SIZE);
        put_name16(buf, off + 8, "__LINKEDIT");
        put_u64(buf, off + 24, spec->vmaddr + LINKEDIT_FILE_OFFSET);
        put_u64(buf, off + 32, PAGE_SIZE);
        put_u64(buf, off + 40, LINKEDIT_FILE_OFFSET);
        put_u64(buf, off + 48, linkedit_cursor - LINKEDIT_FILE_OFFSET);
        put_u32(buf, off + 56, VM_PROT_READ);
        put_u32(buf, off + 60, VM_PROT_READ);
        put_u32(buf, off + 64, 0);
        put_u32(buf, off + 68, 0);
        off += SEGMENT_COMMAND_64_SIZE;
    }

    if (spec->filetype == MH_EXECUTE) {
        put_u32(buf, off + 0, LC_MAIN);
        put_u32(buf, off + 4, ENTRY_POINT_COMMAND_SIZE);
        put_u64(buf, off + 8, CODE_FILE_OFFSET + spec->entry_offset);
        put_u64(buf, off + 16, 0);
        off += ENTRY_POINT_COMMAND_SIZE;
    } else {
        put_dylib_command(buf, off, LC_ID_DYLIB, spec->id_name);
        off += dylib_command_size(spec->id_name);
    }

    if (spec->dependency_name != NULL) {
        put_dylib_command(buf, off, spec->dependency_command, spec->dependency_name);
        off += dylib_command_size(spec->dependency_name);
    }

    if (has_symbols) {
        put_u32(buf, off + 0, LC_SYMTAB);
        put_u32(buf, off + 4, SYMTAB_COMMAND_SIZE);
        put_u32(buf, off + 8, symoff);
        put_u32(buf, off + 12, 1);
        put_u32(buf, off + 16, stroff);
        put_u32(buf, off + 20, strsize);
        off += SYMTAB_COMMAND_SIZE;
    }

    if (has_dyld_info) {
        put_u32(buf, off + 0, LC_DYLD_INFO_ONLY);
        put_u32(buf, off + 4, DYLD_INFO_COMMAND_SIZE);
        put_u32(buf, off + 8, rebase_off);
        put_u32(buf, off + 12, spec->rebase_data_pointer ? sizeof(rebase_stream) : 0);
        put_u32(buf, off + 16, bind_off);
        put_u32(buf, off + 20, (uint32_t)bind_size);
        put_u32(buf, off + 24, 0);
        put_u32(buf, off + 28, 0);
        put_u32(buf, off + 32, 0);
        put_u32(buf, off + 36, 0);
        put_u32(buf, off + 40, 0);
        put_u32(buf, off + 44, 0);
        off += DYLD_INFO_COMMAND_SIZE;
    }

    if (off != HEADER_SIZE + sizeofcmds) {
        fprintf(stderr, "internal load-command size mismatch\n");
        free(buf);
        return false;
    }

    memcpy(buf + CODE_FILE_OFFSET, spec->code, spec->code_size);
    if (spec->data_size != 0) {
        memcpy(buf + DATA_FILE_OFFSET, spec->data, spec->data_size);
    }
    if (has_symbols) {
        put_u32(buf, symoff + 0, 1);
        buf[symoff + 4] = N_SECT | N_EXT;
        buf[symoff + 5] = 1; /* __TEXT,__text is section 1 */
        put_u16(buf, symoff + 6, 0);
        put_u64(buf, symoff + 8, spec->vmaddr + CODE_FILE_OFFSET);
        buf[stroff] = 0;
        memcpy(buf + stroff + 1, spec->export_symbol, strlen(spec->export_symbol) + 1u);
    }
    if (spec->rebase_data_pointer) {
        memcpy(buf + rebase_off, rebase_stream, sizeof(rebase_stream));
    }
    if (spec->bind_symbol != NULL) {
        memcpy(buf + bind_off, bind_stream, bind_size);
    }

    bool ok = write_file(spec->output, buf, file_size);
    free(buf);
    return ok;
}

static void build_indirect_call_code(uint8_t code[7], uint64_t vmaddr)
{
    uint64_t instruction_end = vmaddr + CODE_FILE_OFFSET + 6u;
    uint64_t got_addr = vmaddr + DATA_FILE_OFFSET;
    code[0] = 0xff;
    code[1] = 0x15; /* callq *__got(%rip) */
    put_i32(code, 2, (int32_t)(got_addr - instruction_end));
    code[6] = 0xc3;
}

static void build_rebase_reader_code(uint8_t code[11], uint64_t vmaddr)
{
    uint64_t instruction_end = vmaddr + CODE_FILE_OFFSET + 7u;
    uint64_t pointer_addr = vmaddr + DATA_FILE_OFFSET;
    code[0] = 0x48;
    code[1] = 0x8b;
    code[2] = 0x05; /* movq pointer(%rip), %rax */
    put_i32(code, 3, (int32_t)(pointer_addr - instruction_end));
    code[7] = 0x48;
    code[8] = 0x8b;
    code[9] = 0x00; /* movq (%rax), %rax */
    code[10] = 0xc3;
}

static bool make_bind_fixture(const char *executable, const char *dylib, unsigned backwards)
{
    const uint64_t executable_vmaddr = 0x100000000ull;
    const uint64_t dylib_vmaddr = 0x200000000ull;
    uint8_t main_code[7] = {0};
    build_indirect_call_code(main_code, executable_vmaddr);
    const uint8_t dylib_code[] = {0xb8, 0x2a, 0x00, 0x00, 0x00, 0xc3};

    const image_spec dylib_spec = {
        .output = dylib,
        .filetype = MH_DYLIB,
        .vmaddr = dylib_vmaddr,
        .code = dylib_code,
        .code_size = sizeof(dylib_code),
        .id_name = "libanswer.dylib",
        .export_symbol = "_answer",
    };
    const image_spec executable_spec = {
        .output = executable,
        .filetype = MH_EXECUTE,
        .vmaddr = executable_vmaddr,
        .code = main_code,
        .code_size = sizeof(main_code),
        .dependency_command = LC_LOAD_DYLIB,
        .dependency_name = "@loader_path/libanswer.dylib",
        .bind_symbol = "_answer",
        .bind_backwards = backwards,
    };
    return build_image(&dylib_spec) && build_image(&executable_spec);
}

static bool make_rebase_fixture(const char *executable, const char *dylib)
{
    const uint64_t shared_preferred_vmaddr = 0x100000000ull;
    uint8_t main_code[7] = {0};
    build_indirect_call_code(main_code, shared_preferred_vmaddr);
    uint8_t dylib_code[11] = {0};
    build_rebase_reader_code(dylib_code, shared_preferred_vmaddr);
    uint8_t dylib_data[16] = {0};
    put_u64(dylib_data, 0, shared_preferred_vmaddr + DATA_FILE_OFFSET + 8u);
    put_u64(dylib_data, 8, 42);

    const image_spec dylib_spec = {
        .output = dylib,
        .filetype = MH_DYLIB,
        .vmaddr = shared_preferred_vmaddr,
        .code = dylib_code,
        .code_size = sizeof(dylib_code),
        .id_name = "librebase.dylib",
        .export_symbol = "_rebase_answer",
        .data = dylib_data,
        .data_size = sizeof(dylib_data),
        .rebase_data_pointer = true,
    };
    const image_spec executable_spec = {
        .output = executable,
        .filetype = MH_EXECUTE,
        .vmaddr = shared_preferred_vmaddr,
        .code = main_code,
        .code_size = sizeof(main_code),
        .dependency_command = LC_LOAD_DYLIB,
        .dependency_name = "@loader_path/librebase.dylib",
        .bind_symbol = "_rebase_answer",
    };
    return build_image(&dylib_spec) && build_image(&executable_spec);
}

static bool make_weak_fixture(const char *executable)
{
    const uint8_t code[] = {0xb8, 0x2a, 0x00, 0x00, 0x00, 0xc3};
    const image_spec spec = {
        .output = executable,
        .filetype = MH_EXECUTE,
        .vmaddr = 0x100000000ull,
        .code = code,
        .code_size = sizeof(code),
        .dependency_command = LC_LOAD_WEAK_DYLIB,
        .dependency_name = "@loader_path/libdefinitely_missing.dylib",
    };
    return build_image(&spec);
}

static bool make_backward_fixture(const char *executable, bool force_dyld)
{
    enum { ENTRY_OFFSET = 6 };
    const uint8_t code[] = {
        0xb8, 0x28, 0x00, 0x00, 0x00, /* helper: movl $40, %eax */
        0xc3,                         /* retq */
        0xe8, 0xf5, 0xff, 0xff, 0xff, /* entry: callq helper */
        0x83, 0xc0, 0x02,             /* addl $2, %eax */
        0xc3,                         /* retq */
    };
    const image_spec spec = {
        .output = executable,
        .filetype = MH_EXECUTE,
        .vmaddr = 0x100000000ull,
        .code = code,
        .code_size = sizeof(code),
        .entry_offset = ENTRY_OFFSET,
        .dependency_command = force_dyld ? LC_LOAD_WEAK_DYLIB : 0,
        .dependency_name = force_dyld
            ? "@loader_path/libdefinitely_missing.dylib"
            : NULL,
    };
    return build_image(&spec);
}

static bool make_executable_path_fixture(const char *executable,
                                         const char *middle,
                                         const char *leaf)
{
    const uint64_t executable_vmaddr = 0x100000000ull;
    const uint64_t middle_vmaddr = 0x200000000ull;
    const uint64_t leaf_vmaddr = 0x300000000ull;
    uint8_t main_code[7] = {0};
    uint8_t middle_code[7] = {0};
    build_indirect_call_code(main_code, executable_vmaddr);
    build_indirect_call_code(middle_code, middle_vmaddr);
    const uint8_t leaf_code[] = {0xb8, 0x2a, 0x00, 0x00, 0x00, 0xc3};

    const image_spec leaf_spec = {
        .output = leaf,
        .filetype = MH_DYLIB,
        .vmaddr = leaf_vmaddr,
        .code = leaf_code,
        .code_size = sizeof(leaf_code),
        .id_name = "libleaf.dylib",
        .export_symbol = "_leaf_answer",
    };
    const image_spec middle_spec = {
        .output = middle,
        .filetype = MH_DYLIB,
        .vmaddr = middle_vmaddr,
        .code = middle_code,
        .code_size = sizeof(middle_code),
        .id_name = "libmiddle.dylib",
        .dependency_command = LC_LOAD_DYLIB,
        .dependency_name = "@executable_path/../support/libleaf.dylib",
        .bind_symbol = "_leaf_answer",
        .export_symbol = "_middle_answer",
    };
    const image_spec executable_spec = {
        .output = executable,
        .filetype = MH_EXECUTE,
        .vmaddr = executable_vmaddr,
        .code = main_code,
        .code_size = sizeof(main_code),
        .dependency_command = LC_LOAD_DYLIB,
        .dependency_name = "@loader_path/../../frameworks/nested/libmiddle.dylib",
        .bind_symbol = "_middle_answer",
    };
    return build_image(&leaf_spec) && build_image(&middle_spec) && build_image(&executable_spec);
}

static void usage(void)
{
    fprintf(stderr,
            "usage:\n"
            "  make_dyld_macho bind <executable> <dylib>\n"
            "  make_dyld_macho bind-backward <executable> <dylib>\n"
            "  make_dyld_macho bind-backward-invalid <executable> <dylib>\n"
            "  make_dyld_macho rebase <executable> <dylib>\n"
            "  make_dyld_macho weak <executable>\n"
            "  make_dyld_macho backward-thin <executable>\n"
            "  make_dyld_macho backward-dyld <executable>\n"
            "  make_dyld_macho executable-path <executable> <middle-dylib> <leaf-dylib>\n");
}

int main(int argc, char **argv)
{
    bool ok = false;
    if (argc == 5 && strcmp(argv[1], "executable-path") == 0) {
        ok = make_executable_path_fixture(argv[2], argv[3], argv[4]);
    } else if (argc == 4 && strcmp(argv[1], "bind") == 0) {
        ok = make_bind_fixture(argv[2], argv[3], 0);
    } else if (argc == 4 && strcmp(argv[1], "bind-backward") == 0) {
        ok = make_bind_fixture(argv[2], argv[3], 1);
    } else if (argc == 4 && strcmp(argv[1], "bind-backward-invalid") == 0) {
        ok = make_bind_fixture(argv[2], argv[3], 2);
    } else if (argc == 4 && strcmp(argv[1], "rebase") == 0) {
        ok = make_rebase_fixture(argv[2], argv[3]);
    } else if (argc == 3 && strcmp(argv[1], "weak") == 0) {
        ok = make_weak_fixture(argv[2]);
    } else if (argc == 3 && strcmp(argv[1], "backward-thin") == 0) {
        ok = make_backward_fixture(argv[2], false);
    } else if (argc == 3 && strcmp(argv[1], "backward-dyld") == 0) {
        ok = make_backward_fixture(argv[2], true);
    } else {
        usage();
        return 2;
    }
    return ok ? 0 : 1;
}
