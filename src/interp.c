#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define R2_STACK_SIZE (1024u * 1024u)
#define R2_STACK_BASE 0x700000000000ull
#define R2_RETURN_SENTINEL UINT64_MAX
#define R2_MAX_STEPS 1000000u
#define R2_MAX_SYSCALL_WRITE (1024u * 1024u)
#define R2_MAX_SYSCALL_READ (1024u * 1024u)
#define R2_MAX_SYSCALL_PATH 4096u
#define R2_MAX_RUNTIME_REGIONS 64u
#define R2_MAX_SIGNAL_FRAMES 16u
#define R2_MMAP_BASE 0x500000000000ull
#define R2_MACOS_SYS_EXIT 0x2000001ull
#define R2_MACOS_SYS_READ 0x2000003ull
#define R2_MACOS_SYS_WRITE 0x2000004ull
#define R2_MACOS_SYS_OPEN 0x2000005ull
#define R2_MACOS_SYS_CLOSE 0x2000006ull
#define R2_MACOS_SYS_GETPID 0x2000014ull
#define R2_MACOS_SYS_KILL 0x2000025ull
#define R2_MACOS_SYS_SIGACTION 0x200002eull
#define R2_MACOS_SYS_SIGPROCMASK 0x2000030ull
#define R2_MACOS_SYS_PSYNCH_MUTEXWAIT 0x200012dull
#define R2_MACOS_SYS_PSYNCH_MUTEXDROP 0x200012eull
#define R2_MACOS_SYS_PSYNCH_CVBROAD 0x200012full
#define R2_MACOS_SYS_PSYNCH_CVSIGNAL 0x2000130ull
#define R2_MACOS_SYS_PSYNCH_CVWAIT 0x2000131ull
#define R2_MACOS_SYS_FSTAT 0x20000bdull
#define R2_MACOS_SYS_MMAP 0x20000c5ull
#define R2_MACOS_SYS_BSDTHREAD_CREATE 0x2000168ull
#define R2_MACOS_SYS_BSDTHREAD_TERMINATE 0x2000169ull
#define R2_MACOS_SYS_BSDTHREAD_REGISTER 0x200016eull
#define R2_MACOS_SYS_THREAD_SELFID 0x2000174ull
#define R2_MACOS_SYS_THREAD_SET_TSD_BASE 0x3000003ull
#define R2_DARWIN_NSIG 32u
#define R2_DARWIN_SIGKILL 9u
#define R2_DARWIN_SIGSTOP 17u
#define R2_DARWIN_SIGCHLD 20u
#define R2_DARWIN_SIGURG 16u
#define R2_DARWIN_SIGCONT 19u
#define R2_DARWIN_SIGWINCH 28u
#define R2_DARWIN_SIGINFO 29u
#define R2_DARWIN_SIG_DFL 0ull
#define R2_DARWIN_SIG_IGN 1ull
#define R2_DARWIN_SA_RESETHAND 0x0004u
#define R2_DARWIN_SA_NODEFER 0x0010u
#define R2_DARWIN_SA_SIGINFO 0x0040u
#define R2_DARWIN_SIG_BLOCK 1u
#define R2_DARWIN_SIG_UNBLOCK 2u
#define R2_DARWIN_SIG_SETMASK 3u
#define R2_SIGNAL_RETURN_SENTINEL (UINT64_MAX - 1ull)
#define R2_THREAD_RETURN_SENTINEL (UINT64_MAX - 2ull)
#define R2_SIGINFO_SIZE 104u
#define R2_UCONTEXT_SIZE 1024u
#define R2_MAX_GUEST_THREADS 16u
#define R2_THREAD_STACK_STRIDE 0x200000ull
#define R2_MAIN_THREAD_PORT 0x100u
#define R2_MAX_CV_STATES 16u
#define R2_MAX_MUTEX_STATES 16u
#define R2_MAX_PTHREAD_SIZE (64u * 1024u)
#define R2_PTHREAD_START_CUSTOM 0x01000000u
#define R2_PTHREAD_START_TSD_BASE_SET 0x10000000u
#define R2_PTHREAD_REGISTRATION_DATA_SIZE 56u
#define R2_PTHREAD_REG_DISPATCH_QUEUE_OFFSET 8u
#define R2_PTHREAD_REG_TSD_OFFSET 24u
#define R2_PTHREAD_REG_MACH_THREAD_SELF_OFFSET 32u
#define R2_PTHREAD_REG_MAIN_QOS 16u
#define R2_PTHREAD_REG_STACK_HINT 36u
#define R2_PTHREAD_REG_MUTEX_POLICY 44u
#define R2_X86_USER_CTHREAD 0x0full

typedef struct {
    bool zf;
    bool sf;
    bool cf;
    bool of;
} r2_flags;

typedef struct {
    uint64_t handler;
    uint64_t trampoline;
    uint32_t mask;
    uint32_t flags;
} r2_sigaction_state;

typedef struct {
    uint64_t regs[16];
    r2_flags flags;
    uint32_t signal_mask;
    size_t return_pc;
} r2_signal_frame;

typedef enum {
    R2_THREAD_UNUSED = 0,
    R2_THREAD_RUNNABLE,
    R2_THREAD_BLOCKED_CV,
    R2_THREAD_BLOCKED_MUTEX,
    R2_THREAD_EXITED
} r2_thread_state;

typedef struct {
    r2_thread_state state;
    uint64_t tid;
    uint32_t thread_port;
    uint64_t regs[16];
    r2_flags flags;
    uint32_t signal_mask;
    r2_signal_frame signal_frames[R2_MAX_SIGNAL_FRAMES];
    size_t signal_frame_count;
    size_t pc;
    uint8_t *stack;
    uint64_t stack_base;
    uint64_t stack_size;
    uint64_t gs_base;
    uint64_t wait_cv;
    uint64_t wait_mutex;
    uint64_t wait_relock_mutex;
    uint64_t exit_value;
} r2_guest_thread;

typedef struct {
    bool in_use;
    uint64_t cv;
    uint32_t pending_signals;
} r2_cv_state;

typedef struct {
    bool in_use;
    uint64_t mutex;
    bool locked;
    uint64_t owner_tid;
} r2_mutex_state;

typedef struct {
    uint64_t regs[16];
    r2_flags flags;
    uint32_t signal_mask;
    r2_signal_frame signal_frames[R2_MAX_SIGNAL_FRAMES];
    size_t signal_frame_count;
    const r2_interp_env *env;
    r2_memory_region runtime_regions[R2_MAX_RUNTIME_REGIONS];
    size_t runtime_region_count;
    uint64_t next_mmap_base;
    r2_sigaction_state sigactions[R2_DARWIN_NSIG + 1u];
    uint32_t pending_signals;
    r2_guest_thread threads[R2_MAX_GUEST_THREADS];
    r2_cv_state cv_states[R2_MAX_CV_STATES];
    r2_mutex_state mutex_states[R2_MAX_MUTEX_STATES];
    size_t thread_count;
    size_t current_thread;
    uint64_t next_tid;
    uint32_t next_thread_port;
    bool pthread_registered;
    uint64_t pthread_threadstart;
    uint64_t pthread_wqthread;
    uint64_t pthread_pthsize;
    uint64_t pthread_dispatchqueue_offset;
    uint32_t pthread_tsd_offset;
    uint32_t pthread_mach_thread_self_offset;
} r2_machine;

static void destroy_machine(r2_machine *machine)
{
    for (size_t i = 0; i < machine->runtime_region_count; i += 1) {
        free(machine->runtime_regions[i].data);
    }
    for (size_t i = 0; i < R2_MAX_GUEST_THREADS; i += 1) {
        free(machine->threads[i].stack);
    }
}

static void set_logic_flags(r2_flags *flags, uint64_t value)
{
    flags->zf = value == 0;
    flags->sf = (value >> 63) != 0;
    flags->cf = false;
    flags->of = false;
}

static uint64_t width_mask(uint8_t width)
{
    return width == 4 ? 0xffffffffull : UINT64_MAX;
}

static uint64_t width_sign_bit(uint8_t width)
{
    return width == 4 ? 0x80000000ull : 0x8000000000000000ull;
}

static uint64_t normalize_value(uint64_t value, uint8_t width)
{
    return value & width_mask(width);
}

static void set_reg_width(uint64_t regs[16], uint8_t reg, uint64_t value, uint8_t width)
{
    regs[reg] = normalize_value(value, width);
}

static void set_logic_flags_width(r2_flags *flags, uint64_t value, uint8_t width)
{
    set_logic_flags(flags, normalize_value(value, width));
    flags->sf = (normalize_value(value, width) & width_sign_bit(width)) != 0;
}

static void set_add_flags(r2_flags *flags,
                          uint64_t lhs,
                          uint64_t rhs,
                          uint64_t result,
                          uint8_t width)
{
    uint64_t mask = width_mask(width);
    uint64_t sign = width_sign_bit(width);
    lhs &= mask;
    rhs &= mask;
    result &= mask;
    flags->zf = result == 0;
    flags->sf = (result & sign) != 0;
    flags->cf = result < lhs;
    flags->of = (((~(lhs ^ rhs)) & (lhs ^ result)) & sign) != 0;
}

static void set_sub_flags(r2_flags *flags,
                          uint64_t lhs,
                          uint64_t rhs,
                          uint64_t result,
                          uint8_t width)
{
    uint64_t mask = width_mask(width);
    uint64_t sign = width_sign_bit(width);
    lhs &= mask;
    rhs &= mask;
    result &= mask;
    flags->zf = result == 0;
    flags->sf = (result & sign) != 0;
    flags->cf = lhs < rhs;
    flags->of = (((lhs ^ rhs) & (lhs ^ result)) & sign) != 0;
}

static bool valid_signal(uint64_t signum)
{
    return signum != 0 && signum <= R2_DARWIN_NSIG;
}

static uint32_t signal_bit(uint64_t signum)
{
    return 1u << (uint32_t)(signum - 1u);
}

static bool default_signal_is_ignored(uint64_t signum)
{
    switch (signum) {
    case R2_DARWIN_SIGCHLD:
    case R2_DARWIN_SIGURG:
    case R2_DARWIN_SIGCONT:
    case R2_DARWIN_SIGWINCH:
    case R2_DARWIN_SIGINFO:
        return true;
    default:
        return false;
    }
}

