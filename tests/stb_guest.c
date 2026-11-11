/* Exercise the game's unmodified x86 STB JNI routines on real client assets. */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void require(int ok,const char *message){if(!ok){fprintf(stderr,"FAIL STB: %s\n",message);exit(91);}}
static unsigned char *read_bytes(const char *path,size_t *size){FILE *file=fopen(path,"rb");require(file&&fseek(file,0,SEEK_END)==0,"open game asset");long n=ftell(file);require(n>0&&fseek(file,0,SEEK_SET)==0,"asset size");unsigned char *bytes=malloc((size_t)n);require(bytes&&fread(bytes,1,(size_t)n,file)==(size_t)n,"read asset bytes");fclose(file);*size=(size_t)n;return bytes;}
static void *aligned_allocate(size_t alignment,size_t size){void *result=NULL;return posix_memalign(&result,alignment,size)==0?result:NULL;}
int main(int argc,char **argv)
{
    require(argc==5,"driver, PNG, expected RGBA hash and Vorbis asset");void *library=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);require(library!=NULL,"load official x86 STB library");
#define LOAD(result,name,args) result (*name)args=dlsym(library,#name);require(name!=NULL,#name)
    LOAD(void,Java_org_lwjgl_stb_LibSTB_setupMalloc,(void *,void *,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t));
    LOAD(uintptr_t,Java_org_lwjgl_stb_STBImage_nstbi_1load_1from_1memory__JIJJJI,(void *,void *,uintptr_t,int,uintptr_t,uintptr_t,uintptr_t,int));
    LOAD(void,Java_org_lwjgl_stb_STBImage_nstbi_1image_1free,(void *,void *,uintptr_t));
    LOAD(int,Java_org_lwjgl_stb_STBVorbis_nstb_1vorbis_1decode_1memory__JIJJJ,(void *,void *,uintptr_t,int,uintptr_t,uintptr_t,uintptr_t));
    Java_org_lwjgl_stb_LibSTB_setupMalloc(NULL,NULL,(uintptr_t)malloc,(uintptr_t)calloc,(uintptr_t)realloc,(uintptr_t)free,(uintptr_t)aligned_allocate,(uintptr_t)free);
    size_t size;unsigned char *bytes=read_bytes(argv[2],&size);int width=0,height=0,channels=0;
    unsigned char *pixels=(void *)Java_org_lwjgl_stb_STBImage_nstbi_1load_1from_1memory__JIJJJI(NULL,NULL,(uintptr_t)bytes,(int)size,(uintptr_t)&width,(uintptr_t)&height,(uintptr_t)&channels,4);free(bytes);
    require(pixels&&width==16&&height==16&&channels==1,"decode actual stone grayscale PNG to RGBA");
    uint64_t hash=UINT64_C(14695981039346656037);for(unsigned i=0;i<16*16*4;i++)hash=(hash^pixels[i])*UINT64_C(1099511628211);
    require(hash==strtoull(argv[3],NULL,16),"all decoded texture pixels match independent PNG reference");Java_org_lwjgl_stb_STBImage_nstbi_1image_1free(NULL,NULL,(uintptr_t)pixels);puts("PASS real x86 STB: all stone texture RGBA pixels match");fflush(stdout);
    bytes=read_bytes(argv[4],&size);short *pcm=NULL;int rate=0;channels=0;
    int samples=Java_org_lwjgl_stb_STBVorbis_nstb_1vorbis_1decode_1memory__JIJJJ(NULL,NULL,(uintptr_t)bytes,(int)size,(uintptr_t)&channels,(uintptr_t)&rate,(uintptr_t)&pcm);free(bytes);
    require(samples>0&&pcm&&channels>=1&&channels<=2&&rate>=8000&&rate<=192000,"decode game Ogg/Vorbis sample and metadata");uint64_t energy=0;for(size_t i=0;i<(size_t)samples*channels;i++)energy+=(int64_t)pcm[i]*pcm[i];require(energy>0,"non-silent decoded game PCM");free(pcm);
    printf("PASS real x86 STB Vorbis: %d samples, %d channels, %d Hz, energy=%llu\n",samples,channels,rate,(unsigned long long)energy);require(dlclose(library)==0,"STB shutdown");return 0;
}
