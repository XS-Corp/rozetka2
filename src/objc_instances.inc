/* Native ObjC classes contain ARM closure IMPs, each entering an x86 method.
 * These classes/closures remain valid for late OS releases after guest teardown;
 * the pinned callback is then disabled and cannot access guest memory. */
typedef struct native_objc_imp {
    ffi_cif cif;ffi_type *arguments[NATIVE_MAX_ARGS];char kinds[NATIVE_MAX_ARGS];
    ffi_closure *closure;void *code;native_callback *callback;char *encoding;
    void *(*dispose)(void *);bool dealloc;
    struct native_objc_imp *next;
} native_objc_imp;
static _Atomic(native_objc_imp *) pinned_objc_imps;
static void objc_imp_proxy(ffi_cif *cif,void *output,void **arguments,void *opaque)
{
    native_objc_imp *imp=opaque;uint64_t values[NATIVE_MAX_ARGS]={0},value=0;
    for(unsigned i=0;i<cif->nargs;i++) {
        memcpy(&values[i],arguments[i],cif->arg_types[i]->size);
        if(imp->kinds[i]=='c')values[i]=(uint32_t)(int32_t)(int8_t)values[i];
        else if(imp->kinds[i]=='s')values[i]=(uint32_t)(int32_t)(int16_t)values[i];
    }
    if(imp->dealloc&&atomic_load_explicit(&imp->callback->disabled,memory_order_acquire))imp->dispose((void *)(uintptr_t)values[0]);
    else {char error[R2_ERROR_CAP]={0};(void)callback_invoke(imp->callback,values,cif->nargs,&value,error,sizeof(error));}
    if(cif->rtype!=&ffi_type_void)memcpy(output,&value,sizeof(ffi_arg));
}
static r2_status objc_bridge_class(r2_native_runtime *runtime,uint64_t guest,Class *result,char *error,size_t error_len);
static r2_status objc_install_methods(r2_native_runtime *runtime,Class native,uint64_t ro,char *error,size_t error_len)
{
    uint64_t methods;memcpy(&methods,(void *)(uintptr_t)(ro+32),8);if(!methods)return R2_OK;
    const uint32_t *header=r2_native_pointer(runtime,methods,8,false,error,error_len);if(!header)return R2_ERR_UNSUPPORTED;
    uint32_t flags=header[0],count=header[1];size_t stride=flags&0xfffc;bool relative=(flags&0x80000000)!=0;
    if(count>4096||stride<(relative?12u:24u)||count>(SIZE_MAX-8)/stride){r2_set_error(error,error_len,"invalid guest Objective-C methods");return R2_ERR_UNSUPPORTED;}
    const uint8_t *list=r2_native_pointer(runtime,methods,8+count*stride,false,error,error_len);if(!list)return R2_ERR_UNSUPPORTED;
    Class (*get_class)(const char *)=dlsym(runtime->objc,"objc_getClass");
    SEL (*selector)(const char *)=dlsym(runtime->objc,"sel_registerName");
    void *send=dlsym(runtime->objc,"objc_msgSend");
    bool (*add)(Class,SEL,IMP,const char *)=dlsym(runtime->objc,"class_addMethod");
    void *(*dispose)(void *)=dlsym(runtime->objc,"object_dispose");
    if(!get_class||!selector||!send||!add||!dispose)return R2_ERR_UNSUPPORTED;
    for(uint32_t i=0;i<count;i++) {
        const uint8_t *entry=list+8+i*stride;uint64_t name,types,target;
        if(relative) {
            int32_t offsets[3];memcpy(offsets,entry,12);name=(uintptr_t)entry+(int64_t)offsets[0];types=(uintptr_t)(entry+4)+(int64_t)offsets[1];target=(uintptr_t)(entry+8)+(int64_t)offsets[2];
            if(!(flags&0x40000000)){const void *p=r2_native_pointer(runtime,name,8,false,error,error_len);if(!p)return R2_ERR_UNSUPPORTED;memcpy(&name,p,8);}
        }else {memcpy(&name,entry,8);memcpy(&types,entry+8,8);memcpy(&target,entry+16,8);}
        char method[256],encoding[256];if(!graphics_string(runtime,name,method,error,error_len)||!graphics_string(runtime,types,encoding,error,error_len))return R2_ERR_UNSUPPORTED;
        id signature=((id(*)(Class,SEL,const char *))send)(get_class("NSMethodSignature"),selector("signatureWithObjCTypes:"),encoding);
        if(!signature){r2_set_error(error,error_len,"invalid guest Objective-C encoding: %s",method);return R2_ERR_UNSUPPORTED;}
        unsigned long arguments=((unsigned long(*)(id,SEL))send)(signature,selector("numberOfArguments"));
        const char *returns=((const char *(*)(id,SEL))send)(signature,selector("methodReturnType"));char kind=objc_scalar_type(returns);
        if(!kind||kind=='d'||kind=='f'||arguments<2||arguments>NATIVE_MAX_ARGS){r2_set_error(error,error_len,"unsupported guest Objective-C callback ABI: %s",method);return R2_ERR_UNSUPPORTED;}
        native_objc_imp *imp=calloc(1,sizeof(*imp));if(!imp)return R2_ERR_NO_MEMORY;
        imp->encoding=strdup(encoding);imp->dealloc=strcmp(method,"dealloc")==0;imp->dispose=dispose;
        if(!imp->encoding){free(imp);return R2_ERR_NO_MEMORY;}
        for(unsigned long j=0;j<arguments;j++) {
            const char *argument=((const char *(*)(id,SEL,unsigned long))send)(signature,selector("getArgumentTypeAtIndex:"),j);
            imp->kinds[j]=objc_scalar_type(argument);
            if(!imp->kinds[j]||imp->kinds[j]=='v'||imp->kinds[j]=='f'||imp->kinds[j]=='d'){r2_set_error(error,error_len,"unsupported guest Objective-C callback argument: %s",method);free(imp->encoding);free(imp);return R2_ERR_UNSUPPORTED;}
            imp->arguments[j]=argument_type(imp->kinds[j]);
        }
        imp->callback=callback_create(runtime,target,0,error,error_len);if(!imp->callback){free(imp->encoding);free(imp);return R2_ERR_UNSUPPORTED;}
        imp->closure=ffi_closure_alloc(sizeof(ffi_closure),&imp->code);
        if(!imp->closure||ffi_prep_cif(&imp->cif,FFI_DEFAULT_ABI,(unsigned)arguments,argument_type(kind),imp->arguments)!=FFI_OK||ffi_prep_closure_loc(imp->closure,&imp->cif,objc_imp_proxy,imp,imp->code)!=FFI_OK) {
            atomic_store_explicit(&imp->callback->disabled,true,memory_order_release);if(imp->closure)ffi_closure_free(imp->closure);free(imp->encoding);free(imp);return R2_ERR_NO_MEMORY;
        }
        native_objc_imp *head=atomic_load_explicit(&pinned_objc_imps,memory_order_relaxed);
        do {imp->next=head;}while(!atomic_compare_exchange_weak_explicit(&pinned_objc_imps,&head,imp,memory_order_release,memory_order_relaxed));
        if(!add(native,selector(method),(IMP)imp->code,imp->encoding)){r2_set_error(error,error_len,"cannot install guest Objective-C method: %s",method);return R2_ERR_UNSUPPORTED;}
    }
    return R2_OK;
}
static r2_status objc_bridge_class_depth(r2_native_runtime *runtime,uint64_t guest,Class *result,unsigned depth,char *error,size_t error_len)
{
    if(depth>=64){r2_set_error(error,error_len,"cyclic or excessive guest Objective-C inheritance");return R2_ERR_UNSUPPORTED;}
    if(!runtime->objc)runtime->objc=dlopen("/usr/lib/libobjc.A.dylib",RTLD_NOW|RTLD_LOCAL);
    if(!runtime->objc){r2_set_error(error,error_len,"cannot load Objective-C runtime");return R2_ERR_UNSUPPORTED;}
    *result=(Class)(uintptr_t)guest;if(!guest)return R2_OK;
    for(size_t i=0;i<runtime->objc_class_count;i++)if(runtime->objc_classes[i].guest==guest){*result=runtime->objc_classes[i].native;return R2_OK;}
    bool is_guest=false;uint64_t implementation; r2_status status=objc_guest_method(runtime,guest,"alloc",&is_guest,&implementation,error,error_len);
    if(status!=R2_OK||!is_guest)return status;
    if(runtime->objc_class_count>=64){r2_set_error(error,error_len,"guest Objective-C class limit reached");return R2_ERR_UNSUPPORTED;}
    uint64_t meta,super,ro;memcpy(&meta,(void *)(uintptr_t)guest,8);memcpy(&super,(void *)(uintptr_t)(guest+8),8);memcpy(&ro,(void *)(uintptr_t)(guest+32),8);ro&=~UINT64_C(7);
    if(!objc_guest_data(runtime,ro,72)){r2_set_error(error,error_len,"invalid guest Objective-C class layout");return R2_ERR_UNSUPPORTED;}
    Class parent;status=objc_bridge_class_depth(runtime,super,&parent,depth+1,error,error_len);if(status!=R2_OK)return status;
    if(!runtime->foundation)runtime->foundation=dlopen("/System/Library/Frameworks/Foundation.framework/Foundation",RTLD_NOW|RTLD_LOCAL);
    Class (*allocate)(Class,const char *,size_t)=dlsym(runtime->objc,"objc_allocateClassPair");
    void (*dispose)(Class)=dlsym(runtime->objc,"objc_disposeClassPair");void (*register_class)(Class)=dlsym(runtime->objc,"objc_registerClassPair");
    bool (*add_ivar)(Class,const char *,size_t,uint8_t,const char *)=dlsym(runtime->objc,"class_addIvar");
    Ivar (*get_ivar)(Class,const char *)=dlsym(runtime->objc,"class_getInstanceVariable");ptrdiff_t (*offset)(Ivar)=dlsym(runtime->objc,"ivar_getOffset");
    size_t (*instance_size)(Class)=dlsym(runtime->objc,"class_getInstanceSize");Class (*get_meta)(id)=dlsym(runtime->objc,"object_getClass");
    if(!allocate||!dispose||!register_class||!add_ivar||!get_ivar||!offset||!instance_size||!get_meta||!runtime->foundation)return R2_ERR_UNSUPPORTED;
    char name[256];snprintf(name,sizeof(name),"R2Guest_%p_%llx",(void *)runtime,(unsigned long long)guest);Class native=allocate(parent,name,0);
    if(!native){r2_set_error(error,error_len,"cannot allocate native Objective-C class adapter");return R2_ERR_UNSUPPORTED;}
    uint64_t ivars;memcpy(&ivars,(void *)(uintptr_t)(ro+48),8);
    if(ivars) {
        const uint32_t *header=r2_native_pointer(runtime,ivars,8,false,error,error_len);if(!header){dispose(native);return R2_ERR_UNSUPPORTED;}
        uint32_t stride=header[0],count=header[1];
        if(stride<32||count>256||count>(SIZE_MAX-8)/stride){dispose(native);r2_set_error(error,error_len,"invalid guest Objective-C ivars");return R2_ERR_UNSUPPORTED;}
        const uint8_t *list=r2_native_pointer(runtime,ivars,8+(size_t)count*stride,false,error,error_len);if(!list){dispose(native);return R2_ERR_UNSUPPORTED;}
        for(uint32_t i=0;i<count;i++) {
            const uint8_t *entry=list+8+i*stride;uint64_t off_addr,name_addr,type_addr;uint32_t alignment,size;
            memcpy(&off_addr,entry,8);memcpy(&name_addr,entry+8,8);memcpy(&type_addr,entry+16,8);memcpy(&alignment,entry+24,4);memcpy(&size,entry+28,4);
            const int32_t *expected=r2_native_pointer(runtime,off_addr,4,false,error,error_len);char ivar[256],encoding[256];
            if(!expected||!graphics_string(runtime,name_addr,ivar,error,error_len)||!graphics_string(runtime,type_addr,encoding,error,error_len)||alignment>4||!size||!add_ivar(native,ivar,size,(uint8_t)alignment,encoding)){dispose(native);r2_set_error(error,error_len,"unsupported guest Objective-C ivar layout");return R2_ERR_UNSUPPORTED;}
            Ivar field=get_ivar(native,ivar);if(!field||offset(field)!=*expected){dispose(native);r2_set_error(error,error_len,"native/guest Objective-C ivar offsets differ: %s",ivar);return R2_ERR_UNSUPPORTED;}
        }
    }
    uint32_t expected_size;memcpy(&expected_size,(void *)(uintptr_t)(ro+8),4);
    size_t actual_size=instance_size(native);if(actual_size!=expected_size){dispose(native);r2_set_error(error,error_len,"native/guest Objective-C instance sizes differ (%zu/%u)",actual_size,expected_size);return R2_ERR_UNSUPPORTED;}
    status=objc_install_methods(runtime,native,ro,error,error_len);
    uint64_t meta_ro;memcpy(&meta_ro,(void *)(uintptr_t)(meta+32),8);meta_ro&=~UINT64_C(7);
    if(status==R2_OK) {
        if(!objc_guest_data(runtime,meta_ro,72)){r2_set_error(error,error_len,"invalid guest Objective-C metaclass layout");status=R2_ERR_UNSUPPORTED;}
        else status=objc_install_methods(runtime,get_meta((id)native),meta_ro,error,error_len);
    }
    if(status!=R2_OK){dispose(native);return status;}
    register_class(native);size_t index=runtime->objc_class_count++;runtime->objc_classes[index].guest=guest;runtime->objc_classes[index].native=native;*result=native;return R2_OK;
}
static r2_status objc_bridge_class(r2_native_runtime *runtime,uint64_t guest,Class *result,char *error,size_t error_len)
{
    return objc_bridge_class_depth(runtime,guest,result,0,error,error_len);
}
/* Native IMP getters return a guest stub with the method's exact ABI. Guest
 * closure IMPs map back to their original x86 entry point. */