static bool eval_cc(r2_cc cc, const r2_flags *flags)
{
    switch (cc) {
    case R2_CC_E:
        return flags->zf;
    case R2_CC_NE:
        return !flags->zf;
    case R2_CC_L:
        return flags->sf != flags->of;
    case R2_CC_LE:
        return flags->zf || flags->sf != flags->of;
    case R2_CC_G:
        return !flags->zf && flags->sf == flags->of;
    case R2_CC_GE:
        return flags->sf == flags->of;
    default:
        return false;
    }
}

static bool stack_contains(const r2_machine *machine, uint64_t addr, size_t width, size_t *thread, size_t *offset)
{
    for (size_t i = 0; i < R2_MAX_GUEST_THREADS; i += 1) {
        const r2_guest_thread *candidate = &machine->threads[i];
        if (candidate->state == R2_THREAD_UNUSED || candidate->stack == NULL ||
            width > candidate->stack_size || addr < candidate->stack_base) {
            continue;
        }

        uint64_t end = addr + width;
        uint64_t stack_end = candidate->stack_base + candidate->stack_size;
        if (end < addr || stack_end < candidate->stack_base || end > stack_end) {
            continue;
        }

        *thread = i;
        *offset = (size_t)(addr - candidate->stack_base);
        return true;
    }

    return false;
}

static bool mapped_offset(const r2_machine *machine,
                          uint64_t addr,
                          size_t width,
                          const r2_memory_region **region,
                          size_t *offset)
{
    for (size_t i = 0; i < machine->runtime_region_count; i += 1) {
        const r2_memory_region *candidate = &machine->runtime_regions[i];
        if (candidate->data == NULL || width > candidate->size || addr < candidate->base) {
            continue;
        }

        uint64_t end = addr + width;
        uint64_t region_end = candidate->base + candidate->size;
        if (end < addr || region_end < candidate->base || end > region_end) {
            continue;
        }

        *region = candidate;
        *offset = (size_t)(addr - candidate->base);
        return true;
    }

    if (machine->env == NULL || machine->env->regions == NULL) {
        return false;
    }

    for (size_t i = 0; i < machine->env->region_count; i += 1) {
        const r2_memory_region *candidate = &machine->env->regions[i];
        if (candidate->data == NULL || width > candidate->size || addr < candidate->base) {
            continue;
        }

        uint64_t end = addr + width;
        uint64_t region_end = candidate->base + candidate->size;
        if (end < addr || region_end < candidate->base || end > region_end) {
            continue;
        }

        *region = candidate;
        *offset = (size_t)(addr - candidate->base);
        return true;
    }

    return false;
}

static r2_status read_mem(r2_machine *machine,
                          uint64_t addr,
                          uint8_t width,
                          uint64_t *out,
                          char *error,
                          size_t error_len)
{
    size_t off = 0;
    size_t thread = 0;
    const r2_memory_region *region = NULL;
    const uint8_t *bytes = NULL;
    if (stack_contains(machine, addr, width, &thread, &off)) {
        bytes = machine->threads[thread].stack + off;
    } else if (mapped_offset(machine, addr, width, &region, &off)) {
        bytes = region->data + off;
    } else {
        r2_set_error(error, error_len, "address 0x%016llx is not mapped",
                     (unsigned long long)addr);
        return R2_ERR_UNSUPPORTED;
    }

    uint64_t value = 0;
    for (size_t i = 0; i < width; i += 1) {
        value |= (uint64_t)bytes[i] << (i * 8);
    }
    *out = value;
    return R2_OK;
}

static r2_status write_mem(r2_machine *machine,
                           uint64_t addr,
                           uint8_t width,
                           uint64_t value,
                           char *error,
                           size_t error_len)
{
    size_t off = 0;
    size_t thread = 0;
    const r2_memory_region *region = NULL;
    uint8_t *bytes = NULL;
    if (stack_contains(machine, addr, width, &thread, &off)) {
        bytes = machine->threads[thread].stack + off;
    } else if (mapped_offset(machine, addr, width, &region, &off)) {
        if (!region->writable) {
            r2_set_error(error, error_len, "address 0x%016llx is read-only",
                         (unsigned long long)addr);
            return R2_ERR_UNSUPPORTED;
        }
        bytes = region->data + off;
    } else {
        r2_set_error(error, error_len, "address 0x%016llx is not mapped",
                     (unsigned long long)addr);
        return R2_ERR_UNSUPPORTED;
    }

    for (size_t i = 0; i < width; i += 1) {
        bytes[i] = (uint8_t)((value >> (i * 8)) & 0xffu);
    }
    return R2_OK;
}

static r2_status read_u64(r2_machine *machine,
                          uint64_t addr,
                          uint64_t *out,
                          char *error,
                          size_t error_len)
{
    return read_mem(machine, addr, 8, out, error, error_len);
}

static r2_status write_u64(r2_machine *machine,
                           uint64_t addr,
                           uint64_t value,
                           char *error,
                           size_t error_len)
{
    return write_mem(machine, addr, 8, value, error, error_len);
}

static r2_status read_guest_byte(r2_machine *machine,
                                 uint64_t addr,
                                 uint8_t *out,
                                 char *error,
                                 size_t error_len)
{
    uint64_t value = 0;
    r2_status status = read_mem(machine, addr, 1, &value, error, error_len);
    if (status != R2_OK) {
        return status;
    }
    *out = (uint8_t)value;
    return R2_OK;
}

static r2_status write_guest_byte(r2_machine *machine,
                                  uint64_t addr,
                                  uint8_t value,
                                  char *error,
                                  size_t error_len)
{
    return write_mem(machine, addr, 1, value, error, error_len);
}

static r2_status read_guest_cstring(r2_machine *machine,
                                    uint64_t addr,
                                    char **out,
                                    char *error,
                                    size_t error_len)
{
    char *path = malloc(R2_MAX_SYSCALL_PATH + 1);
    if (path == NULL) {
        r2_set_error(error, error_len, "out of memory reading guest string");
        return R2_ERR_NO_MEMORY;
    }

    for (size_t i = 0; i < R2_MAX_SYSCALL_PATH; i += 1) {
        uint8_t byte = 0;
        r2_status status = read_guest_byte(machine, addr + i, &byte, error, error_len);
        if (status != R2_OK) {
            free(path);
            return status;
        }
        path[i] = (char)byte;
        if (byte == 0) {
            *out = path;
            return R2_OK;
        }
    }

    free(path);
    r2_set_error(error, error_len, "guest string exceeds %u bytes", R2_MAX_SYSCALL_PATH);
    return R2_ERR_UNSUPPORTED;
}

static r2_status copy_guest_to_host(r2_machine *machine,
                                    uint64_t addr,
                                    uint8_t *buf,
                                    size_t len,
                                    char *error,
                                    size_t error_len)
{
    for (size_t i = 0; i < len; i += 1) {
        r2_status status = read_guest_byte(machine, addr + i, &buf[i], error, error_len);
        if (status != R2_OK) {
            return status;
        }
    }
    return R2_OK;
}

static r2_status copy_host_to_guest(r2_machine *machine,
                                    uint64_t addr,
                                    const uint8_t *buf,
                                    size_t len,
                                    char *error,
                                    size_t error_len)
{
    for (size_t i = 0; i < len; i += 1) {
        r2_status status = write_guest_byte(machine, addr + i, buf[i], error, error_len);
        if (status != R2_OK) {
            return status;
        }
    }
    return R2_OK;
}

static r2_status syscall_write(r2_machine *machine,
                               uint64_t fd,
                               uint64_t addr,
                               uint64_t len,
                               char *error,
                               size_t error_len)
{
    if (fd > INT_MAX) {
        r2_set_error(error, error_len, "write syscall fd is too large");
        return R2_ERR_UNSUPPORTED;
    }
    if (len > R2_MAX_SYSCALL_WRITE) {
        r2_set_error(error, error_len, "write syscall length is too large");
        return R2_ERR_UNSUPPORTED;
    }

    uint8_t *buf = malloc((size_t)len == 0 ? 1 : (size_t)len);
    if (buf == NULL) {
        r2_set_error(error, error_len, "out of memory copying write buffer");
        return R2_ERR_NO_MEMORY;
    }

    r2_status status = copy_guest_to_host(machine, addr, buf, (size_t)len, error, error_len);
    if (status != R2_OK) {
        free(buf);
        return status;
    }

    (void)fflush(stdout);
    (void)fflush(stderr);
    ssize_t wrote = write((int)fd, buf, (size_t)len);
    free(buf);
    if (wrote < 0) {
        r2_set_error(error, error_len, "host write failed: %s", strerror(errno));
        return R2_ERR_IO;
    }

    machine->regs[R2_X86_RAX] = (uint64_t)wrote;
    return R2_OK;
}

static r2_status syscall_read(r2_machine *machine,
                              uint64_t fd,
                              uint64_t addr,
                              uint64_t len,
                              char *error,
                              size_t error_len)
{
    if (fd > INT_MAX) {
        r2_set_error(error, error_len, "read syscall fd is too large");
        return R2_ERR_UNSUPPORTED;
    }
    if (len > R2_MAX_SYSCALL_READ) {
        r2_set_error(error, error_len, "read syscall length is too large");
        return R2_ERR_UNSUPPORTED;
    }

    uint8_t *buf = malloc((size_t)len == 0 ? 1 : (size_t)len);
    if (buf == NULL) {
        r2_set_error(error, error_len, "out of memory allocating read buffer");
        return R2_ERR_NO_MEMORY;
    }

    ssize_t got = read((int)fd, buf, (size_t)len);
    if (got < 0) {
        r2_set_error(error, error_len, "host read failed: %s", strerror(errno));
        free(buf);
        return R2_ERR_IO;
    }

    r2_status status = copy_host_to_guest(machine, addr, buf, (size_t)got, error, error_len);
    free(buf);
    if (status != R2_OK) {
        return status;
    }

    machine->regs[R2_X86_RAX] = (uint64_t)got;
    return R2_OK;
}

