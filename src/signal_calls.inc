/* Signal state belongs to the interpreted x86 thread. The host's fault and
 * callback machinery must stay able to receive its own native signals. */
_Static_assert(sizeof(sigset_t)==4,"Darwin sigset_t ABI");
static uint32_t *guest_signal_mask(void) {return native_thread->execution.signal_mask?native_thread->execution.signal_mask:&native_thread->signal_mask;}
static uint32_t mask_catchable(uint32_t mask) {return mask&UINT32_C(0x7fffffff)&~((UINT32_C(1)<<(SIGKILL-1))|(UINT32_C(1)<<(SIGSTOP-1)));}
static r2_status signal_call(r2_native_runtime *runtime,native_import *import,uint64_t regs[16],bool *handled,char *error,size_t error_len)
{
    const char *name=import->definition->name;*handled=true;
    if(strcmp(name,"kill")==0&&(int)regs[R2_X86_RSI]!=0) {
        unsigned signum=(unsigned)regs[R2_X86_RSI];pid_t pid=(pid_t)regs[R2_X86_RDI];
        if(signum>=NSIG||(pid!=0&&pid!=getpid())) {errno=signum>=NSIG?EINVAL:EPERM;regs[R2_X86_RAX]=UINT32_MAX;return R2_OK;}
        atomic_fetch_or_explicit(&runtime->pending,UINT32_C(1)<<(signum-1),memory_order_relaxed);regs[R2_X86_RAX]=0;return R2_OK;
    }
    if(strcmp(name,"pthread_sigmask")==0||strcmp(name,"sigprocmask")==0) {
        bool pthread_api=strcmp(name,"pthread_sigmask")==0;uint32_t *current=guest_signal_mask(),value=0;
        const void *input=regs[R2_X86_RSI]?r2_native_pointer(runtime,regs[R2_X86_RSI],4,false,error,error_len):NULL;
        void *output=regs[R2_X86_RDX]?r2_native_pointer(runtime,regs[R2_X86_RDX],4,true,error,error_len):NULL;
        if((regs[R2_X86_RSI]&&!input)||(regs[R2_X86_RDX]&&!output))return R2_ERR_UNSUPPORTED;
        if(input) {
            memcpy(&value,input,4);value=mask_catchable(value);
            if(regs[R2_X86_RDI]!=SIG_BLOCK&&regs[R2_X86_RDI]!=SIG_UNBLOCK&&regs[R2_X86_RDI]!=SIG_SETMASK) {
                if(!pthread_api)errno=EINVAL;regs[R2_X86_RAX]=pthread_api?EINVAL:UINT32_MAX;return R2_OK;
            }
        }
        if(output)memcpy(output,current,4);
        if(input) {
            if(regs[R2_X86_RDI]==SIG_BLOCK)*current|=value;
            else if(regs[R2_X86_RDI]==SIG_UNBLOCK)*current&=~value;
            else *current=value;
        }
        regs[R2_X86_RAX]=0;return R2_OK;
    }
    if(strcmp(name,"pthread_kill")==0) {
        unsigned signum=(unsigned)regs[R2_X86_RSI];native_thread_state *target=NULL;
        if(signum>=NSIG) {regs[R2_X86_RAX]=EINVAL;return R2_OK;}
        if((uintptr_t)runtime->root_thread->host==regs[R2_X86_RDI])target=runtime->root_thread;
        for(size_t i=0;i<runtime->thread_count;i++)if(runtime->threads[i]&&!atomic_load_explicit(&runtime->threads[i]->done,memory_order_acquire)&&(uintptr_t)runtime->threads[i]->host==regs[R2_X86_RDI])target=runtime->threads[i]->state;
        for(size_t i=0;i<64;i++)if(runtime->callback_threads[i]&&(uintptr_t)runtime->callback_threads[i]->host==regs[R2_X86_RDI])target=runtime->callback_threads[i];
        if(target&&signum)atomic_fetch_or_explicit(&target->pending,UINT32_C(1)<<(signum-1),memory_order_relaxed);
        regs[R2_X86_RAX]=target?0:ESRCH;return R2_OK;
    }
    if(strcmp(name,"sigpending")==0) {
        uint32_t *output=r2_native_pointer(runtime,regs[R2_X86_RDI],4,true,error,error_len);if(!output)return R2_ERR_UNSUPPORTED;
        *output=(atomic_load_explicit(&native_thread->pending,memory_order_relaxed)|atomic_load_explicit(&runtime->pending,memory_order_relaxed)|
            (native_thread->execution.pending_signals?*native_thread->execution.pending_signals:0))&*guest_signal_mask();regs[R2_X86_RAX]=0;return R2_OK;
    }
    if(strcmp(name,"__r2_suspend_complete")==0) {
        if(!native_thread->suspend_depth) {r2_set_error(error,error_len,"missing sigsuspend continuation");return R2_ERR_UNSUPPORTED;}
        size_t i=--native_thread->suspend_depth;
        if(regs[R2_X86_RSP]!=native_thread->suspends[i].stack-8) {r2_set_error(error,error_len,"invalid sigsuspend continuation stack");return R2_ERR_UNSUPPORTED;}
        *guest_signal_mask()=native_thread->suspends[i].mask;regs[R2_X86_RSP]=native_thread->suspends[i].stack;
        errno=EINTR;regs[R2_X86_RAX]=UINT32_MAX;return R2_OK;
    }
    if(strcmp(name,"sigsuspend")==0) {
        const uint32_t *input=r2_native_pointer(runtime,regs[R2_X86_RDI],4,false,error,error_len);if(!input)return R2_ERR_UNSUPPORTED;
        if(!native_thread->execution.env||native_thread->suspend_depth==32||regs[R2_X86_RSP]<16) {r2_set_error(error,error_len,"invalid sigsuspend context");return R2_ERR_UNSUPPORTED;}
        uint32_t temporary=mask_catchable(*input);uint64_t complete;
        r2_status status=r2_native_symbol(runtime,"___r2_suspend_complete",&complete,error,error_len);if(status!=R2_OK)return status;
        void *stack=r2_native_pointer(runtime,regs[R2_X86_RSP]-16,16,true,error,error_len);if(!stack)return R2_ERR_UNSUPPORTED;
        size_t slot=native_thread->suspend_depth++;native_thread->suspends[slot].mask=*guest_signal_mask();native_thread->suspends[slot].stack=regs[R2_X86_RSP];*guest_signal_mask()=temporary;
        for(;;) {
            uint32_t pending=(atomic_load_explicit(&native_thread->pending,memory_order_relaxed)|atomic_load_explicit(&runtime->pending,memory_order_relaxed)|*native_thread->execution.pending_signals)&~temporary;
            for(unsigned signum=1;signum<NSIG;signum++) {
                bool ignored=runtime->signal_handlers[signum]==1||(!runtime->signal_handlers[signum]&&(signum==SIGCHLD||signum==SIGURG||signum==SIGCONT||signum==SIGWINCH||signum==SIGINFO));
                if(ignored)pending&=~(UINT32_C(1)<<(signum-1));
            }
            if(pending)break;
            unsigned depth=engine_suspend(runtime);struct timespec pause={0,1000000};nanosleep(&pause,NULL);engine_resume(runtime,depth);
            if(r2_native_stopping(runtime)) {*guest_signal_mask()=native_thread->suspends[slot].mask;native_thread->suspend_depth--;return r2_native_execution_poll(runtime,error,error_len);}
        }
        memcpy(stack,&complete,8);regs[R2_X86_RSP]-=16;regs[R2_X86_RAX]=UINT32_MAX;return R2_OK;
    }
    *handled=false;return R2_OK;
}
