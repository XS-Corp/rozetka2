/* Vulkan's public 64-bit ABI; this probe needs no host Vulkan SDK headers. */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
typedef struct { uint32_t type; const void *next; const char *name; uint32_t version; const char *engine; uint32_t engine_version, api_version; } app_info;
typedef struct { uint32_t type; const void *next; uint32_t flags; const app_info *app; uint32_t layer_count; const char *const *layers; uint32_t extension_count; const char *const *extensions; } instance_info;
typedef void *(*get_proc)(void *, const char *);
static void require(int ok,const char *message) {if(!ok){fprintf(stderr,"FAIL graphics guest: %s: %s\n",message,dlerror());exit(91);}}
int main(int argc,char **argv) {
    bool windowed=argc>1&&strcmp(argv[1],"sdl")==0;
    void *library=dlopen("libMoltenVK.dylib",RTLD_NOW|RTLD_LOCAL);require(library!=NULL,"open configured GPU driver");
    get_proc procedure=(get_proc)dlsym(library,"vkGetInstanceProcAddr");require(procedure!=NULL,"GPU procedure stub");
    void *sdl=NULL,*window=NULL;
    const char *extensions[16]={"VK_KHR_surface","VK_EXT_metal_surface"};uint32_t extension_count=2;
    if(windowed) {
        sdl=dlopen("libSDL3.dylib",RTLD_NOW|RTLD_LOCAL);require(sdl!=NULL,"open configured SDL3");
        bool (*init)(uint32_t)=(void *)dlsym(sdl,"SDL_Init");
        bool (*load)(const char *)=(void *)dlsym(sdl,"SDL_Vulkan_LoadLibrary");
        void *(*create)(const char *,int,int,uint64_t)=(void *)dlsym(sdl,"SDL_CreateWindow");
        get_proc (*get)(void)=(void *)dlsym(sdl,"SDL_Vulkan_GetVkGetInstanceProcAddr");
        require(init&&load&&create&&get&&init(0x20)&&load(NULL),"SDL3 video and native Vulkan loader");
        window=create("rozetka2 Vulkan bridge test",320,200,UINT64_C(0x10000008));require(window!=NULL,"hidden SDL3 Vulkan window");
        require(get()!=NULL,"SDL returns guest Vulkan procedure stub");
    }
    int32_t (*create_instance)(const instance_info *,const void *,void **)=(void *)procedure(NULL,"vkCreateInstance");
    require(create_instance!=NULL,"instance creation procedure");
    app_info app={.type=0,.name="rozetka2 GPU ABI probe",.engine="rozetka2",.api_version=0x400000};
    instance_info info={.type=1,.flags=0,.app=&app,.extension_count=extension_count,.extensions=extensions};
    void *instance=NULL;
    if(argc>1&&strcmp(argv[1],"callback")==0){create_instance(&info,(void *)0x1234,&instance);return 90;}
    require(create_instance(&info,NULL,&instance)==0&&instance!=NULL,"native Vulkan instance");
    int32_t (*enumerate)(void *,uint32_t *,void **)=(void *)procedure(instance,"vkEnumeratePhysicalDevices");
    void (*properties)(void *,void *)=(void *)procedure(instance,"vkGetPhysicalDeviceProperties");
    void (*destroy)(void *,const void *)=(void *)procedure(instance,"vkDestroyInstance");
    uint32_t count=16;void *devices[16];
    require(enumerate&&properties&&destroy&&enumerate(instance,&count,devices)==0&&count>0,"native GPU enumeration");
    unsigned char device_info[4096]={0};properties(devices[0],device_info);
    require(device_info[20]!=0,"native device properties layout");
    if(windowed) {
        bool (*surface_create)(void *,void *,const void *,uint64_t *)=(void *)dlsym(sdl,"SDL_Vulkan_CreateSurface");
        void (*surface_destroy)(void *,uint64_t,const void *)=(void *)procedure(instance,"vkDestroySurfaceKHR");
        void (*window_destroy)(void *)=(void *)dlsym(sdl,"SDL_DestroyWindow");
        uint64_t surface=0;
        require(surface_create&&surface_destroy&&window_destroy&&surface_create(window,instance,NULL,&surface)&&surface!=0,"SDL3 Metal/Vulkan surface");
        surface_destroy(instance,surface,NULL);window_destroy(window);
        void (*quit)(void)=(void *)dlsym(sdl,"SDL_Quit");require(quit!=NULL,"SDL quit stub");quit();dlclose(sdl);
    }
    printf("PASS guest graphics ABI: GPU=%s%s\n",device_info+20,windowed?", SDL3 surface":"");
    destroy(instance,NULL);dlclose(library);return 0;
}
