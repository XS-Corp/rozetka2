#include <signal.h>
#include <sys/ucontext.h>
#include <sys/mman.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void *page;
static size_t page_size;
static unsigned seen, asynchronous;
static int redirect, corrupt_context;
static uintptr_t copy_source;
static int copy_read_fault;
static uintptr_t expected_sp;
extern const char r2_fault_instruction[], r2_resume_instruction[];
static void require(int ok, const char *message) {
    if (!ok) { fprintf(stderr, "FAIL signal guest: %s\n", message); exit(91); }
}
static void handle(int signal, siginfo_t *info, void *opaque) {
    ucontext_t *context = opaque;
    require(signal == SIGSEGV && info->si_signo == SIGSEGV, "signal number");
    require(info->si_code == SEGV_ACCERR && info->si_addr == page, "fault address and code");
    require(context->uc_mcsize == sizeof(*context->uc_mcontext), "real Darwin mcontext size");
    require(context->uc_mcontext->__es.__trapno == 14 && context->uc_mcontext->__es.__faultvaddr == (uintptr_t)page, "x86 page-fault exception state");
    require(context->uc_mcontext->__ss.__rip == (uintptr_t)r2_fault_instruction, "faulting guest RIP");
    require(context->uc_mcontext->__ss.__rsp == expected_sp, "interrupted guest RSP");
    require(context->uc_mcontext->__ss.__rax == UINT64_C(0x1122334455667788), "pre-instruction register state");
    uint64_t vector; memcpy(&vector, &context->uc_mcontext->__fs.__fpu_xmm15, 8);
    require(vector == UINT64_C(0x1122334455667788), "saved XMM15");
    vector = UINT64_C(0xaabbccddeeff0011); memcpy(&context->uc_mcontext->__fs.__fpu_xmm15, &vector, 8);
    require(++seen <= 2, "fault instruction must recover");
    if (redirect) {
        context->uc_mcontext->__ss.__rip = (uintptr_t)r2_resume_instruction;
        context->uc_mcontext->__ss.__rax = 42;
    } else {
        require(mprotect(page, page_size, PROT_READ | PROT_WRITE) == 0, "unprotect faulting page");
        *(unsigned *)page = 77;
    }
    if (corrupt_context) context->uc_mcsize = 0;
}
static void handle_write(int signal, siginfo_t *info, void *opaque) {
    ucontext_t *context = opaque;
    require(signal == SIGSEGV && info->si_addr == page, "write fault address");
    require((context->uc_mcontext->__es.__err & 2) != 0, "write page-fault error bit");
    require((context->uc_mcontext->__ss.__rflags & 1) != 0, "fault rolls back arithmetic flags");
    require(mprotect(page, page_size, PROT_READ | PROT_WRITE) == 0, "unprotect arithmetic destination");
}
static void handle_pop(int signal, siginfo_t *info, void *opaque) {
    ucontext_t *context = opaque;
    require(signal == SIGSEGV && info->si_addr == page, "POP write fault address");
    require(context->uc_mcontext->__ss.__rsp == expected_sp, "POP fault restores pre-instruction RSP");
    require((context->uc_mcontext->__es.__err & 2) != 0, "POP write fault metadata");
    require(mprotect(page, page_size, PROT_READ | PROT_WRITE) == 0, "unprotect POP destination");
}
static void handle_rep(int signal, siginfo_t *info, void *opaque) {
    ucontext_t *context = opaque;
    uintptr_t boundary = (uintptr_t)page + page_size;
    require(signal == SIGSEGV && (uintptr_t)info->si_addr == boundary, "REP store boundary fault");
    require(context->uc_mcontext->__ss.__rcx == 2 && context->uc_mcontext->__ss.__rdi == boundary,
            "REP fault exposes completed iterations and remaining count");
    require(*(uint64_t *)(boundary - 16) == 42 && *(uint64_t *)(boundary - 8) == 42, "completed REP stores persist");
    require(mprotect((void *)boundary, page_size, PROT_READ | PROT_WRITE) == 0, "unprotect remaining REP stores");
}
static void handle_rep_copy(int signal, siginfo_t *info, void *opaque) {
    ucontext_t *context = opaque;
    uintptr_t boundary = (uintptr_t)page + page_size;
    require(signal == SIGSEGV && (uintptr_t)info->si_addr == boundary, "REP MOVS boundary fault");
    require(context->uc_mcontext->__ss.__rcx == 2, "REP MOVS remaining count at fault");
    require(context->uc_mcontext->__ss.__rsi == (copy_read_fault ? boundary : copy_source + 16), "REP MOVS source advanced only for completed copies");
    require(context->uc_mcontext->__ss.__rdi == (copy_read_fault ? copy_source + 16 : boundary), "REP MOVS destination advanced only for completed copies");
    require(((context->uc_mcontext->__es.__err & 2) != 0) == !copy_read_fault, "REP MOVS read/write fault metadata");
    uint64_t *completed = (uint64_t *)(copy_read_fault ? copy_source : boundary - 16);
    require(completed[0] == 17 && completed[1] == 29, "completed REP copies persist before fault");
    require(mprotect((void *)boundary, page_size, PROT_READ | PROT_WRITE) == 0, "unprotect remaining REP copies");
}
static void handle_x87(int signal,siginfo_t *info,void *opaque) {
    ucontext_t *context=opaque;uint16_t status,exponent;uint64_t significand;
    require(signal==SIGSEGV&&info->si_addr==page,"x87 store fault address");
    memcpy(&status,&context->uc_mcontext->__fs.__fpu_fsw,2);
    require(((status>>11)&7)==7&&context->uc_mcontext->__fs.__fpu_ftw==0x80,"signal preserves nonempty x87 TOP and physical tag bits");
    memcpy(&significand,&context->uc_mcontext->__fs.__fpu_stmm0,8);
    memcpy(&exponent,(char *)&context->uc_mcontext->__fs.__fpu_stmm0+8,2);
    require(significand==UINT64_C(0xc000000000000000)&&exponent==0x3fff,"signal preserves logical ST0 in 80-bit format");
    significand=UINT64_C(0x8000000000000000);exponent=0x4000;
    memcpy(&context->uc_mcontext->__fs.__fpu_stmm0,&significand,8);
    memcpy((char *)&context->uc_mcontext->__fs.__fpu_stmm0+8,&exponent,2);
    require(mprotect(page,page_size,PROT_READ|PROT_WRITE)==0,"unprotect x87 store destination");
}
static void asynchronous_handle(int signal, siginfo_t *info, void *opaque) {
    ucontext_t *context = opaque;
    require(signal == SIGUSR1 && info->si_signo == SIGUSR1 && info->si_code == SI_USER, "async signal metadata");
    require(context->uc_mcontext != NULL && context->uc_mcontext->__ss.__rip != 0 && context->uc_mcontext->__ss.__rsp != 0, "async context registers");
    asynchronous++;
}
__attribute__((noinline)) static unsigned probe(void) {
    uint64_t result, redzone, vector;
    __asm__ volatile(
        "mov %%rsp,%4\n\t"
        "movabs $0x1122334455667788,%%rax\n\t"
        "mov %%rax,-8(%%rsp)\n\t"
        "movq %%rax,%%xmm15\n\t"
        ".globl _r2_fault_instruction\n_r2_fault_instruction:\n\t"
        "mov (%3),%%eax\n\t"
        ".globl _r2_resume_instruction\n_r2_resume_instruction:\n\t"
        "mov -8(%%rsp),%%rdx\n\t"
        "movq %%xmm15,%2"
        : "=&a"(result), "=&d"(redzone), "=r"(vector), "+r"(page), "=m"(expected_sp)
        : : "xmm15", "memory");
    require(redzone == UINT64_C(0x1122334455667788), "signal preserves x86 red zone");
    require(vector == UINT64_C(0xaabbccddeeff0011), "signal return applies XMM changes");
    return (unsigned)result;
}
int main(int argc, char **argv) {
    struct sigaction action = {0}; action.sa_sigaction = handle; action.sa_flags = SA_SIGINFO;
    int unhandled = argc > 1 && strcmp(argv[1], "unhandled") == 0;
    corrupt_context = argc > 1 && strcmp(argv[1], "bad-context") == 0;
    if (!unhandled) require(sigaction(SIGSEGV, &action, NULL) == 0, "install virtual fault handler");
    page_size = (size_t)getpagesize();
    page = mmap(NULL, page_size, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
    require(page != MAP_FAILED, "allocate protected guest page");
    require(probe() == 77, "retry after handler changes protection");
    require(mprotect(page, page_size, PROT_NONE) == 0, "protect guest page again");
    redirect = 1; require(probe() == 42, "resume at handler-modified RIP and RAX");
    require(seen == 2, "both read faults handled");
    require(mprotect(page, page_size, PROT_READ) == 0, "protect arithmetic destination");
    action.sa_sigaction = handle_write;
    require(sigaction(SIGSEGV, &action, NULL) == 0, "install write handler");
    __asm__ volatile("xor %%ecx,%%ecx; cmp $1,%%ecx; addl $1,(%0)" : : "r"(page) : "rcx", "cc", "memory");
    require(*(unsigned *)page == 78, "retry faulting arithmetic store");
    require(mprotect(page, page_size, PROT_READ) == 0, "protect POP destination");
    action.sa_sigaction = handle_pop;
    require(sigaction(SIGSEGV, &action, NULL) == 0, "install POP write handler");
    __asm__ volatile("lea -8(%%rsp),%%rax; mov %%rax,%0; pushq $42; popq (%1)"
        : "=m"(expected_sp) : "r"(page) : "rax", "memory");
    require(*(uint64_t *)page == 42 && munmap(page, page_size) == 0, "retry faulting POP store");
    page = mmap(NULL, page_size * 2, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
    require(page != MAP_FAILED && mprotect(page, page_size, PROT_READ | PROT_WRITE) == 0, "allocate REP boundary pages");
    action.sa_sigaction = handle_rep;
    require(sigaction(SIGSEGV, &action, NULL) == 0, "install REP store handler");
    void *destination = (char *)page + page_size - 16; size_t remaining = 4;
    __asm__ volatile("cld; rep stosq" : "+D"(destination), "+c"(remaining) : "a"(UINT64_C(42)) : "memory", "cc");
    require(remaining == 0 && destination == (char *)page + page_size + 16 &&
            *(uint64_t *)((char *)page + page_size) == 42 && *(uint64_t *)((char *)page + page_size + 8) == 42,
            "REP resumes exactly at faulting iteration");
    require(munmap(page, page_size * 2) == 0, "release REP test pages");
    for (copy_read_fault = 0; copy_read_fault < 2; copy_read_fault++) {
        page = mmap(NULL, page_size * 2, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        require(page != MAP_FAILED, "allocate MOVS boundary pages");
        uint64_t values[4] = {17,29,43,71}, output[4] = {0};
        void *boundary_start = (char *)page + page_size - 16;
        if (copy_read_fault) memcpy(boundary_start, values, sizeof(values));
        require(mprotect((char *)page + page_size, page_size, PROT_NONE) == 0, "protect MOVS boundary");
        action.sa_sigaction = handle_rep_copy;
        require(sigaction(SIGSEGV, &action, NULL) == 0, "install REP MOVS handler");
        void *source = copy_read_fault ? boundary_start : (void *)values;
        destination = copy_read_fault ? (void *)output : boundary_start;
        copy_source = (uintptr_t)(copy_read_fault ? output : values); remaining = 4;
        __asm__ volatile("cld; rep movsq" : "+S"(source), "+D"(destination), "+c"(remaining) : : "memory", "cc");
        require(remaining == 0 && memcmp(copy_read_fault ? (void *)output : boundary_start, values, sizeof(values)) == 0, "REP MOVS retries the failed element exactly once");
        require(munmap(page, page_size * 2) == 0, "release REP MOVS pages");
    }
    page=mmap(NULL,page_size,PROT_NONE,MAP_ANON|MAP_PRIVATE,-1,0);
    require(page!=MAP_FAILED,"allocate protected x87 store destination");
    action.sa_sigaction=handle_x87;
    require(sigaction(SIGSEGV,&action,NULL)==0,"install x87 store fault handler");
    double original=1.5;
    __asm__ volatile("fninit; fldl %0; fstpl (%1)" : : "m"(original),"r"(page) : "memory");
    require(*(double *)page==2.0,"x87 fault retry applies handler-modified ST0 without popping twice");
    require(munmap(page,page_size)==0,"release x87 store page");
    action.sa_sigaction = asynchronous_handle;
    require(sigaction(SIGUSR1, &action, NULL) == 0 && raise(SIGUSR1) == 0 && asynchronous == 1, "async SIGINFO delivery");
    puts("PASS guest Darwin x86 signals: fault retry, ucontext registers, red zone and async delivery");
    return 0;
}
