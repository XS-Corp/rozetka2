#include "internal.h"
#include <math.h>
#include <fenv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef union { uint8_t b[16]; float f[4]; double d[2]; uint64_t q[2]; uint32_t u[4]; } vector;
typedef struct { uint8_t b[256]; size_t n; } program;
static unsigned checks;
static void require(bool ok, const char *message) { if (!ok) { fprintf(stderr,"FAIL SSE: %s\n",message); exit(1); } checks++; }
static void bytes(program *p, const uint8_t *data, size_t n) { memcpy(p->b+p->n,data,n); p->n+=n; }
#define EMIT(p, ...) do { const uint8_t data[] = {__VA_ARGS__}; bytes(p,data,sizeof(data)); } while (0)
static void immediate(program *p, uint64_t value) { EMIT(p,0x48,0xb8); bytes(p,(uint8_t *)&value,8); }
static void load(program *p, unsigned reg, vector value)
{
    immediate(p,value.q[0]); EMIT(p,0x66,reg>=8?0x4c:0x48,0x0f,0x6e,0xc0|((reg&7)<<3));
    unsigned scratch=(reg&1)?3:2;
    immediate(p,value.q[1]); EMIT(p,0x66,0x48,0x0f,0x6e,0xc0|(scratch<<3));
    EMIT(p,0x66); if(reg>=8) EMIT(p,0x44); EMIT(p,0x0f,0x14,0xc0|((reg&7)<<3)|scratch);
}
static uint64_t execute(program base, unsigned destination, unsigned high, bool flags, bool integer)
{
    if(flags) {
        const uint8_t capture[]={0xb8,0,0,0,0,0x0f,0x92,0xc0,0x0f,0x90,0xc4,
            0x0f,0x94,0xc3,0x0f,0x98,0xc7,0x0f,0x9a,0xc1,0x0f,0xb7,0xd3,
            0x48,0xc1,0xe2,0x10,0x48,0x09,0xd0,0x0f,0xb6,0xc9,0x48,0xc1,0xe1,0x20,0x48,0x09,0xc8};
        bytes(&base,capture,sizeof(capture));
    } else if(!integer) {
        if(high) { if(destination>=8) EMIT(&base,0x45); EMIT(&base,0x0f,0x12,0xc0|((destination&7)<<3)|(destination&7)); }
        EMIT(&base,0x66,destination>=8?0x4c:0x48,0x0f,0x7e,0xc0|((destination&7)<<3));
    }
    EMIT(&base,0xc3);
    r2_block *block=malloc(sizeof(*block)); require(block!=NULL,"allocate block");
    char error[R2_ERROR_CAP]={0}; r2_result result={0};
    r2_status status=r2_decode_block(base.b,base.n,block,error,sizeof(error));
    if(status==R2_OK) status=r2_interp_block(block,&result,error,sizeof(error));
    if(status!=R2_OK) { fprintf(stderr,"%s\n",error); exit(1); }
    free(block); return result.rax;
}
static program setup(vector a, vector b, unsigned dst)
{
    program p={0}; load(&p,dst,a); load(&p,dst+1,b); return p;
}
static void compare_vector(program p,unsigned dst,vector expected,const char *name)
{
    for(unsigned half=0;half<2;half++) {
        uint64_t actual=execute(p,dst,half,false,false);
        if(actual!=expected.q[half]) { fprintf(stderr,"%s half %u: got %llx expected %llx\n",name,half,(unsigned long long)actual,(unsigned long long)expected.q[half]); exit(1); }
        checks++;
    }
}
static void arithmetic(void)
{
    const unsigned opcodes[]={0x58,0x59,0x5c,0x5e,0x5d,0x5f,0x51};
    const double answers[][4]={{3.5,1,-0.25,25},{3,-12,-0.125,144},{-0.5,-7,0.75,7},
        {0.75,-0.75,-0.5,16.0/9.0},{1.5,-3,-0.5,9},{2,4,0.25,16},{2,3,4,5}};
    for(unsigned wide=0;wide<2;wide++) for(unsigned scalar=0;scalar<2;scalar++)
    for(unsigned memory=0;memory<2;memory++) for(unsigned extended=0;extended<2;extended++)
    for(unsigned op=0;op<7;op++) {
        unsigned dst=extended?8:0, width=wide?8:4;
        vector a={0},b={0},expected={0};
        const double av[]={1.5,-3,0.25,16},bv[]={2,4,-0.5,9},square[]={4,9,16,25};
        for(unsigned i=0;i<16/width;i++) {
            if(wide) { a.d[i]=av[i]; b.d[i]=op==6?square[i]:bv[i]; expected.d[i]=scalar&&i?av[i]:answers[op][i]; }
            else { a.f[i]=(float)av[i]; b.f[i]=(float)(op==6?square[i]:bv[i]); expected.f[i]=(float)(scalar&&i?av[i]:answers[op][i]); }
        }
        program p=setup(a,b,dst);
        if(memory) { EMIT(&p,0x48,0x83,0xec,0x28); if(extended) EMIT(&p,0x44); EMIT(&p,0x0f,0x11,0x0c,0x24); }
        EMIT(&p,0x31,0xdb,0x83,0xfb,1); /* CF=SF=PF=1, OF=ZF=0. */
        if(scalar) EMIT(&p,wide?0xf2:0xf3); else if(wide) EMIT(&p,0x66);
        if(extended) EMIT(&p,memory?0x44:0x45);
        EMIT(&p,0x0f,opcodes[op],memory?0x04:0xc1); if(memory) EMIT(&p,0x24);
        if(memory) EMIT(&p,0x48,0x8d,0x64,0x24,0x28);
        compare_vector(p,dst,expected,"arithmetic/scalar preservation");
        require(execute(p,dst,0,true,false)==(UINT64_C(1)|(UINT64_C(1)<<24)|(UINT64_C(1)<<32)),"FP arithmetic preserves EFLAGS");
    }
}
static void comparisons(void)
{
    const double av[]={-1,1,2,NAN},bv[]={1,1,1,0};
    for(unsigned wide=0;wide<2;wide++) for(unsigned c=0;c<4;c++) {
        vector a={0},b={0}; if(wide) { a.d[0]=av[c];b.d[0]=bv[c]; } else { a.f[0]=(float)av[c];b.f[0]=(float)bv[c]; }
        program p=setup(a,b,0); EMIT(&p,0x31,0xdb,0x83,0xfb,1);
        if(wide) EMIT(&p,0x66); EMIT(&p,0x0f,0x2e,0xc1);
        uint64_t expected=(c==0||c==3?1:0)|(c==1||c==3?UINT64_C(1)<<16:0)|(c==3?UINT64_C(1)<<32:0);
        require(execute(p,0,0,true,false)==expected,"unordered comparison flags");
        for(unsigned predicate=0;predicate<8;predicate++) {
            bool result= predicate==0?c==1:predicate==1?c==0:predicate==2?c<=1:predicate==3?c==3:
                predicate==4?c!=1:predicate==5?c!=0:predicate==6?c>=2:c!=3;
            program q=setup(a,b,0); EMIT(&q,wide?0xf2:0xf3,0x0f,0xc2,0xc1,predicate);
            vector wanted=a; memset(wanted.b,result?255:0,wide?8:4); compare_vector(q,0,wanted,"comparison mask");
        }
    }
    for(unsigned opcode=0x5d;opcode<=0x5f;opcode+=2) {
        vector a={.f={1,2,3,4}}, b={.f={NAN,-0.0f,0,0}};
        program p=setup(a,b,0); EMIT(&p,0xf3,0x0f,opcode,0xc1); vector wanted=a; wanted.u[0]=b.u[0];
        compare_vector(p,0,wanted,"min/max return second NaN");
        a.f[0]=0; b.f[0]=-0.0f; p=setup(a,b,0); EMIT(&p,0xf3,0x0f,opcode,0xc1); wanted=a; wanted.u[0]=b.u[0];
        compare_vector(p,0,wanted,"min/max signed zero follows second operand");
    }
}
static void conversions(void)
{
    const double input[]={-3.75,2.5,3.5,NAN,INFINITY,2147483648.0};
    const uint32_t truncation[]={0xfffffffd,2,3,0x80000000,0x80000000,0x80000000};
    const uint32_t rounded[]={0xfffffffc,2,4,0x80000000,0x80000000,0x80000000};
    for(unsigned i=0;i<6;i++) for(unsigned wide=0;wide<2;wide++) for(unsigned round=0;round<2;round++) {
        vector a={0},b={0}; if(wide)a.d[0]=input[i];else a.f[0]=(float)input[i];
        program p=setup(a,b,0); EMIT(&p,wide?0xf2:0xf3,0x0f,round?0x2d:0x2c,0xc0);
        require(execute(p,0,0,false,true)==(round?rounded[i]:truncation[i]),"FP to int rounding/overflow/NaN");
    }
    for(unsigned wide=0;wide<2;wide++) {
        vector a={.q={0x1122334455667788,0xaabbccddeeff0011}},b={0},expected=a;
        program p=setup(a,b,0); immediate(&p,(uint64_t)-42); EMIT(&p,wide?0xf2:0xf3,0x48,0x0f,0x2a,0xc0);
        if(wide)expected.d[0]=-42;else expected.f[0]=-42;compare_vector(p,0,expected,"int64 to scalar FP preserves upper lanes");
    }
    vector a={.d={3.75,-3.75}},b={0},expected={.u={3,0xfffffffd,0,0}};
    program p=setup(a,b,0); EMIT(&p,0x66,0x0f,0xe6,0xc0); compare_vector(p,0,expected,"packed truncate doubles zeroes high lanes");
    a=(vector){.u={42,0xffffffd6,0,0}}; expected=(vector){.d={42,-42}};
    p=setup(a,b,0); EMIT(&p,0xf3,0x0f,0xe6,0xc0);compare_vector(p,0,expected,"packed int to double");
    a=(vector){.f={-1.75f,3.75f,5,6}};
    const float answer[]={-2,-2,-1,-1};
    for(unsigned mode=0;mode<4;mode++) {
        p=setup(a,b,0); EMIT(&p,0x66,0x0f,0x3a,0x0a,0xc0,mode|8);expected=a; expected.f[0]=answer[mode];
        compare_vector(p,0,expected,"roundss immediate mode");
    }
    a=(vector){.f={1.25f,-2.5f,3,4}};expected=(vector){.d={1.25,-2.5}};
    p=setup(a,b,0); EMIT(&p,0x0f,0x5a,0xc0);compare_vector(p,0,expected,"packed float to double");
}
static void shuffles_and_bits(void)
{
    vector a={.f={1,2,3,4}},b={.f={5,6,7,8}},expected={.f={4,3,6,5}};
    program p=setup(a,b,0); EMIT(&p,0x0f,0xc6,0xc1,0x1b);compare_vector(p,0,expected,"shufps lanes");
    p=setup(a,b,0);EMIT(&p,0x0f,0x14,0xc1);expected=(vector){.f={1,5,2,6}};compare_vector(p,0,expected,"unpack low");
    p=setup(a,b,0);EMIT(&p,0x0f,0x15,0xc1);expected=(vector){.f={3,7,4,8}};compare_vector(p,0,expected,"unpack high");
    a=(vector){.q={0xffff0000ffff0000,0xaaaaaaaaaaaaaaaa}};b=(vector){.q={0xff00ff00ff00ff00,0x5555555555555555}};
    const vector masks[]={ {.q={0xff000000ff000000,0}}, {.q={0x0000ff000000ff00,0x5555555555555555}}, {.q={0xffffff00ffffff00,UINT64_MAX}} };
    for(unsigned i=0;i<3;i++){p=setup(a,b,0);EMIT(&p,0x0f,0x54+i,0xc1);compare_vector(p,0,masks[i],"packed logical bits");}
    a=(vector){.f={-0.0f,1,-2,3}};p=setup(a,b,0);EMIT(&p,0x0f,0x50,0xc0);require(execute(p,0,0,false,true)==5,"movmskps sign bits");
}
static void integer_vectors(void)
{
    vector a={.q={UINT64_C(0xfedcba9876543210),UINT64_C(0x0123456789abcdef)}};
    vector b={.q={UINT64_C(0x7fffffff800001fe),UINT64_C(0xff00aa5501000804)}}, expected;
    for (unsigned prefix=0; prefix<3; prefix++) for (unsigned control=0; control<256; control+=17) {
        program p=setup(a,b,8); unsigned width=prefix?2:4, base=prefix==2?8:0;
        expected=b;
        for(unsigned i=0;i<4;i++) memcpy(expected.b+base+i*width,b.b+base+((control>>(i*2))&3)*width,width);
        EMIT(&p,prefix==0?0x66:prefix==1?0xf2:0xf3,0x45,0x0f,0x70,0xc1,control);
        compare_vector(p,8,expected,"packed immediate shuffle");
    }
    const unsigned add[]={0xfc,0xfd,0xfe,0xd4},sub[]={0xf8,0xf9,0xfa,0xfb},equal[]={0x74,0x75,0x76,0x29},greater[]={0x64,0x65,0x66,0x37};
    for (unsigned size=0;size<4;size++) for(unsigned op=0;op<4;op++) {
        unsigned width=1u<<size; program p=setup(a,b,8); memset(&expected,0,sizeof(expected));
        for(unsigned i=0;i<16;i+=width) {
            uint64_t left=0,right=0,value,sign=UINT64_C(1)<<(width*8-1);
            memcpy(&left,a.b+i,width);memcpy(&right,b.b+i,width);
            value=op==0?left+right:op==1?left-right:op==2?(left==right?UINT64_MAX:0):((left^sign)>(right^sign)?UINT64_MAX:0);
            memcpy(expected.b+i,&value,width);
        }
        EMIT(&p,0x66,0x45,0x0f); if(op>=2&&size==3) EMIT(&p,0x38);
        EMIT(&p,op==0?add[size]:op==1?sub[size]:op==2?equal[size]:greater[size],0xc1);
        compare_vector(p,8,expected,"packed wrapping arithmetic and signed compare");
    }
    for(unsigned sign=0;sign<2;sign++) for(unsigned form=0;form<6;form++) {
        unsigned input=form<3?1:form<5?2:4,output=form==0?2:form==1||form==3?4:8;
        program p=setup(a,b,8); memset(&expected,0,sizeof(expected));
        for(unsigned i=0;i<16/output;i++) {
            uint64_t value=0;memcpy(&value,b.b+i*input,input);
            if(sign && (value&(UINT64_C(1)<<(input*8-1)))) value|=UINT64_MAX<<(input*8);
            memcpy(expected.b+i*output,&value,output);
        }
        EMIT(&p,0x66,0x45,0x0f,0x38,(sign?0x20:0x30)+form,0xc1);
        compare_vector(p,8,expected,"packed widening signed and unsigned");
    }
    for(unsigned opcode=0x0c;opcode<=0x0e;opcode++) {
        unsigned width=opcode==0x0c?4:opcode==0x0d?8:2; program p=setup(a,b,8);expected=a;
        for(unsigned i=0;i<16/width;i++) if((0x95>>i)&1)memcpy(expected.b+i*width,b.b+i*width,width);
        EMIT(&p,0x66,0x45,0x0f,0x3a,opcode,0xc1,0x95);compare_vector(p,8,expected,"packed immediate blend");
    }
    for(unsigned width=1;width<=8;width*=2) {
        for(unsigned index=0;index<16/width;index++) {
            uint64_t value=UINT64_C(0x8877665544332211);expected=a;
            memcpy(expected.b+index*width,&value,width);
            program p=setup(a,b,8);immediate(&p,value);
            if(width==2) { EMIT(&p,0x66,0x44,0x0f,0xc4,0xc0,index); }
            else { EMIT(&p,0x66,width==8?0x4c:0x44,0x0f,0x3a,width==1?0x20:0x22,0xc0,index); }
            compare_vector(p,8,expected,"insert GP lane preserves other lanes");
            p=setup(a,b,8);immediate(&p,UINT64_MAX);
            EMIT(&p,0x66,width==8?0x4c:0x44,0x0f,0x3a,width==1?0x14:width==2?0x15:0x16,0xc0,index);
            uint64_t answer=0;memcpy(&answer,a.b+index*width,width);
            require(execute(p,8,0,false,true)==answer,"extract GP lane zero extends narrow result");
        }
    }
    vector indices={.b={15,14,13,12,11,10,9,8,0x80,0x81,3,4,5,6,7,0xff}};
    program p=setup(a,indices,8);
    for(unsigned i=0;i<16;i++)expected.b[i]=(indices.b[i]&128)?0:a.b[indices.b[i]&15];
    EMIT(&p,0x66,0x45,0x0f,0x38,0,0xc1);compare_vector(p,8,expected,"PSHUFB byte selection and zero mask");
    vector mask={.q={UINT64_C(0x800000007fffffff),UINT64_C(0x8000000000000000)}};
    for(unsigned mode=0;mode<3;mode++) {
        unsigned width=mode==0?1:mode==1?4:8; p=setup(a,b,8);load(&p,0,mask);expected=a;
        for(unsigned i=0;i<16;i+=width)if(mask.b[i+width-1]&128)memcpy(expected.b+i,b.b+i,width);
        EMIT(&p,0x66,0x45,0x0f,0x38,mode==0?0x10:mode==1?0x14:0x15,0xc1);
        compare_vector(p,8,expected,"implicit XMM0 blend mask");
    }
}