static r2_status syscall_open(r2_machine *machine,
                              uint64_t path_addr,
                              uint64_t flags,
                              uint64_t mode,
                              char *error,
                              size_t error_len)
{
    if (flags > INT_MAX || mode > INT_MAX) {
        r2_set_error(error, error_len, "open syscall flags or mode are too large");
        return R2_ERR_UNSUPPORTED;
    }

    char *path = NULL;
    r2_status status = read_guest_cstring(machine, path_addr, &path, error, error_len);
    if (status != R2_OK) {
        return status;
    }

    int fd = open(path, (int)flags, (mode_t)mode);
    if (fd < 0) {
        r2_set_error(error, error_len, "host open %s failed: %s", path, strerror(errno));
        free(path);
        return R2_ERR_IO;
    }

    free(path);
    machine->regs[R2_X86_RAX] = (uint64_t)fd;
    return R2_OK;
}

static r2_status syscall_close(r2_machine *machine, uint64_t fd, char *error, size_t error_len)
{
    if (fd > INT_MAX) {
        r2_set_error(error, error_len, "close syscall fd is too large");
        return R2_ERR_UNSUPPORTED;
    }
    if (fd <= 2) {
        r2_set_error(error, error_len, "close syscall refuses to close stdio fd %llu",
                     (unsigned long long)fd);
        return R2_ERR_UNSUPPORTED;
    }
    if (close((int)fd) != 0) {
        r2_set_error(error, error_len, "host close failed: %s", strerror(errno));
        return R2_ERR_IO;
    }
    machine->regs[R2_X86_RAX] = 0;
    return R2_OK;
}

static uint64_t align_up_u64(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1u) & ~(alignment - 1u);
}

static bool overlaps_region(uint64_t base, uint64_t size, uint64_t other_base, uint64_t other_size)
{
    uint64_t end = base + size;
    uint64_t other_end = other_base + other_size;
    return end >= base && other_end >= other_base && base < other_end && other_base < end;
}

static bool address_range_is_free(const r2_machine *machine, uint64_t base, uint64_t size)
{
    for (size_t i = 0; i < R2_MAX_GUEST_THREADS; i += 1) {
        const r2_guest_thread *thread = &machine->threads[i];
        if (thread->state != R2_THREAD_UNUSED && thread->stack != NULL &&
            overlaps_region(base, size, thread->stack_base, thread->stack_size)) {
            return false;
        }
    }
    for (size_t i = 0; i < machine->runtime_region_count; i += 1) {
        const r2_memory_region *region = &machine->runtime_regions[i];
        if (overlaps_region(base, size, region->base, region->size)) {
            return false;
        }
    }
    if (machine->env != NULL) {
        for (size_t i = 0; i < machine->env->region_count; i += 1) {
            const r2_memory_region *region = &machine->env->regions[i];
            if (overlaps_region(base, size, region->base, region->size)) {
                return false;
            }
        }
    }
    return true;
}

static r2_status syscall_mmap(r2_machine *machine,
                              uint64_t addr,
                              uint64_t len,
                              uint64_t prot,
                              uint64_t flags,
                              uint64_t fd,
                              uint64_t offset,
                              char *error,
                              size_t error_len)
{
    if (len == 0 || len > 256ull * 1024ull * 1024ull) {
        r2_set_error(error, error_len, "mmap length is unsupported");
        return R2_ERR_UNSUPPORTED;
    }
    if (machine->runtime_region_count >= R2_MAX_RUNTIME_REGIONS) {
        r2_set_error(error, error_len, "too many runtime mmap regions");
        return R2_ERR_UNSUPPORTED;
    }
    if ((flags & MAP_PRIVATE) == 0) {
        r2_set_error(error, error_len, "mmap only supports MAP_PRIVATE mappings");
        return R2_ERR_UNSUPPORTED;
    }
    if ((prot & ~(uint64_t)(PROT_READ | PROT_WRITE | PROT_EXEC)) != 0) {
        r2_set_error(error, error_len, "mmap protection flags are unsupported");
        return R2_ERR_UNSUPPORTED;
    }

    uint64_t map_size = align_up_u64(len, 4096);
    uint64_t base = 0;
    if ((flags & MAP_FIXED) != 0) {
        if (addr == 0 || (addr & 0xfffu) != 0) {
            r2_set_error(error, error_len, "MAP_FIXED requires a non-zero page-aligned address");
            return R2_ERR_UNSUPPORTED;
        }
        base = addr;
        if (!address_range_is_free(machine, base, map_size)) {
            r2_set_error(error, error_len, "MAP_FIXED address range overlaps an existing mapping");
            return R2_ERR_UNSUPPORTED;
        }
    } else {
        base = align_up_u64(machine->next_mmap_base, 4096);
        while (!address_range_is_free(machine, base, map_size)) {
            base += map_size + 4096;
        }
        machine->next_mmap_base = base + map_size + 4096;
    }

    uint8_t *data = calloc(1, (size_t)map_size);
    if (data == NULL) {
        r2_set_error(error, error_len, "out of memory allocating mmap region");
        return R2_ERR_NO_MEMORY;
    }

    if ((flags & MAP_ANON) == 0) {
        if (fd > INT_MAX || offset > (uint64_t)INT64_MAX) {
            free(data);
            r2_set_error(error, error_len, "fd-backed mmap argument is too large");
            return R2_ERR_UNSUPPORTED;
        }
        ssize_t got = pread((int)fd, data, (size_t)len, (off_t)offset);
        if (got < 0) {
            r2_set_error(error, error_len, "host pread for mmap failed: %s", strerror(errno));
            free(data);
            return R2_ERR_IO;
        }
    }

    r2_memory_region *region = &machine->runtime_regions[machine->runtime_region_count];
    region->base = base;
    region->size = map_size;
    region->data = data;
    region->writable = (prot & PROT_WRITE) != 0;
    machine->runtime_region_count += 1;
    machine->regs[R2_X86_RAX] = base;
    return R2_OK;
}

static r2_status syscall_fstat(r2_machine *machine,
                               uint64_t fd,
                               uint64_t stat_addr,
                               char *error,
                               size_t error_len)
{
    if (fd > INT_MAX) {
        r2_set_error(error, error_len, "fstat syscall fd is too large");
        return R2_ERR_UNSUPPORTED;
    }

    struct stat st;
    if (fstat((int)fd, &st) != 0) {
        r2_set_error(error, error_len, "host fstat failed: %s", strerror(errno));
        return R2_ERR_IO;
    }

    r2_status status =
        copy_host_to_guest(machine, stat_addr, (const uint8_t *)&st, sizeof(st), error, error_len);
    if (status != R2_OK) {
        return status;
    }

    machine->regs[R2_X86_RAX] = 0;
    return R2_OK;
}

static r2_status read_kernel_sigaction(r2_machine *machine,
                                       uint64_t act_addr,
                                       r2_sigaction_state *out,
                                       char *error,
                                       size_t error_len)
{
    uint64_t value = 0;
    r2_status status = read_mem(machine, act_addr, 8, &out->handler, error, error_len);
    if (status != R2_OK) {
        return status;
    }
    status = read_mem(machine, act_addr + 8, 8, &out->trampoline, error, error_len);
    if (status != R2_OK) {
        return status;
    }
    status = read_mem(machine, act_addr + 16, 4, &value, error, error_len);
    if (status != R2_OK) {
        return status;
    }
    out->mask = (uint32_t)value;
    status = read_mem(machine, act_addr + 20, 4, &value, error, error_len);
    if (status != R2_OK) {
        return status;
    }
    out->flags = (uint32_t)value;
    return R2_OK;
}

static r2_status write_user_sigaction(r2_machine *machine,
                                      uint64_t act_addr,
                                      const r2_sigaction_state *act,
                                      char *error,
                                      size_t error_len)
{
    r2_status status = write_mem(machine, act_addr, 8, act->handler, error, error_len);
    if (status != R2_OK) {
        return status;
    }
    status = write_mem(machine, act_addr + 8, 4, act->mask, error, error_len);
    if (status != R2_OK) {
        return status;
    }
    return write_mem(machine, act_addr + 12, 4, act->flags, error, error_len);
}

static r2_status syscall_sigaction(r2_machine *machine,
                                   uint64_t signum,
                                   uint64_t new_action_addr,
                                   uint64_t old_action_addr,
                                   char *error,
                                   size_t error_len)
{
    if (!valid_signal(signum)) {
        r2_set_error(error, error_len, "sigaction signal number is unsupported");
        return R2_ERR_UNSUPPORTED;
    }

    r2_sigaction_state new_action = {0};
    if (new_action_addr != 0) {
        if (signum == R2_DARWIN_SIGKILL || signum == R2_DARWIN_SIGSTOP) {
            r2_set_error(error, error_len, "sigaction cannot install handlers for SIGKILL/SIGSTOP");
            return R2_ERR_UNSUPPORTED;
        }
        r2_status status =
            read_kernel_sigaction(machine, new_action_addr, &new_action, error, error_len);
        if (status != R2_OK) {
            return status;
        }
    }

    if (old_action_addr != 0) {
        r2_status status = write_user_sigaction(machine,
                                                old_action_addr,
                                                &machine->sigactions[signum],
                                                error,
                                                error_len);
        if (status != R2_OK) {
            return status;
        }
    }

    if (new_action_addr != 0) {
        machine->sigactions[signum] = new_action;
    }

    machine->regs[R2_X86_RAX] = 0;
    return R2_OK;
}

static r2_status queue_signal(r2_machine *machine, uint64_t signum, char *error, size_t error_len)
{
    if (!valid_signal(signum)) {
        r2_set_error(error, error_len, "signal number is unsupported");
        return R2_ERR_UNSUPPORTED;
    }
    machine->pending_signals |= signal_bit(signum);
    return R2_OK;
}

static r2_status syscall_getpid(r2_machine *machine)
{
    machine->regs[R2_X86_RAX] = (uint64_t)(uint32_t)getpid();
    return R2_OK;
}

static r2_status syscall_kill(r2_machine *machine,
                              uint64_t pid,
                              uint64_t signum,
                              char *error,
                              size_t error_len)
{
    int requested_pid = (int)(uint32_t)pid;
    if (signum == 0) {
        machine->regs[R2_X86_RAX] = 0;
        return R2_OK;
    }
    if (!valid_signal(signum)) {
        r2_set_error(error, error_len, "kill signal number is unsupported");
        return R2_ERR_UNSUPPORTED;
    }
    if (requested_pid != 0 && requested_pid != getpid()) {
        r2_set_error(error, error_len, "kill only supports signalling the emulated process");
        return R2_ERR_UNSUPPORTED;
    }

    r2_status status = queue_signal(machine, signum, error, error_len);
    if (status != R2_OK) {
        return status;
    }
    machine->regs[R2_X86_RAX] = 0;
    return R2_OK;
}

