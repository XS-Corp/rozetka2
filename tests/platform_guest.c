#include <CoreFoundation/CoreFoundation.h>
#include <CoreText/CoreText.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CFNetwork/CFNetwork.h>
#include <copyfile.h>
#include <fcntl.h>
#include <math.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>
static void require(int ok,const char *message){if(!ok){fprintf(stderr,"FAIL platform: %s\n",message);exit(91);}}
static unsigned path_elements,provider_releases;
static void path_element(void *argument,const CGPathElement *element) {
    require(argument==(void *)42&&element&&element->type>=kCGPathElementMoveToPoint&&element->type<=kCGPathElementCloseSubpath,"CGPath callback element layout");
    unsigned count=element->type==kCGPathElementCloseSubpath?0:element->type==kCGPathElementAddQuadCurveToPoint?2:element->type==kCGPathElementAddCurveToPoint?3:1;
    for(unsigned i=0;i<count;i++)require(isfinite(element->points[i].x)&&isfinite(element->points[i].y),"native path coordinates");path_elements++;
}
static void provider_release(void *argument,const void *data,size_t size) {
    require(argument==(void *)43&&size==8&&memcmp(data,"rozetka2",8)==0,"provider release receives original guest pointer and size");provider_releases++;free((void *)data);
}
int main(int argc,char **argv) {
    if(argc>1) {
        if(strcmp(argv[1],"array")==0){CFArrayCallBacks cb={0};cb.release=(void *)(uintptr_t)1;CFArrayCreateMutable(NULL,0,&cb);}
        else {CFDictionaryKeyCallBacks cb={0};cb.hash=(void *)(uintptr_t)1;
            if(strcmp(argv[1],"dictionary")==0)CFDictionaryCreate(NULL,NULL,NULL,0,&cb,NULL);
            else CFDictionaryCreateMutable(NULL,0,&cb,NULL);}
        return 90;
    }
    const UInt8 utf8[]={0x41,0xd0,0x96,0xf0,0x9f,0x98,0x80,0x5a};
    CFStringRef text=CFStringCreateWithBytes(NULL,utf8,sizeof(utf8),kCFStringEncodingUTF8,0);require(text&&CFStringGetLength(text)==5,"UTF-8 input");
    UInt8 bytes[16]={0};CFIndex used=0;
    require(CFStringGetBytes(text,CFRangeMake(1,3),kCFStringEncodingUTF8,0,0,bytes,sizeof(bytes),&used)==3&&used==6&&!memcmp(bytes,utf8+1,6),"nine-word CFRange/stack signature");
    require(CFStringHasPrefix(text,CFSTR("A"))&&CFStringHasSuffix(text,CFSTR("Z"))&&CFStringCompare(text,text,0)==0,"CF string comparisons");
    CFMutableArrayRef array=CFArrayCreateMutable(NULL,0,&kCFTypeArrayCallBacks);CFArrayAppendValue(array,text);
    const void *values[]={text};CFArrayRef source=CFArrayCreate(NULL,values,1,&kCFTypeArrayCallBacks);CFArrayAppendArray(array,source,CFRangeMake(0,1));
    require(CFArrayGetCount(array)==2&&CFArrayGetValueAtIndex(array,1)==text,"mutable array and range append");
    CFMutableDictionaryRef dictionary=CFDictionaryCreateMutable(NULL,0,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(dictionary,CFSTR("key"),text);
    const void *keys[]={CFSTR("key")};CFDictionaryRef copy=CFDictionaryCreate(NULL,keys,values,1,&kCFCopyStringDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    CFRelease(text);require(CFEqual(CFDictionaryGetValue(dictionary,keys[0]),CFDictionaryGetValue(copy,keys[0])),"native dictionary callbacks retain values");
    CFRelease(copy);CFRelease(dictionary);CFRelease(source);CFRelease(array);
    CGAffineTransform input={2,3,5,7,11,13};
    CGAffineTransform (*scale)(CGAffineTransform,CGFloat,CGFloat)=dlsym(RTLD_DEFAULT,"CGAffineTransformScale");require(scale!=NULL,"transform procedure");
    CGAffineTransform scaled=scale(input,17,19);require(scaled.a==34&&scaled.b==51&&scaled.c==95&&scaled.d==133&&scaled.tx==11&&scaled.ty==13,"x86 memory aggregate input and sret output");
    char *provider_bytes=strdup("rozetka2");CGDataProviderRef provider=CGDataProviderCreateWithData((void *)43,provider_bytes,8,provider_release);
    require(provider!=NULL,"owned native font-data provider");CFDataRef provider_copy=CGDataProviderCopyData(provider);
    require(provider_copy&&CFDataGetLength(provider_copy)==8&&!memcmp(CFDataGetBytePtr(provider_copy),"rozetka2",8),"native provider copied guest bytes");
    CGDataProviderRelease(provider);CFRelease(provider_copy);require(provider_releases==1,"provider guest release callback exactly once");
    CFMutableAttributedStringRef attributed=CFAttributedStringCreateMutable(NULL,0);
    CFAttributedStringReplaceString(attributed,CFRangeMake(0,0),CFSTR("rozetka2"));
    CTFontRef font=CTFontCreateUIFontForLanguage(kCTFontUIFontSystem,18,NULL);require(font&&CTFontGetSize(font)==18&&CTFontGetAscent(font)>0,"native CoreText font metrics");
    CFAttributedStringSetAttribute(attributed,CFRangeMake(0,8),kCTFontAttributeName,font);
    CTTypesetterRef typesetter=CTTypesetterCreateWithAttributedStringAndOptions(attributed,NULL);CTLineRef line=CTTypesetterCreateLine(typesetter,CFRangeMake(0,8));
    CFArrayRef runs=CTLineGetGlyphRuns(line);require(runs&&CFArrayGetCount(runs)>0,"native text shaping");
    CTRunRef run=CFArrayGetValueAtIndex(runs,0);CFIndex count=CTRunGetGlyphCount(run);CGGlyph glyphs[32]={0};CGPoint positions[32];
    require(count==8,"shape expected glyph count");CTRunGetGlyphs(run,CFRangeMake(0,count),glyphs);CTRunGetPositions(run,CFRangeMake(0,count),positions);
    require(glyphs[0]!=0&&positions[1].x>positions[0].x&&CTRunGetTypographicBounds(run,CFRangeMake(0,count),NULL,NULL,NULL)>0,"glyph positions and typographic bounds");
    CFRange string_range=CTRunGetStringRange(run);require(string_range.location==0&&string_range.length==8,"CFRange returned in RAX/RDX");
    CGRect glyph_bounds[32];CGRect total_bounds=CTFontGetBoundingRectsForGlyphs(font,kCTFontOrientationDefault,glyphs,glyph_bounds,count);
    require(total_bounds.size.width>0&&total_bounds.size.height>0&&glyph_bounds[0].size.height>0,"CGRect sret and shifted glyph arguments");
    CGPathRef glyph_path=CTFontCreatePathForGlyph(font,glyphs[0],NULL);require(glyph_path!=NULL,"native glyph outline");
    CGPathApply(glyph_path,(void *)42,path_element);require(path_elements>2,"native glyph outline visits guest callback");CFRelease(glyph_path);
    CFRelease(line);CFRelease(typesetter);CFRelease(font);CFRelease(attributed);
    CFDictionaryRef proxies=CFNetworkCopySystemProxySettings();require(proxies&&CFGetTypeID(proxies)==CFDictionaryGetTypeID(),"CFNetwork library resolution");CFRelease(proxies);
    require(CFRunLoopGetCurrent()!=NULL,"native run loop");
    char path[]="/tmp/rozetka2-platform-XXXXXX";int fd=mkstemp(path);require(fd>=0,"temporary file");
    require(write(fd,"rozetka2",8)==8&&fsync(fd)==0,"real file sync");
    struct timespec times[2]={{1700000000,123000000},{1700000001,456000000}};require(futimens(fd,times)==0,"file timestamp ABI");
    struct stat st;require(fstat(fd,&st)==0&&st.st_mtimespec.tv_sec==times[1].tv_sec&&st.st_mtimespec.tv_nsec==times[1].tv_nsec,"file timestamps preserved");
    require(fsetxattr(fd,"com.rozetka2.fixture","value",5,0,0)==0,"write xattr");char value[8]={0};
    require(fgetxattr(fd,"com.rozetka2.fixture",value,sizeof(value),0,0)==5&&!memcmp(value,"value",5),"read xattr and six integer arguments");
    require(fremovexattr(fd,"com.rozetka2.fixture",0)==0,"remove xattr");
    char target[]="/tmp/rozetka2-copy-XXXXXX";int out=mkstemp(target);copyfile_state_t state=copyfile_state_alloc();
    require(out>=0&&state&&lseek(fd,0,SEEK_SET)==0&&fcopyfile(fd,out,state,COPYFILE_DATA)==0&&copyfile_state_free(state)==0,"native copyfile state and copy");
    require(lseek(out,0,SEEK_SET)==0&&read(out,value,sizeof(value))==8&&!memcmp(value,"rozetka2",8),"copied file contents");
    close(out);close(fd);unlink(target);unlink(path);puts("PASS platform batch: Unicode, callbacks, font shaping, proxy settings, file metadata and copy");return 0;
}