static void packed_shifts_and_saturation(void)
{
    vector a={.q={UINT64_C(0x8000ffff7fff0100),UINT64_C(0x80000001fedcba98)}},b={0},expected;
    const unsigned counts[]={0,1,15,16,17,31,32,33,63,64,65,255};
    for(unsigned width=2;width<=8;width*=2)for(unsigned mode=0;mode<3;mode++)for(unsigned n=0;n<sizeof(counts)/sizeof(*counts);n++)for(unsigned variable=0;variable<2;variable++) {
        if(width==8&&mode==1)continue;
        unsigned count=counts[n],bits=width*8;b.q[0]=count;b.q[1]=UINT64_MAX;expected=a;
        for(unsigned i=0;i<16;i+=width) {
            uint64_t value=0;memcpy(&value,a.b+i,width);
            if(mode==1) {
                int64_t signed_input=width==2?(int16_t)value:(int32_t)value;
                value=(uint64_t)(signed_input>>(count>=bits?bits-1:count));
            } else value=count>=bits?0:mode==0?value>>count:value<<count;
            memcpy(expected.b+i,&value,width);
        }
        program p=setup(a,b,8);
        unsigned code=width==2?1:width==4?2:3;
        if(variable){EMIT(&p,0x66,0x45,0x0f,(mode==0?0xd0:mode==1?0xe0:0xf0)+code,0xc1);}
        else{EMIT(&p,0x66,0x41,0x0f,0x70+code,0xc0|((mode==0?2:mode==1?4:6)<<3),count);}
        compare_vector(p,8,expected,"packed per-lane shift boundary counts");
    }
    for(unsigned left=0;left<2;left++)for(unsigned count=0;count<=17;count++) {
        program p=setup(a,b,8);memset(&expected,0,sizeof(expected));
        if(count<16){if(left)memcpy(expected.b+count,a.b,16-count);else memcpy(expected.b,a.b+count,16-count);}
        EMIT(&p,0x66,0x41,0x0f,0x73,left?0xf8:0xd8,count);compare_vector(p,8,expected,"packed whole-register byte shift");
    }
    for(unsigned form=0;form<3;form++) {
        unsigned input=form==2?4:2,output=input/2;bool unsign=form==1;
        a.q[0]=UINT64_C(0xff7fff8080007fff);a.q[1]=UINT64_C(0x000100ff0100ffff);
        b.q[0]=UINT64_C(0x800000007fffffff);b.q[1]=UINT64_C(0x0000000080000001);
        program p=setup(a,b,8);
        for(unsigned bank=0;bank<2;bank++)for(unsigned i=0;i<16/input;i++) {
            uint64_t raw=0;memcpy(&raw,(bank?b.b:a.b)+i*input,input);
            int64_t value=input==4?(int32_t)raw:(int16_t)raw;
            int64_t low=unsign?0:-(INT64_C(1)<<(output*8-1)),high=(INT64_C(1)<<(output*8-!unsign))-1;
            if(value<low)value=low;if(value>high)value=high;
            memcpy(expected.b+bank*8+i*output,&value,output);
        }
        EMIT(&p,0x66,0x45,0x0f,form==0?0x63:form==1?0x67:0x6b,0xc1);
        compare_vector(p,8,expected,"signed and unsigned saturating pack");
    }
    program p=setup(a,b,8);EMIT(&p,0x66,0x41,0x0f,0xd7,0xc0);
    uint64_t mask=0;for(unsigned i=0;i<16;i++)mask|=(uint64_t)(a.b[i]>>7)<<i;
    require(execute(p,8,0,false,true)==mask,"packed byte sign mask");
}

