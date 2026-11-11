#include <pthread.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <poll.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <unistd.h>
#include <mach-o/dyld.h>

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static pthread_key_t key;
static __thread unsigned tls_value = 123;
static unsigned ready, counter, destructors;
static unsigned tlv_destructors;
static __thread unsigned tlv_order;

static int fail, once_fail;
static pthread_once_t once = PTHREAD_ONCE_INIT, nested_once = PTHREAD_ONCE_INIT;
static unsigned once_runs, nested_runs, once_attempts;
static pthread_once_t exit_once = PTHREAD_ONCE_INIT;
static unsigned exit_once_runs;
static void require(int condition, const char *message) {
    if (!condition) { fprintf(stderr, "FAIL thread guest: %s\n", message); exit(91); }
}
static void tlv_destroy(void *argument) {
    require(tlv_order==(uintptr_t)argument,"thread-local destructors run in reverse order");
    tlv_order--;__sync_fetch_and_add(&tlv_destructors,1);
}
static void destroy(void *value) {
    require(value == (void *)42, "destructor argument");
    require(tls_value == 456, "destructor TLS");
    pthread_mutex_lock(&mutex); destructors++; pthread_mutex_unlock(&mutex);
}
static void initialize_nested(void) { nested_runs++; }
static void initialize_once(void) {
    volatile int local = 42;
    uintptr_t top = (uintptr_t)pthread_get_stackaddr_np(pthread_self());
    require((uintptr_t)&local < top && (uintptr_t)&local >= top - pthread_get_stacksize_np(pthread_self()), "once initializer uses the original thread stack");
    require(pthread_once(&nested_once, initialize_nested) == 0 && nested_runs == 1, "nested once initialization");
    /* Let another guest thread enter the same control while this one runs. */
    while (__atomic_load_n(&once_attempts, __ATOMIC_ACQUIRE) != 2) usleep(1000);
    if (once_fail) __asm__ volatile("ud2");
    usleep(10000); once_runs++;
}
static void initialize_exit(void) {
    if (exit_once_runs++ == 0) pthread_exit((void *)49);
}
static void *once_exit_worker(void *unused) {
    (void)unused;
    require(pthread_once(&exit_once, initialize_exit) == 0, "retry once after initializer exits");
    return (void *)50;
}
static void *worker(void *arg) {
    require(tls_value == 123, "fresh thread TLS template"); tls_value = 456;
    tlv_order=2;_tlv_atexit(tlv_destroy,(void *)1);_tlv_atexit(tlv_destroy,(void *)2);
    require(pthread_getspecific(key) == NULL, "fresh pthread key");
    require(pthread_setspecific(key, (void *)42) == 0, "set key");
    require(pthread_get_stacksize_np(pthread_self()) >= 512 * 1024, "guest stack size");
    volatile int local = 0;
    uintptr_t top = (uintptr_t)pthread_get_stackaddr_np(pthread_self());
    require((uintptr_t)&local < top && (uintptr_t)&local >= top - pthread_get_stacksize_np(pthread_self()), "guest stack bounds");
    pthread_mutex_lock(&mutex); ready++; pthread_cond_broadcast(&condition);
    while (counter == 0) pthread_cond_wait(&condition, &mutex);
    if (fail && (uintptr_t)arg == 2) while (1) pthread_cond_wait(&condition, &mutex);
    pthread_mutex_unlock(&mutex);
    if (!fail) {
        __sync_fetch_and_add(&once_attempts, 1);
        require(pthread_once(&once, initialize_once) == 0 && once_runs == 1, "concurrent pthread_once waits for completion");
    }
    for (unsigned i = 0; i < 100; i++) {
        pthread_mutex_lock(&mutex); counter++; pthread_mutex_unlock(&mutex);
    }
    if (fail) { pthread_mutex_lock(&mutex); __asm__ volatile("ud2"); }
    if ((uintptr_t)arg == 1) pthread_exit((void *)43);
    return (void *)44;
}
static int event_pipe[2];
static void *kernel_waiter(void *argument) {
    int kind = (int)(uintptr_t)argument, queue = kind ? kqueue() : -1;
    pthread_mutex_lock(&mutex); ready = 1; pthread_cond_broadcast(&condition); pthread_mutex_unlock(&mutex);
    if (!kind) {
        struct pollfd descriptor = {event_pipe[0], POLLIN, 0};
        require(poll(&descriptor, 1, -1) == 1 && (descriptor.revents & POLLIN), "guest poll wakes on data");
    } else {
        struct kevent change, output;
        EV_SET(&change, event_pipe[0], EVFILT_READ, EV_ADD, 0, 0, NULL);
        require(kevent(queue, &change, 1, &output, 1, NULL) == 1 && output.ident == (uintptr_t)event_pipe[0], "guest kevent wakes on data");
        close(queue);
    }
    char byte; require(read(event_pipe[0], &byte, 1) == 1 && byte == 'x', "guest event read");
    return (void *)47;
}
static void *short_worker(void *argument) { return (void *)((uintptr_t)argument + 1); }
static void resolver_check(int double_free) {
    struct addrinfo hints = {0}, *result = NULL;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM; hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    require(getaddrinfo("127.0.0.1", "25565", &hints, &result) == 0 && result != NULL, "numeric native resolver");
    require(result->ai_family == AF_INET && result->ai_addrlen == sizeof(struct sockaddr_in), "Darwin addrinfo layout");
    struct sockaddr_in *address = (struct sockaddr_in *)result->ai_addr;
    require(address->sin_family == AF_INET && address->sin_port == htons(25565), "native resolver address");
    char host[64], service[16];
    require(getnameinfo(result->ai_addr, result->ai_addrlen, host, sizeof(host), service, sizeof(service), NI_NUMERICHOST | NI_NUMERICSERV) == 0, "native reverse numeric resolver");
    require(strcmp(host, "127.0.0.1") == 0 && strcmp(service, "25565") == 0, "numeric resolver content");
    freeaddrinfo(result);
    if (double_free) freeaddrinfo(result);
}
static void socketpair_check(void) {
    int sockets[2]={-1,-1};char buffer[16]={0};
    require(socketpair(AF_UNIX,SOCK_STREAM,0,sockets)==0&&sockets[0]>=0&&sockets[1]>=0&&sockets[0]!=sockets[1],"native socket pair descriptors");
    require(write(sockets[0],"to-right",8)==8,"write socket pair forward");
    struct pollfd event={sockets[1],POLLIN,0};
    require(poll(&event,1,1000)==1&&(event.revents&POLLIN),"socket pair wakes native poll");
    require(read(sockets[1],buffer,8)==8&&memcmp(buffer,"to-right",8)==0,"read socket pair forward");
    require(write(sockets[1],"to-left",7)==7&&read(sockets[0],buffer,7)==7&&memcmp(buffer,"to-left",7)==0,"socket pair is bidirectional");
    require(shutdown(sockets[0],SHUT_WR)==0&&read(sockets[1],buffer,1)==0,"socket pair half-close produces EOF");
    require(close(sockets[0])==0&&close(sockets[1])==0,"close socket pair descriptors");
    sockets[0]=123;sockets[1]=456;errno=0;
    require(socketpair(-1,SOCK_STREAM,0,sockets)==-1&&errno==EAFNOSUPPORT&&sockets[0]==123&&sockets[1]==456,"socket pair failure preserves errno and output");
}
extern int __ulock_wait(uint32_t,void *,uint64_t,uint32_t);
extern int __ulock_wake(uint32_t,void *,uint64_t);
static uint32_t ulock_word,ulock_ready;
static void *ulock_worker(void *unused) {
    (void)unused;__atomic_store_n(&ulock_ready,1,__ATOMIC_RELEASE);
    while(__atomic_load_n(&ulock_word,__ATOMIC_ACQUIRE)==0) {
        errno=123;int result=__ulock_wait(0x01000001,&ulock_word,0,0);
        require(result>=0||result==-EALREADY||result==-EINTR,"native futex wait/wake result");
        require(errno==123,"NO_ERRNO wait preserves errno");
    }
    return (void *)52;
}
static void ulock_check(void) {
    errno=123;require(__ulock_wait(0x01000001,&ulock_word,0,10000)==-ETIMEDOUT&&errno==123,"NO_ERRNO timeout");
    errno=0;require(__ulock_wait(1,&ulock_word,0,10000)==-1&&errno==ETIMEDOUT,"normal errno timeout");
    pthread_t thread;require(pthread_create(&thread,NULL,ulock_worker,NULL)==0,"create futex waiter");
    while(!__atomic_load_n(&ulock_ready,__ATOMIC_ACQUIRE))usleep(1000);usleep(30000);
    __atomic_store_n(&ulock_word,1,__ATOMIC_RELEASE);errno=123;
    int result=__ulock_wake(0x01000101,&ulock_word,0);require(result>=0||result==-ENOENT,"native futex wake all");
    require(errno==123,"NO_ERRNO wake preserves errno");void *value=NULL;
    require(pthread_join(thread,&value)==0&&value==(void *)52,"futex wait releases guest engine");
}

