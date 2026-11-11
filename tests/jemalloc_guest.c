#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void require(int ok,const char *message) {if(!ok){fprintf(stderr,"FAIL jemalloc guest: %s: %s\n",message,dlerror());exit(91);}}
int main(int argc,char **argv) {
    require(argc==2,"library argument");void *library=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);require(library!=NULL,"load real x86 jemalloc");
    void *(*allocate)(size_t)=(void *)dlsym(library,"je_malloc");void (*release)(void *)=(void *)dlsym(library,"je_free");
    void *(*resize)(void *,size_t)=(void *)dlsym(library,"je_realloc");void *(*zeroed)(size_t,size_t)=(void *)dlsym(library,"je_calloc");
    require(allocate&&release&&resize&&zeroed,"guest jemalloc exports");
    const size_t sizes[]={1,7,32,1000,16384,1234567};
    for(unsigned i=0;i<sizeof(sizes)/sizeof(*sizes);i++) {
        size_t size=sizes[i];unsigned char *pointer=allocate(size);require(pointer!=NULL,"guest allocation");
        memset(pointer,0x5a,size);pointer=resize(pointer,size+32);require(pointer!=NULL,"guest reallocation");
        for(size_t j=0;j<size;j++)require(pointer[j]==0x5a,"reallocation preserves data");release(pointer);
        pointer=zeroed(1,size);require(pointer!=NULL,"guest zero allocation");
        for(size_t j=0;j<size;j++)require(pointer[j]==0,"zeroed allocation");release(pointer);
    }
    dlclose(library);puts("PASS real x86 jemalloc allocation, resize and zeroing");return 0;
}
