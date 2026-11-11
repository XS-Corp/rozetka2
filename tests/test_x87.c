#include "internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
static void check(bool condition,const char *message) {if(!condition){fprintf(stderr,"FAIL x87: %s\n",message);exit(1);}checks++;}
static uint64_t random_bits(void) {static uint64_t seed=0x641472bd739821;seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;return seed;}
static uint64_t bits_of(double value) {uint64_t bits;memcpy(&bits,&value,8);return bits;}
static unsigned quotient_flags(unsigned magnitude) {return ((magnitude&4)?0x100:0)|((magnitude&2)?0x4000:0)|((magnitude&1)?0x200:0);}
static void run(r2_block *block,uint64_t left,uint64_t right,unsigned width,bool nearest)
{
    struct {uint64_t left,right,result;uint16_t status;} data={left,right,0,0};
    char error[R2_ERROR_CAP]={0};r2_result result;
    r2_memory_region region={.base=0x10000,.size=sizeof(data),.data=(uint8_t *)&data,.writable=true};
    r2_interp_env env={.regions=&region,.region_count=1,.has_initial_regs=true};
    env.initial_regs[R2_X86_RDI]=region.base;env.initial_regs[R2_X86_RSI]=region.base+8;env.initial_regs[R2_X86_RDX]=region.base+16;
    check(r2_interp_block_with_env(block,&env,&result,error,sizeof(error))==R2_OK,error);
    if(width==8) {
        double a,b,out,reference;memcpy(&a,&left,8);memcpy(&b,&right,8);memcpy(&out,&data.result,8);
        reference=nearest?remainder(a,b):fmod(a,b);
        check(isnan(reference)?isnan(out):bits_of(reference)==data.result,"double remainder matches native libm bit for bit");
        if(isfinite(a)&&isfinite(b)&&b!=0) {
            int q;double rem=remquo(a,b,&q);unsigned magnitude=q<0?0u-(unsigned)q:(unsigned)q;
            if(!nearest&&rem!=0&&!!signbit(rem)!=!!signbit(a))magnitude--;
            check((data.status&0x4300)==quotient_flags(magnitude),"double quotient magnitude bits in x87 condition codes");
        }
    } else {
        uint32_t a_bits=(uint32_t)left,b_bits=(uint32_t)right,wanted;float a,b,out,reference;
        memcpy(&a,&a_bits,4);memcpy(&b,&b_bits,4);memcpy(&out,&data.result,4);
        reference=nearest?remainderf(a,b):fmodf(a,b);memcpy(&wanted,&reference,4);
        check(isnan(reference)?isnan(out):wanted==(uint32_t)data.result,"float remainder matches native libm bit for bit");
        if(isfinite(a)&&isfinite(b)&&b!=0) {
            int q;float rem=remquof(a,b,&q);unsigned magnitude=q<0?0u-(unsigned)q:(unsigned)q;
            if(!nearest&&rem!=0&&!!signbit(rem)!=!!signbit(a))magnitude--;
            check((data.status&0x4300)==quotient_flags(magnitude),"float quotient magnitude bits in x87 condition codes");
        }
    }
    check(!(data.status&0x400),"partial remainder loop terminates with C2 clear");
}
static r2_result execute_data(r2_block *block,const uint8_t *code,size_t length,uint8_t *data,size_t size,size_t second,size_t output,size_t fourth)
{
    char error[R2_ERROR_CAP];r2_result result;
    check(r2_decode_block(code,length,block,error,sizeof(error))==R2_OK,error);
    r2_memory_region region={.base=0x10000,.size=size,.data=data,.writable=true};
    r2_interp_env env={.regions=&region,.region_count=1,.has_initial_regs=true};
    env.initial_regs[R2_X86_RDI]=region.base;env.initial_regs[R2_X86_RSI]=region.base+second;
    env.initial_regs[R2_X86_RDX]=region.base+output;env.initial_regs[R2_X86_RCX]=region.base+fourth;
    check(r2_interp_block_with_env(block,&env,&result,error,sizeof(error))==R2_OK,error);return result;
}
static void put80(uint8_t *data,uint64_t significand,uint16_t exponent) {memcpy(data,&significand,8);memcpy(data+8,&exponent,2);}
static bool equal80(const uint8_t *data,uint64_t significand,uint16_t exponent) {uint64_t a;uint16_t b;memcpy(&a,data,8);memcpy(&b,data+8,2);return a==significand&&b==exponent;}
static void precision_and_state(r2_block *block)
{
    uint8_t data[640]={0};
    uint8_t remainder80[]={0xdb,0xe3,0xdb,0x2e,0xdb,0x2f,0xd9,0xf8,0xdf,0xe0,0xf6,0xc4,4,0x75,0xf7,0x66,0x89,0x42,16,0xdd,0xd9,0xdb,0x3a,0xc3};
    put80(data,UINT64_C(0x8000000000000001),0x3fff);put80(data+16,UINT64_C(0x8000000000000000),0x3fff);
    execute_data(block,remainder80,sizeof(remainder80),data,sizeof(data),16,32,0);
    check(equal80(data+32,UINT64_C(0x8000000000000000),0x3fc0),"80-bit remainder retains precision unavailable in ARM long double");
    remainder80[7]=0xf5;put80(data,UINT64_C(0xa000000000000001),0x4000);
    execute_data(block,remainder80,sizeof(remainder80),data,sizeof(data),16,32,0);
    check(equal80(data+32,UINT64_C(0xfffffffffffffff8),0xbffd),"nearest remainder uses exact 80-bit tie comparison");
    put80(data,1,0x7fff);
    execute_data(block,remainder80,sizeof(remainder80),data,sizeof(data),16,32,0);
    check(equal80(data+32,UINT64_C(0xc000000000000000),0xffff),"unsupported pseudo-NaN becomes x87 indefinite");
    put80(data,0,0);put80(data+16,1,0);
    execute_data(block,remainder80,sizeof(remainder80),data,sizeof(data),16,32,0);uint16_t status;memcpy(&status,data+48,2);
    check(equal80(data+32,0,0)&&(status&2),"zero modulo an 80-bit denormal preserves zero and records denormal operand");
    uint8_t partial[]={0xdb,0xe3,0xdb,0x2e,0xdb,0x2f,0xd9,0xf8,0xdf,0xe0,0xdd,0xd9,0xdb,0x3a,0xc3};
    put80(data,UINT64_C(0x8000000000000005),0x4063);put80(data+16,UINT64_C(0x8000000000000003),0x3fff);
    r2_result result=execute_data(block,partial,sizeof(partial),data,sizeof(data),16,32,0);uint16_t exponent;memcpy(&exponent,data+40,2);
    check((result.rax&0x400)&&((exponent&0x7fff)<=0x4063-32),"one partial reduction sets C2 and reduces exponent by at least 32");
    uint8_t state[]={0xdb,0xe3,0xdb,0x2f,0xdb,0x2e,0x0f,0xae,0x02,0xdb,0xe3,0x0f,0xae,0x0a,0xd9,0xc9,0xdb,0x39,0xdb,0x79,16,0xdf,0xe0,0xc3};
    put80(data,UINT64_C(0x8000000000000001),0x3fff);put80(data+16,UINT64_C(0xc000000000000000),0xc000);memset(data+64,0xaa,512);
    result=execute_data(block,state,sizeof(state),data,sizeof(data),16,64,32);
    check(data[68]==0xc0&&(data[67]&0x38)==0x30,"FXSAVE uses physical tag bits with a rotated logical register stack");
    check(equal80(data+96,UINT64_C(0xc000000000000000),0xc000)&&equal80(data+112,UINT64_C(0x8000000000000001),0x3fff),"FXSAVE stores logical ST0/ST1 register contents");
    check(equal80(data+32,UINT64_C(0x8000000000000001),0x3fff)&&equal80(data+48,UINT64_C(0xc000000000000000),0xc000)&&!(result.rax&0x3800),"FXRSTOR restores nonempty stack through exchange and two pops");
    for(unsigned i=464;i<512;i++)check(data[64+i]==0xaa,"FXSAVE preserves software-owned bytes");
    const uint8_t convert[]={0xdb,0xe3,0xdb,0x2f,0xdd,0x1a,0xdf,0xe0,0xc3};
    const uint64_t mantissas[]={UINT64_C(0x8000000000000400),UINT64_C(0x8000000000000401),UINT64_C(0x8000000000000c00)};
    const uint64_t expected[]={UINT64_C(0x3ff0000000000000),UINT64_C(0x3ff0000000000001),UINT64_C(0x3ff0000000000002)};
    for(unsigned i=0;i<3;i++) {
        put80(data,mantissas[i],0x3fff);result=execute_data(block,convert,sizeof(convert),data,sizeof(data),16,32,0);uint64_t output;memcpy(&output,data+32,8);
        check(output==expected[i],"80-bit stores round binary64 halfway cases to even");
        check((result.rax&0x20)&&!!(result.rax&0x200)==(i!=0),"inexact stores expose precision and rounded-up status");
    }
    const struct {uint64_t significand;uint16_t exponent;uint64_t expected;unsigned flags;} conversions[]={
        {1,0,0,0x30},{1,0x8000,UINT64_C(0x8000000000000000),0x30},
        {UINT64_MAX,0x7ffe,UINT64_C(0x7ff0000000000000),0x28},
        {UINT64_C(0x8000000000000000),0x7fff,UINT64_C(0x7ff0000000000000),0},
        {UINT64_C(0xc000000000000042),0x7fff,UINT64_C(0x7ff8000000000000),0},
        {UINT64_C(0x8000000000000001),0x7fff,UINT64_C(0x7ff8000000000000),1},
        {1,0x3fff,UINT64_C(0xfff8000000000000),1},
        {UINT64_C(0x8000000000000000),16383-1075,0,0x30},
        {UINT64_C(0x8000000000000001),16383-1075,1,0x30}
    };
    for(unsigned i=0;i<sizeof(conversions)/sizeof(conversions[0]);i++) {
        put80(data,conversions[i].significand,conversions[i].exponent);
        result=execute_data(block,convert,sizeof(convert),data,sizeof(data),16,32,0);uint64_t output;memcpy(&output,data+32,8);
        check(output==conversions[i].expected&&(result.rax&0x3f)==conversions[i].flags,"80-bit conversion underflow, overflow, unsupported formats and NaNs");
    }
}
static void compare_flags(r2_block *block)
{
    /* Ranks describe numeric ordering independently of the 80-bit encoding.
     * Equal ranks include signed zeros and pseudo-denormal aliases. */
    const struct {uint64_t significand;uint16_t exponent;int rank;unsigned nan;} values[]={
        {UINT64_C(0x8000000000000000),0xffff,0,0},
        {UINT64_MAX,0xfffe,1,0},
        {UINT64_C(0x8000000000000001),0xbfff,2,0},
        {UINT64_C(0x8000000000000000),0xbfff,3,0},
        {1,0x8000,4,0},{0,0x8000,5,0},{0,0,5,0},{1,0,6,0},
        {UINT64_C(0x8000000000000000),0,7,0},
        {UINT64_C(0x8000000000000000),1,7,0},
        {UINT64_C(0x8000000000000000),0x3fff,8,0},
        {UINT64_C(0x8000000000000001),0x3fff,9,0},
        {UINT64_MAX,0x7ffe,10,0},{UINT64_C(0x8000000000000000),0x7fff,11,0},
        {UINT64_C(0xc000000000000042),0x7fff,0,1},
        {UINT64_C(0x8000000000000001),0x7fff,0,2},
        {1,0x3fff,0,3},{0,0x7fff,0,3}
    };
    for(unsigned ordered=0;ordered<2;ordered++)for(unsigned pop=0;pop<2;pop++)
    for(unsigned source=0;source<8;source+=source?6:1)
    for(unsigned a=0;a<sizeof(values)/sizeof(values[0]);a++)
    for(unsigned b=0;b<sizeof(values)/sizeof(values[0]);b++) {
        uint8_t data[1088]={0};uint16_t control=0x37f,status=0x4700|(3<<11);uint32_t mxcsr=0x1f80;
        memcpy(data,&control,2);memcpy(data+2,&status,2);data[4]=0xff;memcpy(data+24,&mxcsr,4);
        put80(data+32,values[a].significand,values[a].exponent);
        unsigned right=source?b:a;
        if(source)put80(data+32+source*16,values[b].significand,values[b].exponent);
        uint8_t code[]={0x0f,0xae,0x0f,0x68,0xd7,0x0e,0,0,0x9d,
            pop?0xdf:0xdb,(uint8_t)((ordered?0xf0:0xe8)+source),0x9c,0x58,0x0f,0xae,0x02,0xc3};
        r2_result result=execute_data(block,code,sizeof(code),data,sizeof(data),0,512,0);
        bool unordered=values[a].nan||values[right].nan;
        bool invalid=values[a].nan>=2||values[right].nan>=2||
            (ordered&&(values[a].nan||values[right].nan));
        uint64_t flags=0x602|(unordered?0x45:values[a].rank<values[right].rank?1:values[a].rank==values[right].rank?0x40:0);
        check(result.rax==flags,"FCOMI/FUCOMI numeric ordering, NaNs and cleared OF/SF/AF");
        memcpy(&status,data+514,2);
        check(status==(0x4500|((3+pop)<<11)|(invalid?1:0)),"x87 compare preserves condition codes and changes only IE, C1 and optional TOP");
        check(data[516]==(pop?0xf7:0xff),"x87 compare pops exactly ST0 only for IP forms");
    }
    for(unsigned pop=0;pop<2;pop++) {
        uint8_t data[1088]={0};uint16_t control=0x37f;uint32_t mxcsr=0x1f80;
        memcpy(data,&control,2);data[4]=2;memcpy(data+24,&mxcsr,4);put80(data+48,UINT64_C(0x8000000000000000),0x3fff);
        uint8_t code[]={0x0f,0xae,0x0f,pop?0xdf:0xdb,0xe9,0x9c,0x58,0x0f,0xae,0x02,0xc3};
        r2_result result=execute_data(block,code,sizeof(code),data,sizeof(data),0,512,0);
        uint16_t status;memcpy(&status,data+514,2);
        check((result.rax&0x8d5)==0x45&&(status&0x3a41)==((pop<<11)|0x41),"empty compare gives masked invalid stack fault and unordered flags");
    }
    uint8_t data[1088]={0};
    const uint8_t zero[]={0xdb,0xe3,0x68,0xd7,0x0e,0,0,0x9d,0xd9,0xee,0x9c,0x58,0x0f,0xae,0x02,0xc3};
    r2_result result=execute_data(block,zero,sizeof(zero),data,sizeof(data),0,512,0);
    uint16_t status;memcpy(&status,data+514,2);
    check(result.rax==0xed7&&status==0x3800&&data[516]==0x80&&equal80(data+544,0,0),"FLDZ pushes exact positive zero and preserves EFLAGS");
}