static r2_status objc_get_implementation(r2_native_runtime *runtime,const char *name,uint64_t regs[16],char *error,size_t error_len)
{
    if(!runtime->objc)runtime->objc=dlopen("/usr/lib/libobjc.A.dylib",RTLD_NOW|RTLD_LOCAL);
    Method (*find)(Class,SEL)=dlsym(runtime->objc,"class_getInstanceMethod");
    IMP (*implementation)(Method)=dlsym(runtime->objc,"method_getImplementation");
    unsigned (*count)(Method)=dlsym(runtime->objc,"method_getNumberOfArguments");
    void (*result_type)(Method,char *,size_t)=dlsym(runtime->objc,"method_getReturnType");
    void (*arg_type)(Method,unsigned,char *,size_t)=dlsym(runtime->objc,"method_getArgumentType");
    SEL (*selector)(const char *)=dlsym(runtime->objc,"sel_registerName");
    if(!find||!implementation||!count||!result_type||!arg_type||!selector)return R2_ERR_UNSUPPORTED;
    Method method=(Method)(uintptr_t)regs[R2_X86_RDI];
    if(strcmp(name,"class_getMethodImplementation")==0) {
        Class cls;r2_status status=objc_bridge_class(runtime,regs[R2_X86_RDI],&cls,error,error_len);if(status!=R2_OK)return status;
        char sel[256];if(!graphics_string(runtime,regs[R2_X86_RSI],sel,error,error_len))return R2_ERR_UNSUPPORTED;
        method=find(cls,selector(sel));
    }
    if(!method){regs[R2_X86_RAX]=0;return R2_OK;}
    IMP function=implementation(method);
    for(native_objc_imp *imp=atomic_load_explicit(&pinned_objc_imps,memory_order_acquire);imp;imp=imp->next)
        if(imp->code==(void *)function&&imp->callback->runtime==runtime){regs[R2_X86_RAX]=imp->callback->function;return R2_OK;}
    unsigned arguments=count(method);char encoding[256]={0};result_type(method,encoding,sizeof(encoding));char kind=objc_scalar_type(encoding);
    if(!kind||arguments<2||arguments>NATIVE_MAX_ARGS){r2_set_error(error,error_len,"unsupported native Objective-C IMP ABI");return R2_ERR_UNSUPPORTED;}
    char signature[NATIVE_MAX_ARGS+1];
    for(unsigned i=0;i<arguments;i++){memset(encoding,0,sizeof(encoding));arg_type(method,i,encoding,sizeof(encoding));signature[i]=objc_scalar_type(encoding);if(!signature[i]||signature[i]=='v'){r2_set_error(error,error_len,"unsupported native Objective-C IMP argument ABI");return R2_ERR_UNSUPPORTED;}}
    signature[arguments]=0;
    for(size_t i=0;i<runtime->import_count;i++) {
        const native_import *existing=&runtime->imports[i];
        if(existing->function==(void *)function&&existing->definition->result==kind&&strcmp(existing->definition->arguments,signature)==0){regs[R2_X86_RAX]=(uintptr_t)existing->stub;return R2_OK;}
    }
    if(runtime->objc_imp_definition_count>=128){r2_set_error(error,error_len,"native Objective-C IMP wrapper limit reached");return R2_ERR_UNSUPPORTED;}
    size_t index=runtime->objc_imp_definition_count++;
    snprintf(runtime->objc_imp_definitions[index].name,sizeof(runtime->objc_imp_definitions[index].name),"__r2_objc_imp_%zu",index);
    memcpy(runtime->objc_imp_definitions[index].arguments,signature,arguments+1);
    native_definition *definition=&runtime->objc_imp_definitions[index].definition;
    *definition=(native_definition){runtime->objc_imp_definitions[index].name,runtime->objc_imp_definitions[index].arguments,kind,-1};
    char symbol[66];snprintf(symbol,sizeof(symbol),"_%s",definition->name);
    runtime->override_name=definition->name;runtime->override_definition=definition;runtime->override_function=(void *)function;
    r2_status status=r2_native_symbol(runtime,symbol,&regs[R2_X86_RAX],error,error_len);
    runtime->override_name=NULL;runtime->override_definition=NULL;runtime->override_function=NULL;return status;
}
