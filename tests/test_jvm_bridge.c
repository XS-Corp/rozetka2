#include "internal.h"
#include <dlfcn.h>
#include <errno.h>
#include <fenv.h>
#include <math.h>
#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static unsigned checks;
static void check(bool ok, const char *message)
{
    if (!ok) { fprintf(stderr, "FAIL JVM bridge: %s\n", message); exit(1); }
    checks++;
}

static r2_status invoke6(r2_native_runtime *runtime, const char *symbol, uint64_t a, uint64_t b,
                         uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t *result, char *error)
{
    uint64_t address = 0, regs[16] = {0}, stack[8] = {0};
    uint8_t xmm[16][16] = {{0}};
    r2_status status = r2_native_symbol(runtime, symbol, &address, error, R2_ERROR_CAP);
    if (status != R2_OK) return status;
    uint32_t number;
    memcpy(&number, (const void *)(uintptr_t)(address + 1), 4);
    regs[R2_X86_RAX] = number; regs[R2_X86_RDI] = a; regs[R2_X86_RSI] = b;
    regs[R2_X86_RDX] = c; regs[R2_X86_RCX] = d; regs[R2_X86_RSP] = (uintptr_t)stack;
    regs[R2_X86_R8] = e; regs[R2_X86_R9] = f;
    bool exited = false;
    status = r2_native_call(runtime, regs, xmm, &exited, error, R2_ERROR_CAP);
    *result = regs[R2_X86_RAX];
    return status;
}

static r2_status invoke(r2_native_runtime *runtime, const char *symbol, uint64_t a, uint64_t b,
                         uint64_t c, uint64_t d, uint64_t *result, char *error)
{
    return invoke6(runtime, symbol, a, b, c, d, 0, 0, result, error);
}

static uint64_t call(r2_native_runtime *runtime, const char *symbol, uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    char error[R2_ERROR_CAP] = {0}; uint64_t result;
    check(invoke(runtime, symbol, a, b, c, d, &result, error) == R2_OK, error);
    return result;
}

static void compiler_helpers(r2_native_runtime *runtime)
{
    uint64_t random=UINT64_C(0xd35f781ac9201847);
    for(unsigned signed_division=0;signed_division<2;signed_division++)for(unsigned i=0;i<80;i++) {
        random=random*UINT64_C(6364136223846793005)+1;unsigned __int128 a=(unsigned __int128)random<<64;
        random=random*UINT64_C(6364136223846793005)+1;a|=random;
        random=random*UINT64_C(6364136223846793005)+1;unsigned __int128 b=i%3?(unsigned __int128)random<<64:0;
        random=random*UINT64_C(6364136223846793005)+1;b|=random|1;
        if(i==0){a=(unsigned __int128)1<<127;b=~(unsigned __int128)0;}
        unsigned __int128 expected=signed_division?(i==0?a:(unsigned __int128)((__int128)a/(__int128)b)):a/b;
        uint64_t address=0,regs[16]={0};uint8_t xmm[16][16]={{0}};bool exited=false;char error[R2_ERROR_CAP]={0};
        check(r2_native_symbol(runtime,signed_division?"___divti3":"___udivti3",&address,error,sizeof(error))==R2_OK,error);
        uint32_t number;memcpy(&number,(void *)(uintptr_t)(address+1),4);regs[R2_X86_RAX]=number;
        regs[R2_X86_RDI]=(uint64_t)a;regs[R2_X86_RSI]=(uint64_t)(a>>64);regs[R2_X86_RDX]=(uint64_t)b;regs[R2_X86_RCX]=(uint64_t)(b>>64);
        check(r2_native_call(runtime,regs,xmm,&exited,error,sizeof(error))==R2_OK,error);
        check(regs[R2_X86_RAX]==(uint64_t)expected&&regs[R2_X86_RDX]==(uint64_t)(expected>>64),"128-bit argument pairs and RDX:RAX result");
    }
    const double cases[][6]={{2,3,5,-7,31,1},{-2,3,5,7,-31,1},{0.25,0.5,4,8,-3,4},{1e100,1e-100,1e-100,0,1,1e-200}};
    for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        uint64_t address=0,regs[16]={0};uint8_t xmm[16][16]={{0}};bool exited=false;char error[R2_ERROR_CAP]={0};
        check(r2_native_symbol(runtime,"___muldc3",&address,error,sizeof(error))==R2_OK,error);
        uint32_t number;memcpy(&number,(void *)(uintptr_t)(address+1),4);regs[R2_X86_RAX]=number;
        for(unsigned j=0;j<4;j++)memcpy(xmm[j],&cases[i][j],8);
        check(r2_native_call(runtime,regs,xmm,&exited,error,sizeof(error))==R2_OK,error);
        double real,imaginary;memcpy(&real,xmm[0],8);memcpy(&imaginary,xmm[1],8);
        check(real==cases[i][4]&&fabs(imaginary-cases[i][5])<=fabs(cases[i][5])*1e-15,"complex double input and XMM0/XMM1 return ABI");
    }
}

