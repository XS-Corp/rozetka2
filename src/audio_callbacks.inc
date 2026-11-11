/* CoreAudio owns native trampolines; all DSP and OpenAL code remains x86. */
_Static_assert(sizeof(AudioObjectPropertyAddress)==12,"Darwin AudioObjectPropertyAddress ABI");
_Static_assert(sizeof(AURenderCallbackStruct)==16,"Darwin AURenderCallbackStruct ABI");
_Static_assert(sizeof(AudioTimeStamp)==64,"Darwin AudioTimeStamp ABI");
_Static_assert(sizeof(AudioBuffer)==16,"Darwin AudioBuffer ABI");
static OSStatus audio_listener_proxy(AudioObjectID object,UInt32 count,const AudioObjectPropertyAddress *addresses,void *opaque)
{
    native_callback *callback=opaque;uint64_t args[]={object,count,(uintptr_t)addresses,callback->argument},result=0;
    char error[R2_ERROR_CAP]={0};r2_status status=callback_invoke(callback,args,4,&result,error,sizeof(error));
    return status==R2_OK?(OSStatus)result:kAudio_ParamError;
}
static OSStatus audio_render_proxy(void *opaque,AudioUnitRenderActionFlags *flags,const AudioTimeStamp *time,
                                   UInt32 bus,UInt32 frames,AudioBufferList *buffers)
{
    native_callback *callback=opaque;uint64_t args[]={callback->argument,(uintptr_t)flags,(uintptr_t)time,bus,frames,(uintptr_t)buffers},result=0;
    char error[R2_ERROR_CAP]={0};r2_status status=callback_invoke(callback,args,6,&result,error,sizeof(error));
    return status==R2_OK?(OSStatus)result:kAudio_ParamError;
}
static bool audio_callback_property(AudioUnitPropertyID property)
{
    return property==kAudioUnitProperty_SetRenderCallback||property==kAudioOutputUnitProperty_SetInputCallback;
}
static void audio_quiesce(r2_native_runtime *runtime)
{
    if(runtime->core_audio) {
        OSStatus (*remove)(AudioObjectID,const AudioObjectPropertyAddress *,AudioObjectPropertyListenerProc,void *)=
            dlsym(runtime->core_audio,"AudioObjectRemovePropertyListener");
        for(size_t i=0;i<64;i++)if(runtime->audio_listeners[i].callback) {
            if(remove)remove(runtime->audio_listeners[i].object,&runtime->audio_listeners[i].address,audio_listener_proxy,runtime->audio_listeners[i].callback);
            runtime->audio_listeners[i].callback=NULL;
        }
    }
    if(runtime->audio_toolbox) {
        OSStatus (*stop)(AudioUnit)=dlsym(runtime->audio_toolbox,"AudioOutputUnitStop");
        OSStatus (*dispose)(AudioComponentInstance)=dlsym(runtime->audio_toolbox,"AudioComponentInstanceDispose");
        for(size_t i=0;i<64;i++)if(runtime->audio_units[i]) {
            if(stop)stop(runtime->audio_units[i]);
            if(dispose)dispose(runtime->audio_units[i]);
            runtime->audio_units[i]=NULL;
        }
    }
}
static r2_status audio_special(r2_native_runtime *runtime,native_import *import,uint64_t regs[16],bool *handled,char *error,size_t error_len)
{
    const char *name=import->definition->name;*handled=true;
    if(strcmp(name,"AudioObjectAddPropertyListener")==0||strcmp(name,"AudioObjectRemovePropertyListener")==0) {
        const AudioObjectPropertyAddress *address=r2_native_pointer(runtime,regs[R2_X86_RSI],sizeof(*address),false,error,error_len);
        if(!address)return R2_ERR_UNSUPPORTED;
        size_t slot=64;bool add=strcmp(name,"AudioObjectAddPropertyListener")==0;
        for(size_t i=0;i<64;i++)if(runtime->audio_listeners[i].callback&&runtime->audio_listeners[i].object==(AudioObjectID)regs[R2_X86_RDI]&&
            memcmp(&runtime->audio_listeners[i].address,address,sizeof(*address))==0&&
            runtime->audio_listeners[i].callback->function==regs[R2_X86_RDX]&&runtime->audio_listeners[i].callback->argument==regs[R2_X86_RCX]) {slot=i;break;}
        /* CoreAudio registrations are idempotent for the same tuple. */
        if(add&&slot<64) {regs[R2_X86_RAX]=0;return R2_OK;}
        if(!add&&slot==64) {regs[R2_X86_RAX]=(uint32_t)kAudioHardwareIllegalOperationError;return R2_OK;}
        if(add)for(slot=0;slot<64&&runtime->audio_listeners[slot].callback;slot++) {}
        if(slot==64) {r2_set_error(error,error_len,"too many CoreAudio listeners");return R2_ERR_UNSUPPORTED;}
        native_callback *callback=add?callback_create(runtime,regs[R2_X86_RDX],regs[R2_X86_RCX],error,error_len):runtime->audio_listeners[slot].callback;
        if(!callback)return R2_ERR_UNSUPPORTED;
        AudioObjectPropertyAddress copy=*address;
        if(add) {runtime->audio_listeners[slot].object=(AudioObjectID)regs[R2_X86_RDI];runtime->audio_listeners[slot].address=copy;runtime->audio_listeners[slot].callback=callback;}
        OSStatus (*function)(AudioObjectID,const AudioObjectPropertyAddress *,AudioObjectPropertyListenerProc,void *)=import->function;
        unsigned depth=engine_suspend(runtime);OSStatus result=function((AudioObjectID)regs[R2_X86_RDI],&copy,audio_listener_proxy,callback);engine_resume(runtime,depth);
        if((add&&result!=0)||(!add&&result==0)) {atomic_store_explicit(&callback->disabled,true,memory_order_release);runtime->audio_listeners[slot].callback=NULL;}
        regs[R2_X86_RAX]=(uint32_t)result;return R2_OK;
    }
    if(strcmp(name,"AudioComponentInstanceNew")==0) {
        AudioUnit *output=r2_native_pointer(runtime,regs[R2_X86_RSI],sizeof(*output),true,error,error_len);if(!output)return R2_ERR_UNSUPPORTED;
        size_t slot;for(slot=0;slot<64&&runtime->audio_units[slot];slot++) {}
        if(slot==64) {r2_set_error(error,error_len,"too many guest AudioUnits");return R2_ERR_UNSUPPORTED;}
        OSStatus (*create)(AudioComponent,AudioComponentInstance *)=import->function;AudioUnit unit=NULL;
        unsigned depth=engine_suspend(runtime);OSStatus result=create((AudioComponent)(uintptr_t)regs[R2_X86_RDI],&unit);engine_resume(runtime,depth);
        if(result==0) {*output=unit;runtime->audio_units[slot]=unit;}
        regs[R2_X86_RAX]=(uint32_t)result;return R2_OK;
    }
    AudioUnit unit=(AudioUnit)(uintptr_t)regs[R2_X86_RDI];
    bool dispose=strcmp(name,"AudioComponentInstanceDispose")==0,set=strcmp(name,"AudioUnitSetProperty")==0,get=strcmp(name,"AudioUnitGetProperty")==0;
    if(dispose||set||get) {
        bool owned=false;for(size_t i=0;i<64;i++)if(runtime->audio_units[i]==unit&&unit)owned=true;
        if(!owned) {r2_set_error(error,error_len,"AudioUnit is not owned by guest runtime");return R2_ERR_UNSUPPORTED;}
    }
    if(dispose) {
        OSStatus (*function)(AudioComponentInstance)=import->function;
        unsigned depth=engine_suspend(runtime);OSStatus result=function(unit);engine_resume(runtime,depth);
        if(result==0) {
            for(size_t i=0;i<64;i++)if(runtime->audio_units[i]==unit)runtime->audio_units[i]=NULL;
            for(size_t i=0;i<128;i++)if(runtime->audio_renders[i].unit==unit) {
                if(runtime->audio_renders[i].callback)atomic_store_explicit(&runtime->audio_renders[i].callback->disabled,true,memory_order_release);
                memset(&runtime->audio_renders[i],0,sizeof(runtime->audio_renders[i]));
            }
        }
        regs[R2_X86_RAX]=(uint32_t)result;return R2_OK;
    }
    AudioUnitPropertyID property=(UInt32)regs[R2_X86_RSI];AudioUnitScope scope=(UInt32)regs[R2_X86_RDX];AudioUnitElement element=(UInt32)regs[R2_X86_RCX];
    if((set||get)&&audio_callback_property(property)) {
        size_t slot;for(slot=0;slot<128;slot++)if(runtime->audio_renders[slot].unit==unit&&runtime->audio_renders[slot].property==property&&runtime->audio_renders[slot].scope==scope&&runtime->audio_renders[slot].element==element)break;
        if(get) {
            UInt32 *size=r2_native_pointer(runtime,regs[R2_X86_R9],4,true,error,error_len);if(!size)return R2_ERR_UNSUPPORTED;
            if(*size<16) {regs[R2_X86_RAX]=(uint32_t)kAudioUnitErr_InvalidPropertyValue;return R2_OK;}
            uint64_t *output=r2_native_pointer(runtime,regs[R2_X86_R8],16,true,error,error_len);if(!output)return R2_ERR_UNSUPPORTED;
            AURenderCallbackStruct host={0};UInt32 host_size=sizeof(host);
            OSStatus (*function)(AudioUnit,AudioUnitPropertyID,AudioUnitScope,AudioUnitElement,void *,UInt32 *)=import->function;
            unsigned depth=engine_suspend(runtime);OSStatus result=function(unit,property,scope,element,&host,&host_size);engine_resume(runtime,depth);
            if(result==0) {
                if(host.inputProc&&(slot==128||host.inputProc!=audio_render_proxy||host.inputProcRefCon!=runtime->audio_renders[slot].callback)) {
                    r2_set_error(error,error_len,"AudioUnit returned an unregistered native render callback");return R2_ERR_UNSUPPORTED;
                }
                native_callback *callback=slot<128?runtime->audio_renders[slot].callback:NULL;
                output[0]=host.inputProc&&callback?callback->function:0;output[1]=host.inputProc&&callback?callback->argument:(uintptr_t)host.inputProcRefCon;*size=host_size;
            }
            regs[R2_X86_RAX]=(uint32_t)result;return R2_OK;
        }
        if((UInt32)regs[R2_X86_R9]!=16) {regs[R2_X86_RAX]=(uint32_t)kAudioUnitErr_InvalidPropertyValue;return R2_OK;}
        const uint64_t *guest=r2_native_pointer(runtime,regs[R2_X86_R8],16,false,error,error_len);if(!guest)return R2_ERR_UNSUPPORTED;
        native_callback *callback=guest[0]?callback_create(runtime,guest[0],guest[1],error,error_len):NULL;if(guest[0]&&!callback)return R2_ERR_UNSUPPORTED;
        if(slot==128)for(slot=0;slot<128&&runtime->audio_renders[slot].unit;slot++) {}
        if(slot==128) {if(callback)atomic_store(&callback->disabled,true);r2_set_error(error,error_len,"too many AudioUnit render callbacks");return R2_ERR_UNSUPPORTED;}
        AURenderCallbackStruct host={callback?audio_render_proxy:NULL,callback};
        OSStatus (*function)(AudioUnit,AudioUnitPropertyID,AudioUnitScope,AudioUnitElement,const void *,UInt32)=import->function;
        unsigned depth=engine_suspend(runtime);OSStatus result=function(unit,property,scope,element,&host,sizeof(host));engine_resume(runtime,depth);
        if(result==0) {
            if(runtime->audio_renders[slot].callback)atomic_store_explicit(&runtime->audio_renders[slot].callback->disabled,true,memory_order_release);
            runtime->audio_renders[slot].unit=unit;runtime->audio_renders[slot].property=property;runtime->audio_renders[slot].scope=scope;runtime->audio_renders[slot].element=element;runtime->audio_renders[slot].callback=callback;
        } else if(callback)atomic_store_explicit(&callback->disabled,true,memory_order_release);
        regs[R2_X86_RAX]=(uint32_t)result;return R2_OK;
    }
    if(set) {
        /* These data properties have no embedded function pointers and share
         * the Darwin x86/ARM layout. Unknown properties require review. */
        switch(property) {
        case kAudioUnitProperty_StreamFormat:case kAudioUnitProperty_MaximumFramesPerSlice:
        case kAudioUnitProperty_ShouldAllocateBuffer:case kAudioUnitProperty_AudioChannelLayout:
        case kAudioOutputUnitProperty_CurrentDevice:case kAudioOutputUnitProperty_EnableIO:
        case kAudioOutputUnitProperty_ChannelMap:break;
        default:r2_set_error(error,error_len,"unsupported AudioUnit data property %u",property);return R2_ERR_UNSUPPORTED;
        }
        UInt32 size=(UInt32)regs[R2_X86_R9];void *data=r2_native_pointer(runtime,regs[R2_X86_R8],size,false,error,error_len);if(!data&&size)return R2_ERR_UNSUPPORTED;
        OSStatus (*function)(AudioUnit,AudioUnitPropertyID,AudioUnitScope,AudioUnitElement,const void *,UInt32)=import->function;
        unsigned depth=engine_suspend(runtime);OSStatus result=function(unit,property,scope,element,data,size);engine_resume(runtime,depth);
        regs[R2_X86_RAX]=(uint32_t)result;return R2_OK;
    }
    *handled=false;return R2_OK;
}
