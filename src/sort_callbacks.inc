/* Darwin qsort_r supplies a per-call context, so nested guest sorts never
 * overwrite a global comparator. The OS does not retain these callbacks. */
typedef struct {native_callback callback;bool contextual;r2_status status;char error[R2_ERROR_CAP];} native_sort;
static int sort_compare_proxy(void *opaque,const void *left,const void *right)
{
    native_sort *sort=opaque;if(sort->status!=R2_OK)return 0;
    uint64_t arguments[3];size_t count=0;
    if(sort->contextual)arguments[count++]=sort->callback.argument;
    arguments[count++]=(uintptr_t)left;arguments[count++]=(uintptr_t)right;uint64_t value=0;
    sort->status=callback_invoke(&sort->callback,arguments,count,&value,sort->error,sizeof(sort->error));return (int32_t)value;
}
static r2_status sort_call(r2_native_runtime *runtime,const char *name,uint64_t regs[16],char *error,size_t error_len)
{
    size_t count=regs[R2_X86_RSI],size=regs[R2_X86_RDX];
    if(size&&count>SIZE_MAX/size){r2_set_error(error,error_len,"guest sort array size overflows");return R2_ERR_UNSUPPORTED;}
    if(count<2||!size)return R2_OK;
    void *base=r2_native_pointer(runtime,regs[R2_X86_RDI],count*size,true,error,error_len);if(!base)return R2_ERR_UNSUPPORTED;
    native_sort sort={.contextual=strcmp(name,"qsort_r")==0,.status=R2_OK};
    uint64_t comparator=regs[sort.contextual?R2_X86_R8:R2_X86_RCX];
    if(!callback_initialize(runtime,&sort.callback,comparator,sort.contextual?regs[R2_X86_RCX]:0,error,error_len))return R2_ERR_UNSUPPORTED;
    unsigned depth=engine_suspend(runtime);qsort_r(base,count,size,&sort,sort_compare_proxy);engine_resume(runtime,depth);
    if(sort.status!=R2_OK){r2_set_error(error,error_len,"%s",sort.error);return sort.status;}
    return r2_native_stopping(runtime)?r2_native_execution_poll(runtime,error,error_len):R2_OK;
}