int main(void)
{
    unsetenv("R2_JVM_PROBE");
    char error[R2_ERROR_CAP] = {0}; uint64_t address, result;
    fenv_t saved;
    fegetenv(&saved); fesetround(FE_DOWNWARD); feraiseexcept(FE_INEXACT);
    r2_native_runtime *runtime = r2_native_create(error, sizeof(error));
    check(runtime != NULL, error);
    compiler_helpers(runtime);
    check(fegetround() == FE_TONEAREST, "guest default rounding is isolated");
    sigset_t original_mask, mask, current_mask;
    pthread_sigmask(SIG_SETMASK, NULL, &original_mask);
    sigemptyset(&mask); sigaddset(&mask, SIGUSR2);
    check(call(runtime, "_pthread_sigmask", SIG_BLOCK, (uintptr_t)&mask, 0, 0) == 0, "guest pthread signal mask");
    pthread_sigmask(SIG_SETMASK, NULL, &current_mask);
    check(memcmp(&current_mask,&original_mask,sizeof(current_mask))==0,"virtual masking leaves host signal machinery unchanged");
    sigset_t virtual_mask=0;check(call(runtime,"_pthread_sigmask",SIG_SETMASK,0,(uintptr_t)&virtual_mask,0)==0&&sigismember(&virtual_mask,SIGUSR2)==1,"guest virtual signal is blocked");
    check(!r2_native_library("/usr/lib/libc++.1.dylib"), "diagnostic C++ profile is opt-in");
    check(r2_native_symbol(runtime, "_fork", &address, error, sizeof(error)) == R2_ERR_UNSUPPORTED,
          "unimplemented imports rejected by default");
    check(!r2_native_pop_exit(runtime, &address, &result), "empty exit list");
    pid_t child = (pid_t)12345;
    check(call(runtime, "_posix_spawn", (uintptr_t)&child, (uintptr_t)"/usr/bin/true", 0, 0) == ENOSYS && child == (pid_t)12345,
          "unsupported guest spawn returns an error without launching native code or changing PID");
    check(call(runtime, "_atexit", 0x1234, 0, 0, 0) == 0, "register guest atexit");
    check(call(runtime, "___cxa_atexit", 0x5678, 42, 0xabcd, 0) == 0, "register guest C++ destructor");
    check(r2_native_pop_exit(runtime, &address, &result) && address == 0x5678 && result == 42, "C++ argument and reverse exit order");
    check(r2_native_pop_exit(runtime, &address, &result) && address == 0x1234, "C exit callback remains guest address");
    check(!r2_native_pop_exit(runtime, &address, &result), "exit entries consumed once");

    pthread_key_t key;
    check(call(runtime, "_pthread_key_create", (uintptr_t)&key, 0xdeadbeef, 0, 0) == 0, "key registration keeps destructor in guest");
    check(call(runtime, "_pthread_setspecific", key, 0x12345678, 0, 0) == 0, "set guest TLS value");
    check(call(runtime, "_pthread_getspecific", key, 0, 0, 0) == 0x12345678, "get guest TLS value");
    check(call(runtime, "_pthread_key_delete", key, 0, 0, 0) == 0, "delete key without invoking destructor");
    check(call(runtime, "_pthread_setspecific", key, 1, 0, 0) == EINVAL, "deleted key cannot affect host TLS");
    r2_native_set_stack(runtime, 0x100000, 0x10000);
    check(call(runtime, "_pthread_get_stackaddr_np", (uintptr_t)pthread_self(), 0, 0, 0) == 0x110000, "reports guest stack top");
    check(call(runtime, "_pthread_get_stacksize_np", (uintptr_t)pthread_self(), 0, 0, 0) == 0x10000, "reports guest stack size");
    check(invoke(runtime, "_pthread_get_stackaddr_np", 0, 0, 0, 0, &result, error) == R2_ERR_UNSUPPORTED, "unknown pthread cannot expose ARM stack");

    semaphore_t semaphore;
    check(call(runtime, "_semaphore_create", mach_task_self(), (uintptr_t)&semaphore, SYNC_POLICY_FIFO, 1) == KERN_SUCCESS, "create Mach semaphore");
    check(call(runtime, "_semaphore_wait", semaphore, 0, 0, 0) == KERN_SUCCESS, "consume semaphore token");
    check(call(runtime, "_semaphore_signal", semaphore, 0, 0, 0) == KERN_SUCCESS, "signal semaphore");
    check(call(runtime, "_semaphore_destroy", mach_task_self(), semaphore, 0, 0) == KERN_SUCCESS, "destroy semaphore");
    check(call(runtime, "_semaphore_destroy", mach_task_self(), semaphore, 0, 0) == KERN_INVALID_ARGUMENT, "destroyed semaphore is not reused");

    uint8_t environment[16] = {0}; uint16_t control; uint32_t mxcsr;
    check(call(runtime, "_fegetenv", (uintptr_t)environment, 0, 0, 0) == 0, "save guest FP environment");
    memcpy(&control, environment, 2); memcpy(&mxcsr, environment + 4, 4);
    check(control == 0x037f && (mxcsr & ~63u) == 0x1f80, "x86 fenv layout rather than ARM layout");
    control = 0x027f; mxcsr = 0x1fa0;
    memcpy(environment, &control, 2); memcpy(environment + 4, &mxcsr, 4);
    check(call(runtime, "_fesetenv", (uintptr_t)environment, 0, 0, 0) == 0, "restore masked FP environment");
    memset(environment, 0, sizeof(environment));
    call(runtime, "_fegetenv", (uintptr_t)environment, 0, 0, 0);
    memcpy(&control, environment, 2); memcpy(&mxcsr, environment + 4, 4);
    check(control == 0x027f && (mxcsr & 0x20), "guest precision control and sticky inexact flag");
    mxcsr = 0x3f80; memcpy(environment + 4, &mxcsr, 4);
    check(invoke(runtime, "_fesetenv", (uintptr_t)environment, 0, 0, 0, &result, error) == R2_ERR_UNSUPPORTED,
          "unsupported rounding is rejected");

    int demangle_status=0;size_t demangle_length=0;
    char *demangled=(char *)(uintptr_t)call(runtime,"___cxa_demangle",(uintptr_t)"_Z3fooi",0,(uintptr_t)&demangle_length,(uintptr_t)&demangle_status);
    check(demangled&&strcmp(demangled,"foo(int)")==0&&demangle_status==0&&demangle_length>8,"native demangler parses x86 Itanium names");free(demangled);
    check(call(runtime,"___cxa_demangle",(uintptr_t)"invalid",0,0,(uintptr_t)&demangle_status)==0&&demangle_status==-2,"invalid mangled name status");
    uint64_t stub;
    check(r2_native_symbol(runtime, "_getpid", &stub, error, sizeof(error)) == R2_OK, error);
    check(call(runtime, "_dlsym", (uintptr_t)RTLD_DEFAULT, (uintptr_t)"getpid", 0, 0) == stub, "dlsym returns guest stub");
    check(call(runtime, "_dlsym", (uintptr_t)RTLD_DEFAULT, (uintptr_t)"rozetka2_missing_test_symbol", 0, 0) == 0, "missing optional symbol");
    uint64_t message = call(runtime, "_dlerror", 0, 0, 0, 0);
    check(message && strstr((const char *)(uintptr_t)message, "symbol not found"), "guest dlerror message");
    check(call(runtime, "_dlerror", 0, 0, 0, 0) == 0, "dlerror is consumed once");

    size_t page = (size_t)getpagesize();
    check(invoke6(runtime, "_mmap", 0, page * 2, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE,
                  UINT64_MAX, 0, &result, error) == R2_OK && result != UINT64_MAX, "anonymous guest mapping");
    uint64_t mapping = result;
    unsigned char *memory = r2_native_pointer(runtime, mapping, page * 2, true, error, sizeof(error));
    check(memory != NULL, "writable mapped memory"); memory[0] = 42; memory[page] = 17;
    check(call(runtime, "_munmap", mapping + page, page, 0, 0) == 0 && memory[0] == 42,
          "partial unmap preserves adjacent mapping");
    check(call(runtime, "_munmap", mapping, page, 0, 0) == 0, "unmap remaining guest mapping");
    check(r2_native_pointer(runtime, mapping, 1, false, error, sizeof(error)) == NULL, "unmap invalidates cached memory permissions");
    check(invoke6(runtime, "_mmap", 0, page, PROT_READ | PROT_EXEC, MAP_ANON | MAP_PRIVATE, UINT64_MAX, 0,
                  &result, error) == R2_OK && result != UINT64_MAX, "guest executable mapping");
    check(call(runtime, "_munmap", result, page, 0, 0) == 0, "release guest executable mapping");
    check(invoke6(runtime, "_mmap", (uintptr_t)&mask, page, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | MAP_FIXED,
                  UINT64_MAX, 0, &result, error) == R2_ERR_UNSUPPORTED, "fixed mmap cannot replace host memory");
    check(invoke(runtime, "_munmap", (uintptr_t)&mask & ~(page - 1), page, 0, 0, &result, error) == R2_ERR_UNSUPPORTED, "cannot unmap host memory");
    check(call(runtime, "_munmap", 0, 0, 0, 0) == UINT32_MAX && errno == EINVAL, "munmap error has int ABI result");
    FILE *file = tmpfile(); check(file != NULL, "mapped file fixture");
    const char content[] = "class-file-data";
    check(fwrite(content, 1, sizeof(content), file) == sizeof(content) && fflush(file) == 0, "write mapped fixture");
    char slice[5] = {0};
    check(call(runtime, "_pread", fileno(file), (uintptr_t)slice, 4, 6) == 4 && memcmp(slice, "file", 4) == 0, "pread offset and length ABI");
    check(invoke6(runtime, "_mmap", 0, sizeof(content), PROT_READ, MAP_PRIVATE, (uint64_t)fileno(file), 0,
                  &result, error) == R2_OK && result != UINT64_MAX, "file backed guest mapping");
    mapping = result;
    check(memcmp((void *)(uintptr_t)mapping, content, sizeof(content)) == 0, "mapped file contents");
    check(r2_native_pointer(runtime, mapping, 1, true, error, sizeof(error)) == NULL, "readonly file map cannot be written");
    check(call(runtime, "_munmap", mapping, sizeof(content), 0, 0) == 0, "unmap original unrounded file length");
    fclose(file);
    r2_native_destroy(runtime);
    pthread_sigmask(SIG_SETMASK, NULL, &current_mask);
    check(memcmp(&original_mask, &current_mask, sizeof(current_mask)) == 0, "host signal mask restored after guest");
    check(fegetround() == FE_DOWNWARD && fetestexcept(FE_INEXACT), "host FP environment restored after guest");
    fesetenv(&saved);

    setenv("R2_JVM_PROBE", "1", 1);
    runtime = r2_native_create(error, sizeof(error));
    check(runtime != NULL, error);
    check(r2_native_library("/usr/lib/libc++.1.dylib"), "explicit diagnostic C++ profile");
    check(invoke(runtime, "_fork", 0, 0, 0, 0, &result, error) == R2_ERR_UNSUPPORTED &&
          strstr(error, "JVM probe reached unsupported") != NULL, "diagnostic import traps instead of calling host thread");
    check(r2_native_symbol(runtime, "_rozetka2_unknown_import", &address, error, sizeof(error)) == R2_ERR_UNSUPPORTED,
          "unknown import remains rejected in diagnostic profile");
    r2_native_destroy(runtime);
    unsetenv("R2_JVM_PROBE");
    printf("PASS JVM bridge ABI, resources, FP isolation and diagnostic traps (%u checks)\n", checks);
    return 0;
}
