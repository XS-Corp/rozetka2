/* ARM-only test backend: invoke the registered SDL logger from a native thread. */
#include <pthread.h>
#include <stdio.h>
#include <string.h>
typedef void (*logger)(void *,int,int,const char *);
static void default_log(void *argument,int category,int priority,const char *message) {(void)argument;(void)category;(void)priority;fprintf(stderr,"native default: %s\n",message);}
static logger function=default_log;static void *user;
void SDL_SetLogOutputFunction(logger value,void *argument) {function=value;user=argument;}
void SDL_GetLogOutputFunction(logger *value,void **argument) {*value=function;*argument=user;}
logger SDL_GetDefaultLogOutputFunction(void) {return default_log;}
struct invocation {logger function;void *user;const char *message;};
static void *invoke(void *opaque) {struct invocation *call=opaque;if(call->function){unsigned count=strcmp(call->message,"repeat")==0?1024:1;for(unsigned i=0;i<count;i++)call->function(call->user,0,3,call->message);}return NULL;}
void SDL_Log(const char *message,...) {
    struct invocation call={function,user,message};pthread_t thread;
    if(pthread_create(&thread,NULL,invoke,&call)==0)pthread_join(thread,NULL);
}
