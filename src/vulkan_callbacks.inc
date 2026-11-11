/* Layouts are generated from the pinned Khronos XML; callbacks use integer
 * registers and, for debug-report, two SysV stack arguments. */
static uint32_t vulkan_debug_utils(uint32_t severity,uint32_t types,const void *data,void *opaque)
{
    native_callback *callback=opaque;uint64_t args[]={severity,types,(uintptr_t)data,callback->argument},result;
    char error[R2_ERROR_CAP]={0};return callback_invoke(callback,args,4,&result,error,sizeof(error))==R2_OK?(uint32_t)result:0;
}
static uint32_t vulkan_debug_report(uint32_t flags,uint32_t object_type,uint64_t object,size_t location,int32_t code,const char *prefix,const char *message,void *opaque)
{
    native_callback *callback=opaque;uint64_t args[]={flags,object_type,object,location,(uint32_t)code,(uintptr_t)prefix,(uintptr_t)message,callback->argument},result;
    char error[R2_ERROR_CAP]={0};return callback_invoke(callback,args,8,&result,error,sizeof(error))==R2_OK?(uint32_t)result:0;
}
typedef struct {void *nodes[64];size_t count,slots[64],slot_count;} vulkan_shadow;
static void vulkan_shadow_free(vulkan_shadow *shadow) {for(size_t i=0;i<shadow->count;i++)free(shadow->nodes[i]);}
static void vulkan_callback_retire(r2_native_runtime *runtime,size_t slot)
{
    if(runtime->vk_callbacks[slot].callback)atomic_store_explicit(&runtime->vk_callbacks[slot].callback->disabled,true,memory_order_release);
    memset(&runtime->vk_callbacks[slot],0,sizeof(runtime->vk_callbacks[slot]));
}
static r2_status vulkan_shadow_chain(r2_native_runtime *runtime,uint64_t address,void **output,vulkan_shadow *shadow,char *error,size_t error_len)
{
    *output=NULL;void **link=output;
    while(address) {
        if(shadow->count==64) {r2_set_error(error,error_len,"invalid Vulkan pNext chain");return R2_ERR_UNSUPPORTED;}
        const uint8_t *header=r2_native_pointer(runtime,address,16,false,error,error_len);if(!header)return R2_ERR_UNSUPPORTED;
        uint32_t type;uint64_t next;memcpy(&type,header,4);memcpy(&next,header+8,8);size_t size=0;
        for(size_t i=0;i<sizeof(vulkan_instance_chain_sizes)/sizeof(vulkan_instance_chain_sizes[0]);i++)
            if(vulkan_instance_chain_sizes[i].type==type)size=vulkan_instance_chain_sizes[i].size;
        /* Direct-driver arrays embed executable callbacks of their own. */
        if(!size||type==1000459001) {r2_set_error(error,error_len,"unsupported Vulkan instance pNext structure %u",type);return R2_ERR_UNSUPPORTED;}
        const void *source=r2_native_pointer(runtime,address,size,false,error,error_len);if(!source)return R2_ERR_UNSUPPORTED;
        uint8_t *copy=malloc(size);if(!copy)return R2_ERR_NO_MEMORY;memcpy(copy,source,size);shadow->nodes[shadow->count++]=copy;
        *link=copy;link=(void **)(void *)(copy+8);*link=NULL;
        if(type==1000128004||type==1000011000) {
            bool report=type==1000011000;size_t offset=report?24:32;uint64_t function,argument;
            memcpy(&function,copy+offset,8);memcpy(&argument,copy+offset+8,8);
            native_callback *callback=callback_create(runtime,function,argument,error,error_len);if(!callback)return R2_ERR_UNSUPPORTED;
            size_t slot;for(slot=0;slot<128&&runtime->vk_callbacks[slot].callback;slot++) {}
            if(slot==128) {atomic_store(&callback->disabled,true);r2_set_error(error,error_len,"too many Vulkan debug callbacks");return R2_ERR_UNSUPPORTED;}
            runtime->vk_callbacks[slot].callback=callback;runtime->vk_callbacks[slot].report=report;shadow->slots[shadow->slot_count++]=slot;
            void *proxy=report?(void *)vulkan_debug_report:(void *)vulkan_debug_utils;
            memcpy(copy+offset,&proxy,8);memcpy(copy+offset+8,&callback,8);
        }
        address=next;
    }
    return R2_OK;
}
static void vulkan_callbacks_quiesce(r2_native_runtime *runtime)
{
    void *library=runtime->graphics[1].library;if(!library)return;
    void *(*get)(void *,const char *)=dlsym(library,"vkGetInstanceProcAddr");if(!get)return;
    for(size_t i=0;i<128;i++)if(runtime->vk_callbacks[i].callback) {
        if(runtime->vk_callbacks[i].instance&&runtime->vk_callbacks[i].handle) {
            void (*destroy)(void *,uint64_t,const void *)=get(runtime->vk_callbacks[i].instance,runtime->vk_callbacks[i].report?"vkDestroyDebugReportCallbackEXT":"vkDestroyDebugUtilsMessengerEXT");
            if(destroy)destroy(runtime->vk_callbacks[i].instance,runtime->vk_callbacks[i].handle,NULL);
        }
        vulkan_callback_retire(runtime,i);
    }
}
static r2_status vulkan_special(r2_native_runtime *runtime,native_import *import,uint64_t regs[16],bool *handled,char *error,size_t error_len)
{
    const char *name=import->definition->name;*handled=true;
    bool instance=strcmp(name,"vkCreateInstance")==0,utils=strcmp(name,"vkCreateDebugUtilsMessengerEXT")==0,report=strcmp(name,"vkCreateDebugReportCallbackEXT")==0;
    bool destroy_instance=strcmp(name,"vkDestroyInstance")==0,destroy=strcmp(name,"vkDestroyDebugUtilsMessengerEXT")==0||strcmp(name,"vkDestroyDebugReportCallbackEXT")==0;
    if(destroy_instance||destroy) {
        if(regs[destroy_instance?R2_X86_RSI:R2_X86_RDX]) {r2_set_error(error,error_len,"guest graphics callback is unsupported in %s allocator",name);return R2_ERR_UNSUPPORTED;}
        unsigned depth=engine_suspend(runtime);
        if(destroy_instance) {void (*function)(void *,const void *)=import->function;function((void *)(uintptr_t)regs[R2_X86_RDI],NULL);}
        else {void (*function)(void *,uint64_t,const void *)=import->function;function((void *)(uintptr_t)regs[R2_X86_RDI],regs[R2_X86_RSI],NULL);}
        engine_resume(runtime,depth);
        for(size_t i=0;i<128;i++)if(runtime->vk_callbacks[i].instance==(void *)(uintptr_t)regs[R2_X86_RDI]&&(destroy_instance||runtime->vk_callbacks[i].handle==regs[R2_X86_RSI]))vulkan_callback_retire(runtime,i);
        return R2_OK;
    }
    if(!instance&&!utils&&!report) {*handled=false;return R2_OK;}
    uint64_t allocator=regs[instance?R2_X86_RSI:R2_X86_RDX],output_address=regs[instance?R2_X86_RDX:R2_X86_RCX];
    if(allocator) {r2_set_error(error,error_len,"guest graphics callback is unsupported in %s allocator",name);return R2_ERR_UNSUPPORTED;}
    uint64_t *output=r2_native_pointer(runtime,output_address,8,true,error,error_len);if(!output)return R2_ERR_UNSUPPORTED;
    uint64_t guest=regs[instance?R2_X86_RDI:R2_X86_RSI];vulkan_shadow shadow={0};void *native=NULL;r2_status status;
    uint8_t info[64];
    if(instance) {
        const uint8_t *source=r2_native_pointer(runtime,guest,sizeof(info),false,error,error_len);if(!source)return R2_ERR_UNSUPPORTED;
        memcpy(info,source,sizeof(info));uint64_t next;memcpy(&next,info+8,8);void *chain=NULL;
        status=vulkan_shadow_chain(runtime,next,&chain,&shadow,error,error_len);memcpy(info+8,&chain,8);native=info;
    } else status=vulkan_shadow_chain(runtime,guest,&native,&shadow,error,error_len);
    uint64_t handle=0;int32_t result=-1;
    if(status==R2_OK) {
        unsigned depth=engine_suspend(runtime);
        if(instance) {int32_t (*function)(const void *,const void *,uint64_t *)=import->function;result=function(native,NULL,&handle);}
        else {int32_t (*function)(void *,const void *,const void *,uint64_t *)=import->function;result=function((void *)(uintptr_t)regs[R2_X86_RDI],native,NULL,&handle);}
        engine_resume(runtime,depth);
        if(result==0)*output=handle;
    }
    for(size_t i=0;i<shadow.slot_count;i++) {
        size_t slot=shadow.slots[i];
        if(status!=R2_OK||result!=0)vulkan_callback_retire(runtime,slot);
        else {runtime->vk_callbacks[slot].instance=instance?(void *)(uintptr_t)handle:(void *)(uintptr_t)regs[R2_X86_RDI];runtime->vk_callbacks[slot].handle=instance?0:handle;}
    }
    vulkan_shadow_free(&shadow);regs[R2_X86_RAX]=(uint32_t)result;return status;
}