int main(int argc, char **argv) {
    if(argc>1&&strcmp(argv[1],"socketpair-fail")==0) {socketpair(AF_UNIX,SOCK_STREAM,0,(int *)(uintptr_t)1);return 90;}
    if (argc > 1 && strcmp(argv[1], "resolver-fail") == 0) { resolver_check(1); return 90; }
    resolver_check(0);
    socketpair_check();
    ulock_check();
    fail = argc > 1 && strcmp(argv[1], "fail") == 0;
    once_fail = argc > 1 && strcmp(argv[1], "once-fail") == 0;
    require(pthread_key_create(&key, destroy) == 0, "create key");
    pthread_t a, b;
    require(pthread_create(&a, NULL, worker, (void *)1) == 0, "create a");
    require(pthread_create(&b, NULL, worker, (void *)2) == 0, "create b");
    pthread_mutex_lock(&mutex);
    while (ready != 2) pthread_cond_wait(&condition, &mutex);
    require(tls_value == 123 && pthread_getspecific(key) == NULL, "root TLS unchanged");
    counter = 1; pthread_cond_broadcast(&condition); pthread_mutex_unlock(&mutex);
    void *ra, *rb;
    require(pthread_join(a, &ra) == 0, "join a");
    require(pthread_join(b, &rb) == 0, "join b");
    require(ra == (void *)43 && rb == (void *)44, "return and pthread_exit values");
    require(counter == 201 && destructors == 2 && tlv_destructors == 4, "synchronization and destructors");
    require(pthread_once(&once, initialize_once) == 0 && once_runs == 1 && nested_runs == 1, "completed once does not repeat");
    once = (pthread_once_t)PTHREAD_ONCE_INIT;
    require(pthread_once(&once, initialize_once) == 0 && once_runs == 2 && nested_runs == 1, "reinitialized once storage");
    require(pthread_create(&a, NULL, once_exit_worker, NULL) == 0 && pthread_join(a, &ra) == 0 && ra == (void *)49,
            "initializer pthread_exit propagates through once");
    require(pthread_create(&a, NULL, once_exit_worker, NULL) == 0 && pthread_join(a, &ra) == 0 && ra == (void *)50 && exit_once_runs == 2,
            "initializer exit resets once state for another thread");
    pthread_key_delete(key);
    for (uintptr_t i = 0; i < 300; i++) {
        require(pthread_create(&a, NULL, short_worker, (void *)i) == 0, "reused thread record");
        require(pthread_join(a, &ra) == 0 && ra == (void *)(i + 1), "reused native handle");
    }
    pthread_mutex_lock(&mutex);
    struct timespec deadline; clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += 10000000;
    if (deadline.tv_nsec >= 1000000000) { deadline.tv_nsec -= 1000000000; deadline.tv_sec++; }
    require(pthread_cond_timedwait(&condition, &mutex, &deadline) == ETIMEDOUT, "condition deadline");
    require(pthread_mutex_unlock(&mutex) == 0, "timeout reacquires mutex");
    for (uintptr_t kind = 0; kind < 2; kind++) {
        require(pipe(event_pipe) == 0, "create event pipe");
        ready = 0;
        require(pthread_create(&a, NULL, kernel_waiter, (void *)kind) == 0, "create kernel waiter");
        pthread_mutex_lock(&mutex);
        while (!ready) pthread_cond_wait(&condition, &mutex);
        pthread_mutex_unlock(&mutex);
        require(write(event_pipe[1], "x", 1) == 1, "signal kernel waiter");
        require(pthread_join(a, &ra) == 0 && ra == (void *)47, "join kernel waiter");
        close(event_pipe[0]); close(event_pipe[1]);
    }
    puts("PASS native guest pthreads: condition, mutex, join, exit, TLS and destructors");
    return 0;
}