static r2_status syscall_sigprocmask(r2_machine *machine,
                                     uint64_t how,
                                     uint64_t mask_addr,
                                     uint64_t old_mask_addr,
                                     char *error,
                                     size_t error_len)
{
    if (old_mask_addr != 0) {
        r2_status status =
            write_mem(machine, old_mask_addr, 4, machine->signal_mask, error, error_len);
        if (status != R2_OK) {
            return status;
        }
    }

    if (mask_addr != 0) {
        uint64_t raw_mask = 0;
        r2_status status = read_mem(machine, mask_addr, 4, &raw_mask, error, error_len);
        if (status != R2_OK) {
            return status;
        }
        uint32_t mask = (uint32_t)raw_mask;
        switch (how) {
        case R2_DARWIN_SIG_BLOCK:
            machine->signal_mask |= mask;
            break;
        case R2_DARWIN_SIG_UNBLOCK:
            machine->signal_mask &= ~mask;
            break;
        case R2_DARWIN_SIG_SETMASK:
            machine->signal_mask = mask;
            break;
        default:
            r2_set_error(error, error_len, "sigprocmask how value is unsupported");
            return R2_ERR_UNSUPPORTED;
        }
    }

    machine->regs[R2_X86_RAX] = 0;
    return R2_OK;
}

static uint64_t eval_mem_addr(const r2_insn *insn, const r2_machine *machine)
{
    const r2_mem_ref *mem = &insn->mem;
    uint64_t addr = 0;

    if (mem->rip_relative) {
        uint64_t rip_base = machine->env == NULL ? 0 : machine->env->rip_base;
        addr = rip_base + (uint64_t)(insn->x86_offset + insn->x86_len);
    }
    if (mem->base != R2_NO_REG) {
        addr += machine->regs[mem->base];
    }
    if (mem->index != R2_NO_REG) {
        addr += machine->regs[mem->index] * mem->scale;
    }
    addr += (uint64_t)mem->disp;
    if (mem->gs_relative) {
        addr += machine->threads[machine->current_thread].gs_base;
    }
    return addr;
}

static r2_status push_u64(r2_machine *machine,
                          uint64_t value,
                          char *error,
                          size_t error_len)
{
    machine->regs[R2_X86_RSP] -= 8;
    return write_u64(machine, machine->regs[R2_X86_RSP], value, error, error_len);
}

static r2_status pop_u64(r2_machine *machine,
                         uint64_t *value,
                         char *error,
                         size_t error_len)
{
    r2_status status = read_u64(machine, machine->regs[R2_X86_RSP], value, error, error_len);
    if (status != R2_OK) {
        return status;
    }
    machine->regs[R2_X86_RSP] += 8;
    return R2_OK;
}

static void save_active_thread(r2_machine *machine, size_t pc)
{
    if (machine->current_thread >= R2_MAX_GUEST_THREADS) {
        return;
    }

    r2_guest_thread *thread = &machine->threads[machine->current_thread];
    if (thread->state == R2_THREAD_UNUSED || thread->state == R2_THREAD_EXITED) {
        return;
    }

    memcpy(thread->regs, machine->regs, sizeof(thread->regs));
    thread->flags = machine->flags;
    thread->signal_mask = machine->signal_mask;
    memcpy(thread->signal_frames, machine->signal_frames, sizeof(thread->signal_frames));
    thread->signal_frame_count = machine->signal_frame_count;
    thread->pc = pc;
}

static void load_thread(r2_machine *machine, size_t thread_index, size_t *pc)
{
    r2_guest_thread *thread = &machine->threads[thread_index];
    machine->current_thread = thread_index;
    memcpy(machine->regs, thread->regs, sizeof(machine->regs));
    machine->flags = thread->flags;
    machine->signal_mask = thread->signal_mask;
    memcpy(machine->signal_frames, thread->signal_frames, sizeof(machine->signal_frames));
    machine->signal_frame_count = thread->signal_frame_count;
    *pc = thread->pc;
}

static r2_status schedule_next_thread(r2_machine *machine,
                                      size_t *pc,
                                      char *error,
                                      size_t error_len)
{
    save_active_thread(machine, *pc);

    size_t start = machine->current_thread;
    for (size_t i = 1; i <= R2_MAX_GUEST_THREADS; i += 1) {
        size_t candidate = (start + i) % R2_MAX_GUEST_THREADS;
        if (machine->threads[candidate].state == R2_THREAD_RUNNABLE) {
            load_thread(machine, candidate, pc);
            return R2_OK;
        }
    }

    r2_set_error(error, error_len, "all guest threads are blocked or exited");
    return R2_ERR_UNSUPPORTED;
}

static r2_status find_insn_by_offset(const r2_block *block,
                                     size_t offset,
                                     size_t *index,
                                     char *error,
                                     size_t error_len)
{
    for (size_t i = 0; i < block->count; i += 1) {
        if (block->insns[i].x86_offset == offset) {
            *index = i;
            return R2_OK;
        }
    }

    r2_set_error(error, error_len, "branch target byte offset %zu is not an instruction boundary",
                 offset);
    return R2_ERR_DECODE;
}

static r2_status jump_to_target(const r2_block *block,
                                size_t target,
                                size_t *pc,
                                char *error,
                                size_t error_len)
{
    return find_insn_by_offset(block, target, pc, error, error_len);
}

static r2_status guest_code_addr_to_pc(const r2_block *block,
                                       const r2_machine *machine,
                                       uint64_t guest_addr,
                                       size_t *pc,
                                       char *error,
                                       size_t error_len)
{
    uint64_t rip_base = machine->env == NULL ? 0 : machine->env->rip_base;
    if (guest_addr < rip_base) {
        r2_set_error(error,
                     error_len,
                     "signal handler address 0x%016llx is below code base 0x%016llx",
                     (unsigned long long)guest_addr,
                     (unsigned long long)rip_base);
        return R2_ERR_UNSUPPORTED;
    }

    uint64_t offset = guest_addr - rip_base;
    if (offset > SIZE_MAX) {
        r2_set_error(error, error_len, "signal handler address is too large");
        return R2_ERR_UNSUPPORTED;
    }
    return find_insn_by_offset(block, (size_t)offset, pc, error, error_len);
}

static void clear_thread(r2_guest_thread *thread)
{
    free(thread->stack);
    memset(thread, 0, sizeof(*thread));
}

static bool find_free_thread_slot(const r2_machine *machine, size_t *slot)
{
    for (size_t i = 1; i < R2_MAX_GUEST_THREADS; i += 1) {
        if (machine->threads[i].state == R2_THREAD_UNUSED) {
            *slot = i;
            return true;
        }
    }
    return false;
}

static r2_status allocate_thread_stack(r2_machine *machine,
                                       size_t slot,
                                       r2_guest_thread *thread,
                                       char *error,
                                       size_t error_len)
{
    uint64_t stack_base = R2_STACK_BASE + ((uint64_t)slot * R2_THREAD_STACK_STRIDE);
    if (!address_range_is_free(machine, stack_base, R2_STACK_SIZE)) {
        r2_set_error(error, error_len, "guest thread stack range overlaps an existing mapping");
        return R2_ERR_UNSUPPORTED;
    }

    thread->stack = calloc(1, R2_STACK_SIZE);
    if (thread->stack == NULL) {
        r2_set_error(error, error_len, "out of memory allocating guest thread stack");
        return R2_ERR_NO_MEMORY;
    }
    thread->stack_base = stack_base;
    thread->stack_size = R2_STACK_SIZE;
    return R2_OK;
}

static r2_cv_state *find_cv_state(r2_machine *machine, uint64_t cv, bool create)
{
    r2_cv_state *free_state = NULL;
    for (size_t i = 0; i < R2_MAX_CV_STATES; i += 1) {
        r2_cv_state *state = &machine->cv_states[i];
        if (state->in_use && state->cv == cv) {
            return state;
        }
        if (!state->in_use && free_state == NULL) {
            free_state = state;
        }
    }

    if (!create || free_state == NULL) {
        return NULL;
    }

    memset(free_state, 0, sizeof(*free_state));
    free_state->in_use = true;
    free_state->cv = cv;
    return free_state;
}

static r2_mutex_state *find_mutex_state(r2_machine *machine, uint64_t mutex, bool create)
{
    r2_mutex_state *free_state = NULL;
    for (size_t i = 0; i < R2_MAX_MUTEX_STATES; i += 1) {
        r2_mutex_state *state = &machine->mutex_states[i];
        if (state->in_use && state->mutex == mutex) {
            return state;
        }
        if (!state->in_use && free_state == NULL) {
            free_state = state;
        }
    }

    if (!create || free_state == NULL) {
        return NULL;
    }

    memset(free_state, 0, sizeof(*free_state));
    free_state->in_use = true;
    free_state->mutex = mutex;
    return free_state;
}

static bool wake_one_mutex_waiter(r2_machine *machine, r2_mutex_state *state)
{
    for (size_t i = 0; i < R2_MAX_GUEST_THREADS; i += 1) {
        r2_guest_thread *thread = &machine->threads[i];
        if (thread->state == R2_THREAD_BLOCKED_MUTEX && thread->wait_mutex == state->mutex) {
            state->locked = true;
            state->owner_tid = thread->tid;
            thread->state = R2_THREAD_RUNNABLE;
            thread->wait_mutex = 0;
            thread->wait_relock_mutex = 0;
            return true;
        }
    }
    return false;
}

static void unlock_mutex_state(r2_machine *machine, r2_mutex_state *state)
{
    if (wake_one_mutex_waiter(machine, state)) {
        return;
    }

    state->locked = false;
    state->owner_tid = 0;
}