static void packed_tests_and_reductions(void)
{
    const vector inputs[]={ {.q={0,0}}, {.q={UINT64_MAX,UINT64_MAX}}, {.q={0xff,0xff}}, {.q={0x100,0x1ff}} };
    for(unsigned x=0;x<4;x++)for(unsigned y=0;y<4;y++) {
        program p=setup(inputs[x],inputs[y],8);EMIT(&p,0x31,0xc0,0x83,0xf8,1,0x66,0x45,0x0f,0x38,0x17,0xc1);
        uint64_t both=(inputs[x].q[0]&inputs[y].q[0])|(inputs[x].q[1]&inputs[y].q[1]);
        uint64_t outside=(~inputs[x].q[0]&inputs[y].q[0])|(~inputs[x].q[1]&inputs[y].q[1]);
        require(execute(p,8,0,true,false)==(uint64_t)(outside==0)+((uint64_t)(both==0)<<16),"PTEST flags and clearing arithmetic flags");
    }
    vector a={.q={UINT64_C(0xff007fff80000100),UINT64_C(0x80000001000000ff)}},b={.q={UINT64_C(0x00ff80007fff0100),UINT64_C(0x7fffffff00000100)}},expected;
    for(unsigned width=1;width<=4;width*=2)for(unsigned unsign=0;unsign<2;unsign++)for(unsigned maximum=0;maximum<2;maximum++) {
        program p=setup(a,b,8);
        for(unsigned i=0;i<16;i+=width) {
            uint64_t left=0,right=0,sign=unsign?0:UINT64_C(1)<<(width*8-1),value;
            memcpy(&left,a.b+i,width);memcpy(&right,b.b+i,width);
            value=(((left^sign)<(right^sign))!=maximum)?left:right;memcpy(expected.b+i,&value,width);
        }
        EMIT(&p,0x66,0x45,0x0f);
        if(width==1&&unsign)EMIT(&p,maximum?0xde:0xda);
        else if(width==2&&!unsign)EMIT(&p,maximum?0xee:0xea);
        else {unsigned code=width==1?0x38:width==2?0x3a:unsign?0x3b:0x39;EMIT(&p,0x38,code+(maximum?4:0));}
        EMIT(&p,0xc1);compare_vector(p,8,expected,"signed and unsigned packed min/max");
    }
    for(unsigned mode=0;mode<3;mode++) {
        program p=setup(a,b,8);
        for(unsigned i=0;i<2;i++) {
            if(mode==0){expected.q[i]=0;for(unsigned j=0;j<8;j++)expected.q[i]+=(unsigned)abs((int)a.b[i*8+j]-b.b[i*8+j]);}
            else if(mode==1)expected.q[i]=(uint64_t)a.u[i*2]*b.u[i*2];
            else expected.q[i]=(uint64_t)((int64_t)(int32_t)a.u[i*2]*(int32_t)b.u[i*2]);
        }
        EMIT(&p,0x66,0x45,0x0f);if(mode==2)EMIT(&p,0x38);EMIT(&p,mode==0?0xf6:mode==1?0xf4:0x28,0xc1);
        compare_vector(p,8,expected,"packed SAD and even-dword products");
    }
    program p=setup(a,b,8);for(unsigned i=0;i<4;i++)expected.u[i]=a.u[i]*b.u[i];
    EMIT(&p,0x66,0x45,0x0f,0x38,0x40,0xc1);compare_vector(p,8,expected,"packed low dword products");
}

