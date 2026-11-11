#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
static void require(int ok,const char *message) {
    if(!ok){fprintf(stderr,"FAIL OpenGL guest: %s\n",message);exit(91);}
}
#include "opengl_gui.inc"
int main(int argc,char **argv) {
    void *library=dlopen("/System/Library/Frameworks/OpenGL.framework/Versions/Current/OpenGL",RTLD_NOW|RTLD_LOCAL);
    require(library!=NULL,"shared-cache OpenGL framework");
#define API(name) __typeof__(&name) r2_##name=(void *)dlsym(library,#name);require(r2_##name!=NULL,#name)
    API(CGLChoosePixelFormat);API(CGLCreateContext);API(CGLDestroyPixelFormat);
    API(CGLSetCurrentContext);API(CGLGetCurrentContext);API(CGLDestroyContext);
    API(glGetString);API(glClearColor);API(glGetFloatv);API(glGetDoublev);API(glGetError);
    API(glGenTextures);API(glBindTexture);API(glTexImage2D);API(glDeleteTextures);
    API(glGenFramebuffers);API(glBindFramebuffer);API(glFramebufferTexture2D);
    API(glCheckFramebufferStatus);API(glClear);API(glReadPixels);API(glDeleteFramebuffers);
    API(glGenBuffers);API(glBindBuffer);API(glBufferData);API(glCopyBufferSubData);API(glGetBufferSubData);API(glDeleteBuffers);
#undef API
    CGLPixelFormatAttribute attributes[]={kCGLPFAAccelerated,kCGLPFAAllowOfflineRenderers,kCGLPFAOpenGLProfile,(CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,0};
    CGLPixelFormatObj pixels=NULL;GLint count=0;CGLContextObj context=NULL;
    require(r2_CGLChoosePixelFormat(attributes,&pixels,&count)==kCGLNoError&&pixels&&count>0,"native pixel format");
    require(r2_CGLCreateContext(pixels,NULL,&context)==kCGLNoError&&context,"native context");
    r2_CGLDestroyPixelFormat(pixels);
    require(r2_CGLSetCurrentContext(context)==kCGLNoError&&r2_CGLGetCurrentContext()==context,"thread-local context identity");
    const GLubyte *renderer=r2_glGetString(GL_RENDERER),*version=r2_glGetString(GL_VERSION);
    require(renderer&&renderer[0]&&version&&version[0],"native renderer and version strings");
    r2_glClearColor(0.25f,0.5f,0.75f,1.0f);
    GLfloat color[4]={0};GLdouble doubles[4]={0};
    r2_glGetFloatv(GL_COLOR_CLEAR_VALUE,color);r2_glGetDoublev(GL_COLOR_CLEAR_VALUE,doubles);
    require(color[0]==0.25f&&color[1]==0.5f&&color[2]==0.75f&&color[3]==1.0f&&doubles[0]==0.25&&doubles[2]==0.75,"float arguments and float/double result buffers");
    GLuint texture=0,framebuffer=0;
    r2_glGenTextures(1,&texture);r2_glBindTexture(GL_TEXTURE_2D,texture);
    r2_glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,16,16,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    r2_glGenFramebuffers(1,&framebuffer);r2_glBindFramebuffer(GL_FRAMEBUFFER,framebuffer);
    r2_glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
    require(r2_glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"offscreen framebuffer");
    r2_glClear(GL_COLOR_BUFFER_BIT);GLubyte pixel[4]={0};
    r2_glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
    require(pixel[0]==64&&pixel[1]==128&&pixel[2]==191&&pixel[3]==255&&r2_glGetError()==GL_NO_ERROR,"GPU clear and exact RGBA readback");
    GLuint buffers[2];unsigned char source[64],destination[64],copied[64];
    for(unsigned i=0;i<64;i++){source[i]=(unsigned char)(i*17+3);destination[i]=0xcc;}
    r2_glGenBuffers(2,buffers);r2_glBindBuffer(GL_COPY_READ_BUFFER,buffers[0]);r2_glBufferData(GL_COPY_READ_BUFFER,64,source,GL_STATIC_DRAW);
    r2_glBindBuffer(GL_COPY_WRITE_BUFFER,buffers[1]);r2_glBufferData(GL_COPY_WRITE_BUFFER,64,destination,GL_STATIC_DRAW);
    r2_glCopyBufferSubData(GL_COPY_READ_BUFFER,GL_COPY_WRITE_BUFFER,3,11,37);r2_glGetBufferSubData(GL_COPY_WRITE_BUFFER,0,64,copied);
    for(unsigned i=0;i<64;i++)require(copied[i]==(i>=11&&i<48?source[i-8]:0xcc),"GPU buffer copy offsets, bytes and untouched surroundings");
    require(r2_glGetError()==GL_NO_ERROR,"GPU buffer copy status");r2_glDeleteBuffers(2,buffers);
    if(argc==3)game_gui(library,argv[1],argv[2]);
    r2_glDeleteFramebuffers(1,&framebuffer);r2_glDeleteTextures(1,&texture);
    printf("PASS guest OpenGL GPU readback: %s, %s\n",renderer,version);
    require(r2_CGLSetCurrentContext(NULL)==kCGLNoError&&r2_CGLDestroyContext(context)==kCGLNoError,"context teardown");
    require(dlclose(library)==0,"close framework scope");return 0;
}