static bool try_acquire_mutex_for_thread(r2_machine *machine,
                                         r2_guest_thread *thread,
                                         uint64_t mutex)
{
    r2_mutex_state *state = find_mutex_state(machine, mutex, true);
    if (state == NULL) {
        return false;
    }
    if (!state->locked || state->owner_tid == thread->tid) {
        state->locked = true;
        state->owner_tid = thread->tid;
        return true;
    }
    return false;
}

static void wake_cv_thread(r2_machine *machine, r2_guest_thread *thread)
{
    uint64_t relock_mutex = thread->wait_relock_mutex;
    thread->wait_cv = 0;
    if (relock_mutex == 0 || try_acquire_mutex_for_thread(machine, thread, relock_mutex)) {
        thread->state = R2_THREAD_RUNNABLE;
        thread->wait_mutex = 0;
        thread->wait_relock_mutex = 0;
        return;
    }

    thread->state = R2_THREAD_BLOCKED_MUTEX;
    thread->wait_mutex = relock_mutex;
}

static bool consume_cv_signal(r2_machine *machine, uint64_t cv)
{
    r2_cv_state *state = find_cv_state(machine, cv, false);
    if (state == NULL || state->pending_signals == 0) {
        return false;
    }
    state->pending_signals -= 1;
    if (state->pending_signals == 0) {
        memset(state, 0, sizeof(*state));
    }
    return true;
}

static bool post_cv_signal(r2_machine *machine, uint64_t cv)
{
    for (size_t i = 0; i < R2_MAX_GUEST_THREADS; i += 1) {
        r2_guest_thread *thread = &machine->threads[i];
        if (thread->state == R2_THREAD_BLOCKED_CV && thread->wait_cv == cv) {
            wake_cv_thread(machine, thread);
            return true;
        }
    }

    r2_cv_state *state = find_cv_state(machine, cv, true);
    if (state != NULL && state->pending_signals != UINT32_MAX) {
        state->pending_signals += 1;
    }
    return false;
}

static size_t post_cv_broadcast(r2_machine *machine, uint64_t cv)
{
    size_t woke = 0;
    for (size_t i = 0; i < R2_MAX_GUEST_THREADS; i += 1) {
        r2_guest_thread *thread = &machine->threads[i];
        if (thread->state == R2_THREAD_BLOCKED_CV && thread->wait_cv == cv) {
            wake_cv_thread(machine, thread);
            woke += 1;
        }
    }
    return woke;
}

static r2_status block_current_thread_on_mutex(r2_machine *machine, uint64_t mutex)
{
    r2_guest_thread *thread = &machine->threads[machine->current_thread];
    if (try_acquire_mutex_for_thread(machine, thread, mutex)) {
        machine->regs[R2_X86_RAX] = 0;
        return R2_OK;
    }

    thread->state = R2_THREAD_BLOCKED_MUTEX;
    thread->wait_mutex = mutex;
    machine->regs[R2_X86_RAX] = 0;
    return R2_OK;
}

static r2_status drop_mutex_for_current_thread(r2_machine *machine,
                                               uint64_t mutex,
                                               char *error,
                                               size_t error_len)
{
    r2_mutex_state *state = find_mutex_state(machine, mutex, false);
    if (state == NULL || !state->locked) {
        machine->regs[R2_X86_RAX] = 0;
        return R2_OK;
    }

    uint64_t tid = machine->threads[machine->current_thread].tid;
    if (state->owner_tid != tid) {
        r2_set_error(error, error_len, "psynch_mutexdrop by non-owner guest thread");
        return R2_ERR_UNSUPPORTED;
    }

    unlock_mutex_state(machine, state);
    machine->regs[R2_X86_RAX] = 0;
    return R2_OK;
}

static r2_status syscall_bsdthread_create(const r2_block *block,
                                          r2_machine *machine,
                                          uint64_t func,
                                          uint64_t func_arg,
                                          uint64_t stack,
                                          uint64_t pthread,
                                          uint64_t flags,
                                          char *error,
                                          size_t error_len)
{
    if (func == 0) {
        r2_set_error(error, error_len, "bsdthread_create requires a non-null start function");
        return R2_ERR_UNSUPPORTED;
    }
    if (machine->pthread_registered && (flags & R2_PTHREAD_START_CUSTOM) == 0) {
        r2_set_error(error, error_len, "registered bsdthread_create requires PTHREAD_START_CUSTOM");
        return R2_ERR_UNSUPPORTED;
    }
    if (machine->pthread_registered && machine->pthread_tsd_offset != 0 &&
        pthread > UINT64_MAX - machine->pthread_tsd_offset) {
        r2_set_error(error, error_len, "bsdthread_create TSD base overflows guest address space");
        return R2_ERR_UNSUPPORTED;
    }

    size_t slot = 0;
    if (!find_free_thread_slot(machine, &slot)) {
        r2_set_error(error, error_len, "too many guest threads");
        return R2_ERR_UNSUPPORTED;
    }

    r2_guest_thread *thread = &machine->threads[slot];
    memset(thread, 0, sizeof(*thread));
    thread->state = R2_THREAD_RUNNABLE;
    thread->tid = machine->next_tid;
    machine->next_tid += 1;
    thread->thread_port = machine->next_thread_port;
    machine->next_thread_port += 1;
    thread->signal_mask = machine->signal_mask;
    r2_status status = R2_OK;
    if (machine->pthread_registered && machine->pthread_tsd_offset != 0) {
        thread->gs_base = pthread + machine->pthread_tsd_offset;
    }

    uint64_t stack_top = stack;
    if (stack == 0) {
        status = allocate_thread_stack(machine, slot, thread, error, error_len);
        if (status != R2_OK) {
            clear_thread(thread);
            return status;
        }
        stack_top = thread->stack_base + thread->stack_size;
    } else {
        if (stack < 8u) {
            clear_thread(thread);
            r2_set_error(error, error_len, "bsdthread_create stack pointer is too low");
            return R2_ERR_UNSUPPORTED;
        }
    }

    uint64_t entry = func;
    if (machine->pthread_registered) {
        thread->regs[R2_X86_RSP] = stack_top;
        entry = machine->pthread_threadstart;
        thread->regs[R2_X86_RDI] = pthread;
        thread->regs[R2_X86_RSI] = thread->thread_port;
        thread->regs[R2_X86_RDX] = func;
        thread->regs[R2_X86_RCX] = func_arg;
        thread->regs[R2_X86_R8] = stack_top;
        thread->regs[R2_X86_R9] = flags;
        if (machine->pthread_tsd_offset != 0) {
            thread->regs[R2_X86_R9] |= R2_PTHREAD_START_TSD_BASE_SET;
        }
    } else {
        thread->regs[R2_X86_RSP] = stack_top - 8u;
        status = write_u64(machine,
                           thread->regs[R2_X86_RSP],
                           R2_THREAD_RETURN_SENTINEL,
                           error,
                           error_len);
        if (status != R2_OK) {
            clear_thread(thread);
            return status;
        }
        thread->regs[R2_X86_RDI] = func_arg;
        thread->regs[R2_X86_RSI] = pthread;
        thread->regs[R2_X86_RDX] = flags;
    }

    size_t start_pc = 0;
    status = guest_code_addr_to_pc(block, machine, entry, &start_pc, error, error_len);
    if (status != R2_OK) {
        clear_thread(thread);
        return status;
    }
    if (machine->pthread_mach_thread_self_offset != 0) {
        if (thread->gs_base > UINT64_MAX - machine->pthread_mach_thread_self_offset) {
            clear_thread(thread);
            r2_set_error(error,
                         error_len,
                         "bsdthread_create Mach thread-self slot overflows guest address space");
            return R2_ERR_UNSUPPORTED;
        }
        status = write_u64(machine,
                           thread->gs_base + machine->pthread_mach_thread_self_offset,
                           thread->thread_port,
                           error,
                           error_len);
        if (status != R2_OK) {
            clear_thread(thread);
            return status;
        }
    }
    thread->pc = start_pc;

    if (slot >= machine->thread_count) {
        machine->thread_count = slot + 1u;
    }
    machine->regs[R2_X86_RAX] = machine->pthread_registered ? pthread : thread->tid;
    return R2_OK;
}

