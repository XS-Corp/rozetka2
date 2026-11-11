#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned calls;static int callback_failure;
static void require(int ok,const char *message){if(!ok){fprintf(stderr,"FAIL audio callbacks: %s\n",message);exit(91);}}
static OSStatus render(void *argument,AudioUnitRenderActionFlags *flags,const AudioTimeStamp *time,UInt32 bus,UInt32 frames,AudioBufferList *buffers) {
    require(argument==(void *)42&&flags&&time&&time->mFlags&kAudioTimeStampSampleTimeValid&&bus==0&&frames==128,"render callback arguments");
    require(buffers&&buffers->mNumberBuffers==1&&buffers->mBuffers[0].mNumberChannels==2&&buffers->mBuffers[0].mDataByteSize==128*2*sizeof(float),"native AudioBufferList packing");
    float *data=buffers->mBuffers[0].mData;require(data!=NULL,"render output allocation");
    for(unsigned i=0;i<frames*2;i++)data[i]=((int)i-128)*0.001f;
    calls++;return callback_failure?-123:0;
}
static OSStatus listener(AudioObjectID object,UInt32 count,const AudioObjectPropertyAddress *addresses,void *argument) {
    require(object==kAudioObjectSystemObject&&count&&addresses&&argument==(void *)43,"audio listener arguments");return 0;
}
int main(int argc,char **argv) {
    AudioComponentDescription description={kAudioUnitType_Output,kAudioUnitSubType_GenericOutput,kAudioUnitManufacturer_Apple,0,0};
    AudioComponent component=AudioComponentFindNext(NULL,&description);AudioUnit unit=NULL;require(component&&AudioComponentInstanceNew(component,&unit)==0&&unit,"create generic AudioUnit");
    AURenderCallbackStruct callback={render,(void *)42};
    if(argc>1&&strcmp(argv[1],"invalid")==0)callback.inputProc=(void *)(uintptr_t)1;
    require(AudioUnitSetProperty(unit,kAudioUnitProperty_SetRenderCallback,kAudioUnitScope_Input,0,&callback,sizeof(callback))==0,"register guest render callback");
    AURenderCallbackStruct observed={0};UInt32 size=sizeof(observed);
    OSStatus got=AudioUnitGetProperty(unit,kAudioUnitProperty_SetRenderCallback,kAudioUnitScope_Input,0,&observed,&size);
    require((got==kAudioUnitErr_InvalidProperty&&observed.inputProc==NULL&&observed.inputProcRefCon==NULL)||(got==0&&size==sizeof(observed)&&observed.inputProc==render&&observed.inputProcRefCon==(void *)42),"native write-only property errors or guest callback pointer roundtrip");
    AudioStreamBasicDescription format={44100,kAudioFormatLinearPCM,kAudioFormatFlagIsFloat|kAudioFormatFlagIsPacked,8,1,8,2,32,0};
    require(AudioUnitSetProperty(unit,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Input,0,&format,sizeof(format))==0&&AudioUnitSetProperty(unit,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Output,0,&format,sizeof(format))==0,"configure PCM stream");
    require(AudioUnitInitialize(unit)==0,"initialize unit");
    float data[256]={0};AudioBufferList buffers={1,{{2,sizeof(data),data}}};AudioTimeStamp timestamp={0};timestamp.mFlags=kAudioTimeStampSampleTimeValid;
    AudioUnitRenderActionFlags flags=0;require(AudioUnitRender(unit,&flags,&timestamp,0,128,&buffers)==0&&calls==1,"pull guest PCM callback");
    require(data[0]==-0.128f&&data[128]==0&&data[255]>0.126f&&data[255]<0.128f,"guest PCM reaches native output buffer");
    callback_failure=1;timestamp.mSampleTime=128;require(AudioUnitRender(unit,&flags,&timestamp,0,128,&buffers)==-123&&calls==2,"guest OSStatus propagates");
    AudioObjectPropertyAddress address={kAudioHardwarePropertyDefaultOutputDevice,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
    require(AudioObjectAddPropertyListener(kAudioObjectSystemObject,&address,listener,(void *)43)==0,"register listener");
    require(AudioObjectRemovePropertyListener(kAudioObjectSystemObject,&address,listener,(void *)43)==0,"remove listener with original identity");
    if(argc>1&&strcmp(argv[1],"retire")==0){puts("PASS audio callback retirement");return 0;}
    callback=(AURenderCallbackStruct){0};require(AudioUnitSetProperty(unit,kAudioUnitProperty_SetRenderCallback,kAudioUnitScope_Input,0,&callback,sizeof(callback))==0,"disable callback");
    require(AudioUnitUninitialize(unit)==0&&AudioComponentInstanceDispose(unit)==0,"dispose unit and retire callbacks");
    puts("PASS audio callback arguments, guest function identity, PCM, OSStatus and listener lifecycle");return 0;
}
