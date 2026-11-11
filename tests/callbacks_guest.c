#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
typedef void (*log_function)(void *,int,int,const char *);
static void (*log_message)(const char *,...);
static pthread_t main_thread;
static uintptr_t main_top;
static pthread_key_t key;
static unsigned calls,destructors,initializations,tlv_destructors;
static int foreign_mode,failing,persistent;
static pthread_once_t once=PTHREAD_ONCE_INIT;
static __thread uintptr_t callback_top;
static __thread unsigned repeat_calls;
static __thread unsigned char *dynamic_code;
static void require(int ok,const char *message) {if(!ok){fprintf(stderr,"FAIL callback guest: %s\n",message);exit(91);}}
static void initialize(void) {initializations++;}
static void destroy(void *value) {
    require(value==(void *)43&&callback_top==(uintptr_t)pthread_get_stackaddr_np(pthread_self()),"foreign destructor uses the callback guest stack");
    destructors++;
}
static void tlv_destroy(void *argument) {
    require(argument==(void *)44&&(!foreign_mode||callback_top==(uintptr_t)pthread_get_stackaddr_np(pthread_self())),"foreign TLV destructor keeps original guest stack and TLS");tlv_destructors++;
    if(dynamic_code){require(munmap(dynamic_code,(size_t)getpagesize())==0,"release patched callback code");dynamic_code=NULL;}
}
static void callback(void *argument,int category,int priority,const char *message)
{
    char marker;uintptr_t top=(uintptr_t)pthread_get_stackaddr_np(pthread_self());
    size_t size=pthread_get_stacksize_np(pthread_self());
    require(argument==(void *)42&&category==0&&priority==(foreign_mode?3:4),"callback arguments use the guest ABI");
    require((uintptr_t)&marker<top&&(uintptr_t)&marker>=top-size,"callback runs within its advertised guest stack");
    require(foreign_mode?!pthread_equal(pthread_self(),main_thread):(pthread_equal(pthread_self(),main_thread)&&top==main_top),"callback thread and original stack identity");
    if(persistent)require((repeat_calls==0?pthread_getspecific(key)==NULL:pthread_getspecific(key)==(void *)43)&&(!repeat_calls||callback_top==top),"TLS and stack persist across callbacks on one native thread");
    if(persistent) {
        if(!dynamic_code) {
            dynamic_code=mmap(NULL,(size_t)getpagesize(),PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANON,-1,0);
            require(dynamic_code!=MAP_FAILED,"allocate callback x86 code");
            const unsigned char code[]={0xb8,0,0,0,0,0xc3};memcpy(dynamic_code,code,sizeof(code));
        }
        unsigned value=42+repeat_calls;memcpy(dynamic_code+1,&value,4);
        require(((unsigned (*)(void))(uintptr_t)dynamic_code)()==value,"persistent callback cache observes patched x86 instructions");
    }
    callback_top=top;require(pthread_setspecific(key,(void *)43)==0,"callback thread-local destructor registration");
    require(pthread_once(&once,initialize)==0&&initializations==1,"callback pthread_once");
    if(failing)__asm__ volatile("ud2");
    if(!persistent||repeat_calls++==0)_tlv_atexit(tlv_destroy,(void *)44);calls++;
    if(strcmp(message,"outer")==0)log_message("inner");
    else require(strcmp(message,persistent?"repeat":"inner")==0,"callback message text");
}
int main(int argc,char **argv)
{
    persistent=argc>1&&strcmp(argv[1],"persistent")==0;
    failing=argc>1&&strcmp(argv[1],"fail")==0;foreign_mode=persistent||failing||(argc>1&&strcmp(argv[1],"foreign")==0);
    main_thread=pthread_self();main_top=(uintptr_t)pthread_get_stackaddr_np(main_thread);
    require(pthread_key_create(&key,destroy)==0,"callback destructor key");
    void *library=dlopen("libSDL3.dylib",RTLD_NOW|RTLD_LOCAL);require(library!=NULL,"load SDL callback backend");
    void (*set)(log_function,void *)=dlsym(library,"SDL_SetLogOutputFunction");
    void (*get)(log_function *,void **)=dlsym(library,"SDL_GetLogOutputFunction");
    log_function (*get_default)(void)=dlsym(library,"SDL_GetDefaultLogOutputFunction");
    log_message=dlsym(library,"SDL_Log");require(set&&get&&get_default&&log_message,"typed SDL logging procedures");
    log_function original=NULL,observed=NULL;void *argument=NULL,*original_argument=NULL;
    get(&original,&original_argument);require(get_default()!=NULL,"native default logger becomes a guest stub");
    set(callback,(void *)42);get(&observed,&argument);
    require(observed==callback&&argument==(void *)42,"callback getter returns guest function and argument");
    log_message(persistent?"repeat":"outer");
    require(calls==(persistent?1024u:2u)&&initializations==1,"nested/repeated callbacks complete");
    require(destructors==(persistent?1u:foreign_mode?2u:0u)&&tlv_destructors==(persistent?1u:foreign_mode?2u:0u),"foreign callback thread destructors run exactly once");
    set(NULL,NULL);get(&observed,&argument);require(observed==NULL,"disable guest logging");
    set(original,original_argument);pthread_setspecific(key,NULL);pthread_key_delete(key);
    require(dlclose(library)==0,"close callback backend");
    puts("PASS guest SDL callbacks: nested calls, stack identity, native threads, TLS and retirement");return 0;
}