static void horizontal_and_insert(void)
{
    for(unsigned wide=0;wide<2;wide++)for(unsigned mode=0;mode<3;mode++) {
        vector a={0},b={0},expected={0};unsigned width=wide?8:4,lanes=16/width;
        for(unsigned i=0;i<lanes;i++){if(wide){a.d[i]=(double)i+1.25;b.d[i]=(double)i*2-5;}else{a.f[i]=(float)i+1.25f;b.f[i]=(float)i*2-5;}}
        for(unsigned i=0;i<lanes;i++) {
            unsigned index=(i%(lanes/2))*2;
            if(wide){double left=i<lanes/2?a.d[index]:b.d[index],right=i<lanes/2?a.d[index+1]:b.d[index+1];expected.d[i]=mode==2?(i&1?a.d[i]+b.d[i]:a.d[i]-b.d[i]):mode==0?left+right:left-right;}
            else{float left=i<lanes/2?a.f[index]:b.f[index],right=i<lanes/2?a.f[index+1]:b.f[index+1];expected.f[i]=mode==2?(i&1?a.f[i]+b.f[i]:a.f[i]-b.f[i]):mode==0?left+right:left-right;}
        }
        program p=setup(a,b,8);EMIT(&p,wide?0x66:0xf2,0x45,0x0f,mode==0?0x7c:mode==1?0x7d:0xd0,0xc1);
        compare_vector(p,8,expected,"horizontal add/subtract and alternating arithmetic");
    }
    vector a={.u={0x11223344,0x55667788,0x99aabbcc,0xddeeff00}},b={.u={10,20,30,40}},expected;
    for(unsigned memory=0;memory<2;memory++)for(unsigned control=0;control<256;control++) {
        program p=setup(a,b,8);expected=a;
        expected.u[(control>>4)&3]=b.u[memory?3:control>>6];
        for(unsigned i=0;i<4;i++)if((control>>i)&1)expected.u[i]=0;
        if(memory){EMIT(&p,0x48,0x83,0xec,0x18,0x44,0x0f,0x11,0x0c,0x24,0x66,0x44,0x0f,0x3a,0x21,0x44,0x24,12,control,0x48,0x83,0xc4,0x18);}
        else{EMIT(&p,0x66,0x45,0x0f,0x3a,0x21,0xc1,control);}
        compare_vector(p,8,expected,"INSERTPS source/destination lane and zero mask");
    }
}