static r2_status syscall_bsdthread_register(r2_machine *machine,
                                            uint64_t threadstart,
                                            uint64_t wqthread,
                                            uint64_t pthsize,
                                            uint64_t pthread_init_data,
                                            uint64_t pthread_init_data_size,
                                            uint64_t dispatchqueue_offset,
                                            char *error,
                                            size_t error_len)
{
    if (threadstart == 0) {
        r2_set_error(error, error_len, "bsdthread_register requires a non-null threadstart");
        return R2_ERR_UNSUPPORTED;
    }
    if (pthsize > R2_MAX_PTHREAD_SIZE) {
        r2_set_error(error, error_len, "bsdthread_register pthread size is too large");
        return R2_ERR_UNSUPPORTED;
    }
    if (machine->pthread_registered) {
        r2_set_error(error, error_len, "bsdthread_register called more than once");
        return R2_ERR_UNSUPPORTED;
    }

    uint64_t registration_dispatchqueue_offset = dispatchqueue_offset;
    uint32_t tsd_offset = 0;
    uint32_t mach_thread_self_offset = 0;
    if (pthread_init_data != 0) {
        if (pthread_init_data_size < 8u) {
            r2_set_error(error, error_len, "bsdthread_register init data is too small");
            return R2_ERR_UNSUPPORTED;
        }

        uint64_t version = 0;
        r2_status status =
            read_u64(machine, pthread_init_data, &version, error, error_len);
        if (status != R2_OK) {
            return status;
        }
        if (version != pthread_init_data_size) {
            r2_set_error(error, error_len, "bsdthread_register init data version mismatch");
            return R2_ERR_UNSUPPORTED;
        }

        if (pthread_init_data_size >= R2_PTHREAD_REG_DISPATCH_QUEUE_OFFSET + 8u) {
            status = read_u64(machine,
                              pthread_init_data + R2_PTHREAD_REG_DISPATCH_QUEUE_OFFSET,
                              &registration_dispatchqueue_offset,
                              error,
                              error_len);
            if (status != R2_OK) {
                return status;
            }
        }
        if (pthread_init_data_size >= R2_PTHREAD_REG_TSD_OFFSET + 4u) {
            uint64_t raw_tsd_offset = 0;
            status = read_mem(machine,
                              pthread_init_data + R2_PTHREAD_REG_TSD_OFFSET,
                              4,
                              &raw_tsd_offset,
                              error,
                              error_len);
            if (status != R2_OK) {
                return status;
            }
            tsd_offset = (uint32_t)raw_tsd_offset;
        }
        if (pthread_init_data_size >= R2_PTHREAD_REG_MACH_THREAD_SELF_OFFSET + 4u) {
            uint64_t raw_mach_thread_self_offset = 0;
            status = read_mem(machine,
                              pthread_init_data + R2_PTHREAD_REG_MACH_THREAD_SELF_OFFSET,
                              4,
                              &raw_mach_thread_self_offset,
                              error,
                              error_len);
            if (status != R2_OK) {
                return status;
            }
            mach_thread_self_offset = (uint32_t)raw_mach_thread_self_offset;
        }
        if (tsd_offset != 0 &&
            (pthsize < sizeof(uint64_t) || tsd_offset > pthsize - sizeof(uint64_t))) {
            tsd_offset = 0;
        }
        if (tsd_offset == 0 ||
            mach_thread_self_offset > pthsize - tsd_offset - sizeof(uint64_t)) {
            mach_thread_self_offset = 0;
        }

        status = write_u64(machine,
                           pthread_init_data,
                           R2_PTHREAD_REGISTRATION_DATA_SIZE,
                           error,
                           error_len);
        if (status != R2_OK) {
            return status;
        }
        if (pthread_init_data_size >= R2_PTHREAD_REG_TSD_OFFSET + 4u) {
            status = write_mem(machine,
                               pthread_init_data + R2_PTHREAD_REG_TSD_OFFSET,
                               4,
                               tsd_offset,
                               error,
                               error_len);
            if (status != R2_OK) {
                return status;
            }
        }
        if (pthread_init_data_size >= R2_PTHREAD_REG_MACH_THREAD_SELF_OFFSET + 4u) {
            status = write_mem(machine,
                               pthread_init_data + R2_PTHREAD_REG_MACH_THREAD_SELF_OFFSET,
                               4,
                               mach_thread_self_offset,
                               error,
                               error_len);
            if (status != R2_OK) {
                return status;
            }
        }
        if (pthread_init_data_size >= R2_PTHREAD_REG_MAIN_QOS + 8u) {
            status = write_u64(machine,
                               pthread_init_data + R2_PTHREAD_REG_MAIN_QOS,
                               0,
                               error,
                               error_len);
            if (status != R2_OK) {
                return status;
            }
        }
        if (pthread_init_data_size >= R2_PTHREAD_REG_STACK_HINT + 8u) {
            status = write_u64(machine,
                               pthread_init_data + R2_PTHREAD_REG_STACK_HINT,
                               R2_STACK_BASE + R2_STACK_SIZE,
                               error,
                               error_len);
            if (status != R2_OK) {
                return status;
            }
        }
        if (pthread_init_data_size >= R2_PTHREAD_REG_MUTEX_POLICY + 4u) {
            status = write_mem(machine,
                               pthread_init_data + R2_PTHREAD_REG_MUTEX_POLICY,
                               4,
                               0,
                               error,
                               error_len);
            if (status != R2_OK) {
                return status;
            }
        }
    }

    machine->pthread_registered = true;
    machine->pthread_threadstart = threadstart;
    machine->pthread_wqthread = wqthread;
    machine->pthread_pthsize = pthsize;
    machine->pthread_dispatchqueue_offset = registration_dispatchqueue_offset;
    machine->pthread_tsd_offset = tsd_offset;
    machine->pthread_mach_thread_self_offset = mach_thread_self_offset;
    machine->regs[R2_X86_RAX] = 0;
    return R2_OK;
}

static r2_status syscall_bsdthread_terminate(r2_machine *machine, uint64_t sema_or_ulock)
{
    r2_guest_thread *thread = &machine->threads[machine->current_thread];
    thread->state = R2_THREAD_EXITED;
    thread->wait_cv = 0;
    thread->wait_mutex = 0;
    thread->wait_relock_mutex = 0;
    thread->exit_value = machine->regs[R2_X86_RAX];
    if (sema_or_ulock != 0) {
        (void)post_cv_signal(machine, sema_or_ulock);
    }
    return R2_OK;
}

static r2_status syscall_thread_selfid(r2_machine *machine)
{
    machine->regs[R2_X86_RAX] = machine->threads[machine->current_thread].tid;
    return R2_OK;
}

static r2_status syscall_thread_set_tsd_base(r2_machine *machine, uint64_t tsd_base)
{
    machine->threads[machine->current_thread].gs_base = tsd_base;
    machine->regs[R2_X86_RAX] = R2_X86_USER_CTHREAD;
    return R2_OK;
}

static r2_status syscall_psynch_mutexwait(r2_machine *machine, uint64_t mutex)
{
    return block_current_thread_on_mutex(machine, mutex);
}

static r2_status syscall_psynch_mutexdrop(r2_machine *machine,
                                          uint64_t mutex,
                                          char *error,
                                          size_t error_len)
{
    return drop_mutex_for_current_thread(machine, mutex, error, error_len);
}

static r2_status syscall_psynch_cvwait(r2_machine *machine,
                                       uint64_t cv,
                                       uint64_t mutex,
                                       char *error,
                                       size_t error_len)
{
    if (consume_cv_signal(machine, cv)) {
        machine->regs[R2_X86_RAX] = 0;
        return R2_OK;
    }

    if (mutex != 0) {
        r2_mutex_state *state = find_mutex_state(machine, mutex, false);
        uint64_t tid = machine->threads[machine->current_thread].tid;
        if (state != NULL && state->locked && state->owner_tid != tid) {
            r2_set_error(error, error_len, "psynch_cvwait mutex is owned by another guest thread");
            return R2_ERR_UNSUPPORTED;
        }
        if (state != NULL && state->locked) {
            unlock_mutex_state(machine, state);
        }
    }

    r2_guest_thread *thread = &machine->threads[machine->current_thread];
    thread->state = R2_THREAD_BLOCKED_CV;
    thread->wait_cv = cv;
    thread->wait_relock_mutex = mutex;
    machine->regs[R2_X86_RAX] = 0;
    return R2_OK;
}

static r2_status syscall_psynch_cvsignal(r2_machine *machine, uint64_t cv)
{
    (void)post_cv_signal(machine, cv);
    machine->regs[R2_X86_RAX] = 0;
    return R2_OK;
}

static r2_status syscall_psynch_cvbroad(r2_machine *machine, uint64_t cv)
{
    (void)post_cv_broadcast(machine, cv);
    machine->regs[R2_X86_RAX] = 0;
    return R2_OK;
}

static r2_status zero_guest_memory(r2_machine *machine,
                                   uint64_t addr,
                                   size_t len,
                                   char *error,
                                   size_t error_len)
{
    for (size_t i = 0; i < len; i += 1) {
        r2_status status = write_guest_byte(machine, addr + i, 0, error, error_len);
        if (status != R2_OK) {
            return status;
        }
    }
    return R2_OK;
}

static r2_status build_signal_info(r2_machine *machine,
                                   uint64_t signum,
                                   uint64_t *siginfo_addr,
                                   uint64_t *ucontext_addr,
                                   char *error,
                                   size_t error_len)
{
    machine->regs[R2_X86_RSP] = (machine->regs[R2_X86_RSP] - R2_UCONTEXT_SIZE) & ~0xfull;
    *ucontext_addr = machine->regs[R2_X86_RSP];
    r2_status status =
        zero_guest_memory(machine, *ucontext_addr, R2_UCONTEXT_SIZE, error, error_len);
    if (status != R2_OK) {
        return status;
    }

    machine->regs[R2_X86_RSP] = (machine->regs[R2_X86_RSP] - R2_SIGINFO_SIZE) & ~0xfull;
    *siginfo_addr = machine->regs[R2_X86_RSP];
    status = zero_guest_memory(machine, *siginfo_addr, R2_SIGINFO_SIZE, error, error_len);
    if (status != R2_OK) {
        return status;
    }

    status = write_mem(machine, *siginfo_addr, 4, signum, error, error_len);
    if (status != R2_OK) {
        return status;
    }
    status = write_mem(machine, *siginfo_addr + 12, 4, (uint64_t)(uint32_t)getpid(), error, error_len);
    if (status != R2_OK) {
        return status;
    }
    return write_mem(machine, *siginfo_addr + 16, 4, (uint64_t)(uint32_t)getuid(), error, error_len);
}

static r2_status deliver_pending_signal(const r2_block *block,
                                        r2_machine *machine,
                                        size_t *pc,
                                        r2_result *result,
                                        bool *completed,
                                        char *error,
                                        size_t error_len)
{
    *completed = false;
    uint32_t deliverable = machine->pending_signals & ~machine->signal_mask;
    while (deliverable != 0) {
        uint64_t signum = 1;
        uint32_t bit = 1u;
        while ((deliverable & bit) == 0) {
            signum += 1;
            bit <<= 1;
        }

        machine->pending_signals &= ~bit;
        r2_sigaction_state action = machine->sigactions[signum];
        if (action.handler == R2_DARWIN_SIG_IGN) {
            deliverable = machine->pending_signals & ~machine->signal_mask;
            continue;
        }
        if (action.handler == R2_DARWIN_SIG_DFL) {
            if (default_signal_is_ignored(signum)) {
                deliverable = machine->pending_signals & ~machine->signal_mask;
                continue;
            }
            result->rax = 128u + signum;
            *completed = true;
            return R2_OK;
        }
        if (machine->signal_frame_count >= R2_MAX_SIGNAL_FRAMES) {
            r2_set_error(error, error_len, "too many nested signal handlers");
            return R2_ERR_UNSUPPORTED;
        }

        size_t handler_pc = 0;
        r2_status status =
            guest_code_addr_to_pc(block, machine, action.handler, &handler_pc, error, error_len);
        if (status != R2_OK) {
            return status;
        }

        r2_signal_frame *frame = &machine->signal_frames[machine->signal_frame_count];
        memcpy(frame->regs, machine->regs, sizeof(frame->regs));
        frame->flags = machine->flags;
        frame->signal_mask = machine->signal_mask;
        frame->return_pc = *pc;
        machine->signal_frame_count += 1;

        machine->signal_mask |= action.mask;
        if ((action.flags & R2_DARWIN_SA_NODEFER) == 0) {
            machine->signal_mask |= bit;
        }
        if ((action.flags & R2_DARWIN_SA_RESETHAND) != 0) {
            memset(&machine->sigactions[signum], 0, sizeof(machine->sigactions[signum]));
        }

        machine->regs[R2_X86_RDI] = signum;
        if ((action.flags & R2_DARWIN_SA_SIGINFO) != 0) {
            uint64_t siginfo_addr = 0;
            uint64_t ucontext_addr = 0;
            status =
                build_signal_info(machine, signum, &siginfo_addr, &ucontext_addr, error, error_len);
            if (status != R2_OK) {
                return status;
            }
            machine->regs[R2_X86_RSI] = siginfo_addr;
            machine->regs[R2_X86_RDX] = ucontext_addr;
        }

        status = push_u64(machine, R2_SIGNAL_RETURN_SENTINEL, error, error_len);
        if (status != R2_OK) {
            return status;
        }
        *pc = handler_pc;
        return R2_OK;
    }

    return R2_OK;
}

