/* Same-version scalar reference and the game's unmodified x86 JNI resizer. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stb_image_resize2.h"
#ifndef R2_STB_REFERENCE
#include <dlfcn.h>
typedef uintptr_t (*resize_jni)(void *,void *,uintptr_t,int,int,int,uintptr_t,int,int,int,int);
static resize_jni routines[3];
static void *aligned_allocate(size_t alignment,size_t size){void *p=NULL;return posix_memalign(&p,alignment,size)==0?p:NULL;}
#endif
static void require(int ok,const char *message){if(!ok){fprintf(stderr,"FAIL STB resize: %s\n",message);exit(91);}}
int main(int argc,char **argv)
{
#ifdef R2_STB_REFERENCE
    require(argc==2,"reference output path");FILE *file=fopen(argv[1],"wb");
#else
    require(argc==3,"driver and output path");void *library=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);require(library!=NULL,"load official x86 STB");FILE *file=fopen(argv[2],"wb");
    void (*setup)(void *,void *,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t)=dlsym(library,"Java_org_lwjgl_stb_LibSTB_setupMalloc");require(setup!=NULL,"STB allocator setup");
    setup(NULL,NULL,(uintptr_t)malloc,(uintptr_t)calloc,(uintptr_t)realloc,(uintptr_t)free,(uintptr_t)aligned_allocate,(uintptr_t)free);
    const char *names[]={"Java_org_lwjgl_stb_STBImageResize_nstbir_1resize_1uint8_1linear","Java_org_lwjgl_stb_STBImageResize_nstbir_1resize_1uint8_1srgb","Java_org_lwjgl_stb_STBImageResize_nstbir_1resize_1float_1linear"};
    for(unsigned i=0;i<3;i++){routines[i]=dlsym(library,names[i]);require(routines[i]!=NULL,"actual JNI resize entry point");}
#endif
    require(file!=NULL,"open output fixture");
    const stbir_pixel_layout layouts[]={STBIR_1CHANNEL,STBIR_2CHANNEL,STBIR_RGB,STBIR_RGBA,STBIR_4CHANNEL};const unsigned channels[]={1,2,3,4,4};
    const unsigned sizes[][4]={{16,16,8,8},{7,9,19,13},{23,5,3,2},{1,1,17,7},{31,17,31,17}};unsigned cases=0;
    for(unsigned kind=0;kind<3;kind++)for(unsigned layout=0;layout<5;layout++)for(unsigned dimensions=0;dimensions<5;dimensions++)for(unsigned pattern=0;pattern<2;pattern++) {
        unsigned iw=sizes[dimensions][0],ih=sizes[dimensions][1],ow=sizes[dimensions][2],oh=sizes[dimensions][3],n=channels[layout],element=kind==2?4:1;
        unsigned in_stride=(iw*n+3)*element,out_stride=(ow*n+5)*element;size_t input_size=(size_t)in_stride*ih,output_size=(size_t)out_stride*oh;
        unsigned char *input=malloc(input_size+64),*output=malloc(output_size+64),*copy=malloc(input_size+64);require(input&&output&&copy,"allocate guarded pixels");memset(input,0xa5,input_size+64);memset(output,0xa5,output_size+64);
        for(unsigned y=0;y<ih;y++)for(unsigned x=0;x<iw;x++)for(unsigned c=0;c<n;c++) {
            unsigned hash=pattern?(x*37+y*19+c*73)%256:(c+1)*43;
            if(kind==2){float value=pattern?((int)hash-128)/64.0f:(c+1)*0.125f;if(layout==3&&c==3)value=0.25f+(hash%192)/256.0f;memcpy(input+32+y*in_stride+(x*n+c)*4,&value,4);}
            else input[32+y*in_stride+x*n+c]=(unsigned char)(layout==3&&c==3?32+hash%224:hash);
        }
        memcpy(copy,input,input_size+64);void *result;
#ifdef R2_STB_REFERENCE
        if(kind==0)result=stbir_resize_uint8_linear(input+32,iw,ih,in_stride,output+32,ow,oh,out_stride,layouts[layout]);
        else if(kind==1)result=stbir_resize_uint8_srgb(input+32,iw,ih,in_stride,output+32,ow,oh,out_stride,layouts[layout]);
        else result=stbir_resize_float_linear((float *)(input+32),iw,ih,in_stride,(float *)(output+32),ow,oh,out_stride,layouts[layout]);
#else
        result=(void *)routines[kind](NULL,NULL,(uintptr_t)(input+32),iw,ih,in_stride,(uintptr_t)(output+32),ow,oh,out_stride,layouts[layout]);
#endif
        require(result==output+32&&memcmp(input,copy,input_size+64)==0,"resize result and immutable input");
        for(unsigned i=0;i<32;i++)require(output[i]==0xa5&&output[32+output_size+i]==0xa5,"output allocation guards");
        for(unsigned y=0;y<oh;y++)for(unsigned i=ow*n*element;i<out_stride;i++)require(output[32+y*out_stride+i]==0xa5,"output row padding");
        uint32_t header[]={kind,ow*oh*n*element};require(fwrite(header,4,2,file)==2,"write reference metadata");
        for(unsigned y=0;y<oh;y++)require(fwrite(output+32+y*out_stride,1,ow*n*element,file)==ow*n*element,"write resized pixels");
        free(input);free(output);free(copy);cases++;
    }
    require(fclose(file)==0,"close output fixture");printf("PASS STB resizing %u cases: linear/sRGB/float, layouts, alpha, strides and guards\n",cases);return 0;
}
