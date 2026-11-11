#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {uint32_t type;const void *next;const char *name;uint32_t version;const char *engine;uint32_t engine_version,api;} app_info;
typedef struct {uint32_t type;const void *next;uint32_t flags;const app_info *app;uint32_t layer_count;const char *const *layers;uint32_t extension_count;const char *const *extensions;} instance_info;
typedef struct {uint32_t type;const void *next;uint32_t flags,severity,types;void *function,*argument;} utils_info;
typedef struct {uint32_t type;const void *next;uint32_t flags;void *function,*argument;} report_info;
typedef struct {uint32_t type;const void *next;uint32_t flags;const char *id;int32_t number;const char *message;uint32_t queue_count;const void *queue;uint32_t command_count;const void *commands;uint32_t object_count;const void *objects;} message_data;
static unsigned utils_calls,report_calls;
static void require(int ok,const char *message){if(!ok){fprintf(stderr,"FAIL Vulkan callback: %s\n",message);exit(91);}}
static uint32_t utils_callback(uint32_t severity,uint32_t types,const message_data *data,void *argument) {
    require(argument==(void *)42&&data&&data->message,"debug-utils native argument layout");
    if(strcmp(data->message,"r2-utils")==0) {require(severity==0x1000&&types==1&&data->number==-123&&strcmp(data->id,"R2")==0,"debug-utils message fields");utils_calls++;}
    return 0;
}
static uint32_t report_callback(uint32_t flags,uint32_t object_type,uint64_t object,size_t location,int32_t code,const char *prefix,const char *message,void *argument) {
    require(argument==(void *)43&&prefix&&message,"debug-report stack arguments");
    if(strcmp(message,"r2-report")==0) {require(flags==8&&object_type==2&&object==UINT64_C(0x123456789abcdef0)&&location==987&&code==-123&&strcmp(prefix,"R2")==0,"all eight debug-report arguments");report_calls++;}
    return 0;
}
int main(int argc,char **argv) {
    void *library=dlopen("libMoltenVK.dylib",RTLD_NOW|RTLD_LOCAL);require(library!=NULL,"native driver");
    void *(*proc)(void *,const char *)=dlsym(library,"vkGetInstanceProcAddr");require(proc!=NULL,"native procedure stub");
    int32_t (*create)(const instance_info *,const void *,void **)=proc(NULL,"vkCreateInstance");
    const char *extensions[]={"VK_EXT_debug_utils","VK_EXT_debug_report"};app_info app={.type=0,.name="rozetka2 callbacks",.api=0x400000};
    report_info report={1000011000,NULL,8,(void *)report_callback,(void *)43};
    utils_info utils={1000128004,&report,0,0x1111,7,(void *)utils_callback,(void *)42};
    if(argc>1&&strcmp(argv[1],"invalid")==0)utils.function=(void *)(uintptr_t)1;
    instance_info info={.type=1,.next=&utils,.app=&app,.extension_count=2,.extensions=extensions};void *instance=NULL;
    require(create(&info,NULL,&instance)==0&&instance,"instance with shadow pNext callback chain");
    require(info.next==&utils&&utils.next==&report&&utils.function==(void *)utils_callback&&utils.argument==(void *)42&&report.function==(void *)report_callback,"guest pNext memory is unchanged");
    int32_t (*create_utils)(void *,const utils_info *,const void *,uint64_t *)=proc(instance,"vkCreateDebugUtilsMessengerEXT");
    int32_t (*create_report)(void *,const report_info *,const void *,uint64_t *)=proc(instance,"vkCreateDebugReportCallbackEXT");
    void (*submit_utils)(void *,uint32_t,uint32_t,const message_data *)=proc(instance,"vkSubmitDebugUtilsMessageEXT");
    void (*submit_report)(void *,uint32_t,uint32_t,uint64_t,size_t,int32_t,const char *,const char *)=proc(instance,"vkDebugReportMessageEXT");
    void (*destroy_utils)(void *,uint64_t,const void *)=proc(instance,"vkDestroyDebugUtilsMessengerEXT");
    void (*destroy_report)(void *,uint64_t,const void *)=proc(instance,"vkDestroyDebugReportCallbackEXT");
    void (*destroy)(void *,const void *)=proc(instance,"vkDestroyInstance");require(create_utils&&create_report&&submit_utils&&submit_report&&destroy_utils&&destroy_report&&destroy,"debug extension procedures");
    utils.next=NULL;uint64_t messenger=0,reporter=0;
    require(create_utils(instance,&utils,NULL,&messenger)==0&&messenger&&create_report(instance,&report,NULL,&reporter)==0&&reporter,"register debug callbacks");
    message_data data={.type=1000128003,.id="R2",.number=-123,.message="r2-utils"};submit_utils(instance,0x1000,1,&data);
    submit_report(instance,8,2,UINT64_C(0x123456789abcdef0),987,-123,"R2","r2-report");
    require(utils_calls>0&&report_calls>0,"actual Vulkan delivered both guest callbacks");
    if(argc>1&&strcmp(argv[1],"retire")==0){puts("PASS Vulkan callbacks and runtime retirement");return 0;}
    destroy_utils(instance,messenger,NULL);destroy_report(instance,reporter,NULL);destroy(instance,NULL);require(dlclose(library)==0,"close driver");
    puts("PASS Vulkan debug callbacks, eight arguments, pNext shadows and retirement");return 0;
}
