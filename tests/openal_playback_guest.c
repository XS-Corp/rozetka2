/* The actual x86 OpenAL Soft driver mixes PCM; CoreAudio pulls it via bridge. */
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static void require(int ok,const char *message) {if(!ok){fprintf(stderr,"FAIL OpenAL: %s\n",message);exit(91);}}
int main(int argc,char **argv) {
    require(argc==2||(argc==3&&strcmp(argv[2],"--loopback")==0),"driver argument");
    int loopback=argc==3;void *library=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);require(library!=NULL,"load x86 OpenAL");
#define FN(result,name,args) result (*name)args=dlsym(library,#name);require(name!=NULL,#name)
    FN(void *,alcOpenDevice,(const char *));FN(int,alcCloseDevice,(void *));
    FN(void *,alcCreateContext,(void *,const int *));FN(int,alcMakeContextCurrent,(void *));FN(void,alcDestroyContext,(void *));
    FN(void,alGenBuffers,(int,unsigned *));FN(void,alDeleteBuffers,(int,const unsigned *));
    FN(void,alBufferData,(unsigned,int,const void *,int,int));FN(void,alGenSources,(int,unsigned *));FN(void,alDeleteSources,(int,const unsigned *));
    FN(void,alSourcei,(unsigned,int,int));FN(void,alSourcePlay,(unsigned));FN(void,alSourceStop,(unsigned));FN(void,alGetSourcei,(unsigned,int,int *));FN(int,alGetError,(void));
    FN(void,alGetSourcef,(unsigned,int,float *));
    FN(void,alcGetIntegerv,(void *,int,int,int *));FN(int,alcIsExtensionPresent,(void *,const char *));
    void *device;
    void (*render)(void *,void *,int)=NULL;
    if(loopback) {
        FN(void *,alcLoopbackOpenDeviceSOFT,(const char *));FN(void,alcRenderSamplesSOFT,(void *,void *,int));
        device=alcLoopbackOpenDeviceSOFT(NULL);render=alcRenderSamplesSOFT;
    } else device=alcOpenDevice(NULL);
    require(device!=NULL,"open output device");
    const int attributes[]={0x1990,0x1501,0x1991,0x1406,0x1007,48000,0x1992,0,0};
    void *context=alcCreateContext(device,loopback?attributes:NULL);require(context!=NULL&&alcMakeContextCurrent(context),"create/make current context");
    if(alcIsExtensionPresent(device,"ALC_SOFT_HRTF")){int hrtf=-1;alcGetIntegerv(device,0x1993,1,&hrtf);printf("OpenAL HRTF status=%d\n",hrtf);}
    short *pcm=malloc(44100*sizeof(short));require(pcm!=NULL,"PCM allocation");
    for(int i=0;i<44100;i++)pcm[i]=(short)(sin(i*(2.0*3.141592653589793*440.0/44100))*800.0);
    unsigned buffer=0,source=0;alGenBuffers(1,&buffer);alBufferData(buffer,0x1101,pcm,44100*sizeof(short),44100);free(pcm);
    alGenSources(1,&source);alSourcei(source,0x1009,(int)buffer);require(alGetError()==0,"upload real PCM and attach source");alSourcePlay(source);
    int offset=0;
    if(loopback) {
        struct timespec start,end;require(clock_gettime(CLOCK_MONOTONIC,&start)==0,"start mixer timer");
        uint64_t hash=UINT64_C(14695981039346656037);float peak=0;
        for(unsigned block=0;block<64;block++) {
            float pcm[520];for(unsigned i=0;i<520;i++)pcm[i]=1234;
            render(device,pcm+4,256);
            for(unsigned i=0;i<4;i++)require(pcm[i]==1234&&pcm[516+i]==1234,"loopback output guards");
            for(unsigned i=4;i<516;i++){require(isfinite(pcm[i]),"finite loopback PCM");if(fabsf(pcm[i])>peak)peak=fabsf(pcm[i]);}
            const unsigned char *bytes=(const unsigned char *)(pcm+4);
            for(unsigned i=0;i<512*sizeof(float);i++){hash^=bytes[i];hash*=UINT64_C(1099511628211);}
        }
        require(clock_gettime(CLOCK_MONOTONIC,&end)==0&&peak>1e-5f&&peak<1,"real non-silent loopback mix");
        double elapsed=end.tv_sec-start.tv_sec+(end.tv_nsec-start.tv_nsec)*1e-9;
        printf("PASS loopback 16384 stereo frames: %.6f seconds, PCM hash=%llx\n",elapsed,(unsigned long long)hash);
        alGetSourcei(source,0x1025,&offset);
    } else for(int i=0;i<30&&offset==0;i++){usleep(20000);alGetSourcei(source,0x1025,&offset);}
    require(alGetError()==0&&offset>0,"CoreAudio render callbacks consumed PCM");
    float seconds = -1; alGetSourcef(source,0x1024,&seconds);
    require(alGetError()==0&&isfinite(seconds)&&seconds>=0&&seconds<=1,"floating point playback offset");
    alSourceStop(source);alDeleteSources(1,&source);alDeleteBuffers(1,&buffer);require(alGetError()==0,"release source/buffer");
    require(alcMakeContextCurrent(NULL),"clear current context");alcDestroyContext(context);require(alcCloseDevice(device),"close output device");
    printf("PASS real x86 OpenAL PCM playback, sample offset=%d and clean shutdown\n",offset);return 0;
}
