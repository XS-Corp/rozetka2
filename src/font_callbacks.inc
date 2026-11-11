/* Public CoreGraphics/CoreText aggregates differ in x86 and ARM return ABI. */
_Static_assert(sizeof(CGAffineTransform)==48&&sizeof(CGRect)==32&&sizeof(CFRange)==16,"Darwin font aggregate layouts");
static void path_apply_proxy(void *opaque,const CGPathElement *element)
{
    native_callback *callback=opaque;uint64_t args[]={callback->argument,(uintptr_t)element},result;
    char error[R2_ERROR_CAP]={0};(void)callback_invoke(callback,args,2,&result,error,sizeof(error));
}
typedef struct {
    native_callback *callback;
    const void *original;
    void *copy;
    size_t size;
    _Atomic unsigned references;
    _Atomic bool released;
} native_provider_data;
static void provider_drop(native_provider_data *data) {if(atomic_fetch_sub_explicit(&data->references,1,memory_order_acq_rel)==1)free(data);}
static void provider_release_proxy(void *opaque,const void *bytes,size_t size)
{
    (void)bytes;(void)size;native_provider_data *data=opaque;
    if(atomic_exchange_explicit(&data->released,true,memory_order_acq_rel))return;
    if(data->callback) {
        uint64_t args[]={data->callback->argument,(uintptr_t)data->original,data->size},result;char error[R2_ERROR_CAP]={0};
        (void)callback_invoke(data->callback,args,3,&result,error,sizeof(error));atomic_store_explicit(&data->callback->disabled,true,memory_order_release);
    }
    free(data->copy);provider_drop(data);
}
static r2_status font_call(r2_native_runtime *runtime,native_import *import,uint64_t regs[16],uint8_t xmm[16][16],bool *handled,char *error,size_t error_len)
{
    const char *name=import->definition->name;*handled=true;
    if(strcmp(name,"CGAffineTransformScale")==0) {
        void *output=r2_native_pointer(runtime,regs[R2_X86_RDI],48,true,error,error_len);
        const void *input=r2_native_pointer(runtime,regs[R2_X86_RSP]+8,48,false,error,error_len);if(!output||!input)return R2_ERR_UNSUPPORTED;
        CGAffineTransform transform;memcpy(&transform,input,48);double x,y;memcpy(&x,xmm[0],8);memcpy(&y,xmm[1],8);
        CGAffineTransform (*function)(CGAffineTransform,CGFloat,CGFloat)=import->function;
        CGAffineTransform result=function(transform,x,y);memcpy(output,&result,48);regs[R2_X86_RAX]=regs[R2_X86_RDI];return R2_OK;
    }
    if(strcmp(name,"CTFontGetBoundingRectsForGlyphs")==0) {
        CGRect *output=r2_native_pointer(runtime,regs[R2_X86_RDI],32,true,error,error_len);if(!output)return R2_ERR_UNSUPPORTED;
        CFIndex count=(CFIndex)regs[R2_X86_R9];if(count<0||(uint64_t)count>SIZE_MAX/32) {r2_set_error(error,error_len,"invalid font glyph count");return R2_ERR_UNSUPPORTED;}
        const CGGlyph *glyphs=r2_native_pointer(runtime,regs[R2_X86_RCX],(size_t)count*2,false,error,error_len);if(count&&!glyphs)return R2_ERR_UNSUPPORTED;
        CGRect *bounds=regs[R2_X86_R8]?r2_native_pointer(runtime,regs[R2_X86_R8],(size_t)count*32,true,error,error_len):NULL;if(regs[R2_X86_R8]&&count&&!bounds)return R2_ERR_UNSUPPORTED;
        CGRect (*function)(CTFontRef,CTFontOrientation,const CGGlyph *,CGRect *,CFIndex)=import->function;
        *output=function((CTFontRef)(uintptr_t)regs[R2_X86_RSI],(CTFontOrientation)regs[R2_X86_RDX],glyphs,bounds,count);regs[R2_X86_RAX]=regs[R2_X86_RDI];return R2_OK;
    }
    if(strcmp(name,"CTRunGetStringRange")==0) {
        CFRange (*function)(CTRunRef)=import->function;CFRange range=function((CTRunRef)(uintptr_t)regs[R2_X86_RDI]);
        regs[R2_X86_RAX]=(uint64_t)range.location;regs[R2_X86_RDX]=(uint64_t)range.length;return R2_OK;
    }
    if(strcmp(name,"CGPathApply")==0) {
        native_callback callback={0};if(!callback_initialize(runtime,&callback,regs[R2_X86_RDX],regs[R2_X86_RSI],error,error_len))return R2_ERR_UNSUPPORTED;
        void (*function)(CGPathRef,void *,CGPathApplierFunction)=import->function;
        unsigned depth=engine_suspend(runtime);function((CGPathRef)(uintptr_t)regs[R2_X86_RDI],&callback,path_apply_proxy);engine_resume(runtime,depth);
        return r2_native_stopping(runtime)?r2_native_execution_poll(runtime,error,error_len):R2_OK;
    }
    if(strcmp(name,"CGDataProviderCreateWithData")==0) {
        size_t size=(size_t)regs[R2_X86_RDX];const void *original=size?r2_native_pointer(runtime,regs[R2_X86_RSI],size,false,error,error_len):(void *)(uintptr_t)regs[R2_X86_RSI];
        if(size&&!original)return R2_ERR_UNSUPPORTED;
        native_provider_data *data=calloc(1,sizeof(*data));if(!data)return R2_ERR_NO_MEMORY;
        data->copy=malloc(size?size:1);if(!data->copy){free(data);return R2_ERR_NO_MEMORY;}
        if(size)memcpy(data->copy,original,size);data->original=original;data->size=size;atomic_init(&data->references,2);atomic_init(&data->released,false);
        if(regs[R2_X86_RCX]) {
            data->callback=callback_create(runtime,regs[R2_X86_RCX],regs[R2_X86_RDI],error,error_len);
            if(!data->callback){free(data->copy);free(data);return R2_ERR_UNSUPPORTED;}
        }
        CGDataProviderRef (*function)(void *,const void *,size_t,CGDataProviderReleaseDataCallback)=import->function;
        unsigned depth=engine_suspend(runtime);CGDataProviderRef provider=function(data,data->copy,size,provider_release_proxy);engine_resume(runtime,depth);
        if(!provider&&!atomic_exchange_explicit(&data->released,true,memory_order_acq_rel)) {
            if(data->callback)atomic_store_explicit(&data->callback->disabled,true,memory_order_release);
            free(data->copy);provider_drop(data);
        }
        provider_drop(data);regs[R2_X86_RAX]=(uintptr_t)provider;return R2_OK;
    }
    *handled=false;return R2_OK;
}