static void reciprocal_estimates(void)
{
    const vector cases[]={ {.u={0,0x80000000,1,0x80000001}}, {.u={0x7f800000,0xff800000,0x7fc12345,0x7f812345}},
        {.f={4,-4,16,-16}}, {.u={0x7f7fffff,0x00800000,0x40400000,0x40000000}} };
    vector a={.u={0x11223344,0x55667788,0x99aabbcc,0xddeeff00}};
    for(unsigned c=0;c<sizeof(cases)/sizeof(*cases);c++)for(unsigned root=0;root<2;root++)for(unsigned scalar=0;scalar<2;scalar++)for(unsigned memory=0;memory<2;memory++) {
        program p=setup(a,cases[c],8);
        if(memory){EMIT(&p,0x48,0x83,0xec,0x18,0x44,0x0f,0x11,0x0c,0x24);}
        if(scalar)EMIT(&p,0xf3);
        EMIT(&p,memory?0x44:0x45,0x0f,root?0x52:0x53);
        if(memory){EMIT(&p,0x04,0x24,0x48,0x83,0xc4,0x18);}else{EMIT(&p,0xc1);}
        vector actual;fenv_t saved;fegetenv(&saved);
        for(unsigned half=0;half<2;half++) {
            feclearexcept(FE_ALL_EXCEPT);feraiseexcept(FE_DIVBYZERO);
            actual.q[half]=execute(p,8,half,false,false);
            require(fetestexcept(FE_ALL_EXCEPT)==FE_DIVBYZERO,"reciprocal estimates suppress FP exceptions");
        }
        fesetenv(&saved);
        for(unsigned lane=0;lane<4;lane++) {
            if(scalar&&lane){require(actual.u[lane]==a.u[lane],"scalar reciprocal preserves upper lanes");continue;}
            uint32_t bits=cases[c].u[lane],exponent=bits&0x7f800000,fraction=bits&0x7fffff;
            if(exponent==0x7f800000&&fraction)require(actual.u[lane]==(bits|0x400000),"reciprocal quiets NaN without losing payload");
            else if(!exponent)require(actual.u[lane]==((bits&0x80000000)|0x7f800000),"reciprocal handles signed zero and denormals");
            else if(root&&(bits&0x80000000))require(actual.u[lane]==0xffc00000,"reciprocal sqrt of negative normal is indefinite");
            else if(exponent==0x7f800000)require(actual.u[lane]==(bits&0x80000000),"reciprocal of infinity");
            else {
                double expected=root?1.0/sqrt((double)cases[c].f[lane]):1.0/(double)cases[c].f[lane];
                if(fabs(expected)<0x1p-126)require(actual.u[lane]==(bits&0x80000000),"reciprocal flushes tiny result");
                else require(fabs((double)actual.f[lane]-expected)<=fabs(expected)*1.5/4096,"reciprocal estimate relative accuracy");
            }
        }
    }
}

