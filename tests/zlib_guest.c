#include <zlib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void check(int ok,const char *message) {if(!ok){fprintf(stderr,"FAIL zlib guest: %s\n",message);abort();}}
int main(int argc,char **argv)
{
    (void)argv;
    unsigned char compressed[512],output[512];
    const char input[]="Minecraft JAR decompression through rozetka2: 1234567890";
    z_stream stream={0};
    if(argc>1)stream.zalloc=(alloc_func)(uintptr_t)0x12345678;
    check(deflateInit2(&stream,6,Z_DEFLATED,-15,8,Z_DEFAULT_STRATEGY)==Z_OK,"initialize deflater");
    check(stream.zalloc==NULL&&stream.zfree==NULL,"host callbacks stay private");
    stream.next_in=(Bytef *)input;stream.avail_in=sizeof(input);stream.next_out=compressed;stream.avail_out=sizeof(compressed);
    check(deflate(&stream,Z_FINISH)==Z_STREAM_END,"compress");
    unsigned length=(unsigned)stream.total_out;
    check(deflateEnd(&stream)==Z_OK,"end compression");
    memset(&stream,0,sizeof(stream));check(inflateInit2(&stream,-15)==Z_OK,"initialize inflater");
    for(unsigned i=0;i<3;i++) {
        stream.next_in=compressed;stream.avail_in=length;stream.next_out=output;stream.avail_out=sizeof(output);
        check(inflate(&stream,Z_FINISH)==Z_STREAM_END,"decompress");
        check(stream.total_in==length&&stream.total_out==sizeof(input)&&memcmp(input,output,sizeof(input))==0,"content and counters");
        check(stream.zalloc==NULL&&stream.zfree==NULL,"callbacks remain guest values");
        check(inflateReset(&stream)==Z_OK,"reuse shadow stream");
    }
    check(inflateEnd(&stream)==Z_OK,"end decompression");
    puts("PASS guest zlib roundtrip, stream reset and callback isolation");return 0;
}
