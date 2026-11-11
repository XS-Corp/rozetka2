/* Native bundle entry point; Python prepares argv, then execs the translator. */
#include <errno.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int main(void)
{
    char executable[PATH_MAX], path[PATH_MAX];
    uint32_t capacity=sizeof(executable);
    if(_NSGetExecutablePath(executable,&capacity)!=0)return 1;
    char *slash=strrchr(executable,'/');if(!slash)return 1;*slash=0;
    if(snprintf(path,sizeof(path),"%s/../Resources/launch.bin",executable)>=(int)sizeof(path))return 1;
    FILE *file=fopen(path,"rb");if(!file){perror("Minecraft launch configuration");return 1;}
    if(fseek(file,0,SEEK_END)!=0){fclose(file);return 1;}
    long length=ftell(file);
    if(length<=0||length>65536||fseek(file,0,SEEK_SET)!=0){fclose(file);return 1;}
    char *data=malloc((size_t)length);if(!data){fclose(file);return 1;}
    if(fread(data,1,(size_t)length,file)!=(size_t)length){fclose(file);free(data);return 1;}
    fclose(file);
    char *fields[64];size_t count=0,position=0;
    while(position<(size_t)length&&count<63) {
        char *end=memchr(data+position,0,(size_t)length-position);
        if(!end||end==data+position){free(data);return 1;}
        fields[count++]=data+position;position=(size_t)(end-data)+1;
    }
    if(position!=(size_t)length||count<3){free(data);return 1;}fields[count]=NULL;
    const char *log=fields[0];
    if(access(log,F_OK)==0) {
        time_t now=time(NULL);struct tm local;char stamp[32];
        if(!localtime_r(&now,&local)||!strftime(stamp,sizeof(stamp),"%Y%m%d-%H%M%S",&local))return 1;
        if(snprintf(path,sizeof(path),"%s.%s-%d",log,stamp,(int)getpid())>=(int)sizeof(path))return 1;
        if(rename(log,path)!=0){perror("Preserve Minecraft log");return 1;}
    } else if(errno!=ENOENT){perror("Minecraft log");return 1;}
    if(!freopen(log,"w",stdout)||dup2(STDOUT_FILENO,STDERR_FILENO)<0)return 1;
    setenv("R2_TRACE_LOADER","1",1);setenv("R2_TRACE_OBJC","1",1);setenv("R2_TRACE_COMPILATION","1",1);
    execv(fields[1],fields+1);perror("Launch Minecraft translator");return 1;
}
