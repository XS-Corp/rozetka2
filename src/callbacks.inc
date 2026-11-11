/* Native libraries receive ARM trampolines. Their context retains the x86
 * target, never an executable cast of a guest function pointer. Retired
 * contexts remain pinned for late OS notifications, with guest access disabled. */
static bool callback_initialize(r2_native_runtime *runtime,native_callback *callback,uint64_t function,uint64_t argument,char *error,size_t error_len)
{
    bool dynamic;
    if(!function||!native_thread||!native_thread->execution.env||!executable_span(runtime,function,&dynamic)) {
        r2_set_error(error,error_len,"callback target is outside guest executable code");return false;
    }
    callback->runtime=runtime;callback->env=*native_thread->execution.env;
    callback->env.borrowed_stack_base=0;callback->function=function;callback->argument=argument;
    atomic_init(&callback->disabled,false);atomic_init(&callback->inflight,0);return true;
}
static native_callback *callback_create(r2_native_runtime *runtime,uint64_t function,uint64_t argument,char *error,size_t error_len)
{
    native_callback *callback=calloc(1,sizeof(*callback));if(!callback)return NULL;
    if(!callback_initialize(runtime,callback,function,argument,error,error_len)){free(callback);return NULL;}
    if(atomic_fetch_add_explicit(&pinned_callback_count,1,memory_order_relaxed)>=4096) {
        atomic_fetch_sub_explicit(&pinned_callback_count,1,memory_order_relaxed);free(callback);
        r2_set_error(error,error_len,"native callback context limit reached");return NULL;
    }
    native_callback *head=atomic_load_explicit(&pinned_callbacks,memory_order_relaxed);
    do {callback->next=head;} while(!atomic_compare_exchange_weak_explicit(&pinned_callbacks,&head,callback,memory_order_release,memory_order_relaxed));
    return callback;
}
static r2_status callback_entry(r2_native_runtime *runtime,const r2_interp_env *template,uint64_t function,
                                 const uint64_t *arguments,size_t count,uint64_t stack,uint64_t size,uint64_t sp,
                                 r2_result *result,char *error,size_t error_len)
{
    bool isolated=native_thread->execution.env!=NULL;
    r2_block *block=isolated?NULL:native_thread->callback_block;
    if(!block) {
        block=calloc(1,sizeof(*block));if(!block)return R2_ERR_NO_MEMORY;
        block->insns=block->storage;block->capacity=R2_MAX_INSNS;block->runtime=runtime;
        if(!isolated)native_thread->callback_block=block;
    }
    /* Repeated foreign-thread callbacks reuse their decoded instructions.
     * The normal decoder still verifies permissions and bytes on every fetch,
     * so JIT patches and unmapped code cannot hide in this cache. */
    /* Reentry has its own decoded block. Preserve the caller's lookup table
     * instead of rebuilding a JVM-sized cache after every log/audio callback. */
    size_t *previous_hash=native_thread->hash,previous_capacity=native_thread->hash_capacity;
    r2_block *previous_block=native_thread->hash_block;
    size_t previous_count=previous_block?previous_block->count:0;
    if(isolated) {
        size_t *hash=calloc(R2_MAX_INSNS,sizeof(*hash));
        if(!hash){free(block);return R2_ERR_NO_MEMORY;}
        native_thread->hash=hash;native_thread->hash_capacity=R2_MAX_INSNS;native_thread->hash_block=NULL;
    }
    r2_interp_env env=*template;env.has_initial_regs=true;memset(env.initial_regs,0,sizeof(env.initial_regs));
    const unsigned registers[]={R2_X86_RDI,R2_X86_RSI,R2_X86_RDX,R2_X86_RCX,R2_X86_R8,R2_X86_R9};
    for(size_t i=0;i<count&&i<6;i++)env.initial_regs[registers[i]]=arguments[i];
    if(count>6) {
        size_t words=(count-6+1)&~(size_t)1;
        if(sp<stack||sp-stack<words*8) {
            if(isolated)free_guest_block(block);
            if(isolated){free(native_thread->hash);native_thread->hash=previous_hash;native_thread->hash_capacity=previous_capacity;native_thread->hash_block=previous_block;}
            r2_set_error(error,error_len,"callback stack arguments overflow guest stack");return R2_ERR_UNSUPPORTED;
        }
        sp-=words*8;memcpy((void *)(uintptr_t)(sp+8),arguments+6,(count-6)*8);
    }
    env.initial_regs[R2_X86_RSP]=sp;env.borrowed_stack_base=stack;env.stack_size=size;
    env.has_initial_rip=true;env.initial_rip=function;
    if(getenv("R2_TRACE_CALLBACKS"))fprintf(stderr,"callback entry env=%p process=%p guestsp=%llx\n",(void *)&env,(void *)env.process,(unsigned long long)sp);
    r2_status status=r2_interp_block_with_env(block,&env,result,error,error_len);
    if(isolated)free_guest_block(block);
    if(isolated) {
        free(native_thread->hash);native_thread->hash=previous_hash;native_thread->hash_capacity=previous_capacity;
        native_thread->hash_block=previous_block&&previous_block->count==previous_count?previous_block:NULL;
    }
    return status;
}
static void callback_fail(r2_native_runtime *runtime,r2_status status,const char *error)
{
    pthread_mutex_lock(&runtime->execution_mutex);runtime->active_callbacks++;
    r2_native_request_stop(runtime,status,error);
    runtime->active_callbacks--;pthread_mutex_unlock(&runtime->execution_mutex);
}
typedef struct native_callback_thread {
    native_callback *lifetime;
    native_thread_state *state;
    uint64_t stack,size;
    size_t slot;
    struct native_callback_thread *next;
} native_callback_thread;
static _Thread_local native_callback_thread *callback_thread_cache;
static pthread_key_t callback_thread_key;
static pthread_once_t callback_thread_key_once=PTHREAD_ONCE_INIT;
static int callback_thread_key_error;
static void callback_thread_destructor(void *opaque)
{
    native_callback_thread *entry=opaque;callback_thread_cache=NULL;
    while(entry) {
        native_callback_thread *next=entry->next;native_callback *lifetime=entry->lifetime;
        atomic_fetch_add_explicit(&lifetime->inflight,1,memory_order_acq_rel);
        if(!atomic_load_explicit(&lifetime->disabled,memory_order_acquire)) {
            r2_native_runtime *runtime=lifetime->runtime;native_thread_state *previous=native_thread;native_thread=entry->state;
            engine_enter(runtime);runtime->active_callbacks++;r2_status status=R2_OK;r2_result result={0};char error[R2_ERROR_CAP]={0};
            fenv_t host;fegetenv(&host);fesetenv(FE_DFL_ENV);
            uint64_t sp=entry->stack+entry->size-8;
            while(native_thread->tlv_exit_count&&status==R2_OK&&!r2_native_stopping(runtime)) {
                size_t index=--native_thread->tlv_exit_count;uint64_t values[]={native_thread->tlv_exits[index].argument};native_thread->thread_returning=false;
                status=callback_entry(runtime,&lifetime->env,native_thread->tlv_exits[index].function,values,1,entry->stack,entry->size,sp,&result,error,sizeof(error));
            }
            for(unsigned pass=0;pass<PTHREAD_DESTRUCTOR_ITERATIONS&&status==R2_OK&&!r2_native_stopping(runtime);pass++) {
                bool invoked=false;
                for(size_t i=0;i<runtime->key_count;i++) {
                    uint64_t destructor=runtime->keys[i].destructor;void *argument=native_thread->key_values[i];
                    if(!argument||!destructor)continue;
                    native_thread->key_values[i]=NULL;invoked=true;uint64_t values[]={(uintptr_t)argument};native_thread->thread_returning=false;
                    status=callback_entry(runtime,&lifetime->env,destructor,values,1,entry->stack,entry->size,sp,&result,error,sizeof(error));
                    if(status!=R2_OK||r2_native_stopping(runtime))break;
                }
                if(!invoked)break;
            }
            for(size_t i=0;i<runtime->tls_count;i++) {free(native_thread->tls_values[i]);native_thread->tls_values[i]=NULL;}
            if(status!=R2_OK)r2_native_request_stop(runtime,status,error);
            if(r2_native_stopping(runtime))abandon_mutexes();
            else r2_native_stack_release(runtime,(void *)(uintptr_t)entry->stack,(size_t)entry->size);
            runtime->callback_threads[entry->slot]=NULL;runtime->active_callbacks--;fesetenv(&host);engine_leave(runtime);native_thread=previous;
        }
        /* After quiescence the runtime owns/unmaps the guest stack. Only
         * native allocations are touched by this potentially late destructor. */
        destroy_thread_state(entry->state);free(entry);
        atomic_fetch_sub_explicit(&lifetime->inflight,1,memory_order_release);entry=next;
    }
}
static void callback_thread_key_initialize(void) {callback_thread_key_error=pthread_key_create(&callback_thread_key,callback_thread_destructor);}
static r2_status callback_invoke(native_callback *callback,const uint64_t *arguments,size_t count,uint64_t *value,char *error,size_t error_len)
{
    *value=0;atomic_fetch_add_explicit(&callback->inflight,1,memory_order_acq_rel);
    if(atomic_load_explicit(&callback->disabled,memory_order_acquire)) {
        atomic_fetch_sub_explicit(&callback->inflight,1,memory_order_release);return R2_OK;
    }
    r2_native_runtime *runtime=callback->runtime;
    native_thread_state *previous=native_thread;bool foreign=!previous||previous->runtime!=runtime;
    native_callback_thread *entry=NULL;
    if(foreign) {
        pthread_once(&callback_thread_key_once,callback_thread_key_initialize);
        for(entry=callback_thread_cache;entry;entry=entry->next)
            if(entry->lifetime&&entry->lifetime->runtime==runtime&&!atomic_load_explicit(&entry->lifetime->disabled,memory_order_acquire))break;
        if(!entry&&!callback_thread_key_error) {
            entry=calloc(1,sizeof(*entry));
            if(entry)entry->state=allocate_thread_state(runtime);
            if(entry&&!entry->state){free(entry);entry=NULL;}
            if(entry) {entry->slot=64;entry->next=callback_thread_cache;callback_thread_cache=entry;pthread_setspecific(callback_thread_key,entry);}
        }
        if(!entry) {
            r2_set_error(error,error_len,"cannot allocate foreign callback thread state");callback_fail(runtime,R2_ERR_NO_MEMORY,error);
            atomic_fetch_sub_explicit(&callback->inflight,1,memory_order_release);return R2_ERR_NO_MEMORY;
        }
        native_thread=entry->state;
    }
    engine_enter(runtime);runtime->active_callbacks++;
    if(getenv("R2_TRACE_CALLBACKS"))fprintf(stderr,"callback begin foreign=%d target=%llx thread=%p\n",foreign,(unsigned long long)callback->function,(void *)pthread_self());
    r2_status status=R2_OK;r2_result result={0};uint64_t stack=0,size=0,sp=0;
    r2_x87_state saved_x87=native_thread->x87;uint32_t saved_mxcsr=native_thread->mxcsr;
    bool saved_returning=native_thread->thread_returning;fenv_t host_environment;fegetenv(&host_environment);
    if(count>NATIVE_MAX_ARGS) {r2_set_error(error,error_len,"callback has too many arguments");status=R2_ERR_UNSUPPORTED;goto done;}
    if(r2_native_stopping(runtime)) {status=r2_native_execution_poll(runtime,error,error_len);goto done;}
    const r2_interp_env *template=&callback->env;
    if(foreign) {
        if(!entry->lifetime) {
            for(entry->slot=0;entry->slot<64&&runtime->callback_threads[entry->slot];entry->slot++) {}
            if(entry->slot==64) {r2_set_error(error,error_len,"too many simultaneous native callback threads");status=R2_ERR_UNSUPPORTED;goto done;}
            native_thread->execution.env=&callback->env;
            entry->lifetime=callback_create(runtime,callback->function,callback->argument,error,error_len);
            native_thread->execution.env=NULL;
            if(!entry->lifetime) {status=R2_ERR_NO_MEMORY;goto done;}
            entry->size=(uint64_t)getpagesize()*2048;
            entry->stack=(uintptr_t)r2_native_stack_allocate(runtime,(size_t)entry->size);
            if(!entry->stack) {status=R2_ERR_NO_MEMORY;r2_set_error(error,error_len,"cannot allocate callback guest stack");goto done;}
            runtime->callback_threads[entry->slot]=native_thread;
        }
        stack=entry->stack;size=entry->size;sp=stack+size-8;
    } else {
        if(!native_thread->execution.env) {status=R2_ERR_UNSUPPORTED;r2_set_error(error,error_len,"callback has no active guest stack");goto done;}
        template=native_thread->execution.env;stack=native_thread->execution.stack_base;size=native_thread->execution.stack_size;
        sp=native_thread->callback_stack;
        if(sp<stack||sp-stack<160) {status=R2_ERR_UNSUPPORTED;r2_set_error(error,error_len,"callback would overflow the guest stack");goto done;}
        sp=((sp-128)&~UINT64_C(15))-8;
    }
    fesetenv(FE_DFL_ENV);memset(&native_thread->x87,0,sizeof(native_thread->x87));
    native_thread->x87.control=saved_x87.control;native_thread->thread_returning=false;
    status=callback_entry(runtime,template,callback->function,arguments,count,stack,size,sp,&result,error,error_len);
    if(getenv("R2_TRACE_CALLBACKS"))fprintf(stderr,"callback returned foreign=%d status=%d process=%p\n",foreign,status,(void *)template->process);
    *value=result.rax;
    if(status==R2_OK&&native_thread->thread_returning) {r2_set_error(error,error_len,"guest callback attempted pthread_exit");status=R2_ERR_UNSUPPORTED;}

done:
    if(getenv("R2_TRACE_CALLBACKS"))fprintf(stderr,"callback finish foreign=%d status=%d\n",foreign,status);
    if(status!=R2_OK)r2_native_request_stop(runtime,status,error);
    native_thread->x87=saved_x87;native_thread->mxcsr=saved_mxcsr;native_thread->thread_returning=saved_returning;
    fesetenv(&host_environment);runtime->active_callbacks--;engine_leave(runtime);
    if(foreign) {
        native_thread=previous;
        if(!entry->lifetime||!entry->stack) {
            if(entry->lifetime)atomic_store_explicit(&entry->lifetime->disabled,true,memory_order_release);
            callback_thread_cache=entry->next;pthread_setspecific(callback_thread_key,callback_thread_cache);
            destroy_thread_state(entry->state);free(entry);
        }
    }
    atomic_fetch_sub_explicit(&callback->inflight,1,memory_order_release);return status;
}
static void sdl_log_proxy(void *opaque,int category,int priority,const char *message)
{
    native_callback *callback=opaque;uint64_t arguments[]={callback->argument,(uint32_t)category,(uint32_t)priority,(uintptr_t)message},result;
    char error[R2_ERROR_CAP]={0};(void)callback_invoke(callback,arguments,4,&result,error,sizeof(error));
}
static void callbacks_quiesce(r2_native_runtime *runtime)
{
    /* Disable every entry before stopping devices: stop/dispose may itself
     * deliver a final notification on a native thread. */
    for(native_callback *node=atomic_load_explicit(&pinned_callbacks,memory_order_acquire);node;node=node->next)
        if(node->runtime==runtime)atomic_store_explicit(&node->disabled,true,memory_order_release);
    audio_quiesce(runtime);
    objc_callbacks_quiesce(runtime);
    vulkan_callbacks_quiesce(runtime);
    native_callback *callback=runtime->sdl_log_callback;
    if(callback)atomic_store_explicit(&callback->disabled,true,memory_order_release);
    if(runtime->sdl_log_saved&&runtime->graphics[0].library) {
        void (*set)(native_sdl_log,void *)=dlsym(runtime->graphics[0].library,"SDL_SetLogOutputFunction");
        if(set)set((native_sdl_log)runtime->sdl_original_log,runtime->sdl_original_log_argument);
        runtime->sdl_log_saved=false;
    }
    for(native_callback *node=atomic_load_explicit(&pinned_callbacks,memory_order_acquire);node;node=node->next) {
        if(node->runtime!=runtime)continue;
        atomic_store_explicit(&node->disabled,true,memory_order_release);
        while(atomic_load_explicit(&node->inflight,memory_order_acquire)) {struct timespec pause={0,1000000};nanosleep(&pause,NULL);}
    }
}
