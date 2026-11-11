/* Compile the actual game's GLSL with its official x86 Shaderc library. */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void require(int ok,const char *message){if(!ok){fprintf(stderr,"FAIL Minecraft shaders: %s\n",message);exit(91);}}
int main(int argc,char **argv) {
    require(argc>=3,"compiler and shader sources");void *library=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);require(library!=NULL,"load actual x86 Shaderc");
#define FN(result,name,args) result (*name)args=dlsym(library,#name);require(name!=NULL,#name)
    FN(void *,shaderc_compiler_initialize,(void));FN(void,shaderc_compiler_release,(void *));
    FN(void *,shaderc_compile_options_initialize,(void));FN(void,shaderc_compile_options_release,(void *));
    FN(void,shaderc_compile_options_set_auto_bind_uniforms,(void *,int));FN(void,shaderc_compile_options_set_auto_map_locations,(void *,int));
    FN(void *,shaderc_compile_into_spv,(void *,const char *,size_t,int,const char *,const char *,const void *));
    FN(int,shaderc_result_get_compilation_status,(void *));FN(size_t,shaderc_result_get_length,(void *));FN(const char *,shaderc_result_get_bytes,(void *));
    FN(const char *,shaderc_result_get_error_message,(void *));FN(void,shaderc_result_release,(void *));
    void *compiler=shaderc_compiler_initialize(),*options=shaderc_compile_options_initialize();require(compiler&&options,"initialize shader compiler/options");
    shaderc_compile_options_set_auto_bind_uniforms(options,1);shaderc_compile_options_set_auto_map_locations(options,1);
    for(int i=2;i<argc;i++) {
        FILE *file=fopen(argv[i],"rb");require(file&&fseek(file,0,SEEK_END)==0,"open shader source");long size=ftell(file);require(size>0&&fseek(file,0,SEEK_SET)==0,"shader size");
        char *source=malloc((size_t)size+1);require(source&&fread(source,1,(size_t)size,file)==(size_t)size,"read source");source[size]=0;fclose(file);
        int fragment=strstr(argv[i],".fsh")!=NULL;
        void *result=shaderc_compile_into_spv(compiler,source,(size_t)size,fragment?1:0,argv[i],"main",options);free(source);require(result!=NULL,"compile result");
        if(shaderc_result_get_compilation_status(result)!=0)fprintf(stderr,"%s: %s\n",argv[i],shaderc_result_get_error_message(result));
        require(shaderc_result_get_compilation_status(result)==0,"compile actual Minecraft shader");
        size_t length=shaderc_result_get_length(result);const char *bytes=shaderc_result_get_bytes(result);uint32_t magic=0;if(length>=4)memcpy(&magic,bytes,4);
        require(length>=20&&length%4==0&&magic==0x07230203,"valid SPIR-V output");
        char path[4096];require(strlen(argv[i])+5<sizeof(path),"output path");snprintf(path,sizeof(path),"%s.spv",argv[i]);file=fopen(path,"wb");
        require(file&&fwrite(bytes,1,length,file)==length&&fclose(file)==0,"persist compiled game shader");shaderc_result_release(result);
        printf("PASS Minecraft shader %s: %zu SPIR-V bytes\n",argv[i],length);fflush(stdout);
    }
    shaderc_compile_options_release(options);shaderc_compiler_release(compiler);require(dlclose(library)==0,"compiler shutdown");return 0;
}