static void mxcsr_mode(program *p,uint32_t mode)
{
    EMIT(p,0x48,0x83,0xec,0x18,0xc7,0x04,0x24);bytes(p,(uint8_t *)&mode,4);
    EMIT(p,0x0f,0xae,0x14,0x24,0x48,0x83,0xc4,0x18);
}
static void denormal_modes(void)
{
    for(unsigned wide=0;wide<2;wide++)for(unsigned negative=0;negative<2;negative++)
    for(unsigned mode=0;mode<4;mode++)for(unsigned scalar=0;scalar<2;scalar++) {
        uint32_t csr=0x1f80|((mode&1)?0x40:0)|((mode&2)?0x8000:0);unsigned width=wide?8:4;
        vector a={0},b={0},expected={0};
        for(unsigned i=0;i<16/width;i++) {
            if(wide){a.d[i]=negative?-0x1p-1022:0x1p-1022;b.d[i]=0.5;expected.d[i]=scalar&&i?a.d[i]:(mode&2?copysign(0.0,a.d[i]):a.d[i]*0.5);}
            else {a.f[i]=negative?-0x1p-126f:0x1p-126f;b.f[i]=0.5f;expected.f[i]=scalar&&i?a.f[i]:(mode&2?copysignf(0.0f,a.f[i]):a.f[i]*0.5f);}
        }
        program p=setup(a,b,0);mxcsr_mode(&p,csr);if(scalar)EMIT(&p,wide?0xf2:0xf3);else if(wide)EMIT(&p,0x66);EMIT(&p,0x0f,0x59,0xc1);
        compare_vector(p,0,expected,"FTZ output and signed zero independently of DAZ");
        for(unsigned i=0;i<16/width;i++) {
            if(wide){a.d[i]=negative?-0x1p-1023:0x1p-1023;b.d[i]=4;expected.d[i]=scalar&&i?a.d[i]:(mode&1?copysign(0.0,a.d[i]):a.d[i]*4);}
            else {a.f[i]=negative?-0x1p-127f:0x1p-127f;b.f[i]=4;expected.f[i]=scalar&&i?a.f[i]:(mode&1?copysignf(0.0f,a.f[i]):a.f[i]*4);}
        }
        p=setup(a,b,0);mxcsr_mode(&p,csr);if(scalar)EMIT(&p,wide?0xf2:0xf3);else if(wide)EMIT(&p,0x66);EMIT(&p,0x0f,0x59,0xc1);
        compare_vector(p,0,expected,"DAZ input independently of FTZ");
        b=(vector){0};p=setup(a,b,0);mxcsr_mode(&p,csr);EMIT(&p,wide?0xf2:0xf3,0x0f,0xc2,0xc1,0);
        expected=a;memset(expected.b,mode&1?255:0,width);compare_vector(p,0,expected,"DAZ scalar comparisons preserve upper bits");
    }
    for(unsigned mode=0;mode<4;mode++) {
        vector a={.f={0x1p-127f,0,0,0}},b={0},expected={.d={mode&1?0:0x1p-127,0}};
        program p=setup(a,b,0);mxcsr_mode(&p,0x1f80|((mode&1)?0x40:0)|((mode&2)?0x8000:0));EMIT(&p,0xf3,0x0f,0x5a,0xc0);
        compare_vector(p,0,expected,"DAZ float to double conversion");
        a=(vector){.d={0x1p-127,0}};expected=a;expected.f[0]=mode&2?0:0x1p-127f;
        p=setup(a,b,0);mxcsr_mode(&p,0x1f80|((mode&1)?0x40:0)|((mode&2)?0x8000:0));EMIT(&p,0xf2,0x0f,0x5a,0xc0);
        compare_vector(p,0,expected,"FTZ double to float conversion");
        a=(vector){.f={0x1p-127f,1,2,3}};p=setup(a,b,0);mxcsr_mode(&p,0x1f80|((mode&1)?0x40:0)|((mode&2)?0x8000:0));EMIT(&p,0xf3,0x0f,0x5f,0xc1);
        expected=a;if(mode&1)expected.f[0]=0;compare_vector(p,0,expected,"MAX does not flush its selected operand with FTZ");
    }
}

#include "ssse3_reference.inc"

int main(void) { ssse3_and_word_multiply(); arithmetic(); comparisons(); conversions(); shuffles_and_bits(); integer_vectors(); packed_shifts_and_saturation(); packed_tests_and_reductions(); horizontal_and_insert(); reciprocal_estimates(); denormal_modes(); printf("PASS SSE floating point, conversions, flags and lane semantics (%u checks)\n",checks); return 0; }
