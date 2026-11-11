#include "internal.h"
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static unsigned checks;
static void check(bool ok, const char *message)
{
    if (!ok) { fprintf(stderr, "FAIL dynamic code: %s\n", message); exit(1); }
    checks++;
}
static uint64_t call(r2_native_runtime *runtime, const char *symbol, uint64_t a, uint64_t b,
                      uint64_t c, uint64_t d, uint64_t e, uint64_t f)
{
    char error[R2_ERROR_CAP] = {0}; uint64_t address, regs[16] = {0}, stack[8] = {0};
    uint8_t xmm[16][16] = {{0}};
    check(r2_native_symbol(runtime, symbol, &address, error, sizeof(error)) == R2_OK, error);
    uint32_t number; memcpy(&number, (void *)(uintptr_t)(address + 1), 4);
    regs[R2_X86_RAX] = number; regs[R2_X86_RDI] = a; regs[R2_X86_RSI] = b;
    regs[R2_X86_RDX] = c; regs[R2_X86_RCX] = d; regs[R2_X86_R8] = e; regs[R2_X86_R9] = f;
    regs[R2_X86_RSP] = (uintptr_t)stack;
    bool exited = false;
    check(r2_native_call(runtime, regs, xmm, &exited, error, sizeof(error)) == R2_OK, error);
    return regs[R2_X86_RAX];
}
static r2_status execute(r2_block *block, r2_native_runtime *runtime, uint64_t address, uint64_t *value)
{
    char error[R2_ERROR_CAP] = {0}; r2_result result = {0};
    r2_interp_env env = {.native=runtime, .has_initial_rip=true, .initial_rip=address};
    r2_status status = r2_interp_block_with_env(block, &env, &result, error, sizeof(error));
    *value = result.rax;
    return status;
}
static void check_guarded_instructions(r2_block *block, uint8_t *end)
{
    char error[R2_ERROR_CAP];size_t pc=SIZE_MAX;
    for(unsigned hinted=0;hinted<2;hinted++)for(size_t length=1;length<=15;length++) {
        uint8_t *instruction=end-length;
        memset(instruction,0x66,length);instruction[length-1]=0x90;
        check((hinted?r2_native_decode_next(block,(uintptr_t)instruction,pc,&pc,error,sizeof(error)):r2_native_decode(block,(uintptr_t)instruction,&pc,error,sizeof(error)))==R2_OK&&block->insns[pc].x86_len==length,"decode instruction against inaccessible page");
        check((hinted?r2_native_decode_next(block,(uintptr_t)instruction,pc,&pc,error,sizeof(error)):r2_native_decode(block,(uintptr_t)instruction,&pc,error,sizeof(error)))==R2_OK&&block->insns[pc].op==R2_OP_NOP,"reuse guarded instruction without overread");
        for(size_t byte=0;byte<length;byte++) {
            uint8_t original=instruction[byte];instruction[byte]=byte+1==length?0xc3:0x2e;
            check((hinted?r2_native_decode_next(block,(uintptr_t)instruction,pc,&pc,error,sizeof(error)):r2_native_decode(block,(uintptr_t)instruction,&pc,error,sizeof(error)))==R2_OK&&
                  block->insns[pc].encoding[byte]==instruction[byte]&&
                  block->insns[pc].op==(byte+1==length?R2_OP_RET:R2_OP_NOP),"every changed instruction byte invalidates cache");
            instruction[byte]=original;
            check((hinted?r2_native_decode_next(block,(uintptr_t)instruction,pc,&pc,error,sizeof(error)):r2_native_decode(block,(uintptr_t)instruction,&pc,error,sizeof(error)))==R2_OK&&block->insns[pc].op==R2_OP_NOP,"restored guarded instruction is decoded again");
        }
    }
}
int main(void)
{
    char error[R2_ERROR_CAP] = {0}; uint64_t value;
    size_t page = (size_t)getpagesize();
    r2_native_runtime *runtime = r2_native_create(error, sizeof(error));
    check(runtime != NULL, error);
    uint64_t reserve = call(runtime, "_mmap", 0, page * 3, PROT_NONE, MAP_PRIVATE | MAP_ANON, UINT64_MAX, 0);
    check(reserve != UINT64_MAX, "reserve address range");
    uint64_t middle = call(runtime, "_mmap", reserve + page, page, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANON | MAP_FIXED, UINT64_MAX, 0);
    check(middle == reserve + page, "commit within owned reservation");
    check(r2_native_pointer(runtime, reserve, 1, false, error, sizeof(error)) == NULL, "left reservation remains inaccessible");
    unsigned char *bytes = r2_native_pointer(runtime, middle, page, true, error, sizeof(error));
    check(bytes != NULL, "committed page writable"); bytes[0] = 42;
    check(r2_native_pointer(runtime, middle + page, 1, false, error, sizeof(error)) == NULL, "right reservation remains inaccessible");
    check(call(runtime, "_munmap", reserve, page * 3, 0, 0, 0, 0) == 0, "release mixed reservation and committed spans");

    uint64_t code = call(runtime, "_mmap", 0, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, UINT64_MAX, 0);
    check(code != UINT64_MAX, "allocate code buffer");
    bytes = (void *)(uintptr_t)code;
    const uint8_t short_code[] = {0xb8,42,0,0,0,0xc3};
    memcpy(bytes, short_code, sizeof(short_code));
    r2_block *block = calloc(1, sizeof(*block)); check(block != NULL, "allocate instruction cache");
    block->insns = block->storage; block->capacity = R2_MAX_INSNS; block->runtime = runtime;
    check(execute(block, runtime, code, &value) == R2_ERR_DECODE, "RW data cannot execute as guest code");
    check(call(runtime, "_mprotect", code, page * 2, PROT_READ | PROT_EXEC, 0, 0, 0) == 0, "publish guest RX code");
    check(execute(block, runtime, code, &value) == R2_OK && value == 42, "execute generated x86 code");
    check(call(runtime, "_mprotect", code, page * 2, PROT_READ | PROT_WRITE | PROT_EXEC, 0, 0, 0) == 0, "publish guest RWX code");
    mach_vm_address_t region = code; mach_vm_size_t region_size = 0;
    vm_region_basic_info_data_64_t info; mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    check(mach_vm_region(mach_task_self(), &region, &region_size, VM_REGION_BASIC_INFO_64,
        (vm_region_info_t)&info, &count, &object) == KERN_SUCCESS && region <= code && !(info.protection & VM_PROT_EXECUTE),
        "guest x86 code pages never execute as ARM");
    if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
    bytes[1] = 7;
    check(execute(block, runtime, code, &value) == R2_OK && value == 7, "patch immediate invalidates decoded instruction");
    const uint8_t longer[] = {0x48,0xc7,0xc0,42,0,0,0,0xc3};
    memcpy(bytes, longer, sizeof(longer));
    check(execute(block, runtime, code, &value) == R2_OK && value == 42, "patch instruction length updates fallthrough");
    memcpy(bytes + page - 3, short_code, sizeof(short_code));
    check(execute(block, runtime, code + page - 3, &value) == R2_OK && value == 42, "instruction crosses executable pages");
    check(call(runtime, "_mprotect", code + page, page, PROT_NONE, 0, 0, 0) == 0, "protect second page");
    check(execute(block, runtime, code + page - 3, &value) == R2_ERR_DECODE, "cached instruction cannot cross inaccessible page");
    check(call(runtime, "_mprotect", code, page, PROT_NONE, 0, 0, 0) == 0, "withdraw guest code permissions");
    check(execute(block, runtime, code, &value) == R2_ERR_DECODE, "cached code cannot execute after protection change");
    check(call(runtime, "_munmap", code, page * 2, 0, 0, 0, 0) == 0, "unmap cached code");
    check(execute(block, runtime, code, &value) == R2_ERR_DECODE, "unmapped cached code cannot execute");
    size_t procedure_length=getenv("R2_TEST_LARGE_CACHE")?2100000:160000;
    size_t long_size = ((procedure_length + page - 1) / page) * page;
    uint64_t long_code = call(runtime, "_mmap", 0, long_size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, UINT64_MAX, 0);
    check(long_code != UINT64_MAX, "allocate large generated procedure");
    memset((void *)(uintptr_t)long_code, 0x90, procedure_length);
    memcpy((void *)(uintptr_t)(long_code + procedure_length-sizeof(short_code)), short_code, sizeof(short_code));
    check(execute(block, runtime, long_code, &value) == R2_OK && value == 42, "instruction cache grows beyond old fixed limit");
    check(execute(block, runtime, long_code, &value) == R2_OK && value == 42, "grown instruction cache is reused");
    check(call(runtime, "_munmap", long_code, long_size, 0, 0, 0, 0) == 0, "release large generated procedure");

    uint64_t source_pages = call(runtime, "_mmap", 0, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, UINT64_MAX, 0);
    check(source_pages != UINT64_MAX, "allocate guarded SIMD input");
    check(call(runtime, "_mprotect", source_pages + page, page, PROT_NONE, 0, 0, 0) == 0, "guard after SIMD input");
    uint64_t tiny_code = call(runtime, "_mmap", 0, page, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, UINT64_MAX, 0);
    check(tiny_code != UINT64_MAX, "allocate SIMD procedure");
    uint8_t insert[] = {0x48,0xb8,0,0,0,0,0,0,0,0,0x66,0x0f,0x3a,0x21,0x00,0x80,0x66,0x0f,0x7e,0xc0,0xc3};
    uint64_t input_address = source_pages + page - 4; uint32_t scalar = 0x12345678;
    memcpy(insert + 2, &input_address, 8); memcpy((void *)(uintptr_t)input_address, &scalar, 4);
    memcpy((void *)(uintptr_t)tiny_code, insert, sizeof(insert));
    check(execute(block, runtime, tiny_code, &value) == R2_OK && value == scalar, "INSERTPS memory reads exactly four bytes and ignores source index");
    uint8_t widen[] = {0x48,0xb8,0,0,0,0,0,0,0,0,0x66,0x0f,0x38,0x32,0x00,0x66,0x48,0x0f,0x7e,0xc0,0xc3};
    input_address = source_pages + page - 2; memcpy(widen + 2, &input_address, 8);
    memcpy((void *)(uintptr_t)tiny_code, widen, sizeof(widen));
    check(execute(block, runtime, tiny_code, &value) == R2_OK && value == 0x34, "PMOVZXBQ memory reads exactly two bytes");
    check(call(runtime, "_mprotect", source_pages + page, page, PROT_READ, 0, 0, 0) == 0, "make adjacent input page readable");
    check(r2_native_pointer(runtime, source_pages + page - 2, 8, false, error, sizeof(error)) != NULL, "unaligned read may cross adjacent native protection regions");
    check(r2_native_pointer(runtime, source_pages + page - 2, 8, true, error, sizeof(error)) == NULL, "cross-region read cache cannot authorize read-only writes");
    check(call(runtime, "_mprotect", source_pages + page, page, PROT_NONE, 0, 0, 0) == 0, "revoke adjacent page again");
    check(r2_native_pointer(runtime, source_pages + page - 2, 8, false, error, sizeof(error)) == NULL, "cross-region read is rejected after permission change");
    check(call(runtime, "_munmap", source_pages, page * 2, 0, 0, 0, 0) == 0, "release guarded SIMD input");
    check(call(runtime, "_munmap", tiny_code, page, 0, 0, 0, 0) == 0, "release SIMD procedure");

    unsigned char image[128] = {0};
    r2_memory_region regions[] = {{.base=(uintptr_t)image + 32,.size=32,.data=image + 32,.writable=false}};
    size_t region_count = 1;
    r2_native_set_regions(runtime, regions, &region_count);
    check(r2_native_pointer(runtime, (uintptr_t)image, 1, true, error, sizeof(error)) != NULL, "native heap before image");
    check(r2_native_pointer(runtime, (uintptr_t)image + 32, 1, true, error, sizeof(error)) == NULL, "heap permission cache cannot cover read-only image");
    check(r2_native_pointer(runtime, (uintptr_t)image + 32, 16, false, error, sizeof(error)) != NULL, "read pinned image data");
    check(r2_native_pointer(runtime, (uintptr_t)image + 64, 16, true, error, sizeof(error)) != NULL, "native heap after image");
    check(r2_native_pointer(runtime, (uintptr_t)image + 31, 2, true, error, sizeof(error)) == NULL, "write cannot cross into read-only image");
    r2_native_set_regions(runtime, NULL, NULL);
    uint64_t guarded=call(runtime,"_mmap",0,page*2,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANON,UINT64_MAX,0);
    check(guarded!=UINT64_MAX,"allocate guarded generated instruction cache");
    check(call(runtime,"_mprotect",guarded+page,page,PROT_NONE,0,0,0)==0,"guard generated instruction comparisons");
    check_guarded_instructions(block,(uint8_t *)(uintptr_t)(guarded+page));
    check(call(runtime,"_munmap",guarded,page*2,0,0,0,0)==0,"release guarded generated instructions");
    uint8_t *pinned=mmap(NULL,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    check(pinned!=MAP_FAILED&&mprotect(pinned+page,page,PROT_NONE)==0,"allocate guarded pinned image instructions");
    r2_native_add_code(runtime,(uintptr_t)pinned,page);
    check_guarded_instructions(block,pinned+page);
    if (block->insns != block->storage) free(block->insns);
    free(block); r2_native_destroy(runtime);
    munmap(pinned,page*2);
    printf("PASS guest VM reservations, commit, protection and self-modifying x86 code (%u checks)\n", checks);
    return 0;
}
