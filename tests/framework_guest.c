#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <mach-o/dyld.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <mach/mach.h>
static void require(int ok,const char *message) {if(!ok){fprintf(stderr,"FAIL framework guest: %s\n",message);exit(91);}}
static int scan(const char *text,const char *format,...) {
    va_list arguments;va_start(arguments,format);int count=vsscanf(text,format,arguments);va_end(arguments);return count;
}
int main(int argc,char **argv) {
    CFURLRef url=CFURLCreateWithFileSystemPath(NULL,CFSTR("/System/Library/Frameworks/CoreFoundation.framework"),kCFURLPOSIXPathStyle,1);
    require(url!=NULL,"framework URL");
    UInt8 bundle_path[1024];
    require(CFURLGetFileSystemRepresentation(url,1,bundle_path,sizeof(bundle_path))&&strcmp((const char *)bundle_path,"/System/Library/Frameworks/CoreFoundation.framework")==0,"native CFURL filesystem representation");
    CFBundleRef bundle=CFBundleCreate(NULL,url);CFRelease(url);require(bundle!=NULL,"framework bundle");
    require(CFBundleCopyResourceURL(bundle,CFSTR("rozetka2_missing_resource"),NULL,NULL)==NULL,"missing bundle resource lookup");
    CFBundleRef main_bundle=CFBundleGetMainBundle();
    require(!main_bundle||CFGetRetainCount(main_bundle)>0,"native main bundle query");
    require(CFBundleGetBundleWithIdentifier(CFSTR("com.apple.CoreFoundation"))!=NULL,"bundle identifier");
    if(argc>1&&strcmp(argv[1],"unsupported")==0) {
        CFBundleGetFunctionPointerForName(bundle,CFSTR("CFBundleGetAllBundles"));return 90;
    }
    if(argc>1&&strcmp(argv[1],"audio-callbacks")==0) {
        AURenderCallbackStruct callback={(AURenderCallback)(uintptr_t)0x12345678,NULL};
        AudioUnitSetProperty((AudioUnit)(uintptr_t)1,kAudioUnitProperty_SetRenderCallback,kAudioUnitScope_Input,0,&callback,sizeof(callback));return 90;
    }
    CFIndex (*length)(CFStringRef)=(void *)CFBundleGetFunctionPointerForName(bundle,CFSTR("CFStringGetLength"));
    require(length!=NULL&&length(CFSTR("rozetka2"))==8,"framework function uses guest ABI stub");
    require(CFBundleGetFunctionPointerForName(bundle,CFSTR("rozetka2_missing_function"))==NULL,"optional missing framework symbol");
    CFStringRef string=CFStringCreateWithCStringNoCopy(NULL,"native bridge",kCFStringEncodingUTF8,kCFAllocatorNull);
    require(string!=NULL&&length(string)==13,"no-copy CF string ABI");CFRelease(string);CFRelease(bundle);
    uint32_t count=_dyld_image_count();require(count>0&&_dyld_get_image_name(0)!=NULL&&_dyld_get_image_name(count)==NULL,"guest image enumeration");
    const struct mach_header_64 *header=(const void *)_dyld_get_image_header(0);
    require(header&&header->magic==MH_MAGIC_64&&header->cputype==CPU_TYPE_X86_64&&header->filetype==MH_EXECUTE,"guest x86 image header rather than native ARM header");
    require(_dyld_get_image_header(count)==NULL,"out-of-range image header");
    int integer=0;double number=0;require(scan("42 2.5","%d %lf",&integer,&number)==2&&integer==42&&number==2.5,"x86 va_list to native scanf");
    void *framework=dlopen("libCoreFoundation.dylib",RTLD_NOW|RTLD_LOCAL);
    require(framework!=NULL&&dlsym(framework,"CFStringGetLength")== (void *)length,"named CoreFoundation ABI scope");
    require(dlclose(framework)==0,"close CoreFoundation scope");
    const UniChar text[]={0x0052,0x0416,0xd83d,0xde00,0x0032};
    CFStringRef unicode=CFStringCreateWithCharacters(NULL,text,5);
    require(unicode&&CFStringGetLength(unicode)==5,"native UTF-16 string creation");
    const UniChar *direct_characters=CFStringGetCharactersPtr(unicode);
    require(!direct_characters||memcmp(direct_characters,text,sizeof(text))==0,"optional direct native UTF-16 storage");
    UniChar characters[]={0,0,0,0xeeee};
    CFStringGetCharacters(unicode,CFRangeMake(1,3),characters);
    require(characters[0]==0x0416&&characters[1]==0xd83d&&characters[2]==0xde00&&characters[3]==0xeeee,"CFRange by-value ABI and UTF-16 surrogate pair");
    if(argc>1&&strcmp(argv[1],"callbacks")==0) {
        CFArrayCallBacks callbacks={0}; callbacks.retain=(CFArrayRetainCallBack)(uintptr_t)0x12345678;
        const void *values[]={unicode}; CFArrayCreate(NULL,values,1,&callbacks); return 90;
    }
    const void *values[]={unicode};
    CFArrayRef array=CFArrayCreate(NULL,values,1,&kCFTypeArrayCallBacks);
    require(array&&CFArrayGetCount(array)==1&&CFArrayGetValueAtIndex(array,0)==unicode,"native-owned CFArray callbacks");
    CFRelease(unicode);
    require(CFStringGetLength(CFArrayGetValueAtIndex(array,0))==5,"native callbacks retain array values");
    CFRelease(array);
    CFStringRef type=UTTypeCreatePreferredIdentifierForTag(kUTTagClassFilenameExtension,CFSTR("png"),NULL);
    require(type&&CFEqual(type,CFSTR("public.png")),"file extension to native uniform type identifier");
    CFStringRef mime=UTTypeCopyPreferredTagWithClass(type,kUTTagClassMIMEType);
    require(mime&&CFEqual(mime,CFSTR("image/png")),"native uniform type identifier to MIME type");
    CFRelease(mime);CFRelease(type);
    AudioObjectPropertyAddress address={kAudioHardwarePropertyDefaultOutputDevice,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
    UInt32 size=0;
    require(AudioObjectGetPropertyDataSize(kAudioObjectSystemObject,&address,0,NULL,&size)==0&&size==sizeof(AudioDeviceID),"CoreAudio property size ABI");
    AudioDeviceID device=0;
    require(AudioObjectGetPropertyData(kAudioObjectSystemObject,&address,0,NULL,&size,&device)==0&&size==sizeof(device),"CoreAudio default output device query");
    AudioComponentDescription description={kAudioUnitType_Output,kAudioUnitSubType_GenericOutput,kAudioUnitManufacturer_Apple,0,0};
    AudioComponent component=AudioComponentFindNext(NULL,&description);AudioComponentInstance unit=NULL;
    require(component&&AudioComponentInstanceNew(component,&unit)==0&&unit,"native audio component instance");
    Boolean writable=0;size=0;
    require(AudioUnitGetPropertyInfo(unit,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Input,0,&size,&writable)==0&&size==sizeof(AudioStreamBasicDescription),"audio format property metadata ABI");
    AudioStreamBasicDescription format={0};
    require(AudioUnitGetProperty(unit,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Input,0,&format,&size)==0&&format.mSampleRate>0&&format.mChannelsPerFrame>0,"native audio stream format fields");
    require(AudioUnitInitialize(unit)==0&&AudioUnitUninitialize(unit)==0&&AudioComponentInstanceDispose(unit)==0,"audio component lifecycle");
    void *iokit=dlopen("libIOKit.dylib",RTLD_NOW|RTLD_GLOBAL);
    require(iokit!=NULL,"named IOKit shared-cache scope");
    void *(*matching)(const char *)=dlsym(iokit,"IOServiceMatching");
    unsigned (*service)(unsigned,void *)=dlsym(iokit,"IOServiceGetMatchingService");
    int (*retain)(unsigned)=dlsym(iokit,"IOObjectRetain"),(*release)(unsigned)=dlsym(iokit,"IOObjectRelease");
    int (*name)(unsigned,char *)=dlsym(iokit,"IORegistryEntryGetName");
    require(matching&&service&&retain&&release&&name,"IOKit function stubs");
    unsigned object=service(0,matching("IOPlatformExpertDevice")); char object_name[128]={0};
    require(object!=0&&name(object,object_name)==0&&object_name[0],"read actual IOKit platform object");
    require(dlsym(RTLD_DEFAULT,"IORegistryEntryGetName")==(void *)name,"global IOKit scope");
    require(retain(object)==0&&release(object)==0&&release(object)==0,"owned IOKit send-right references");
    require(release(object)==KERN_INVALID_NAME,"duplicate IOKit release does not consume unrelated host rights");
    require(dlclose(iokit)==0,"close IOKit scope");
    require(dlsym(RTLD_DEFAULT,"IORegistryEntryGetName")== (void *)name,"global native image remains pinned after close");
    puts("PASS guest framework function stubs, image enumeration and vsscanf ABI");return 0;
}
