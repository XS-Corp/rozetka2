/* Native Cocoa objects use native method encodings. Methods defined in guest
 * Mach-O metadata enter x86 IMPs on the original guest stack. They are never
 * registered as executable ARM implementations. */
static bool objc_guest_data(r2_native_runtime *runtime,uint64_t address,size_t size)
{
    size_t count=runtime->image_region_count?*runtime->image_region_count:0;
    for(size_t i=0;i<count;i++) {const r2_memory_region *r=&runtime->image_regions[i];if(address>=r->base&&address-r->base<r->size&&size<=r->size-(address-r->base))return true;}
    return false;
}
static r2_status objc_guest_method(r2_native_runtime *runtime,uint64_t receiver,const char *selector,bool *guest,uint64_t *implementation,char *error,size_t error_len)
{
    *guest=false;*implementation=0;if(!objc_guest_data(runtime,receiver,8))return R2_OK;
    uint64_t isa;memcpy(&isa,(const void *)(uintptr_t)receiver,8);if(!objc_guest_data(runtime,isa,40))return R2_OK;
    uint64_t ro;memcpy(&ro,(const void *)(uintptr_t)(isa+32),8);ro&=~UINT64_C(7);
    if(!objc_guest_data(runtime,ro,40)) {r2_set_error(error,error_len,"invalid guest Objective-C class metadata");return R2_ERR_UNSUPPORTED;}
    uint32_t flags;memcpy(&flags,(const void *)(uintptr_t)ro,4);
    if(!(flags&1)) {r2_set_error(error,error_len,"guest Objective-C instance dispatch is unsupported");return R2_ERR_UNSUPPORTED;}
    *guest=true;uint64_t methods;memcpy(&methods,(const void *)(uintptr_t)(ro+32),8);if(!methods)return R2_OK;
    const uint32_t *header=r2_native_pointer(runtime,methods,8,false,error,error_len);if(!header)return R2_ERR_UNSUPPORTED;
    uint32_t encoded=header[0],count=header[1];size_t stride=encoded&0xfffc;bool relative=(encoded&0x80000000)!=0;
    if(count>4096||stride<(relative?12u:24u)||count>(SIZE_MAX-8)/stride) {r2_set_error(error,error_len,"invalid guest Objective-C method list");return R2_ERR_UNSUPPORTED;}
    const uint8_t *list=r2_native_pointer(runtime,methods,8+(size_t)count*stride,false,error,error_len);if(!list)return R2_ERR_UNSUPPORTED;
    for(uint32_t i=0;i<count;i++) {
        const uint8_t *entry=list+8+(size_t)i*stride;uint64_t name,target;
        if(relative) {
            int32_t offsets[3];memcpy(offsets,entry,12);name=(uintptr_t)entry+(int64_t)offsets[0];target=(uintptr_t)(entry+8)+(int64_t)offsets[2];
            if(!(encoded&0x40000000)) {const void *reference=r2_native_pointer(runtime,name,8,false,error,error_len);if(!reference)return R2_ERR_UNSUPPORTED;memcpy(&name,reference,8);}
        } else {memcpy(&name,entry,8);memcpy(&target,entry+16,8);}
        char method[256];if(!graphics_string(runtime,name,method,error,error_len))return R2_ERR_UNSUPPORTED;
        if(strcmp(method,selector)==0) {
            bool dynamic;if(!executable_span(runtime,target,&dynamic)) {r2_set_error(error,error_len,"guest Objective-C implementation is outside executable code");return R2_ERR_UNSUPPORTED;}
            *implementation=target;return R2_OK;
        }
    }
    return R2_OK;
}
#include "objc_instances.inc"
static _Atomic(native_callback *) objc_exception_current;
static void objc_exception_proxy(void *exception)
{
    native_callback *callback=atomic_load_explicit(&objc_exception_current,memory_order_acquire);if(!callback)return;
    uint64_t arguments[]={(uintptr_t)exception},result;char error[R2_ERROR_CAP]={0};(void)callback_invoke(callback,arguments,1,&result,error,sizeof(error));
}
static void objc_callbacks_quiesce(r2_native_runtime *runtime)
{
    if(!runtime->objc_exception_saved||!runtime->foundation)return;
    void (*set)(void *)=dlsym(runtime->foundation,"NSSetUncaughtExceptionHandler");
    atomic_store_explicit(&objc_exception_current,runtime->previous_objc_exception,memory_order_release);
    if(set)set(runtime->original_objc_exception);runtime->objc_exception_saved=false;
}
static r2_status objc_log(r2_native_runtime *runtime,native_import *import,uint64_t regs[16],uint8_t xmm[16][16],char *error,size_t error_len)
{
    if(!runtime->core_foundation)runtime->core_foundation=dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation",RTLD_NOW|RTLD_LOCAL);
    unsigned char (*get)(const void *,char *,long,uint32_t)=runtime->core_foundation?dlsym(runtime->core_foundation,"CFStringGetCString"):NULL;
    char format[1024],types[NATIVE_MAX_ARGS]={'p'};size_t count=1;
    if(!get||!get((void *)(uintptr_t)regs[R2_X86_RDI],format,sizeof(format),0x08000100)||!printf_types(format,types,&count)){r2_set_error(error,error_len,"unsupported NSLog format");return R2_ERR_UNSUPPORTED;}
    uint64_t values[NATIVE_MAX_ARGS]={regs[R2_X86_RDI]};ffi_type *kinds[NATIVE_MAX_ARGS];void *arguments[NATIVE_MAX_ARGS];size_t gp=1,fp=0,stack=0;
    for(size_t i=0;i<count;i++) {
        if(i) {
            if(types[i]=='d'&&fp<8)memcpy(&values[i],xmm[fp++],8);
            else if(types[i]!='d'&&gp<6)values[i]=guest_argument(regs,gp++);
            else {const void *p=r2_native_pointer(runtime,regs[R2_X86_RSP]+8+8*stack++,8,false,error,error_len);if(!p)return R2_ERR_UNSUPPORTED;memcpy(&values[i],p,8);}
        }
        kinds[i]=argument_type(types[i]);arguments[i]=&values[i];
    }
    ffi_cif cif;if(ffi_prep_cif_var(&cif,FFI_DEFAULT_ABI,1,(unsigned)count,&ffi_type_void,kinds)!=FFI_OK)return R2_ERR_UNSUPPORTED;
    unsigned depth=engine_suspend(runtime);ffi_call(&cif,FFI_FN(import->function),NULL,arguments);engine_resume(runtime,depth);
    return r2_native_stopping(runtime)?r2_native_execution_poll(runtime,error,error_len):R2_OK;
}
static r2_status objc_special(r2_native_runtime *runtime,native_import *import,uint64_t regs[16],bool *handled,char *error,size_t error_len)
{
    const char *name=import->definition->name;*handled=true;
    /* x86 Mach-O selector references have not passed through native dyld's
     * selector uniquing. C runtime queries require the same canonical SELs
     * as objc_msgSend, including both operands of sel_isEqual. */
    bool selector_query=strcmp(name,"class_getInstanceMethod")==0||strcmp(name,"class_getClassMethod")==0||
        strcmp(name,"class_respondsToSelector")==0||strcmp(name,"protocol_addMethodDescription")==0||strcmp(name,"sel_isEqual")==0;
    if(strcmp(name,"objc_alloc")==0||strcmp(name,"objc_alloc_init")==0||strcmp(name,"objc_opt_class")==0) {
        Class mapped;r2_status status=objc_bridge_class(runtime,regs[R2_X86_RDI],&mapped,error,error_len);
        if(status!=R2_OK)return status;regs[R2_X86_RDI]=(uintptr_t)mapped;*handled=false;return R2_OK;
    }
    if(strcmp(name,"method_getImplementation")==0||strcmp(name,"class_getMethodImplementation")==0)
        return objc_get_implementation(runtime,name,regs,error,error_len);
    if(strncmp(name,"class_",6)==0||strcmp(name,"object_isClass")==0||strcmp(name,"objc_retain")==0||strcmp(name,"objc_release")==0||strcmp(name,"objc_autorelease")==0||strcmp(name,"objc_retainAutorelease")==0||strcmp(name,"objc_opt_respondsToSelector")==0) {
        Class mapped;r2_status status=objc_bridge_class(runtime,regs[R2_X86_RDI],&mapped,error,error_len);
        if(status!=R2_OK)return status;regs[R2_X86_RDI]=(uintptr_t)mapped;
    }
    if(selector_query) {
        unsigned registers[]={R2_X86_RSI,R2_X86_RDI};size_t count=strcmp(name,"sel_isEqual")==0?2:1;
        if(!runtime->objc)runtime->objc=dlopen("/usr/lib/libobjc.A.dylib",RTLD_NOW|RTLD_LOCAL);
        void *(*register_selector)(const char *)=runtime->objc?dlsym(runtime->objc,"sel_registerName"):NULL;
        if(!register_selector)return R2_ERR_UNSUPPORTED;
        for(size_t i=0;i<count;i++)if(regs[registers[i]]) {
            char selector[256];if(!graphics_string(runtime,regs[registers[i]],selector,error,error_len))return R2_ERR_UNSUPPORTED;
            regs[registers[i]]=(uintptr_t)register_selector(selector);
        }
        *handled=false;return R2_OK;
    }
    if(strcmp(name,"objc_opt_respondsToSelector")==0) {
        bool guest=false;uint64_t implementation=0;
        r2_status status=objc_guest_method(runtime,regs[R2_X86_RDI],"respondsToSelector:",&guest,&implementation,error,error_len);
        if(status!=R2_OK)return status;
        if(guest){r2_set_error(error,error_len,"optimized Objective-C call on a guest class is unsupported");return R2_ERR_UNSUPPORTED;}
        if(regs[R2_X86_RSI]) {
            char selector[256];if(!graphics_string(runtime,regs[R2_X86_RSI],selector,error,error_len))return R2_ERR_UNSUPPORTED;
            if(!runtime->objc)runtime->objc=dlopen("/usr/lib/libobjc.A.dylib",RTLD_NOW|RTLD_LOCAL);
            void *(*register_selector)(const char *)=dlsym(runtime->objc,"sel_registerName");if(!register_selector)return R2_ERR_UNSUPPORTED;
            regs[R2_X86_RSI]=(uintptr_t)register_selector(selector);
        }
        *handled=false;return R2_OK;
    }
    if(strcmp(name,"NSSetUncaughtExceptionHandler")==0) {
        native_callback *callback=regs[R2_X86_RDI]?callback_create(runtime,regs[R2_X86_RDI],0,error,error_len):NULL;
        if(regs[R2_X86_RDI]&&!callback)return R2_ERR_UNSUPPORTED;
        if(!runtime->objc_exception_saved) {
            void *(*get)(void)=dlsym(runtime->foundation,"NSGetUncaughtExceptionHandler");if(!get)return R2_ERR_UNSUPPORTED;
            runtime->original_objc_exception=get();runtime->previous_objc_exception=atomic_load_explicit(&objc_exception_current,memory_order_acquire);runtime->objc_exception_saved=true;
        }
        if(runtime->objc_exception_callback)atomic_store_explicit(&runtime->objc_exception_callback->disabled,true,memory_order_release);
        runtime->objc_exception_callback=callback;atomic_store_explicit(&objc_exception_current,callback,memory_order_release);
        void (*set)(void *)=import->function;set(callback?(void *)objc_exception_proxy:NULL);return R2_OK;
    }
    if(strcmp(name,"NSGetUncaughtExceptionHandler")==0) {
        void *(*get)(void)=import->function;void *function=get();
        if(function==(void *)objc_exception_proxy) {native_callback *callback=atomic_load_explicit(&objc_exception_current,memory_order_acquire);regs[R2_X86_RAX]=callback?callback->function:0;return R2_OK;}
        if(!function){regs[R2_X86_RAX]=0;return R2_OK;}
        static const native_definition definition={"__r2_native_objc_exception","p",'v',-1};
        runtime->override_name=definition.name;runtime->override_definition=&definition;runtime->override_function=function;
        r2_status status=r2_native_symbol(runtime,"___r2_native_objc_exception",&regs[R2_X86_RAX],error,error_len);
        runtime->override_name=NULL;runtime->override_definition=NULL;runtime->override_function=NULL;return status;
    }
    *handled=false;return R2_OK;
}