int main(void)
{
    uint8_t code[]={0xdb,0xe3,0xdd,0x06,0xdd,0x07,0xd9,0xf8,0xdf,0xe0,0xf6,0xc4,4,0x75,0xf7,0x66,0x89,0x42,8,0xdd,0xd9,0xdd,0x1a,0x31,0xc0,0xc3};
    r2_block *block=calloc(1,sizeof(*block));check(block!=NULL,"allocate x87 block");char error[R2_ERROR_CAP];
    const uint64_t cases[]={0,UINT64_C(0x8000000000000000),1,UINT64_C(0x000fffffffffffff),UINT64_C(0x0010000000000000),
        UINT64_C(0x7fefffffffffffff),UINT64_C(0x7ff0000000000000),UINT64_C(0xfff0000000000000),UINT64_C(0x7ff8000000000042),
        UINT64_C(0x7ff0000000000001),UINT64_C(0x4004000000000000),UINT64_C(0x400c000000000000),UINT64_C(0x3ff0000000000000)};
    for(unsigned nearest=0;nearest<2;nearest++)for(unsigned width=4;width<=8;width+=4) {
        code[2]=code[4]=code[21]=width==8?0xdd:0xd9;code[7]=nearest?0xf5:0xf8;
        check(r2_decode_block(code,sizeof(code),block,error,sizeof(error))==R2_OK,error);
        for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++)for(unsigned j=0;j<sizeof(cases)/sizeof(cases[0]);j++) {
            uint64_t left=cases[i],right=cases[j];
            if(width==4) {double a,b;float x,y;memcpy(&a,&left,8);memcpy(&b,&right,8);x=(float)a;y=(float)b;left=0;right=0;memcpy(&left,&x,4);memcpy(&right,&y,4);}
            run(block,left,right,width,nearest);
        }
        for(unsigned i=0;i<2500;i++)run(block,random_bits(),random_bits(),width,nearest);
    }
    precision_and_state(block);
    compare_flags(block);
    if(block->insns!=block->storage)free(block->insns);free(block);
    printf("PASS x87 80-bit state, partial remainders and flag comparisons (%u checks)\n",checks);return 0;
}
