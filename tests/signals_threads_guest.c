#include <signal.h>
#include <setjmp.h>
#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/ucontext.h>
static pthread_t target;static volatile unsigned calls,ready;static sigjmp_buf jump;static int jumping;
static void require(int ok,const char *message){if(!ok){fprintf(stderr,"FAIL thread signals: %s\n",message);exit(91);}}
static void handler(int signum,siginfo_t *info,void *opaque) {
    require(signum==SIGUSR1&&info&&info->si_signo==SIGUSR1&&pthread_equal(pthread_self(),target),"virtual signal enters target x86 pthread");
    sigset_t mask;require(pthread_sigmask(SIG_SETMASK,NULL,&mask)==0&&sigismember(&mask,SIGUSR1),"handler automatically blocks its signal");
    ucontext_t *context=opaque;require(context&&context->uc_mcontext&&context->uc_mcontext->__ss.__rip,"thread signal x86 ucontext");
    __sync_fetch_and_add(&calls,1);if(jumping){jumping=0;siglongjmp(jump,17);}
}
static void *worker(void *unused) {
    (void)unused;sigset_t mask,empty;sigemptyset(&empty);
    require(pthread_sigmask(SIG_SETMASK,NULL,&mask)==0&&sigismember(&mask,SIGUSR1),"pthread inherits virtual signal mask");
    __atomic_store_n(&ready,1,__ATOMIC_RELEASE);errno=0;
    require(sigsuspend(&empty)==-1&&errno==EINTR,"sigsuspend returns after handler with EINTR");
    require(pthread_sigmask(SIG_SETMASK,NULL,&mask)==0&&sigismember(&mask,SIGUSR1),"sigsuspend restores original blocked mask");
    require(sigpending(&mask)==0&&!sigismember(&mask,SIGUSR1),"delivered thread signal leaves pending set");return (void *)43;
}
int main(void) {
    target=pthread_self();struct sigaction action={0};action.sa_sigaction=handler;action.sa_flags=SA_SIGINFO;
    require(sigaction(SIGUSR1,&action,NULL)==0,"install x86 signal handler");
    sigset_t blocked,empty,mask;sigemptyset(&blocked);sigaddset(&blocked,SIGUSR1);sigemptyset(&empty);
    require(sigprocmask(SIG_BLOCK,&blocked,NULL)==0&&pthread_kill(target,0)==0&&pthread_kill(target,SIGUSR1)==0&&calls==0,"blocked pthread signal is deferred");
    require(sigpending(&mask)==0&&sigismember(&mask,SIGUSR1),"blocked signal is pending");
    require(sigprocmask(SIG_UNBLOCK,&blocked,NULL)==0&&calls==1,"unblocking delivers pending signal");
    require(kill(getpid(),SIGUSR1)==0&&calls==2,"process signal remains within guest runtime");
    require(pthread_sigmask(SIG_BLOCK,&blocked,NULL)==0,"block signal before pthread creation");
    pthread_t thread;require(pthread_create(&thread,NULL,worker,NULL)==0,"create suspended signal thread");target=thread;
    while(!__atomic_load_n(&ready,__ATOMIC_ACQUIRE))usleep(1000);
    require(pthread_kill(thread,SIGWINCH)==0,"ignored signal does not end sigsuspend");usleep(10000);
    require(calls==2&&pthread_kill(thread,SIGUSR1)==0,"send wakeup signal to suspended thread");
    void *value=NULL;require(pthread_join(thread,&value)==0&&value==(void *)43&&calls==3,"thread handler, wait and join complete");target=pthread_self();
    require(pthread_sigmask(SIG_SETMASK,NULL,&mask)==0&&sigismember(&mask,SIGUSR1),"worker leaves parent mask untouched");
    int returned=sigsetjmp(jump,1);
    if(returned==0) {jumping=1;require(sigprocmask(SIG_UNBLOCK,&blocked,NULL)==0,"unblock before signal jump");pthread_kill(target,SIGUSR1);require(0,"signal handler did not jump");}
    require(returned==17&&calls==4,"siglongjmp leaves signal frame");
    require(sigprocmask(SIG_SETMASK,NULL,&mask)==0&&sigismember(&mask,SIGUSR1),"siglongjmp restores saved virtual mask");
    require(pthread_kill(target,SIGUSR1)==0&&calls==4&&sigprocmask(SIG_UNBLOCK,&blocked,NULL)==0&&calls==5,"signal delivery after siglongjmp has no stale frame");
    sigjmp_buf unsaved;returned=sigsetjmp(unsaved,0);
    if(returned==0){require(sigprocmask(SIG_BLOCK,&blocked,NULL)==0,"block before unsaved jump");siglongjmp(unsaved,19);}
    require(returned==19&&sigprocmask(SIG_SETMASK,NULL,&mask)==0&&sigismember(&mask,SIGUSR1),"sigsetjmp savesigs=0 leaves current mask");
    sigfillset(&mask);require(sigprocmask(SIG_SETMASK,&mask,NULL)==0&&sigprocmask(SIG_SETMASK,NULL,&mask)==0&&!sigismember(&mask,SIGKILL)&&!sigismember(&mask,SIGSTOP),"uncatchable signals cannot be blocked");
    mask=0x1234;errno=0;require(sigprocmask(777,&blocked,&mask)==-1&&errno==EINVAL&&mask==0x1234,"invalid sigprocmask preserves output");
    errno=0;require(pthread_sigmask(777,&blocked,NULL)==EINVAL&&errno==0&&pthread_kill(target,32)==EINVAL&&pthread_kill(target,99)==EINVAL&&pthread_kill((pthread_t)(uintptr_t)1,0)==ESRCH,"pthread error conventions and unknown thread");
    require(sigprocmask(SIG_SETMASK,&empty,NULL)==0,"restore empty virtual mask");puts("PASS virtual pthread signals, masks, pending, suspend and signal longjmp");return 0;
}
