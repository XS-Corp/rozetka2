#include "internal.h"
#include "x86_signal.h"

#include <errno.h>
#include <fcntl.h>
#include <fenv.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

#define R2_STACK_SIZE (1024u * 1024u)
#define R2_STACK_BASE 0x700000000000ull
#define R2_RETURN_SENTINEL UINT64_MAX
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
#define R2_UCONTEXT_SIZE R2_X86_UC_SIZE
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
    bool pf;
    bool af;
    uint64_t extra;
} r2_flags;

typedef struct {
    uint64_t handler;
    uint64_t trampoline;
    uint32_t mask;
    uint32_t flags;
} r2_sigaction_state;

typedef struct {
    uint64_t regs[16];
    uint8_t xmm[16][16];
    r2_flags flags;
    r2_x87_state x87;
    uint32_t signal_mask;
    size_t return_pc;
    uint64_t context_address;
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
    uint8_t xmm[16][16];
    r2_flags flags;
    r2_x87_state x87;
    uint32_t signal_mask;
    r2_signal_frame signal_frames[R2_MAX_SIGNAL_FRAMES];
    size_t signal_frame_count;
    size_t pc;
    uint8_t *stack;
    bool borrowed_stack;
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
    uint8_t xmm[16][16];
    r2_flags flags;
    uint32_t signal_mask;
    r2_signal_frame signal_frames[R2_MAX_SIGNAL_FRAMES];
    size_t signal_frame_count;
    const r2_interp_env *env;
    r2_native_execution previous_execution;
    r2_native_fast_state fast;
    uint32_t mxcsr;
    r2_x87_state x87;
    uint64_t current_address, instruction_sp;
    r2_flags instruction_flags;
    bool memory_fault, fault_write, delivering_fault, instruction_active;
    uint64_t fault_address;
    int fault_code;
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

static void destroy_machine(r2_machine *machine, r2_status status, const char *error)
{
    if (machine->env != NULL && machine->env->native != NULL && status != R2_OK)
        r2_native_request_stop(machine->env->native, status, error);
    for (size_t i = 0; i < machine->runtime_region_count; i += 1) {
        free(machine->runtime_regions[i].data);
    }
    for (size_t i = 0; i < R2_MAX_GUEST_THREADS; i += 1) {
        if (machine->threads[i].borrowed_stack) continue;
        if (machine->env != NULL && machine->env->native != NULL && i == 0)
            { if (!r2_native_stopping(machine->env->native))
                r2_native_stack_release(machine->env->native, machine->threads[i].stack, (size_t)machine->threads[i].stack_size); }
        else free(machine->threads[i].stack);
    }
    if (machine->env != NULL && machine->env->native != NULL)
        r2_native_execution_leave(machine->env->native, &machine->previous_execution);
}

static void set_logic_flags(r2_flags *flags, uint64_t value)
{
    flags->zf = value == 0;
    flags->sf = (value >> 63) != 0;
    flags->cf = false;
    flags->of = false;
    flags->af = false;
    flags->pf = (__builtin_popcount((unsigned)(value & 0xffu)) & 1) == 0;
}

static uint64_t width_mask(uint8_t width)
{
    return width == 8 ? UINT64_MAX : (UINT64_C(1) << (width * 8u)) - 1u;
}

static uint64_t width_sign_bit(uint8_t width)
{
    return UINT64_C(1) << (width * 8u - 1u);
}

static uint64_t normalize_value(uint64_t value, uint8_t width)
{
    return value & width_mask(width);
}

static void set_reg_width(uint64_t regs[16], uint8_t reg, uint64_t value, uint8_t width)
{
    if (width < 4) regs[reg] = (regs[reg] & ~width_mask(width)) | normalize_value(value, width);
    else regs[reg] = normalize_value(value, width);
}

static void set_operand_reg(uint64_t regs[16], uint8_t reg, bool high8,
                             uint64_t value, uint8_t width)
{
    if (high8) regs[reg] = (regs[reg] & ~UINT64_C(0xff00)) | ((value & 255) << 8);
    else set_reg_width(regs, reg, value, width);
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
    flags->af = ((lhs ^ rhs ^ result) & 16) != 0;
    flags->of = (((~(lhs ^ rhs)) & (lhs ^ result)) & sign) != 0;
    flags->pf = (__builtin_popcount((unsigned)(result & 0xffu)) & 1) == 0;
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
    flags->af = ((lhs ^ rhs ^ result) & 16) != 0;
    flags->of = (((lhs ^ rhs) & (lhs ^ result)) & sign) != 0;
    flags->pf = (__builtin_popcount((unsigned)(result & 0xffu)) & 1) == 0;
}

static uint64_t read_flags(const r2_flags *flags)
{
    return UINT64_C(2) | flags->extra | (uint64_t)flags->cf | ((uint64_t)flags->pf << 2) |
        ((uint64_t)flags->af << 4) | ((uint64_t)flags->zf << 6) | ((uint64_t)flags->sf << 7) |
        ((uint64_t)flags->of << 11);
}

static void cpu_identification(uint64_t regs[16])
{
    uint32_t leaf = (uint32_t)regs[R2_X86_RAX], subleaf = (uint32_t)regs[R2_X86_RCX];
    uint32_t out[4] = {0};
    switch (leaf) {
    case 0: out[0] = 4; out[1] = 0x756e6547; out[2] = 0x6c65746e; out[3] = 0x49656e69; break;
    case 1:
        out[0] = 0x000006f0; out[1] = (1u << 16) | (8u << 8);
        out[3] = (1u << 0) | (1u << 4) | (1u << 8) | (1u << 15) | (1u << 19) |
                 (1u << 24) | (1u << 25) | (1u << 26);
        break;
    case 4:
        if (subleaf < 3) {
            out[0] = (subleaf == 0 ? 1u : subleaf == 1 ? 2u : 3u) | ((subleaf < 2 ? 1u : 2u) << 5) | (1u << 8);
            out[1] = (7u << 22) | 63; out[2] = subleaf < 2 ? 63 : 511;
        }
        break;
    case 0x80000000: out[0] = 0x80000008; break;
    case 0x80000001: out[3] = (1u << 11) | (1u << 20) | (1u << 29); break;
    case 0x80000002: case 0x80000003: case 0x80000004: {
        const char brand[48] = "rozetka2 Virtual x86-64 SSE2 Processor";
        memcpy(out, brand + (leaf - 0x80000002) * 16, 16); break;
    }
    case 0x80000008: out[0] = 48u | (48u << 8); break;
    default: break;
    }
    regs[R2_X86_RAX] = out[0]; regs[R2_X86_RBX] = out[1];
    regs[R2_X86_RCX] = out[2]; regs[R2_X86_RDX] = out[3];
}

static __int128 signed_value(uint64_t value, uint8_t width)
{
    value = normalize_value(value, width);
    return (__int128)value - ((value & width_sign_bit(width)) ? ((__int128)1 << (width * 8u)) : 0);
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
    case R2_CC_O: return flags->of;
    case R2_CC_NO: return !flags->of;
    case R2_CC_S: return flags->sf;
    case R2_CC_NS: return !flags->sf;
    case R2_CC_P: return flags->pf;
    case R2_CC_NP: return !flags->pf;
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
    case R2_CC_B:
        return flags->cf;
    case R2_CC_AE:
        return !flags->cf;
    case R2_CC_BE:
        return flags->cf || flags->zf;
    case R2_CC_A:
        return !flags->cf && !flags->zf;
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

    size_t region_count = machine->env->dynamic_region_count != NULL
        ? *machine->env->dynamic_region_count : machine->env->region_count;
    for (size_t i = 0; i < region_count; i += 1) {
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

static uint8_t *memory_pointer(r2_machine *machine, uint64_t addr, size_t width,
                                bool writable, char *error, size_t error_len)
{
    /* Native guest addresses already are host addresses. The native cache
     * includes pinned image permissions and is invalidated on VM changes. */
    if (machine->env != NULL && machine->env->native != NULL && machine->thread_count == 1 && machine->runtime_region_count == 0) {
        void *pointer = r2_native_pointer(machine->env->native, addr, width, writable, error, error_len);
        if (pointer == NULL) {
            machine->memory_fault = true; machine->fault_address = addr; machine->fault_write = writable;
            machine->fault_code = r2_native_fault_code(machine->env->native, addr);
            char detail[R2_ERROR_CAP]; snprintf(detail, sizeof(detail), "%s", error != NULL ? error : "guest memory access fault");
            r2_set_error(error, error_len, "RIP=0x%llx: %s", (unsigned long long)machine->current_address, detail);
        }
        return pointer;
    }
    size_t off = 0, thread = 0;
    const r2_memory_region *region = NULL;
    if (stack_contains(machine, addr, width, &thread, &off)) {
        if (machine->env != NULL && machine->env->native != NULL)
            return r2_native_pointer(machine->env->native, addr, width, writable, error, error_len);
        return machine->threads[thread].stack + off;
    }
    if (mapped_offset(machine, addr, width, &region, &off)) {
        if (writable && !region->writable) {
            r2_set_error(error, error_len, "address 0x%016llx is read-only", (unsigned long long)addr);
            return NULL;
        }
        return region->data + off;
    }
    if (machine->env != NULL && machine->env->native != NULL)
        return r2_native_pointer(machine->env->native, addr, width, writable, error, error_len);
    r2_set_error(error, error_len, "address 0x%016llx is not mapped", (unsigned long long)addr);
    return NULL;
}

static r2_status read_mem(r2_machine *machine, uint64_t addr, uint8_t width,
                          uint64_t *out, char *error, size_t error_len)
{
    uint8_t *bytes = memory_pointer(machine, addr, width, false, error, error_len);
    if (bytes == NULL) return R2_ERR_UNSUPPORTED;
    uint64_t value = 0;
    for (size_t i = 0; i < width; i += 1) value |= (uint64_t)bytes[i] << (i * 8);
    *out = value;
    return R2_OK;
}

static r2_status write_mem(r2_machine *machine, uint64_t addr, uint8_t width,
                           uint64_t value, char *error, size_t error_len)
{
    uint8_t *bytes = memory_pointer(machine, addr, width, true, error, error_len);
    if (bytes == NULL) return R2_ERR_UNSUPPORTED;
    for (size_t i = 0; i < width; i += 1) bytes[i] = (uint8_t)(value >> (i * 8));
    return R2_OK;
}

static uint64_t atomic_value(const r2_insn *insn, uint64_t old, uint64_t source, uint64_t carry)
{
    if (insn->op == R2_OP_INC) return old + 1;
    if (insn->op == R2_OP_DEC) return old - 1;
    if (insn->op == R2_OP_NEG) return 0 - old;
    if (insn->op == R2_OP_NOT) return ~old;
    switch (insn->alu) {
    case R2_ALU_ADD: return old + source;
    case R2_ALU_ADC: return old + source + carry;
    case R2_ALU_SUB: return old - source;
    case R2_ALU_SBB: return old - source - carry;
    case R2_ALU_OR: return old | source;
    case R2_ALU_AND: return old & source;
    case R2_ALU_XOR: return old ^ source;
    default: return old;
    }
}

static r2_status atomic_memory(r2_machine *machine, const r2_insn *insn,
                                uint64_t addr, uint64_t source, uint64_t expected,
                                uint64_t *old, char *error, size_t error_len)
{
    uint8_t *bytes = memory_pointer(machine, addr, insn->width, true, error, error_len);
    if (bytes == NULL) return R2_ERR_UNSUPPORTED;
    /* ARM atomics require alignment. Do not turn an unsupported unaligned
     * guest atomic into a host fault or a non-atomic load/store pair. */
    if ((uintptr_t)bytes % insn->width != 0) {
        r2_set_error(error, error_len, "unaligned atomic memory operand");
        return R2_ERR_UNSUPPORTED;
    }
#define ATOMIC_WIDTH(type) do { \
    type *pointer = (type *)(void *)bytes; \
    if (insn->op == R2_OP_XCHG) *old = __atomic_exchange_n(pointer, (type)source, __ATOMIC_SEQ_CST); \
    else if (insn->op == R2_OP_XADD) *old = __atomic_fetch_add(pointer, (type)source, __ATOMIC_SEQ_CST); \
    else if (insn->op == R2_OP_CMPXCHG) { \
        type compare = (type)expected; \
        __atomic_compare_exchange_n(pointer, &compare, (type)source, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); \
        *old = compare; \
    } else { \
        type compare = __atomic_load_n(pointer, __ATOMIC_SEQ_CST), desired; \
        do { desired = (type)atomic_value(insn, compare, source, expected); } \
        while (!__atomic_compare_exchange_n(pointer, &compare, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)); \
        *old = compare; \
    } \
} while (0)
    switch (insn->width) {
    case 1: ATOMIC_WIDTH(uint8_t); break;
    case 2: ATOMIC_WIDTH(uint16_t); break;
    case 4: ATOMIC_WIDTH(uint32_t); break;
    case 8: ATOMIC_WIDTH(uint64_t); break;
    default: return R2_ERR_UNSUPPORTED;
    }
#undef ATOMIC_WIDTH
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
        size_t region_count = machine->env->dynamic_region_count != NULL
            ? *machine->env->dynamic_region_count : machine->env->region_count;
        for (size_t i = 0; i < region_count; i += 1) {
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

static uint64_t instruction_next_guest_addr(const r2_insn *insn,
                                            const r2_machine *machine)
{
    uint64_t rip_base = machine->env == NULL ? 0 : machine->env->rip_base;
    return rip_base + insn->x86_offset + (uint64_t)insn->x86_len;
}

static r2_status eval_indirect_target(const r2_insn *insn,
                                      r2_machine *machine,
                                      uint64_t *target,
                                      char *error,
                                      size_t error_len)
{
    if (!insn->mem.present) {
        *target = machine->regs[insn->src];
        return R2_OK;
    }

    uint64_t operand_addr = eval_mem_addr(insn, machine);
    return read_u64(machine, operand_addr, target, error, error_len);
}

static r2_status sse_source(r2_machine *machine, const r2_insn *insn,
                            uint8_t source[16], size_t length, char *error, size_t error_len)
{
    if (insn->mem.present) {
        uint64_t address = eval_mem_addr(insn, machine);
        if (length == 16 && (address & 15u) != 0) {
            r2_set_error(error, error_len, "unaligned guest packed-SSE operand"); return R2_ERR_UNSUPPORTED;
        }
        uint8_t *bytes = memory_pointer(machine, address, length, false, error, error_len);
        if (bytes == NULL) return R2_ERR_UNSUPPORTED;
        memcpy(source, bytes, length);
    } else memcpy(source, machine->xmm[insn->src], length);
    return R2_OK;
}

static void sse_denormals(r2_machine *machine,uint8_t *bytes,size_t width,size_t length,bool input)
{
    uint32_t mxcsr=machine->mxcsr;
    if(!input&&!(mxcsr&0x8000))return;
    uint32_t flags=0;
    for(size_t i=0;i<length;i+=width) {
        uint64_t bits=0;memcpy(&bits,bytes+i,width);
        uint64_t exponent=width==4?UINT64_C(0x7f800000):UINT64_C(0x7ff0000000000000);
        uint64_t fraction=width==4?UINT64_C(0x007fffff):UINT64_C(0x000fffffffffffff);
        if((bits&exponent)||(bits&fraction)==0)continue;
        if(input?(mxcsr&0x40):(mxcsr&0x8000)) {
            bits&=width==4?UINT64_C(0x80000000):UINT64_C(0x8000000000000000);memcpy(bytes+i,&bits,width);
            if(!input)flags|=0x30; /* FTZ raises masked underflow and precision. */
        } else if(input)flags|=2;
    }
    if(flags) {
        machine->mxcsr=mxcsr|flags;
        if(machine->env&&machine->env->native)
            r2_native_set_mxcsr(machine->env->native,r2_native_get_mxcsr(machine->env->native)|flags);
    }
}

static r2_status execute_sse(r2_machine *machine, const r2_insn *insn, char *error, size_t error_len)
{
    uint8_t source[16] = {0}, original[16];
    uint8_t *destination = machine->xmm[insn->dst];
    memcpy(original, destination, 16);
    r2_status status;
    if (insn->op == R2_OP_SSE_GP) {
        if (insn->subop == 0xd7) {
            uint32_t value = 0;
            for (unsigned i = 0; i < 16; i++) value |= (uint32_t)(machine->xmm[insn->src][i] >> 7) << i;
            set_reg_width(machine->regs, insn->dst, value, 4); return R2_OK;
        }
        bool extract = insn->subop >= 0x14 && insn->subop <= 0x17;
        bool store = extract || insn->subop == 0xd6 || (insn->subop == 0x7e && !insn->has_imm);
        uint64_t value = 0;
        if (store) {
            size_t offset = extract ? ((unsigned)insn->imm & (16u / insn->width - 1u)) * insn->width : 0;
            memcpy(&value, original + offset, insn->width);
            if (insn->mem.present) return write_mem(machine, eval_mem_addr(insn, machine), insn->width, value, error, error_len);
            if (insn->has_imm) {
                memset(machine->xmm[insn->src], 0, 16); memcpy(machine->xmm[insn->src], &value, insn->width);
            } else set_reg_width(machine->regs, insn->src, value, extract && insn->width < 4 ? 4 : insn->width);
        } else {
            if (insn->has_imm) {
                status = sse_source(machine, insn, source, insn->width, error, error_len);
                if (status != R2_OK) return status;
                memcpy(&value, source, insn->width);
            } else if (insn->mem.present) {
                status = read_mem(machine, eval_mem_addr(insn, machine), insn->width, &value, error, error_len);
                if (status != R2_OK) return status;
            } else value = machine->regs[insn->src];
            bool insert = insn->subop == 0x20 || insn->subop == 0x22 || insn->subop == 0xc4;
            size_t offset = insert ? ((unsigned)insn->imm & (16u / insn->width - 1u)) * insn->width : 0;
            if (!insert) memset(destination, 0, 16);
            memcpy(destination + offset, &value, insn->width);
        }
        return R2_OK;
    }
    if (insn->op == R2_OP_SSE_CONVERT) {
        double value;
        if (insn->has_imm) {
            status = sse_source(machine, insn, source, (size_t)insn->imm * insn->src_width, error, error_len);
            if (status != R2_OK) return status;
            if(insn->subop!=0xe8&&insn->subop!=0x5b)sse_denormals(machine,source,insn->src_width,(size_t)insn->imm*insn->src_width,true);
            memset(destination, 0, 16);
            for (size_t i = 0; i < (size_t)insn->imm; i++) {
                if (insn->subop == 0xe8 || insn->subop == 0x5b) {
                    int32_t integer; memcpy(&integer, source + i * 4, 4); value = integer;
                } else if (insn->src_width == 4) {
                    float scalar; memcpy(&scalar, source + i * 4, 4); value = scalar;
                } else memcpy(&value, source + i * 8, 8);
                if (insn->subop == 0xe6 || insn->subop == 0xe7 || insn->subop == 0x5c || insn->subop == 0x5d) {
                    value = insn->subop == 0xe6 || insn->subop == 0x5d ? trunc(value) : nearbyint(value);
                    uint32_t integer = isnan(value) || value < -2147483648.0 || value >= 2147483648.0
                        ? UINT32_C(0x80000000) : (uint32_t)(int32_t)value;
                    memcpy(destination + i * 4, &integer, 4);
                } else if (insn->width == 4) {
                    float scalar = (float)value; memcpy(destination + i * 4, &scalar, 4);
                } else memcpy(destination + i * 8, &value, 8);
            }
            if(insn->subop!=0xe6&&insn->subop!=0xe7&&insn->subop!=0x5c&&insn->subop!=0x5d)
                sse_denormals(machine,destination,insn->width,(size_t)insn->imm*insn->width,false);
            return R2_OK;
        }
        if (insn->subop == 0x2a) {
            uint64_t integer;
            if (insn->mem.present) {
                status = read_mem(machine, eval_mem_addr(insn, machine), insn->src_width, &integer, error, error_len);
                if (status != R2_OK) return status;
            } else integer = machine->regs[insn->src];
            if (insn->width == 4) {
                float result = insn->src_width == 8 ? (float)(int64_t)integer : (float)(int32_t)integer;
                memcpy(destination, &result, 4);
            } else {
                value = insn->src_width == 8 ? (double)(int64_t)integer : (double)(int32_t)integer;
                memcpy(destination, &value, 8);
            }
            return R2_OK;
        }
        status = sse_source(machine, insn, source, insn->src_width, error, error_len);
        if (status != R2_OK) return status;
        sse_denormals(machine,source,insn->src_width,insn->src_width,true);
        if (insn->src_width == 4) { float scalar; memcpy(&scalar, source, 4); value = scalar; }
        else memcpy(&value, source, 8);
        if (insn->subop == 0x5a) {
            if (insn->width == 4) { float result = (float)value; memcpy(destination, &result, 4); }
            else memcpy(destination, &value, 8);
            sse_denormals(machine,destination,insn->width,insn->width,false);
        } else {
            value = insn->subop == 0x2c ? trunc(value) : nearbyint(value);
            double limit = insn->width == 4 ? 2147483648.0 : 9223372036854775808.0;
            uint64_t integer = isnan(value) || value < -limit || value >= limit
                ? (insn->width == 4 ? UINT64_C(0x80000000) : UINT64_C(0x8000000000000000))
                : (uint64_t)(int64_t)value;
            set_reg_width(machine->regs, insn->dst, integer, insn->width);
        }
        return R2_OK;
    }
    if (insn->op == R2_OP_SSE_SHUFFLE) {
        if (insn->subop == 0x21) {
            status = sse_source(machine, insn, source, insn->mem.present ? 4 : 16, error, error_len);
            if (status != R2_OK) return status;
            unsigned control = (unsigned)insn->imm;
            memcpy(destination + ((control >> 4) & 3) * 4, source + (insn->mem.present ? 0 : (control >> 6) * 4), 4);
            for (unsigned i = 0; i < 4; i++) if ((control >> i) & 1) memset(destination + i * 4, 0, 4);
            return R2_OK;
        }
        if (insn->subop == 0x70 && insn->mem.present && (eval_mem_addr(insn, machine) & 15)) {
            r2_set_error(error, error_len, "unaligned SSE shuffle memory operand"); return R2_ERR_UNSUPPORTED;
        }
        bool halves = insn->subop == 0x12 || insn->subop == 0x16;
        size_t length = halves && insn->mem.present && insn->src_width != 0xf3 ? 8 : 16;
        status = sse_source(machine, insn, source, length, error, error_len);
        if (status != R2_OK) return status;
        if (insn->subop == 0x70) {
            memcpy(destination, source, 16);
            unsigned base = insn->width == 2 && insn->src_width == 0xf3 ? 8 : 0;
            for (unsigned i = 0; i < 4; i++)
                memcpy(destination + base + i * insn->width,
                       source + base + (((unsigned)insn->imm >> (i * 2)) & 3u) * insn->width, insn->width);
        } else if (insn->subop == 0xc6) {
            for (unsigned i = 0; i < 16u / insn->width; i++) {
                unsigned index = insn->width == 8 ? ((unsigned)insn->imm >> i) & 1u : ((unsigned)insn->imm >> (i * 2)) & 3u;
                const uint8_t *bank = i < 8u / insn->width ? original : source;
                memcpy(destination + i * insn->width, bank + index * insn->width, insn->width);
            }
        } else if (!halves) {
            unsigned offset = insn->subop == 0x15 ? 8 : 0;
            for (unsigned i = 0; i < 8u / insn->width; i++) {
                memcpy(destination + i * insn->width * 2, original + offset + i * insn->width, insn->width);
                memcpy(destination + (i * 2 + 1) * insn->width, source + offset + i * insn->width, insn->width);
            }
        } else if (insn->src_width == 0xf2) {
            memcpy(destination, source, 8); memcpy(destination + 8, source, 8);
        } else if (insn->src_width == 0xf3) {
            for (unsigned i = 0; i < 4; i++) memcpy(destination + i * 4, source + ((i & ~1u) + (insn->subop == 0x16)) * 4, 4);
        } else if (insn->subop == 0x12) memcpy(destination, source + (insn->mem.present ? 0 : 8), 8);
        else memcpy(destination + 8, source, 8);
        return R2_OK;
    }
    size_t length = insn->has_imm ? insn->width : 16;
    status = sse_source(machine, insn, source, length, error, error_len);
    if (status != R2_OK) return status;
    if (insn->subop == 0x52 || insn->subop == 0x53) {
        if (!insn->has_imm && insn->mem.present && (eval_mem_addr(insn, machine) & 15)) {
            r2_set_error(error, error_len, "unaligned reciprocal SSE operand"); return R2_ERR_UNSUPPORTED;
        }
        /* Exact single-precision estimates satisfy the ISA's relative error
         * bound. These instructions suppress FP exceptions and flush denormals. */
        fenv_t saved; fegetenv(&saved);
        for (size_t i = 0; i < length; i += 4) {
            uint32_t bits, answer; memcpy(&bits, source + i, 4);
            uint32_t exponent = bits & 0x7f800000, fraction = bits & 0x007fffff;
            if (exponent == 0x7f800000 && fraction) answer = bits | 0x00400000;
            else if (exponent == 0) answer = (bits & 0x80000000) | 0x7f800000;
            else if (insn->subop == 0x52 && (bits & 0x80000000)) answer = 0xffc00000;
            else if (exponent == 0x7f800000) answer = bits & 0x80000000;
            else {
                float input, output; memcpy(&input, &bits, 4);
                output = insn->subop == 0x53 ? 1.0f / input : (float)(1.0 / sqrt((double)input));
                memcpy(&answer, &output, 4);
                if ((answer & 0x7f800000) == 0) answer &= 0x80000000;
            }
            memcpy(destination + i, &answer, 4);
        }
        fesetenv(&saved); return R2_OK;
    }
    if(insn->subop!=0x50) {
        if(insn->subop!=0x51&&insn->subop!=0x08)sse_denormals(machine,original,insn->width,length,true);
        sse_denormals(machine,source,insn->width,length,true);
    }
    if (insn->subop == 0x7c || insn->subop == 0x7d) {
        if (insn->mem.present && (eval_mem_addr(insn, machine) & 15)) {
            r2_set_error(error, error_len, "unaligned horizontal SSE operand"); return R2_ERR_UNSUPPORTED;
        }
        unsigned lanes = 16u / insn->width;
        for (unsigned i = 0; i < lanes; i++) {
            const uint8_t *bank = i < lanes / 2 ? original : source;
            unsigned offset = (i % (lanes / 2)) * 2 * insn->width;
            if (insn->width == 4) {
                float left, right, value; memcpy(&left, bank + offset, 4); memcpy(&right, bank + offset + 4, 4);
                value = insn->subop == 0x7c ? left + right : left - right; memcpy(destination + i * 4, &value, 4);
            } else {
                double left, right, value; memcpy(&left, bank + offset, 8); memcpy(&right, bank + offset + 8, 8);
                value = insn->subop == 0x7c ? left + right : left - right; memcpy(destination + i * 8, &value, 8);
            }
        }
        sse_denormals(machine,destination,insn->width,16,false);return R2_OK;
    }
    if (insn->subop == 0x50) {
        uint64_t mask = 0;
        for (unsigned i = 0; i < 16u / insn->width; i++)
            mask |= (uint64_t)(source[(i + 1) * insn->width - 1] >> 7) << i;
        set_reg_width(machine->regs, insn->dst, mask, 4);
        return R2_OK;
    }
    if (insn->subop == 0x2e || insn->subop == 0x2f) {
        double a, b;
        if (insn->width == 4) { float x, y; memcpy(&x, original, 4); memcpy(&y, source, 4); a = x; b = y; }
        else { memcpy(&a, original, 8); memcpy(&b, source, 8); }
        bool unordered = isnan(a) || isnan(b);
        machine->flags.zf = unordered || a == b; machine->flags.cf = unordered || a < b;
        machine->flags.pf = unordered; machine->flags.of = false; machine->flags.sf = false;
        return R2_OK;
    }
    for (size_t i = 0; i < length; i += insn->width) {
        if (insn->subop == 0xc2) {
            double a, b;
            if (insn->width == 4) { float x, y; memcpy(&x, original + i, 4); memcpy(&y, source + i, 4); a = x; b = y; }
            else { memcpy(&a, original + i, 8); memcpy(&b, source + i, 8); }
            bool unordered = isnan(a) || isnan(b), result;
            switch (insn->imm) {
            case 0: result = !unordered && a == b; break; case 1: result = !unordered && a < b; break;
            case 2: result = !unordered && a <= b; break; case 3: result = unordered; break;
            case 4: result = unordered || a != b; break; case 5: result = unordered || a >= b; break;
            case 6: result = unordered || a > b; break; default: result = !unordered; break;
            }
            memset(destination + i, result ? 255 : 0, insn->width);
            continue;
        }
        if (insn->width == 4) {
            float a, b, result; memcpy(&a, original + i, 4); memcpy(&b, source + i, 4);
            switch (insn->subop) {
            case 0x08: {
                unsigned mode = insn->imm & 4 ? 0 : insn->imm & 3;
                result = mode == 1 ? floorf(b) : mode == 2 ? ceilf(b) : mode == 3 ? truncf(b) : nearbyintf(b);
                break;
            }
            case 0xd0: result = (i / insn->width) & 1 ? a + b : a - b; break;
            case 0x58: result = a + b; break; case 0x59: result = a * b; break;
            case 0x5c: result = a - b; break; case 0x5e: result = a / b; break;
            case 0x51: result = sqrtf(b); break; case 0x5d: result = a < b ? a : b; break;
            default: result = a > b ? a : b; break;
            }
            memcpy(destination + i, &result, 4);
        } else {
            double a, b, result; memcpy(&a, original + i, 8); memcpy(&b, source + i, 8);
            switch (insn->subop) {
            case 0x08: {
                unsigned mode = insn->imm & 4 ? 0 : insn->imm & 3;
                result = mode == 1 ? floor(b) : mode == 2 ? ceil(b) : mode == 3 ? trunc(b) : nearbyint(b);
                break;
            }
            case 0xd0: result = (i / insn->width) & 1 ? a + b : a - b; break;
            case 0x58: result = a + b; break; case 0x59: result = a * b; break;
            case 0x5c: result = a - b; break; case 0x5e: result = a / b; break;
            case 0x51: result = sqrt(b); break; case 0x5d: result = a < b ? a : b; break;
            default: result = a > b ? a : b; break;
            }
            memcpy(destination + i, &result, 8);
        }
    }
    if(insn->subop!=0x5d&&insn->subop!=0x5f&&insn->subop!=0xc2)
        sse_denormals(machine,destination,insn->width,length,false);
    return R2_OK;
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
    memcpy(thread->xmm, machine->xmm, sizeof(thread->xmm));
    thread->flags = machine->flags;
    thread->x87 = machine->x87;
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
    memcpy(machine->xmm, thread->xmm, sizeof(machine->xmm));
    machine->flags = thread->flags;
    machine->x87 = thread->x87;
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
                                     uint64_t offset,
                                     size_t *index,
                                     char *error,
                                     size_t error_len)
{
    if (block->runtime != NULL) {
        return r2_native_decode((r2_block *)block, offset, index, error, error_len);
    }
    for (size_t i = 0; i < block->count; i += 1) {
        if (block->insns[i].x86_offset == offset) {
            *index = i;
            return R2_OK;
        }
    }

    r2_set_error(error,
                 error_len,
                 "branch target guest address 0x%016llx is not an instruction boundary",
                 (unsigned long long)offset);
    return R2_ERR_DECODE;
}

static r2_status jump_to_target(const r2_block *block,
                                uint64_t target,
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
                     "guest code address 0x%016llx is below code base 0x%016llx",
                     (unsigned long long)guest_addr,
                     (unsigned long long)rip_base);
        return R2_ERR_UNSUPPORTED;
    }

    uint64_t offset = guest_addr - rip_base;
    return find_insn_by_offset(block, offset, pc, error, error_len);
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
    thread->x87.control = machine->x87.control;
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

#include "x87.inc"

static const uint8_t signal_register_order[16] = {
    R2_X86_RAX, R2_X86_RBX, R2_X86_RCX, R2_X86_RDX,
    R2_X86_RDI, R2_X86_RSI, R2_X86_RBP, R2_X86_RSP,
    R2_X86_R8, R2_X86_R9, R2_X86_R10, R2_X86_R11,
    R2_X86_R12, R2_X86_R13, R2_X86_R14, R2_X86_R15
};
static r2_status build_signal_info(const r2_block *block, r2_machine *machine,
                                   uint64_t signum, uint64_t *siginfo_addr,
                                   uint64_t *ucontext_addr, char *error, size_t error_len)
{
    r2_signal_frame *frame = &machine->signal_frames[machine->signal_frame_count - 1];
    machine->regs[R2_X86_RSP] = (machine->regs[R2_X86_RSP] - R2_X86_MC_SIZE) & ~UINT64_C(15);
    uint64_t mcontext = machine->regs[R2_X86_RSP];
    uint8_t *mc = memory_pointer(machine, mcontext, R2_X86_MC_SIZE, true, error, error_len);
    if (mc == NULL) return R2_ERR_UNSUPPORTED;
    memset(mc, 0, R2_X86_MC_SIZE);
    if (machine->delivering_fault) {
        uint16_t trap = 14; uint32_t code = 4 | (machine->fault_write ? 2 : 0) | (machine->fault_code == SEGV_ACCERR ? 1 : 0);
        memcpy(mc, &trap, 2); memcpy(mc + 4, &code, 4); memcpy(mc + 8, &machine->fault_address, 8);
    }
    for (unsigned i = 0; i < 16; i++) memcpy(mc + R2_X86_MC_THREAD + i * 8, &frame->regs[signal_register_order[i]], 8);
    uint64_t rip = (machine->env ? machine->env->rip_base : 0) + block->insns[frame->return_pc].x86_offset;
    uint64_t flags = read_flags(&frame->flags), cs = 0x2b;
    memcpy(mc + R2_X86_MC_RIP, &rip, 8); memcpy(mc + R2_X86_MC_RFLAGS, &flags, 8); memcpy(mc + R2_X86_MC_CS, &cs, 8);
    uint32_t mxcsr = machine->mxcsr, mask = 0xffff;
    if (machine->env != NULL && machine->env->native != NULL) {
        mxcsr = r2_native_get_mxcsr(machine->env->native);
    }
    x87_save(machine_x87(machine),mc+R2_X86_MC_FCW,mc+R2_X86_MC_STMM);
    memcpy(mc + R2_X86_MC_MXCSR, &mxcsr, 4); memcpy(mc + R2_X86_MC_MXCSR_MASK, &mask, 4);
    memcpy(mc + R2_X86_MC_XMM, frame->xmm, sizeof(frame->xmm));

    machine->regs[R2_X86_RSP] = (machine->regs[R2_X86_RSP] - R2_UCONTEXT_SIZE) & ~UINT64_C(15);
    *ucontext_addr = machine->regs[R2_X86_RSP]; frame->context_address = *ucontext_addr;
    uint8_t *uc = memory_pointer(machine, *ucontext_addr, R2_UCONTEXT_SIZE, true, error, error_len);
    if (uc == NULL) return R2_ERR_UNSUPPORTED;
    memset(uc, 0, R2_UCONTEXT_SIZE);
    memcpy(uc + R2_X86_UC_MASK, &frame->signal_mask, 4);
    const r2_guest_thread *thread = &machine->threads[machine->current_thread];
    memcpy(uc + R2_X86_UC_STACK, &thread->stack_base, 8); memcpy(uc + R2_X86_UC_STACK + 8, &thread->stack_size, 8);
    uint64_t mcsize = R2_X86_MC_SIZE;
    memcpy(uc + R2_X86_UC_MCSIZE, &mcsize, 8); memcpy(uc + R2_X86_UC_MCONTEXT, &mcontext, 8);

    machine->regs[R2_X86_RSP] = (machine->regs[R2_X86_RSP] - R2_SIGINFO_SIZE) & ~UINT64_C(15);
    *siginfo_addr = machine->regs[R2_X86_RSP];
    uint8_t *info = memory_pointer(machine, *siginfo_addr, R2_SIGINFO_SIZE, true, error, error_len);
    if (info == NULL) return R2_ERR_UNSUPPORTED;
    memset(info, 0, R2_SIGINFO_SIZE);
    uint32_t signal_number = (uint32_t)signum;
    int code = machine->delivering_fault ? machine->fault_code : SI_USER;
    memcpy(info, &signal_number, 4); memcpy(info + 8, &code, 4);
    if (machine->delivering_fault) memcpy(info + 24, &machine->fault_address, 8);
    else {
        uint32_t pid = (uint32_t)getpid(), uid = (uint32_t)getuid();
        memcpy(info + 12, &pid, 4); memcpy(info + 16, &uid, 4);
    }
    return R2_OK;
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
        memcpy(frame->xmm, machine->xmm, sizeof(frame->xmm));
        frame->flags = machine->flags;
        frame->x87 = *machine_x87(machine);
        frame->signal_mask = machine->signal_mask;
        frame->return_pc = *pc; frame->context_address = 0;
        machine->signal_frame_count += 1;

        machine->signal_mask |= action.mask;
        if ((action.flags & R2_DARWIN_SA_NODEFER) == 0) {
            machine->signal_mask |= bit;
        }
        if ((action.flags & R2_DARWIN_SA_RESETHAND) != 0) {
            memset(&machine->sigactions[signum], 0, sizeof(machine->sigactions[signum]));
            if (machine->env && machine->env->native) r2_native_reset_signal_action(machine->env->native, (unsigned)signum);
        }

        /* Preserve the interrupted function's SysV x86_64 red zone. */
        machine->regs[R2_X86_RSP] = (machine->regs[R2_X86_RSP] - 128) & ~UINT64_C(15);
        machine->regs[R2_X86_RDI] = signum;
        if ((action.flags & R2_DARWIN_SA_SIGINFO) != 0) {
            uint64_t siginfo_addr = 0;
            uint64_t ucontext_addr = 0;
            status =
                build_signal_info(block, machine, signum, &siginfo_addr, &ucontext_addr, error, error_len);
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

static r2_status restore_signal_frame(const r2_block *block, r2_machine *machine,
                                      size_t *pc, char *error, size_t error_len)
{
    machine->instruction_active = false;
    if (machine->signal_frame_count == 0) {
        r2_set_error(error, error_len, "signal return without an active signal frame"); return R2_ERR_DECODE;
    }
    const r2_signal_frame *frame = &machine->signal_frames[machine->signal_frame_count - 1];
    uint8_t context[R2_X86_MC_SIZE]; uint32_t mask = frame->signal_mask;
    size_t return_pc = frame->return_pc;
    if (frame->context_address != 0) {
        const uint8_t *uc = memory_pointer(machine, frame->context_address, R2_X86_UC_SIZE, false, error, error_len);
        if (uc == NULL) return R2_ERR_UNSUPPORTED;
        uint64_t address, size, rip, flags; uint16_t control; uint32_t mxcsr;
        memcpy(&address, uc + R2_X86_UC_MCONTEXT, 8); memcpy(&size, uc + R2_X86_UC_MCSIZE, 8);
        memcpy(&mask, uc + R2_X86_UC_MASK, 4);
        if (size != R2_X86_MC_SIZE) { r2_set_error(error, error_len, "unsupported guest mcontext size"); return R2_ERR_UNSUPPORTED; }
        const void *source = memory_pointer(machine, address, sizeof(context), false, error, error_len);
        if (source == NULL) return R2_ERR_UNSUPPORTED;
        memcpy(context, source, sizeof(context));
        memcpy(&rip, context + R2_X86_MC_RIP, 8); memcpy(&flags, context + R2_X86_MC_RFLAGS, 8);
        memcpy(&control, context + R2_X86_MC_FCW, 2); memcpy(&mxcsr, context + R2_X86_MC_MXCSR, 4);
        if ((control & 0x0c3f) != 0x003f || !r2_mxcsr_supported(mxcsr) || (flags & 0x100)) {
            r2_set_error(error, error_len, "unsupported FP or single-step state on guest signal return"); return R2_ERR_UNSUPPORTED;
        }
        r2_status status = guest_code_addr_to_pc(block, machine, rip, &return_pc, error, error_len);
        if (status != R2_OK) return status;
        for (unsigned i = 0; i < 16; i++) memcpy(&machine->regs[signal_register_order[i]], context + R2_X86_MC_THREAD + i * 8, 8);
        memcpy(machine->xmm, context + R2_X86_MC_XMM, sizeof(machine->xmm));
        machine->flags.cf = !!(flags & 1); machine->flags.pf = !!(flags & 4); machine->flags.af = !!(flags & 16);
        machine->flags.zf = !!(flags & 64); machine->flags.sf = !!(flags & 128); machine->flags.of = !!(flags & 0x800);
        machine->flags.extra = 0x200 | (flags & (0x200000 | 0x40000 | 0x400));
        x87_restore(machine_x87(machine),context+R2_X86_MC_FCW,context+R2_X86_MC_STMM);machine->mxcsr = mxcsr;
        if (machine->env != NULL && machine->env->native != NULL) {
            r2_native_set_mxcsr(machine->env->native, mxcsr);
        }
    } else {
        memcpy(machine->regs, frame->regs, sizeof(machine->regs)); memcpy(machine->xmm, frame->xmm, sizeof(machine->xmm)); machine->flags = frame->flags;
        *machine_x87(machine)=frame->x87;
    }
    machine->signal_frame_count--;
    machine->signal_mask = mask; *pc = return_pc; return R2_OK;
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

    r2_machine *machine = calloc(1, sizeof(*machine));
    if (machine == NULL) {
        r2_set_error(error, error_len, "out of memory allocating interpreter state");
        return R2_ERR_NO_MEMORY;
    }
    machine->flags.extra = 0x200;
    machine->mxcsr = 0x1f80; machine->x87.control = 0x037f;
    machine->env = env;
    size_t pc = 0, steps = 0;
    r2_status status = R2_OK;
    if (env != NULL && env->native != NULL) {
        r2_native_execution_enter(env->native,(r2_block *)block,env,&machine->signal_mask,&machine->pending_signals,&machine->signal_frame_count,&machine->previous_execution);
        machine->mxcsr=r2_native_get_mxcsr(env->native);
        r2_native_fast_state_get(env->native,&machine->fast);
    }
    machine->next_mmap_base = R2_MMAP_BASE;
    machine->thread_count = 1;
    machine->current_thread = 0;
    machine->next_tid = 2;
    machine->next_thread_port = R2_MAIN_THREAD_PORT + 1u;

    r2_guest_thread *main_thread = &machine->threads[0];
    main_thread->state = R2_THREAD_RUNNABLE;
    main_thread->tid = 1;
    main_thread->thread_port = R2_MAIN_THREAD_PORT;
    main_thread->stack_base = R2_STACK_BASE;
    main_thread->stack_size = R2_STACK_SIZE;
    /* macOS x86 HotSpot treats the main stack as at least 2048 OS pages.
     * Allocate that real guest range rather than advertising unmapped space. */
    if (env != NULL && env->native != NULL) main_thread->stack_size =
        env->stack_size ? env->stack_size : (uint64_t)getpagesize() * 2048;
    main_thread->borrowed_stack = env != NULL && env->native != NULL && env->borrowed_stack_base != 0;
    main_thread->stack = main_thread->borrowed_stack ? (void *)(uintptr_t)env->borrowed_stack_base : env != NULL && env->native != NULL
        ? r2_native_stack_allocate(env->native, (size_t)main_thread->stack_size) : calloc(1, R2_STACK_SIZE);
    if (main_thread->stack == NULL) {
        r2_set_error(error, error_len, "out of memory allocating emulated stack");
        status = R2_ERR_NO_MEMORY; goto execution_error;
    }
    if (env != NULL && env->has_initial_regs) {
        memcpy(machine->regs, env->initial_regs, sizeof(machine->regs));
    }

    if (env != NULL && env->native != NULL) {
        main_thread->stack_base = (uintptr_t)main_thread->stack;
        r2_native_set_stack(env->native, main_thread->stack_base, main_thread->stack_size);
    }
    machine->regs[R2_X86_RSP] = main_thread->borrowed_stack ? env->initial_regs[R2_X86_RSP] : main_thread->stack_base + main_thread->stack_size - 8;
    if (main_thread->borrowed_stack && (!env->has_initial_regs || main_thread->stack_size < 8 ||
        machine->regs[R2_X86_RSP] < main_thread->stack_base ||
        machine->regs[R2_X86_RSP] - main_thread->stack_base > main_thread->stack_size - 8)) {
        r2_set_error(error, error_len, "callback stack pointer is outside the borrowed guest stack");
        status = R2_ERR_UNSUPPORTED;goto execution_error;
    }
    status =
        write_u64(machine, machine->regs[R2_X86_RSP], R2_RETURN_SENTINEL, error, error_len);
    if (status != R2_OK) {
        goto execution_error;
    }

    if (env != NULL && env->has_initial_rip) {
        status = guest_code_addr_to_pc(block,
                                       machine,
                                       env->initial_rip,
                                       &pc,
                                       error,
                                       error_len);
        if (status != R2_OK) {
            goto execution_error;
        }
    }
    main_thread->pc = pc;
resume_execution:
    while (true) {
        machine->instruction_active = false;
        if (env != NULL && env->native != NULL) {
            if(atomic_load_explicit(machine->fast.stopping,memory_order_acquire)||++*machine->fast.quantum>=1024) {
                status = r2_native_execution_poll(env->native, error, error_len);
                if (status != R2_OK) { goto execution_error; }
            }
        }
        status = env != NULL && env->native != NULL && machine->thread_count == 1
                    ? R2_OK : schedule_next_thread(machine, &pc, error, error_len);
        if (status != R2_OK) {
            goto execution_error;
        }

        if (pc >= block->count) {
            r2_set_error(error, error_len, "guest thread ran off the decoded block");
            status = R2_ERR_DECODE; goto execution_error;
        }

        if (steps++ > R2_MAX_STEPS && (env == NULL || env->native == NULL)) {
            r2_set_error(error, error_len, "interpreter step limit reached");
            status = R2_ERR_UNSUPPORTED; goto execution_error;
        }

        bool completed = false;
        if (env != NULL && env->native != NULL) {
            uint32_t pending = (atomic_load_explicit(machine->fast.thread_pending,memory_order_relaxed)|
                atomic_load_explicit(machine->fast.process_pending,memory_order_relaxed))?r2_native_pending(env->native):0;
            if (pending != 0) {
                machine->pending_signals |= pending;
                for (unsigned i = 1; i <= R2_DARWIN_NSIG; i += 1)
                    r2_native_signal_action(env->native, i, &machine->sigactions[i].handler,
                                            &machine->sigactions[i].mask, &machine->sigactions[i].flags);
            }
        }
        status = deliver_pending_signal(block, machine, &pc, result, &completed, error, error_len);
        if (status != R2_OK) {
            goto execution_error;
        }
        if (completed) {
            if (env != NULL && env->process != NULL) {
                env->process->kind = R2_PROCESS_IMMEDIATE;
                env->process->code = result->rax;
            }
            status = R2_OK; goto execution_error;
        }

        const r2_insn *insn = &block->insns[pc];
        machine->current_address = insn->x86_offset;
        machine->instruction_active = true; machine->memory_fault = false;
        machine->instruction_sp = machine->regs[R2_X86_RSP]; machine->instruction_flags = machine->flags;
        uint64_t next_address = insn->x86_offset + insn->x86_len;
        size_t next_pc = pc + 1;

        switch (insn->op) {
        case R2_OP_NOP:
            break;
        case R2_OP_DIRECTION:
            machine->flags.extra = (machine->flags.extra & ~UINT64_C(0x400)) | (insn->subop ? 0x400 : 0);
            break;
        case R2_OP_MOVS:
        case R2_OP_STOS: {
            if (insn->has_imm && machine->regs[R2_X86_RCX] == 0) break;
            uint64_t value = machine->regs[R2_X86_RAX];
            if (insn->op == R2_OP_MOVS) {
                status = read_mem(machine, eval_mem_addr(insn, machine), insn->width, &value, error, error_len);
                if (status != R2_OK) goto execution_error;
            }
            status = write_mem(machine, machine->regs[R2_X86_RDI], insn->width,
                               value, error, error_len);
            if (status != R2_OK) goto execution_error;
            uint64_t stride = (machine->flags.extra & 0x400) ? -(uint64_t)insn->width : insn->width;
            machine->regs[R2_X86_RDI] += stride;
            if (insn->op == R2_OP_MOVS) machine->regs[R2_X86_RSI] += stride;
            /* Repeat one element per interpreter iteration: faults and signals
             * expose the already completed stores and remaining RCX exactly. */
            if (insn->has_imm && --machine->regs[R2_X86_RCX] != 0) {
                next_address = insn->x86_offset; next_pc = pc;
            }
            break;
        }
        case R2_OP_FX_STATE: {
            uint64_t address = eval_mem_addr(insn, machine); bool save = insn->subop == 0;
            if (address & 15) {
                r2_set_error(error, error_len, "unaligned guest FX state buffer"); status = R2_ERR_UNSUPPORTED; goto execution_error;
            }
            uint8_t *buffer = memory_pointer(machine, address, 512, save, error, error_len);
            if (buffer == NULL) { status = R2_ERR_UNSUPPORTED; goto execution_error; }
            if (save) {
                uint32_t mxcsr = machine->mxcsr, mask = 0xffff;
                if (env != NULL && env->native != NULL) {
                    mxcsr = r2_native_get_mxcsr(env->native);
                }
                memset(buffer, 0, 464);x87_save(machine_x87(machine),buffer,buffer+32);
                memcpy(buffer + 24, &mxcsr, 4); memcpy(buffer + 28, &mask, 4);
                memcpy(buffer + 160, machine->xmm, sizeof(machine->xmm));
            } else {
                uint16_t control; uint32_t mxcsr;
                memcpy(&control, buffer, 2);memcpy(&mxcsr, buffer + 24, 4);
                if ((control & 0x0c3f) != 0x003f || !r2_mxcsr_supported(mxcsr)) {
                    r2_set_error(error, error_len, "unsupported guest FX state: FP control mode"); status = R2_ERR_UNSUPPORTED; goto execution_error;
                }
                x87_restore(machine_x87(machine),buffer,buffer+32);machine->mxcsr = mxcsr;
                if (env != NULL && env->native != NULL) {
                    r2_native_set_mxcsr(env->native, mxcsr);
                }
                memcpy(machine->xmm, buffer + 160, sizeof(machine->xmm));
            }
            break;
        }
        case R2_OP_X87:
            status=execute_x87(machine,insn,error,error_len);
            if(status!=R2_OK)goto execution_error;
            break;
        case R2_OP_MXCSR: {
            uint64_t value = env != NULL && env->native != NULL ? r2_native_get_mxcsr(env->native) : machine->mxcsr;
            if (insn->subop == 3) status = write_mem(machine, eval_mem_addr(insn, machine), 4, value, error, error_len);
            else {
                status = read_mem(machine, eval_mem_addr(insn, machine), 4, &value, error, error_len);
                if (status == R2_OK && !r2_mxcsr_supported((uint32_t)value)) {
                    r2_set_error(error, error_len, "unsupported guest MXCSR mode 0x%llx", (unsigned long long)value);
                    status = R2_ERR_UNSUPPORTED;
                }
                if (status == R2_OK) {
                    machine->mxcsr = (uint32_t)value;
                    if (env != NULL && env->native != NULL) r2_native_set_mxcsr(env->native, (uint32_t)value);
                }
            }
            if (status != R2_OK) { goto execution_error; }
            break;
        }
        case R2_OP_RDTSC: {
            struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
            uint64_t ticks = (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
            machine->regs[R2_X86_RAX] = (uint32_t)ticks;
            machine->regs[R2_X86_RDX] = (uint32_t)(ticks >> 32);
            break;
        }
        case R2_OP_CPUID:
            cpu_identification(machine->regs);
            break;
        case R2_OP_PUSH_FLAGS:
            status = push_u64(machine, read_flags(&machine->flags), error, error_len);
            if (status != R2_OK) { goto execution_error; }
            break;
        case R2_OP_POP_FLAGS: {
            uint64_t value;
            status = pop_u64(machine, &value, error, error_len);
            if (status != R2_OK) { goto execution_error; }
            if (value & 0x100) { r2_set_error(error, error_len, "guest single-step mode is unsupported"); status = R2_ERR_UNSUPPORTED; goto execution_error; }
            machine->flags.cf = !!(value & 1); machine->flags.pf = !!(value & 4); machine->flags.af = !!(value & 16);
            machine->flags.zf = !!(value & 64); machine->flags.sf = !!(value & 128); machine->flags.of = !!(value & 0x800);
            machine->flags.extra = 0x200 | (value & (0x200000 | 0x40000 | 0x400));
            break;
        }
        case R2_OP_AH_FLAGS:
            if (insn->subop == 0x9f) set_operand_reg(machine->regs, R2_X86_RAX, true, read_flags(&machine->flags) & 255, 1);
            else {
                uint64_t value = machine->regs[R2_X86_RAX] >> 8;
                machine->flags.cf = !!(value & 1); machine->flags.pf = !!(value & 4); machine->flags.af = !!(value & 16);
                machine->flags.zf = !!(value & 64); machine->flags.sf = !!(value & 128);
            }
            break;
        case R2_OP_FENCE:
            if (insn->mem.present && memory_pointer(machine, eval_mem_addr(insn, machine), 1, false, error, error_len) == NULL) {
                status = R2_ERR_UNSUPPORTED; goto execution_error;
            }
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            break;
        case R2_OP_MOV_IMM:
            if (insn->dst_high8) machine->regs[insn->dst] =
                (machine->regs[insn->dst] & ~UINT64_C(0xff00)) | (((uint64_t)insn->imm & 0xff) << 8);
            else set_reg_width(machine->regs, insn->dst, (uint64_t)insn->imm, insn->width);
            break;
        case R2_OP_DOUBLE_SHIFT: {
            uint64_t value;
            if (insn->mem.present) {
                status = read_mem(machine, eval_mem_addr(insn, machine), insn->width, &value, error, error_len);
                if (status != R2_OK) { goto execution_error; }
            } else value = machine->regs[insn->dst];
            value = normalize_value(value, insn->width);
            uint64_t source = normalize_value(machine->regs[insn->src], insn->width);
            unsigned bits = insn->width * 8u;
            unsigned count = (unsigned)(insn->has_imm ? (uint64_t)insn->imm : machine->regs[R2_X86_RCX]) & (bits == 64 ? 63u : 31u);
            if (count != 0) {
                __uint128_t pair = insn->subop ? ((__uint128_t)source << bits) | value
                                               : ((__uint128_t)value << bits) | source;
                uint64_t result_value = normalize_value((uint64_t)(insn->subop ? pair >> count
                                                              : (pair << count) >> bits), insn->width);
                bool carry = ((pair >> (insn->subop ? count - 1u : bits * 2u - count)) & 1u) != 0;
                bool overflow = machine->flags.of;
                if (count == 1) overflow = insn->subop
                    ? ((value ^ result_value) & width_sign_bit(insn->width)) != 0
                    : ((result_value & width_sign_bit(insn->width)) != 0) != carry;
                set_logic_flags_width(&machine->flags, result_value, insn->width);
                machine->flags.cf = carry; machine->flags.of = overflow;
                value = result_value;
            }
            if (insn->mem.present) {
                status = write_mem(machine, eval_mem_addr(insn, machine), insn->width, value, error, error_len);
                if (status != R2_OK) { goto execution_error; }
            } else set_reg_width(machine->regs, insn->dst, value, insn->width);
            break;
        }
        case R2_OP_BIT_SCAN: {
            uint64_t value;
            if (insn->mem.present) {
                status = read_mem(machine, eval_mem_addr(insn, machine), insn->width, &value, error, error_len);
                if (status != R2_OK) { goto execution_error; }
            } else value = machine->regs[insn->src];
            value = normalize_value(value, insn->width);
            unsigned bits = insn->width * 8, result = bits;
            if (value != 0) result = insn->subop & 1 ? (unsigned)__builtin_clzll(value) - (64 - bits) : (unsigned)__builtin_ctzll(value);
            if (insn->subop & 2) {
                machine->flags.cf = value == 0; machine->flags.zf = result == 0;
                set_reg_width(machine->regs, insn->dst, result, insn->width);
            } else {
                machine->flags.zf = value == 0;
                if (value != 0) set_reg_width(machine->regs, insn->dst, insn->subop & 1 ? bits - 1 - result : result, insn->width);
            }
            break;
        }
        case R2_OP_BSWAP:
            set_reg_width(machine->regs, insn->dst, insn->width == 8
                ? __builtin_bswap64(machine->regs[insn->dst])
                : __builtin_bswap32((uint32_t)machine->regs[insn->dst]), insn->width);
            break;
        case R2_OP_SIGN_PAIR:
            set_reg_width(machine->regs, R2_X86_RDX,
                          (machine->regs[R2_X86_RAX] & width_sign_bit(insn->width)) ? UINT64_MAX : 0,
                          insn->width);
            break;
        case R2_OP_BIT: {
            unsigned bits = insn->width * 8u;
            uint64_t index = insn->has_imm ? (uint64_t)insn->imm : machine->regs[insn->src];
            uint64_t address = insn->mem.present ? eval_mem_addr(insn, machine) : 0;
            if (insn->mem.present && !insn->has_imm)
                address += (uint64_t)(signed_value(index, insn->width) >> (insn->width == 8 ? 6 : insn->width == 4 ? 5 : 4)) * insn->width;
            uint64_t value;
            if (insn->mem.present) {
                status = read_mem(machine, address, insn->width, &value, error, error_len);
                if (status != R2_OK) { goto execution_error; }
            } else value = machine->regs[insn->dst];
            uint64_t bit = UINT64_C(1) << (index & (bits - 1u));
            machine->flags.cf = (value & bit) != 0;
            if (insn->subop != 0) {
                if (insn->subop == 1) value |= bit;
                else if (insn->subop == 2) value &= ~bit;
                else value ^= bit;
                if (insn->mem.present) {
                    status = write_mem(machine, address, insn->width, value, error, error_len);
                    if (status != R2_OK) { goto execution_error; }
                } else set_reg_width(machine->regs, insn->dst, value, insn->width);
            }
            break;
        }
        case R2_OP_IMUL: {
            uint64_t source;
            if (insn->mem.present) {
                status = read_mem(machine, eval_mem_addr(insn, machine), insn->width, &source, error, error_len);
                if (status != R2_OK) { goto execution_error; }
            } else source = machine->regs[insn->src];
            __int128 lhs = signed_value(insn->has_imm ? source : machine->regs[insn->dst], insn->width);
            __int128 rhs = signed_value(insn->has_imm ? (uint64_t)insn->imm : source, insn->width);
            __int128 product = lhs * rhs;
            __int128 bound = (__int128)1 << (insn->width * 8u - 1u);
            machine->flags.cf = machine->flags.of = product < -bound || product >= bound;
            set_reg_width(machine->regs, insn->dst, (uint64_t)product, insn->width);
            break;
        }
        case R2_OP_MULDIV: {
            uint64_t operand;
            if (insn->mem.present) {
                status = read_mem(machine, eval_mem_addr(insn, machine), insn->width, &operand, error, error_len);
                if (status != R2_OK) { goto execution_error; }
            } else operand = machine->regs[insn->dst] >> (insn->dst_high8 ? 8u : 0u);
            operand = normalize_value(operand, insn->width);
            unsigned bits = insn->width * 8u;
            uint64_t low = normalize_value(machine->regs[R2_X86_RAX], insn->width);
            uint64_t high = insn->width == 1 ? (machine->regs[R2_X86_RAX] >> 8) & 255
                                            : normalize_value(machine->regs[R2_X86_RDX], insn->width);
            __uint128_t combined = ((__uint128_t)high << bits) | low;
            uint64_t result_low, result_high;
            if (insn->subop == 4) {
                __uint128_t product = (__uint128_t)low * operand;
                result_low = (uint64_t)product; result_high = (uint64_t)(product >> bits);
                machine->flags.cf = machine->flags.of = result_high != 0;
            } else if (insn->subop == 5) {
                __int128 product = signed_value(low, insn->width) * signed_value(operand, insn->width);
                result_low = (uint64_t)product;
                result_high = (uint64_t)((__uint128_t)product >> bits);
                __int128 bound = (__int128)1 << (bits - 1u);
                machine->flags.cf = machine->flags.of = product < -bound || product >= bound;
            } else if (operand == 0) {
                r2_set_error(error, error_len, "guest integer divide by zero");
                status = R2_ERR_UNSUPPORTED; goto execution_error;
            } else if (insn->subop == 6) {
                __uint128_t quotient = combined / operand;
                if (quotient > width_mask(insn->width)) {
                    r2_set_error(error, error_len, "guest integer quotient overflow");
                    status = R2_ERR_UNSUPPORTED; goto execution_error;
                }
                result_low = (uint64_t)quotient; result_high = (uint64_t)(combined % operand);
            } else {
                __int128 dividend;
                if (bits == 64) dividend = (__int128)combined;
                else dividend = (__int128)combined - ((high & width_sign_bit(insn->width))
                                                     ? ((__int128)1 << (bits * 2u)) : 0);
                __int128 divisor = signed_value(operand, insn->width);
                __int128 minimum128 = -((__int128)1 << 126) - ((__int128)1 << 126);
                if (dividend == minimum128 && divisor == -1) {
                    r2_set_error(error, error_len, "guest integer quotient overflow");
                    status = R2_ERR_UNSUPPORTED; goto execution_error;
                }
                __int128 quotient = dividend / divisor;
                __int128 bound = (__int128)1 << (bits - 1u);
                if (quotient < -bound || quotient >= bound) {
                    r2_set_error(error, error_len, "guest integer quotient overflow");
                    status = R2_ERR_UNSUPPORTED; goto execution_error;
                }
                result_low = (uint64_t)quotient; result_high = (uint64_t)(dividend % divisor);
            }
            if (insn->width == 1) set_reg_width(machine->regs, R2_X86_RAX,
                (result_low & 255) | ((result_high & 255) << 8), 2);
            else {
                set_reg_width(machine->regs, R2_X86_RAX, result_low, insn->width);
                set_reg_width(machine->regs, R2_X86_RDX, result_high, insn->width);
            }
            break;
        }
        case R2_OP_SHIFT: {
            uint64_t value;
            if (insn->mem.present) {
                status = read_mem(machine, eval_mem_addr(insn, machine), insn->width, &value, error, error_len);
                if (status != R2_OK) { goto execution_error; }
            } else value = machine->regs[insn->dst] >> (insn->dst_high8 ? 8u : 0u);
            value = normalize_value(value, insn->width);
            unsigned bits = insn->width * 8u;
            unsigned count = (unsigned)(insn->has_imm ? (uint64_t)insn->imm : machine->regs[R2_X86_RCX]) & (bits == 64 ? 63u : 31u);
            uint64_t shifted = value;
            if (count != 0) {
                bool carry = machine->flags.cf, overflow = machine->flags.of;
                if (insn->subop <= 3) {
                    unsigned ring_bits = bits + (insn->subop >= 2 ? 1u : 0u);
                    unsigned amount = count % ring_bits;
                    __uint128_t ring = value | (insn->subop >= 2 ? (__uint128_t)carry << bits : 0);
                    __uint128_t mask = ((__uint128_t)1 << ring_bits) - 1;
                    if (amount != 0) {
                        ring = (insn->subop == 0 || insn->subop == 2)
                            ? ((ring << amount) | (ring >> (ring_bits - amount))) & mask
                            : ((ring >> amount) | (ring << (ring_bits - amount))) & mask;
                    }
                    shifted = (uint64_t)ring & width_mask(insn->width);
                    carry = insn->subop >= 2 ? (ring >> bits) != 0
                          : insn->subop == 0 ? (shifted & 1) != 0 : (shifted >> (bits - 1u)) != 0;
                    if (count == 1) overflow = insn->subop == 0 || insn->subop == 2
                        ? ((shifted >> (bits - 1u)) != 0) != carry
                        : ((shifted >> (bits - 1u)) ^ (shifted >> (bits - 2u))) & 1u;
                    if (amount != 0) { machine->flags.cf = carry; machine->flags.of = overflow; }
                } else {
                    if (insn->subop == 4 || insn->subop == 6) {
                        carry = count <= bits && ((value >> (bits - count)) & 1u);
                        shifted = count >= bits ? 0 : normalize_value(value << count, insn->width);
                        if (count == 1) overflow = ((shifted >> (bits - 1u)) != 0) != carry;
                    } else if (insn->subop == 5) {
                        carry = count <= bits && ((value >> (count - 1u)) & 1u);
                        shifted = count >= bits ? 0 : value >> count;
                        if (count == 1) overflow = (value >> (bits - 1u)) != 0;
                    } else {
                        carry = count >= bits ? (value >> (bits - 1u)) != 0 : ((value >> (count - 1u)) & 1u);
                        shifted = (uint64_t)(signed_value(value, insn->width) >> (count >= bits ? bits - 1u : count));
                        shifted = normalize_value(shifted, insn->width);
                        if (count == 1) overflow = false;
                    }
                    set_logic_flags_width(&machine->flags, shifted, insn->width);
                    machine->flags.cf = carry; machine->flags.of = overflow;
                }
            }
            if (insn->mem.present) {
                status = write_mem(machine, eval_mem_addr(insn, machine), insn->width, shifted, error, error_len);
                if (status != R2_OK) { goto execution_error; }
            } else if (insn->dst_high8) {
                machine->regs[insn->dst] = (machine->regs[insn->dst] & ~UINT64_C(0xff00)) | (shifted << 8);
            } else set_reg_width(machine->regs, insn->dst, shifted, insn->width);
            break;
        }
        case R2_OP_SSE_FP: case R2_OP_SSE_CONVERT: case R2_OP_SSE_GP: case R2_OP_SSE_SHUFFLE:
            status = execute_sse(machine, insn, error, error_len);
            if (status != R2_OK) { goto execution_error; }
            break;
        case R2_OP_PACKED_EXT: {
            uint8_t source[16],original[16];memcpy(original,machine->xmm[insn->dst],16);
            status=sse_source(machine,insn,source,16,error,error_len);if(status!=R2_OK)goto execution_error;
            uint8_t *destination=machine->xmm[insn->dst];unsigned op=insn->subop;
            if(op==0xe0||op==0xe3) {
                unsigned width=op==0xe0?1:2;
                for(unsigned i=0;i<16;i+=width){uint32_t a=0,b=0;memcpy(&a,original+i,width);memcpy(&b,source+i,width);uint32_t value=(a+b+1)>>1;memcpy(destination+i,&value,width);}
            } else if(op==0xd8||op==0xd9||op==0xdc||op==0xdd||op==0xe8||op==0xe9||op==0xec||op==0xed) {
                unsigned width=(op&1)?2:1;bool signed_lanes=(op&0x20)!=0;
                int64_t low=signed_lanes?-(INT64_C(1)<<(width*8-1)):0,high=(INT64_C(1)<<(width*8-(signed_lanes?1:0)))-1;
                for(unsigned i=0;i<16;i+=width) {
                    uint64_t a=0,b=0;memcpy(&a,original+i,width);memcpy(&b,source+i,width);
                    int64_t left=signed_lanes?(int64_t)signed_value(a,(uint8_t)width):(int64_t)a,right=signed_lanes?(int64_t)signed_value(b,(uint8_t)width):(int64_t)b;
                    int64_t value=(op&4)?left+right:left-right;if(value<low)value=low;if(value>high)value=high;
                    uint64_t bits=(uint64_t)value;memcpy(destination+i,&bits,width);
                }
            } else if(op==0xd5||op==0xe4||op==0xe5||op==0xf5) {
                unsigned width=op==0xf5?4:2;
                for(unsigned i=0;i<16;i+=width) {
                    uint16_t a,b;memcpy(&a,original+i,2);memcpy(&b,source+i,2);uint32_t bits;
                    if(op==0xf5) {
                        int16_t c,d;memcpy(&c,original+i+2,2);memcpy(&d,source+i+2,2);
                        bits=(uint32_t)((int64_t)(int16_t)a*(int16_t)b+(int64_t)c*d);
                    } else if(op==0xe5)bits=(uint32_t)(((int32_t)(int16_t)a*(int16_t)b)>>16);
                    else {bits=(uint32_t)a*b;if(op==0xe4)bits>>=16;}
                    memcpy(destination+i,&bits,width);
                }
            } else if(op==0x0f) {
                for(unsigned i=0;i<16;i++){unsigned index=i+(unsigned)insn->imm;destination[i]=index<16?source[index]:index<32?original[index-16]:0;}
            } else if(op==4) {
                for(unsigned i=0;i<16;i+=2) {
                    int value=(int)original[i]*(int)(int8_t)source[i]+(int)original[i+1]*(int)(int8_t)source[i+1];
                    if(value>32767)value=32767;if(value< -32768)value= -32768;
                    uint16_t bits=(uint16_t)value;memcpy(destination+i,&bits,2);
                }
            } else if(op>=1&&op<=7) {
                unsigned width=op==2||op==6?4:2,lanes=16/width;
                for(unsigned i=0;i<lanes;i++) {
                    const uint8_t *bank=i<lanes/2?original:source;unsigned index=(i%(lanes/2))*2*width;
                    uint64_t left=0,right=0;memcpy(&left,bank+index,width);memcpy(&right,bank+index+width,width);
                    int64_t a=(int64_t)signed_value(left,(uint8_t)width),b=(int64_t)signed_value(right,(uint8_t)width);
                    int64_t value=op<=3?a+b:a-b;
                    if(op==3||op==7){if(value>32767)value=32767;if(value< -32768)value= -32768;}
                    uint64_t bits=(uint64_t)value;memcpy(destination+i*width,&bits,width);
                }
            } else if(op==0x0b) {
                for(unsigned i=0;i<16;i+=2) {
                    int16_t a,b;memcpy(&a,original+i,2);memcpy(&b,source+i,2);
                    int32_t rounded=((int32_t)a*b+16384)>>15;
                    /* PMULHRSW narrows without saturation, including MIN*MIN. */
                    uint16_t bits=(uint16_t)rounded;memcpy(destination+i,&bits,2);
                }
            } else if((op>=8&&op<=10)||(op>=0x1c&&op<=0x1e)) {
                unsigned width=op>=0x1c?1u<<(op-0x1c):1u<<(op-8);
                for(unsigned i=0;i<16;i+=width) {
                    uint64_t a=0,b=0;memcpy(&a,original+i,width);memcpy(&b,source+i,width);
                    int64_t control=(int64_t)signed_value(b,(uint8_t)width);
                    uint64_t value=op>=0x1c?(control<0?UINT64_C(0)-b:b):control<0?UINT64_C(0)-a:control==0?0:a;
                    memcpy(destination+i,&value,width);
                }
            } else {r2_set_error(error,error_len,"invalid extended packed integer opcode");status=R2_ERR_UNSUPPORTED;goto execution_error;}
            break;
        }
        case R2_OP_PACKED_INT: {
            uint8_t source[16], mask[16], original[16];
            memcpy(mask, machine->xmm[0], 16);
            memcpy(original, machine->xmm[insn->dst], 16);
            if (insn->subop == 15 || insn->subop == 16) {
                if (insn->mem.present && (eval_mem_addr(insn, machine) & 15)) {
                    r2_set_error(error, error_len, "unaligned packed saturation operand"); status = R2_ERR_UNSUPPORTED; goto execution_error;
                }
                status = sse_source(machine, insn, source, 16, error, error_len);
                if (status != R2_OK) { goto execution_error; }
                int64_t low = insn->subop == 16 ? 0 : -(INT64_C(1) << (insn->width * 8 - 1));
                int64_t high = (INT64_C(1) << (insn->width * 8 - (insn->subop == 15))) - 1;
                for (unsigned bank = 0; bank < 2; bank++) for (unsigned i = 0; i < 16u / insn->src_width; i++) {
                    uint64_t raw = 0; memcpy(&raw, (bank ? source : original) + i * insn->src_width, insn->src_width);
                    int64_t value = (int64_t)signed_value(raw, insn->src_width);
                    if (value < low) value = low; if (value > high) value = high;
                    memcpy(machine->xmm[insn->dst] + bank * 8 + i * insn->width, &value, insn->width);
                }
                break;
            }
            if (insn->subop >= 10 && insn->subop <= 14) {
                uint64_t count = (uint64_t)insn->imm;
                if (!insn->has_imm) {
                    if (insn->mem.present && (eval_mem_addr(insn, machine) & 15)) {
                        r2_set_error(error, error_len, "unaligned packed shift count"); status = R2_ERR_UNSUPPORTED; goto execution_error;
                    }
                    status = sse_source(machine, insn, source, 16, error, error_len);
                    if (status != R2_OK) { goto execution_error; }
                    memcpy(&count, source, 8);
                }
                uint8_t *destination = machine->xmm[insn->dst];
                if (insn->subop >= 13) {
                    memset(destination, 0, 16);
                    if (count < 16) {
                        if (insn->subop == 13) memcpy(destination, original + count, 16 - (size_t)count);
                        else memcpy(destination + count, original, 16 - (size_t)count);
                    }
                } else for (unsigned i = 0; i < 16; i += insn->src_width) {
                    uint64_t value = 0; unsigned bits = insn->src_width * 8;
                    memcpy(&value, original + i, insn->src_width);
                    if (insn->subop == 11) value = (uint64_t)(signed_value(value, insn->src_width) >> (count >= bits ? bits - 1 : (unsigned)count));
                    else value = count >= bits ? 0 : insn->subop == 10 ? value >> count : value << count;
                    memcpy(destination + i, &value, insn->src_width);
                }
                break;
            }
            if ((insn->subop != 7 && insn->subop != 8) && insn->mem.present && (eval_mem_addr(insn, machine) & 15)) {
                r2_set_error(error, error_len, "unaligned packed integer operand"); status = R2_ERR_UNSUPPORTED; goto execution_error;
            }
            size_t input_size = (insn->subop == 7 || insn->subop == 8) ? (16u / insn->width) * insn->src_width : 16;
            status = sse_source(machine, insn, source, input_size, error, error_len);
            if (status != R2_OK) { goto execution_error; }
            if (insn->subop == 7 || insn->subop == 8) {
                for (unsigned i = 0; i < 16u / insn->width; i++) {
                    uint64_t value = 0;
                    memcpy(&value, source + i * insn->src_width, insn->src_width);
                    if (insn->subop == 7 && (value & width_sign_bit(insn->src_width))) value |= ~width_mask(insn->src_width);
                    memcpy(machine->xmm[insn->dst] + i * insn->width, &value, insn->width);
                }
                break;
            }
            if (insn->subop == 17) {
                uint64_t both = 0, outside = 0;
                for (unsigned i = 0; i < 16; i++) { both |= original[i] & source[i]; outside |= (uint8_t)~original[i] & source[i]; }
                machine->flags.zf = both == 0; machine->flags.cf = outside == 0;
                machine->flags.of = machine->flags.sf = machine->flags.af = machine->flags.pf = false;
                break;
            }
            if (insn->subop >= 22) {
                for (unsigned i = 0; i < 16; i += 8) {
                    uint64_t value = 0;
                    if (insn->subop == 22) for (unsigned j = 0; j < 8; j++) value += (unsigned)abs((int)original[i+j] - source[i+j]);
                    else {
                        uint32_t left, right; memcpy(&left, original+i, 4); memcpy(&right, source+i, 4);
                        value = insn->subop == 23 ? (uint64_t)left * right : (uint64_t)((int64_t)(int32_t)left * (int32_t)right);
                    }
                    memcpy(machine->xmm[insn->dst] + i, &value, 8);
                }
                break;
            }
            unsigned lane = insn->src_width;
            for (unsigned i = 0; i < 16; i += lane) {
                if (insn->subop == 9) {
                    if (((unsigned)insn->imm >> (i / lane)) & 1) memcpy(machine->xmm[insn->dst] + i, source + i, lane);
                    continue;
                }
                if (insn->subop == 6) {
                    machine->xmm[insn->dst][i] = source[i] & 0x80 ? 0 : original[source[i] & 15];
                    continue;
                }
                if (insn->subop == 4) {
                    if (mask[i + lane - 1] & 0x80) memcpy(machine->xmm[insn->dst] + i, source + i, lane);
                    continue;
                }
                uint64_t left = 0, right = 0, value;
                memcpy(&left, machine->xmm[insn->dst] + i, lane); memcpy(&right, source + i, lane);
                if (insn->subop < 2) {
                    uint64_t sign = UINT64_C(1) << (lane * 8 - 1);
                    bool test = insn->subop == 0 ? (left ^ sign) > (right ^ sign) : left == right;
                    value = test ? UINT64_MAX : 0;
                } else if (insn->subop >= 18 && insn->subop <= 21) {
                    uint64_t sign = insn->subop < 20 ? width_sign_bit((uint8_t)lane) : 0;
                    bool smaller = (left ^ sign) < (right ^ sign);
                    value = (smaller != ((insn->subop & 1) != 0)) ? left : right;
                } else value = insn->subop == 2 ? left + right : insn->subop == 5 ? left * right : left - right;
                memcpy(machine->xmm[insn->dst] + i, &value, lane);
            }
            break;
        }
        case R2_OP_VECTOR: {
            if (insn->mem.present && insn->subop != 0 && (eval_mem_addr(insn, machine) & 15u) != 0) {
                r2_set_error(error, error_len, "unaligned guest aligned-SSE access");
                status = R2_ERR_UNSUPPORTED; goto execution_error;
            }
            uint8_t source[16];
            if (insn->mem.present && !insn->mem_is_dst) {
                uint8_t *bytes = memory_pointer(machine, eval_mem_addr(insn, machine), insn->width,
                                                false, error, error_len);
                if (bytes == NULL) { status = R2_ERR_UNSUPPORTED; goto execution_error; }
                memcpy(source, bytes, insn->width);
            } else memcpy(source, machine->xmm[insn->src], insn->width);
            if (insn->mem.present && insn->mem_is_dst) {
                uint8_t *bytes = memory_pointer(machine, eval_mem_addr(insn, machine), insn->width,
                                                true, error, error_len);
                if (bytes == NULL) { status = R2_ERR_UNSUPPORTED; goto execution_error; }
                memcpy(bytes, source, insn->width);
            } else if (insn->imm == 1) {
                for (size_t i = 0; i < insn->width; i += 1) machine->xmm[insn->dst][i] ^= source[i];
            } else if (insn->imm >= 3) {
                for (size_t i = 0; i < 16; i++) {
                    uint8_t *destination = &machine->xmm[insn->dst][i];
                    *destination = insn->imm == 3 ? *destination & source[i] :
                                   insn->imm == 4 ? (uint8_t)~*destination & source[i] : *destination | source[i];
                }
            } else if (insn->imm == 2) {
                for (size_t i = 0; i < 16; i += 4) {
                    uint32_t left, right;
                    memcpy(&left, machine->xmm[insn->dst] + i, 4);
                    memcpy(&right, source + i, 4);
                    uint32_t equal = left == right ? UINT32_MAX : 0;
                    memcpy(machine->xmm[insn->dst] + i, &equal, 4);
                }
            } else {
                if (insn->mem.present) memset(machine->xmm[insn->dst], 0, 16);
                memcpy(machine->xmm[insn->dst], source, insn->width);
            }
            break;
        }
        case R2_OP_BINARY: {
            uint64_t lhs, rhs;
            if (insn->locked) lhs = 0;
            else if (insn->mem.present && insn->mem_is_dst) {
                status = read_mem(machine, eval_mem_addr(insn, machine), insn->width, &lhs, error, error_len);
            } else {
                lhs = machine->regs[insn->dst] >> (insn->dst_high8 ? 8u : 0u);
            }
            if (status != R2_OK) { goto execution_error; }
            if (insn->has_imm) rhs = (uint64_t)insn->imm;
            else if (insn->mem.present && !insn->mem_is_dst) {
                status = read_mem(machine, eval_mem_addr(insn, machine), insn->width, &rhs, error, error_len);
            } else rhs = machine->regs[insn->src] >> (insn->src_high8 ? 8u : 0u);
            if (status != R2_OK) { goto execution_error; }
            lhs = normalize_value(lhs, insn->width);
            rhs = normalize_value(rhs, insn->width);
            if (insn->locked) {
                status = atomic_memory(machine, insn, eval_mem_addr(insn, machine), rhs, machine->flags.cf, &lhs, error, error_len);
                if (status != R2_OK) { goto execution_error; }
            }
            uint64_t carry = machine->flags.cf, value = 0;
            switch (insn->alu) {
            case R2_ALU_ADD: case R2_ALU_ADC: {
                uint64_t partial = normalize_value(lhs + rhs, insn->width);
                value = normalize_value(partial + (insn->alu == R2_ALU_ADC ? carry : 0), insn->width);
                set_add_flags(&machine->flags, lhs, rhs, value, insn->width);
                machine->flags.cf = partial < lhs || value < partial;
                break;
            }
            case R2_ALU_SUB: case R2_ALU_SBB: case R2_ALU_CMP:
                carry = insn->alu == R2_ALU_SBB ? carry : 0;
                value = normalize_value(lhs - rhs - carry, insn->width);
                set_sub_flags(&machine->flags, lhs, rhs, value, insn->width);
                machine->flags.cf = lhs < rhs || (carry && lhs == rhs);
                break;
            case R2_ALU_OR: value = lhs | rhs; break;
            case R2_ALU_AND: case R2_ALU_TEST: value = lhs & rhs; break;
            case R2_ALU_XOR: value = lhs ^ rhs; break;
            }
            if (insn->alu == R2_ALU_OR || insn->alu == R2_ALU_AND ||
                insn->alu == R2_ALU_XOR || insn->alu == R2_ALU_TEST)
                set_logic_flags_width(&machine->flags, value, insn->width);
            if (insn->alu != R2_ALU_CMP && insn->alu != R2_ALU_TEST) {
                if (insn->locked) { /* The compare-exchange loop already stored the result. */ }
                else if (insn->mem.present && insn->mem_is_dst) {
                    status = write_mem(machine, eval_mem_addr(insn, machine), insn->width, value, error, error_len);
                    if (status != R2_OK) { goto execution_error; }
                } else if (insn->dst_high8) {
                    machine->regs[insn->dst] = (machine->regs[insn->dst] & ~UINT64_C(0xff00)) | (value << 8);
                } else set_reg_width(machine->regs, insn->dst, value, insn->width);
            }
            break;
        }
        case R2_OP_MOV_REG:
            if (insn->dst_high8) machine->regs[insn->dst] =
                (machine->regs[insn->dst] & ~UINT64_C(0xff00)) |
                (((machine->regs[insn->src] >> (insn->src_high8 ? 8u : 0u)) & 0xff) << 8);
            else set_reg_width(machine->regs, insn->dst,
                               machine->regs[insn->src] >> (insn->src_high8 ? 8u : 0u), insn->width);
            break;
        case R2_OP_XCHG:
        case R2_OP_CMPXCHG:
        case R2_OP_XADD: {
            uint64_t source = normalize_value(machine->regs[insn->src] >> (insn->src_high8 ? 8 : 0), insn->width);
            uint64_t accumulator = normalize_value(machine->regs[R2_X86_RAX], insn->width);
            uint64_t old;
            if (insn->mem.present) {
                status = atomic_memory(machine, insn, eval_mem_addr(insn, machine),
                                        source, accumulator, &old, error, error_len);
                if (status != R2_OK) { goto execution_error; }
            } else old = normalize_value(machine->regs[insn->dst] >> (insn->dst_high8 ? 8 : 0), insn->width);
            if (insn->op == R2_OP_CMPXCHG) {
                set_sub_flags(&machine->flags, accumulator, old, accumulator - old, insn->width);
                if (accumulator == old) {
                    if (!insn->mem.present) set_operand_reg(machine->regs, insn->dst, insn->dst_high8, source, insn->width);
                } else set_reg_width(machine->regs, R2_X86_RAX, old, insn->width);
            } else {
                set_operand_reg(machine->regs, insn->src, insn->src_high8, old, insn->width);
                uint64_t value = insn->op == R2_OP_XADD ? old + source : source;
                if (insn->op == R2_OP_XADD) set_add_flags(&machine->flags, old, source, value, insn->width);
                if (!insn->mem.present) set_operand_reg(machine->regs, insn->dst, insn->dst_high8, value, insn->width);
            }
            break;
        }
        case R2_OP_INC:
        case R2_OP_DEC:
        case R2_OP_NEG:
        case R2_OP_NOT: {
            uint64_t value;
            if (insn->locked) {
                status = atomic_memory(machine, insn, eval_mem_addr(insn, machine), 0, 0, &value, error, error_len);
                if (status != R2_OK) { goto execution_error; }
            } else if (insn->mem.present) {
                status = read_mem(machine, eval_mem_addr(insn, machine), insn->width, &value, error, error_len);
                if (status != R2_OK) { goto execution_error; }
            } else value = machine->regs[insn->dst] >> (insn->dst_high8 ? 8u : 0u);
            value = normalize_value(value, insn->width);
            uint64_t result_value = insn->op == R2_OP_INC ? value + 1
                                  : insn->op == R2_OP_DEC ? value - 1
                                  : insn->op == R2_OP_NEG ? 0 - value : ~value;
            result_value = normalize_value(result_value, insn->width);
            bool carry = machine->flags.cf;
            if (insn->op == R2_OP_INC) set_add_flags(&machine->flags, value, 1, result_value, insn->width);
            else if (insn->op == R2_OP_DEC) set_sub_flags(&machine->flags, value, 1, result_value, insn->width);
            else if (insn->op == R2_OP_NEG) set_sub_flags(&machine->flags, 0, value, result_value, insn->width);
            if (insn->op == R2_OP_INC || insn->op == R2_OP_DEC) machine->flags.cf = carry;
            if (insn->locked) { /* Result committed atomically above. */ }
            else if (insn->mem.present) {
                status = write_mem(machine, eval_mem_addr(insn, machine), insn->width, result_value, error, error_len);
                if (status != R2_OK) { goto execution_error; }
            } else if (insn->dst_high8) {
                machine->regs[insn->dst] = (machine->regs[insn->dst] & ~UINT64_C(0xff00)) | (result_value << 8);
            } else set_reg_width(machine->regs, insn->dst, result_value, insn->width);
            break;
        }
        case R2_OP_CMOV: {
            uint64_t value = 0;
            if (insn->mem.present) {
                status = read_mem(machine, eval_mem_addr(insn, machine), insn->width, &value, error, error_len);
                if (status != R2_OK) { goto execution_error; }
            } else value = machine->regs[insn->src];
            if (eval_cc(insn->cc, &machine->flags)) set_reg_width(machine->regs, insn->dst, value, insn->width);
            else if (insn->width == 4) machine->regs[insn->dst] = (uint32_t)machine->regs[insn->dst];
            break;
        }
        case R2_OP_MOVZX:
        case R2_OP_MOVSX: {
            uint64_t value;
            if (insn->mem.present) {
                uint64_t addr = eval_mem_addr(insn, machine);
                status = read_mem(machine, addr, insn->src_width, &value, error, error_len);
                if (status != R2_OK) {
                    goto execution_error;
                }
            } else {
                value = machine->regs[insn->src] >> (insn->src_high8 ? 8u : 0u);
            }
            unsigned bits = insn->src_width * 8u;
            uint64_t mask = (UINT64_C(1) << bits) - 1u;
            value &= mask;
            if (insn->op == R2_OP_MOVSX && (value & (UINT64_C(1) << (bits - 1u))) != 0) {
                value |= ~mask;
            }
            set_reg_width(machine->regs, insn->dst, value, insn->width);
            break;
        }
        case R2_OP_SETCC: {
            uint64_t value = eval_cc(insn->cc, &machine->flags);
            if (insn->mem.present) {
                uint64_t addr = eval_mem_addr(insn, machine);
                status = write_mem(machine, addr, 1, value, error, error_len);
                if (status != R2_OK) {
                    goto execution_error;
                }
            } else {
                unsigned shift = insn->dst_high8 ? 8u : 0u;
                uint64_t mask = UINT64_C(0xff) << shift;
                machine->regs[insn->dst] =
                    (machine->regs[insn->dst] & ~mask) | (value << shift);
            }
            break;
        }
        case R2_OP_MOV_LOAD: {
            uint64_t addr = eval_mem_addr(insn, machine);
            uint64_t value = 0;
            status = read_mem(machine, addr, insn->width, &value, error, error_len);
            if (status != R2_OK) {
                goto execution_error;
            }
            if (insn->dst_high8) machine->regs[insn->dst] =
                (machine->regs[insn->dst] & ~UINT64_C(0xff00)) | ((value & 0xff) << 8);
            else set_reg_width(machine->regs, insn->dst, value, insn->width);
            break;
        }
        case R2_OP_MOV_STORE: {
            uint64_t addr = eval_mem_addr(insn, machine);
            status =
                write_mem(machine, addr, insn->width,
                           machine->regs[insn->src] >> (insn->src_high8 ? 8u : 0u), error, error_len);
            if (status != R2_OK) {
                goto execution_error;
            }
            break;
        }
        case R2_OP_MOV_MEM_IMM: {
            uint64_t addr = eval_mem_addr(insn, machine);
            status = write_mem(machine, addr, insn->width, (uint64_t)insn->imm, error, error_len);
            if (status != R2_OK) {
                goto execution_error;
            }
            break;
        }
        case R2_OP_LEA:
            set_reg_width(machine->regs, insn->dst, eval_mem_addr(insn, machine), insn->width);
            break;
        case R2_OP_ADD_IMM: {
            uint64_t lhs = machine->regs[insn->dst];
            uint64_t rhs = (uint64_t)insn->imm;
            uint64_t value = normalize_value(lhs + rhs, insn->width);
            set_reg_width(machine->regs, insn->dst, value, insn->width);
            set_add_flags(&machine->flags, lhs, rhs, value, insn->width);
            break;
        }
        case R2_OP_ADD_REG: {
            uint64_t lhs = machine->regs[insn->dst];
            uint64_t rhs = machine->regs[insn->src];
            uint64_t value = normalize_value(lhs + rhs, insn->width);
            set_reg_width(machine->regs, insn->dst, value, insn->width);
            set_add_flags(&machine->flags, lhs, rhs, value, insn->width);
            break;
        }
        case R2_OP_SUB_IMM: {
            uint64_t lhs = machine->regs[insn->dst];
            uint64_t rhs = (uint64_t)insn->imm;
            uint64_t value = normalize_value(lhs - rhs, insn->width);
            set_reg_width(machine->regs, insn->dst, value, insn->width);
            set_sub_flags(&machine->flags, lhs, rhs, value, insn->width);
            break;
        }
        case R2_OP_SUB_REG: {
            uint64_t lhs = machine->regs[insn->dst];
            uint64_t rhs = machine->regs[insn->src];
            uint64_t value = normalize_value(lhs - rhs, insn->width);
            set_reg_width(machine->regs, insn->dst, value, insn->width);
            set_sub_flags(&machine->flags, lhs, rhs, value, insn->width);
            break;
        }
        case R2_OP_ADC_IMM:
        case R2_OP_ADC_REG: {
            uint64_t lhs = normalize_value(machine->regs[insn->dst], insn->width);
            uint64_t rhs = normalize_value(insn->op == R2_OP_ADC_IMM
                                           ? (uint64_t)insn->imm : machine->regs[insn->src],
                                           insn->width);
            uint64_t carry = machine->flags.cf;
            uint64_t partial = normalize_value(lhs + rhs, insn->width);
            uint64_t value = normalize_value(partial + carry, insn->width);
            set_reg_width(machine->regs, insn->dst, value, insn->width);
            set_add_flags(&machine->flags, lhs, rhs, value, insn->width);
            machine->flags.cf = partial < lhs || value < partial;
            break;
        }
        case R2_OP_SBB_IMM:
        case R2_OP_SBB_REG: {
            uint64_t lhs = normalize_value(machine->regs[insn->dst], insn->width);
            uint64_t rhs = normalize_value(insn->op == R2_OP_SBB_IMM
                                           ? (uint64_t)insn->imm : machine->regs[insn->src],
                                           insn->width);
            uint64_t borrow = machine->flags.cf;
            uint64_t value = normalize_value(lhs - rhs - borrow, insn->width);
            set_reg_width(machine->regs, insn->dst, value, insn->width);
            set_sub_flags(&machine->flags, lhs, rhs, value, insn->width);
            machine->flags.cf = lhs < rhs || (borrow != 0 && lhs == rhs);
            break;
        }
        case R2_OP_XOR_REG: {
            uint64_t value =
                normalize_value(machine->regs[insn->dst] ^ machine->regs[insn->src], insn->width);
            set_reg_width(machine->regs, insn->dst, value, insn->width);
            set_logic_flags_width(&machine->flags, value, insn->width);
            break;
        }
        case R2_OP_CMP_IMM: {
            uint64_t lhs = machine->regs[insn->dst];
            uint64_t rhs = (uint64_t)insn->imm;
            set_sub_flags(&machine->flags, lhs, rhs, lhs - rhs, insn->width);
            break;
        }
        case R2_OP_CMP_REG: {
            uint64_t lhs = machine->regs[insn->dst];
            uint64_t rhs = machine->regs[insn->src];
            set_sub_flags(&machine->flags, lhs, rhs, lhs - rhs, insn->width);
            break;
        }
        case R2_OP_TEST_REG: {
            uint64_t value =
                normalize_value(machine->regs[insn->dst] & machine->regs[insn->src], insn->width);
            set_logic_flags_width(&machine->flags, value, insn->width);
            break;
        }
        case R2_OP_PUSH_REG: {
            uint64_t value = machine->regs[insn->src];
            if (insn->mem.present) {
                status = read_u64(machine, eval_mem_addr(insn, machine), &value, error, error_len);
                if (status != R2_OK) { goto execution_error; }
            }
            status = push_u64(machine, value, error, error_len);
            if (status != R2_OK) {
                goto execution_error;
            }
            break;
        }
        case R2_OP_PUSH_IMM:
            status = push_u64(machine, (uint64_t)insn->imm, error, error_len);
            if (status != R2_OK) {
                goto execution_error;
            }
            break;
        case R2_OP_POP_REG: {
            uint64_t value;
            status = pop_u64(machine, &value, error, error_len);
            if (status != R2_OK) {
                goto execution_error;
            }
            /* The stack increment precedes destination address calculation,
             * and POP RSP must assign the popped value after that increment. */
            if (insn->mem.present)
                status = write_u64(machine, eval_mem_addr(insn, machine), value, error, error_len);
            else machine->regs[insn->dst] = value;
            if (status != R2_OK) goto execution_error;
            break;
        }
        case R2_OP_CALL:
            status = push_u64(machine,
                              instruction_next_guest_addr(insn, machine),
                              error,
                              error_len);
            if (status != R2_OK) {
                goto execution_error;
            }
            status = jump_to_target(block, insn->target, &pc, error, error_len);
            if (status != R2_OK) {
                goto execution_error;
            }
            continue;
        case R2_OP_CALL_INDIRECT: {
            uint64_t target = 0;
            status = eval_indirect_target(insn, machine, &target, error, error_len);
            if (status != R2_OK) {
                goto execution_error;
            }
            status = push_u64(machine,
                              instruction_next_guest_addr(insn, machine),
                              error,
                              error_len);
            if (status != R2_OK) {
                goto execution_error;
            }
            status =
                guest_code_addr_to_pc(block, machine, target, &pc, error, error_len);
            if (status != R2_OK) {
                goto execution_error;
            }
            continue;
        }
        case R2_OP_JMP:
            status = jump_to_target(block, insn->target, &pc, error, error_len);
            if (status != R2_OK) {
                goto execution_error;
            }
            continue;
        case R2_OP_JMP_INDIRECT: {
            uint64_t target = 0;
            status = eval_indirect_target(insn, machine, &target, error, error_len);
            if (status != R2_OK) {
                goto execution_error;
            }
            status =
                guest_code_addr_to_pc(block, machine, target, &pc, error, error_len);
            if (status != R2_OK) {
                goto execution_error;
            }
            continue;
        }
        case R2_OP_JCC:
            if (eval_cc(insn->cc, &machine->flags)) {
                status = jump_to_target(block, insn->target, &pc, error, error_len);
                if (status != R2_OK) {
                    goto execution_error;
                }
                continue;
            }
            break;
        case R2_OP_SYSCALL:
            if (env != NULL && env->native != NULL && r2_native_is_call(machine->regs[R2_X86_RAX])) {
                bool exited = false;
                r2_native_set_stack(env->native, machine->threads[machine->current_thread].stack_base,
                                      machine->threads[machine->current_thread].stack_size);
                status = r2_native_call(env->native, machine->regs, machine->xmm, &exited, error, error_len);
                machine->mxcsr=r2_native_get_mxcsr(env->native);
                r2_native_set_stack(env->native, machine->threads[machine->current_thread].stack_base,
                                      machine->threads[machine->current_thread].stack_size);
                if (exited && env->process != NULL && env->process->kind==R2_PROCESS_RUNNING && !r2_native_thread_returning(env->native)) {
                    env->process->kind = R2_PROCESS_EXIT;
                    env->process->code = machine->regs[R2_X86_RAX];
                }
                bool process_exited = env->process != NULL && env->process->kind != R2_PROCESS_RUNNING;
                if (status != R2_OK || exited || process_exited) {
                    if (exited || process_exited) result->rax = process_exited ? env->process->code : machine->regs[R2_X86_RAX];
                    goto execution_error;
                }
                break;
            }
            if (machine->regs[R2_X86_RAX] == R2_MACOS_SYS_READ) {
                status = syscall_read(machine,
                                      machine->regs[R2_X86_RDI],
                                      machine->regs[R2_X86_RSI],
                                      machine->regs[R2_X86_RDX],
                                      error,
                                      error_len);
                if (status != R2_OK) {
                    goto execution_error;
                }
            } else if (machine->regs[R2_X86_RAX] == R2_MACOS_SYS_WRITE) {
                status = syscall_write(machine,
                                       machine->regs[R2_X86_RDI],
                                       machine->regs[R2_X86_RSI],
                                       machine->regs[R2_X86_RDX],
                                       error,
                                       error_len);
                if (status != R2_OK) {
                    goto execution_error;
                }
            } else if (machine->regs[R2_X86_RAX] == R2_MACOS_SYS_OPEN) {
                status = syscall_open(machine,
                                      machine->regs[R2_X86_RDI],
                                      machine->regs[R2_X86_RSI],
                                      machine->regs[R2_X86_RDX],
                                      error,
                                      error_len);
                if (status != R2_OK) {
                    goto execution_error;
                }
            } else if (machine->regs[R2_X86_RAX] == R2_MACOS_SYS_CLOSE) {
                status = syscall_close(machine, machine->regs[R2_X86_RDI], error, error_len);
                if (status != R2_OK) {
                    goto execution_error;
                }
            } else if (machine->regs[R2_X86_RAX] == R2_MACOS_SYS_GETPID) {
                status = syscall_getpid(machine);
                if (status != R2_OK) {
                    goto execution_error;
                }
            } else if (machine->regs[R2_X86_RAX] == R2_MACOS_SYS_KILL) {
                status = syscall_kill(machine,
                                      machine->regs[R2_X86_RDI],
                                      machine->regs[R2_X86_RSI],
                                      error,
                                      error_len);
                if (status != R2_OK) {
                    goto execution_error;
                }
            } else if (machine->regs[R2_X86_RAX] == R2_MACOS_SYS_SIGACTION) {
                status = syscall_sigaction(machine,
                                           machine->regs[R2_X86_RDI],
                                           machine->regs[R2_X86_RSI],
                                           machine->regs[R2_X86_RDX],
                                           error,
                                           error_len);
                if (status != R2_OK) {
                    goto execution_error;
                }
            } else if (machine->regs[R2_X86_RAX] == R2_MACOS_SYS_SIGPROCMASK) {
                status = syscall_sigprocmask(machine,
                                             machine->regs[R2_X86_RDI],
                                             machine->regs[R2_X86_RSI],
                                             machine->regs[R2_X86_RDX],
                                             error,
                                             error_len);
                if (status != R2_OK) {
                    goto execution_error;
                }
            } else if (machine->regs[R2_X86_RAX] == R2_MACOS_SYS_PSYNCH_MUTEXWAIT) {
                status = syscall_psynch_mutexwait(machine, machine->regs[R2_X86_RDI]);
                if (status != R2_OK) {
                    goto execution_error;
                }
            } else if (machine->regs[R2_X86_RAX] == R2_MACOS_SYS_PSYNCH_MUTEXDROP) {
                status =
                    syscall_psynch_mutexdrop(machine, machine->regs[R2_X86_RDI], error, error_len);
                if (status != R2_OK) {
                    goto execution_error;
                }
            } else if (machine->regs[R2_X86_RAX] == R2_MACOS_SYS_PSYNCH_CVBROAD) {
                status = syscall_psynch_cvbroad(machine, machine->regs[R2_X86_RDI]);
                if (status != R2_OK) {
                    goto execution_error;
                }
            } else if (machine->regs[R2_X86_RAX] == R2_MACOS_SYS_PSYNCH_CVSIGNAL) {
                status = syscall_psynch_cvsignal(machine, machine->regs[R2_X86_RDI]);
                if (status != R2_OK) {
                    goto execution_error;
                }
            } else if (machine->regs[R2_X86_RAX] == R2_MACOS_SYS_PSYNCH_CVWAIT) {
                status = syscall_psynch_cvwait(machine,
                                               machine->regs[R2_X86_RDI],
                                               machine->regs[R2_X86_R10],
                                               error,
                                               error_len);
                if (status != R2_OK) {
                    goto execution_error;
                }
            } else if (machine->regs[R2_X86_RAX] == R2_MACOS_SYS_FSTAT) {
                status = syscall_fstat(machine,
                                       machine->regs[R2_X86_RDI],
                                       machine->regs[R2_X86_RSI],
                                       error,
                                       error_len);
                if (status != R2_OK) {
                    goto execution_error;
                }
            } else if (machine->regs[R2_X86_RAX] == R2_MACOS_SYS_MMAP) {
                status = syscall_mmap(machine,
                                      machine->regs[R2_X86_RDI],
                                      machine->regs[R2_X86_RSI],
                                      machine->regs[R2_X86_RDX],
                                      machine->regs[R2_X86_R10],
                                      machine->regs[R2_X86_R8],
                                      machine->regs[R2_X86_R9],
                                      error,
                                      error_len);
                if (status != R2_OK) {
                    goto execution_error;
                }
            } else if (machine->regs[R2_X86_RAX] == R2_MACOS_SYS_BSDTHREAD_CREATE) {
                status = syscall_bsdthread_create(block,
                                                  machine,
                                                  machine->regs[R2_X86_RDI],
                                                  machine->regs[R2_X86_RSI],
                                                  machine->regs[R2_X86_RDX],
                                                  machine->regs[R2_X86_R10],
                                                  machine->regs[R2_X86_R8],
                                                  error,
                                                  error_len);
                if (status != R2_OK) {
                    goto execution_error;
                }
            } else if (machine->regs[R2_X86_RAX] == R2_MACOS_SYS_BSDTHREAD_TERMINATE) {
                status = syscall_bsdthread_terminate(machine, machine->regs[R2_X86_R10]);
                if (status != R2_OK) {
                    goto execution_error;
                }
            } else if (machine->regs[R2_X86_RAX] == R2_MACOS_SYS_BSDTHREAD_REGISTER) {
                status = syscall_bsdthread_register(machine,
                                                    machine->regs[R2_X86_RDI],
                                                    machine->regs[R2_X86_RSI],
                                                    machine->regs[R2_X86_RDX],
                                                    machine->regs[R2_X86_R10],
                                                    machine->regs[R2_X86_R8],
                                                    machine->regs[R2_X86_R9],
                                                    error,
                                                    error_len);
                if (status != R2_OK) {
                    goto execution_error;
                }
            } else if (machine->regs[R2_X86_RAX] == R2_MACOS_SYS_THREAD_SELFID) {
                status = syscall_thread_selfid(machine);
                if (status != R2_OK) {
                    goto execution_error;
                }
            } else if (machine->regs[R2_X86_RAX] == R2_MACOS_SYS_THREAD_SET_TSD_BASE) {
                status = syscall_thread_set_tsd_base(machine, machine->regs[R2_X86_RDI]);
                if (status != R2_OK) {
                    goto execution_error;
                }
            } else if (machine->regs[R2_X86_RAX] == R2_MACOS_SYS_EXIT) {
                result->rax = machine->regs[R2_X86_RDI];
                if (env != NULL && env->process != NULL) {
                    env->process->kind = R2_PROCESS_IMMEDIATE;
                    env->process->code = result->rax;
                }
                status = R2_OK; goto execution_error;
            } else {
                r2_set_error(error,
                             error_len,
                             "unsupported macOS x86_64 syscall 0x%llx",
                             (unsigned long long)machine->regs[R2_X86_RAX]);
                status = R2_ERR_UNSUPPORTED; goto execution_error;
            }
            break;
        case R2_OP_LEAVE: {
            uint64_t value = 0;
            machine->regs[R2_X86_RSP] = machine->regs[R2_X86_RBP];
            status = pop_u64(machine, &value, error, error_len);
            if (status != R2_OK) {
                goto execution_error;
            }
            machine->regs[R2_X86_RBP] = value;
            break;
        }
        case R2_OP_RET: {
            uint64_t ret_offset = 0;
            status = pop_u64(machine, &ret_offset, error, error_len);
            if (status != R2_OK) {
                goto execution_error;
            }
            if (ret_offset == R2_RETURN_SENTINEL) {
                result->rax = machine->regs[R2_X86_RAX];
                status = R2_OK; goto execution_error;
            }
            if (ret_offset == R2_THREAD_RETURN_SENTINEL) {
                r2_guest_thread *thread = &machine->threads[machine->current_thread];
                thread->state = R2_THREAD_EXITED;
                thread->exit_value = machine->regs[R2_X86_RAX];
                continue;
            }
            if (ret_offset == R2_SIGNAL_RETURN_SENTINEL) {
                status = restore_signal_frame(block, machine, &pc, error, error_len);
                if (status != R2_OK) {
                    goto execution_error;
                }
                continue;
            }
            status =
                guest_code_addr_to_pc(block, machine, ret_offset, &pc, error, error_len);
            if (status != R2_OK) {
                goto execution_error;
            }
            continue;
        }
        default:
            r2_set_error(error, error_len, "unknown decoded op at instruction %zu", pc);
            status = R2_ERR_UNSUPPORTED; goto execution_error;
        }

        if (block->runtime != NULL) {
            status = r2_native_decode_next((r2_block *)block,next_address,next_pc,&next_pc,error,error_len);
            if (status != R2_OK) {
                goto execution_error;
            }
        }
        pc = next_pc;
    }

execution_error:
    if (status == R2_ERR_UNSUPPORTED && machine->instruction_active && machine->memory_fault && env != NULL && env->native != NULL) {
        unsigned signum = SIGSEGV; uint32_t bit = UINT32_C(1) << (signum - 1);
        r2_sigaction_state *action = &machine->sigactions[signum];
        r2_native_signal_action(env->native, signum, &action->handler, &action->mask, &action->flags);
        if (action->handler > R2_DARWIN_SIG_IGN && !(machine->signal_mask & bit)) {
            machine->regs[R2_X86_RSP] = machine->instruction_sp; machine->flags = machine->instruction_flags;
            machine->instruction_active = false; machine->memory_fault = false; machine->delivering_fault = true;
            uint32_t pending = machine->pending_signals; machine->pending_signals = bit;
            bool completed = false;
            if (getenv("R2_TRACE_IMPORTS") || getenv("R2_TRACE_FAULTS")) fprintf(stderr, "guest SIGSEGV RIP=0x%llx address=0x%llx write=%d\n", (unsigned long long)machine->current_address, (unsigned long long)machine->fault_address, machine->fault_write);
            status = deliver_pending_signal(block, machine, &pc, result, &completed, error, error_len);
            machine->pending_signals |= pending; machine->delivering_fault = false;
            if (status == R2_OK && !completed) { if (error != NULL && error_len) error[0] = 0; goto resume_execution; }
        }
    }
    destroy_machine(machine, status, error);
    free(machine);
    return status;
}

r2_status r2_interp_block(const r2_block *block,
                          r2_result *result,
                          char *error,
                          size_t error_len)
{
    r2_interp_env env = {0};
    return r2_interp_block_with_env(block, &env, result, error, error_len);
}