static r2_status restore_signal_frame(r2_machine *machine,
                                      size_t *pc,
                                      char *error,
                                      size_t error_len)
{
    if (machine->signal_frame_count == 0) {
        r2_set_error(error, error_len, "signal return without an active signal frame");
        return R2_ERR_DECODE;
    }

    machine->signal_frame_count -= 1;
    const r2_signal_frame *frame = &machine->signal_frames[machine->signal_frame_count];
    memcpy(machine->regs, frame->regs, sizeof(machine->regs));
    machine->flags = frame->flags;
    machine->signal_mask = frame->signal_mask;
    *pc = frame->return_pc;
    return R2_OK;
}

r2_status r2_interp_block_with_env(const r2_block *block,
                                   const r2_interp_env *env,
                                   r2_result *result,
                                   char *error,
                                   size_t error_len)
{
    if (block == NULL || result == NULL) {
        r2_set_error(error, error_len, "null argument");
        return R2_ERR_BAD_ARGUMENT;
    }

    r2_machine machine;
    memset(&machine, 0, sizeof(machine));
    machine.env = env;
    machine.next_mmap_base = R2_MMAP_BASE;
    machine.thread_count = 1;
    machine.current_thread = 0;
    machine.next_tid = 2;
    machine.next_thread_port = R2_MAIN_THREAD_PORT + 1u;

    r2_guest_thread *main_thread = &machine.threads[0];
    main_thread->state = R2_THREAD_RUNNABLE;
    main_thread->tid = 1;
    main_thread->thread_port = R2_MAIN_THREAD_PORT;
    main_thread->stack_base = R2_STACK_BASE;
    main_thread->stack_size = R2_STACK_SIZE;
    main_thread->stack = calloc(1, R2_STACK_SIZE);
    if (main_thread->stack == NULL) {
        r2_set_error(error, error_len, "out of memory allocating emulated stack");
        return R2_ERR_NO_MEMORY;
    }
    if (env != NULL && env->has_initial_regs) {
        memcpy(machine.regs, env->initial_regs, sizeof(machine.regs));
    }

    machine.regs[R2_X86_RSP] = R2_STACK_BASE + R2_STACK_SIZE - 8;
    r2_status status =
        write_u64(&machine, machine.regs[R2_X86_RSP], R2_RETURN_SENTINEL, error, error_len);
    if (status != R2_OK) {
        destroy_machine(&machine);
        return status;
    }

    size_t pc = 0;
    size_t steps = 0;
    while (true) {
        status = schedule_next_thread(&machine, &pc, error, error_len);
        if (status != R2_OK) {
            destroy_machine(&machine);
            return status;
        }

        if (pc >= block->count) {
            r2_set_error(error, error_len, "guest thread ran off the decoded block");
            destroy_machine(&machine);
            return R2_ERR_DECODE;
        }

        if (steps++ > R2_MAX_STEPS) {
            r2_set_error(error, error_len, "interpreter step limit reached");
            destroy_machine(&machine);
            return R2_ERR_UNSUPPORTED;
        }

        bool completed = false;
        status = deliver_pending_signal(block, &machine, &pc, result, &completed, error, error_len);
        if (status != R2_OK) {
            destroy_machine(&machine);
            return status;
        }
        if (completed) {
            destroy_machine(&machine);
            return R2_OK;
        }

        const r2_insn *insn = &block->insns[pc];
        size_t next_pc = pc + 1;

        switch (insn->op) {
        case R2_OP_NOP:
            break;
        case R2_OP_MOV_IMM:
            set_reg_width(machine.regs, insn->dst, (uint64_t)insn->imm, insn->width);
            break;
        case R2_OP_MOV_REG:
            set_reg_width(machine.regs, insn->dst, machine.regs[insn->src], insn->width);
            break;
        case R2_OP_MOV_LOAD: {
            uint64_t addr = eval_mem_addr(insn, &machine);
            uint64_t value = 0;
            status = read_mem(&machine, addr, insn->width, &value, error, error_len);
            if (status != R2_OK) {
                destroy_machine(&machine);
                return status;
            }
            set_reg_width(machine.regs, insn->dst, value, insn->width);
            break;
        }
        case R2_OP_MOV_STORE: {
            uint64_t addr = eval_mem_addr(insn, &machine);
            status =
                write_mem(&machine, addr, insn->width, machine.regs[insn->src], error, error_len);
            if (status != R2_OK) {
                destroy_machine(&machine);
                return status;
            }
            break;
        }
        case R2_OP_MOV_MEM_IMM: {
            uint64_t addr = eval_mem_addr(insn, &machine);
            status = write_mem(&machine, addr, insn->width, (uint64_t)insn->imm, error, error_len);
            if (status != R2_OK) {
                destroy_machine(&machine);
                return status;
            }
            break;
        }
        case R2_OP_LEA:
            machine.regs[insn->dst] = eval_mem_addr(insn, &machine);
            break;
        case R2_OP_ADD_IMM: {
            uint64_t lhs = machine.regs[insn->dst];
            uint64_t rhs = (uint64_t)insn->imm;
            uint64_t value = normalize_value(lhs + rhs, insn->width);
            set_reg_width(machine.regs, insn->dst, value, insn->width);
            set_add_flags(&machine.flags, lhs, rhs, value, insn->width);
            break;
        }
        case R2_OP_ADD_REG: {
            uint64_t lhs = machine.regs[insn->dst];
            uint64_t rhs = machine.regs[insn->src];
            uint64_t value = normalize_value(lhs + rhs, insn->width);
            set_reg_width(machine.regs, insn->dst, value, insn->width);
            set_add_flags(&machine.flags, lhs, rhs, value, insn->width);
            break;
        }
        case R2_OP_SUB_IMM: {
            uint64_t lhs = machine.regs[insn->dst];
            uint64_t rhs = (uint64_t)insn->imm;
            uint64_t value = normalize_value(lhs - rhs, insn->width);
            set_reg_width(machine.regs, insn->dst, value, insn->width);
            set_sub_flags(&machine.flags, lhs, rhs, value, insn->width);
            break;
        }
        case R2_OP_SUB_REG: {
            uint64_t lhs = machine.regs[insn->dst];
            uint64_t rhs = machine.regs[insn->src];
            uint64_t value = normalize_value(lhs - rhs, insn->width);
            set_reg_width(machine.regs, insn->dst, value, insn->width);
            set_sub_flags(&machine.flags, lhs, rhs, value, insn->width);
            break;
        }
        case R2_OP_XOR_REG: {
            uint64_t value =
                normalize_value(machine.regs[insn->dst] ^ machine.regs[insn->src], insn->width);
            set_reg_width(machine.regs, insn->dst, value, insn->width);
            set_logic_flags_width(&machine.flags, value, insn->width);
            break;
        }
        case R2_OP_CMP_IMM: {
            uint64_t lhs = machine.regs[insn->dst];
            uint64_t rhs = (uint64_t)insn->imm;
            set_sub_flags(&machine.flags, lhs, rhs, lhs - rhs, insn->width);
            break;
        }
        case R2_OP_CMP_REG: {
            uint64_t lhs = machine.regs[insn->dst];
            uint64_t rhs = machine.regs[insn->src];
            set_sub_flags(&machine.flags, lhs, rhs, lhs - rhs, insn->width);
            break;
        }
        case R2_OP_TEST_REG: {
            uint64_t value =
                normalize_value(machine.regs[insn->dst] & machine.regs[insn->src], insn->width);
            set_logic_flags_width(&machine.flags, value, insn->width);
            break;
        }
        case R2_OP_PUSH_REG:
            status = push_u64(&machine, machine.regs[insn->src], error, error_len);
            if (status != R2_OK) {
                destroy_machine(&machine);
                return status;
            }
            break;
        case R2_OP_PUSH_IMM:
            status = push_u64(&machine, (uint64_t)insn->imm, error, error_len);
            if (status != R2_OK) {
                destroy_machine(&machine);
                return status;
            }
            break;
        case R2_OP_POP_REG:
            status = pop_u64(&machine, &machine.regs[insn->dst], error, error_len);
            if (status != R2_OK) {
                destroy_machine(&machine);
                return status;
            }
            break;
        case R2_OP_CALL:
            status = push_u64(&machine, insn->x86_offset + insn->x86_len, error, error_len);
            if (status != R2_OK) {
                destroy_machine(&machine);
                return status;
            }
            status = jump_to_target(block, insn->target, &pc, error, error_len);
            if (status != R2_OK) {
                destroy_machine(&machine);
                return status;
            }
            continue;
        case R2_OP_JMP:
            status = jump_to_target(block, insn->target, &pc, error, error_len);
            if (status != R2_OK) {
                destroy_machine(&machine);
                return status;
            }
            continue;
        case R2_OP_JCC:
            if (eval_cc(insn->cc, &machine.flags)) {
                status = jump_to_target(block, insn->target, &pc, error, error_len);
                if (status != R2_OK) {
                    destroy_machine(&machine);
                    return status;
                }
                continue;
            }
            break;
        case R2_OP_SYSCALL:
            if (machine.regs[R2_X86_RAX] == R2_MACOS_SYS_READ) {
                status = syscall_read(&machine,
                                      machine.regs[R2_X86_RDI],
                                      machine.regs[R2_X86_RSI],
                                      machine.regs[R2_X86_RDX],
                                      error,
                                      error_len);
                if (status != R2_OK) {
                    destroy_machine(&machine);
                    return status;
                }
            } else if (machine.regs[R2_X86_RAX] == R2_MACOS_SYS_WRITE) {
                status = syscall_write(&machine,
                                       machine.regs[R2_X86_RDI],
                                       machine.regs[R2_X86_RSI],
                                       machine.regs[R2_X86_RDX],
                                       error,
                                       error_len);
                if (status != R2_OK) {
                    destroy_machine(&machine);
                    return status;
                }
            } else if (machine.regs[R2_X86_RAX] == R2_MACOS_SYS_OPEN) {
                status = syscall_open(&machine,
                                      machine.regs[R2_X86_RDI],
                                      machine.regs[R2_X86_RSI],
                                      machine.regs[R2_X86_RDX],
                                      error,
                                      error_len);
                if (status != R2_OK) {
                    destroy_machine(&machine);
                    return status;
                }
            } else if (machine.regs[R2_X86_RAX] == R2_MACOS_SYS_CLOSE) {
                status = syscall_close(&machine, machine.regs[R2_X86_RDI], error, error_len);
                if (status != R2_OK) {
                    destroy_machine(&machine);
                    return status;
                }
            } else if (machine.regs[R2_X86_RAX] == R2_MACOS_SYS_GETPID) {
                status = syscall_getpid(&machine);
                if (status != R2_OK) {
                    destroy_machine(&machine);
                    return status;
                }
            } else if (machine.regs[R2_X86_RAX] == R2_MACOS_SYS_KILL) {
                status = syscall_kill(&machine,
                                      machine.regs[R2_X86_RDI],
                                      machine.regs[R2_X86_RSI],
                                      error,
                                      error_len);
                if (status != R2_OK) {
                    destroy_machine(&machine);
                    return status;
                }
            } else if (machine.regs[R2_X86_RAX] == R2_MACOS_SYS_SIGACTION) {
                status = syscall_sigaction(&machine,
                                           machine.regs[R2_X86_RDI],
                                           machine.regs[R2_X86_RSI],
                                           machine.regs[R2_X86_RDX],
                                           error,
                                           error_len);
                if (status != R2_OK) {
                    destroy_machine(&machine);
                    return status;
                }
            } else if (machine.regs[R2_X86_RAX] == R2_MACOS_SYS_SIGPROCMASK) {
                status = syscall_sigprocmask(&machine,
                                             machine.regs[R2_X86_RDI],
                                             machine.regs[R2_X86_RSI],
                                             machine.regs[R2_X86_RDX],
                                             error,
                                             error_len);
                if (status != R2_OK) {
                    destroy_machine(&machine);
                    return status;
                }
            } else if (machine.regs[R2_X86_RAX] == R2_MACOS_SYS_PSYNCH_MUTEXWAIT) {
                status = syscall_psynch_mutexwait(&machine, machine.regs[R2_X86_RDI]);
                if (status != R2_OK) {
                    destroy_machine(&machine);
                    return status;
                }
            } else if (machine.regs[R2_X86_RAX] == R2_MACOS_SYS_PSYNCH_MUTEXDROP) {
                status =
                    syscall_psynch_mutexdrop(&machine, machine.regs[R2_X86_RDI], error, error_len);
                if (status != R2_OK) {
                    destroy_machine(&machine);
                    return status;
                }
            } else if (machine.regs[R2_X86_RAX] == R2_MACOS_SYS_PSYNCH_CVBROAD) {
                status = syscall_psynch_cvbroad(&machine, machine.regs[R2_X86_RDI]);
                if (status != R2_OK) {
                    destroy_machine(&machine);
                    return status;
                }
            } else if (machine.regs[R2_X86_RAX] == R2_MACOS_SYS_PSYNCH_CVSIGNAL) {
                status = syscall_psynch_cvsignal(&machine, machine.regs[R2_X86_RDI]);
                if (status != R2_OK) {
                    destroy_machine(&machine);
                    return status;
                }
            } else if (machine.regs[R2_X86_RAX] == R2_MACOS_SYS_PSYNCH_CVWAIT) {
                status = syscall_psynch_cvwait(&machine,
                                               machine.regs[R2_X86_RDI],
                                               machine.regs[R2_X86_R10],
                                               error,
                                               error_len);
                if (status != R2_OK) {
                    destroy_machine(&machine);
                    return status;
                }
            } else if (machine.regs[R2_X86_RAX] == R2_MACOS_SYS_FSTAT) {
                status = syscall_fstat(&machine,
                                       machine.regs[R2_X86_RDI],
                                       machine.regs[R2_X86_RSI],
                                       error,
                                       error_len);
                if (status != R2_OK) {
                    destroy_machine(&machine);
                    return status;
                }
            } else if (machine.regs[R2_X86_RAX] == R2_MACOS_SYS_MMAP) {
                status = syscall_mmap(&machine,
                                      machine.regs[R2_X86_RDI],
                                      machine.regs[R2_X86_RSI],
                                      machine.regs[R2_X86_RDX],
                                      machine.regs[R2_X86_R10],
                                      machine.regs[R2_X86_R8],
                                      machine.regs[R2_X86_R9],
                                      error,
                                      error_len);
                if (status != R2_OK) {
                    destroy_machine(&machine);
                    return status;
                }
            } else if (machine.regs[R2_X86_RAX] == R2_MACOS_SYS_BSDTHREAD_CREATE) {
                status = syscall_bsdthread_create(block,
                                                  &machine,
                                                  machine.regs[R2_X86_RDI],
                                                  machine.regs[R2_X86_RSI],
                                                  machine.regs[R2_X86_RDX],
                                                  machine.regs[R2_X86_R10],
                                                  machine.regs[R2_X86_R8],
                                                  error,
                                                  error_len);
                if (status != R2_OK) {
                    destroy_machine(&machine);
                    return status;
                }
            } else if (machine.regs[R2_X86_RAX] == R2_MACOS_SYS_BSDTHREAD_TERMINATE) {
                status = syscall_bsdthread_terminate(&machine, machine.regs[R2_X86_R10]);
                if (status != R2_OK) {
                    destroy_machine(&machine);
                    return status;
                }
            } else if (machine.regs[R2_X86_RAX] == R2_MACOS_SYS_BSDTHREAD_REGISTER) {
                status = syscall_bsdthread_register(&machine,
                                                    machine.regs[R2_X86_RDI],
                                                    machine.regs[R2_X86_RSI],
                                                    machine.regs[R2_X86_RDX],
                                                    machine.regs[R2_X86_R10],
                                                    machine.regs[R2_X86_R8],
                                                    machine.regs[R2_X86_R9],
                                                    error,
                                                    error_len);
                if (status != R2_OK) {
                    destroy_machine(&machine);
                    return status;
                }
            } else if (machine.regs[R2_X86_RAX] == R2_MACOS_SYS_THREAD_SELFID) {
                status = syscall_thread_selfid(&machine);
                if (status != R2_OK) {
                    destroy_machine(&machine);
                    return status;
                }
            } else if (machine.regs[R2_X86_RAX] == R2_MACOS_SYS_THREAD_SET_TSD_BASE) {
                status = syscall_thread_set_tsd_base(&machine, machine.regs[R2_X86_RDI]);
                if (status != R2_OK) {
                    destroy_machine(&machine);
                    return status;
                }
            } else if (machine.regs[R2_X86_RAX] == R2_MACOS_SYS_EXIT) {
                result->rax = machine.regs[R2_X86_RDI];
                destroy_machine(&machine);
                return R2_OK;
            } else {
                r2_set_error(error,
                             error_len,
                             "unsupported macOS x86_64 syscall 0x%llx",
                             (unsigned long long)machine.regs[R2_X86_RAX]);
                destroy_machine(&machine);
                return R2_ERR_UNSUPPORTED;
            }
            break;
        case R2_OP_LEAVE: {
            uint64_t value = 0;
            machine.regs[R2_X86_RSP] = machine.regs[R2_X86_RBP];
            status = pop_u64(&machine, &value, error, error_len);
            if (status != R2_OK) {
                destroy_machine(&machine);
                return status;
            }
            machine.regs[R2_X86_RBP] = value;
            break;
        }
        case R2_OP_RET: {
            uint64_t ret_offset = 0;
            status = pop_u64(&machine, &ret_offset, error, error_len);
            if (status != R2_OK) {
                destroy_machine(&machine);
                return status;
            }
            if (ret_offset == R2_RETURN_SENTINEL) {
                result->rax = machine.regs[R2_X86_RAX];
                destroy_machine(&machine);
                return R2_OK;
            }
            if (ret_offset == R2_THREAD_RETURN_SENTINEL) {
                r2_guest_thread *thread = &machine.threads[machine.current_thread];
                thread->state = R2_THREAD_EXITED;
                thread->exit_value = machine.regs[R2_X86_RAX];
                continue;
            }
            if (ret_offset == R2_SIGNAL_RETURN_SENTINEL) {
                status = restore_signal_frame(&machine, &pc, error, error_len);
                if (status != R2_OK) {
                    destroy_machine(&machine);
                    return status;
                }
                continue;
            }
            if (ret_offset > SIZE_MAX) {
                r2_set_error(error, error_len, "return address is too large");
                destroy_machine(&machine);
                return R2_ERR_DECODE;
            }
            status = jump_to_target(block, (size_t)ret_offset, &pc, error, error_len);
            if (status != R2_OK) {
                destroy_machine(&machine);
                return status;
            }
            continue;
        }
        default:
            r2_set_error(error, error_len, "unknown decoded op at instruction %zu", pc);
            destroy_machine(&machine);
            return R2_ERR_UNSUPPORTED;
        }

        pc = next_pc;
    }

    destroy_machine(&machine);
    r2_set_error(error, error_len, "decoded block ran off the end without ret");
    return R2_ERR_DECODE;
}

r2_status r2_interp_block(const r2_block *block,
                          r2_result *result,
                          char *error,
                          size_t error_len)
{
    r2_interp_env env = {0};
    return r2_interp_block_with_env(block, &env, result, error, error_len);
}
