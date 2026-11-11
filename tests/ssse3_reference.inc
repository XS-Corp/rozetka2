/* AArch64 SIMD provides an independent hardware oracle for x86 integer lanes. */
#include <arm_neon.h>
static vector packed_reference(unsigned op,vector a,vector b,unsigned control)
{
    uint8x16_t x=vld1q_u8(a.b),y=vld1q_u8(b.b),z=vdupq_n_u8(0);vector result;
    int16x8_t aw=vreinterpretq_s16_u8(x),bw=vreinterpretq_s16_u8(y);
    int32x4_t ad=vreinterpretq_s32_u8(x),bd=vreinterpretq_s32_u8(y);
    switch(op) {
    case 0:z=vqtbl1q_u8(x,vandq_u8(y,vdupq_n_u8(0x8f)));break;
    case 1:z=vreinterpretq_u8_s16(vpaddq_s16(aw,bw));break;
    case 2:z=vreinterpretq_u8_s32(vpaddq_s32(ad,bd));break;
    case 3:z=vreinterpretq_u8_s16(vqaddq_s16(vuzp1q_s16(aw,bw),vuzp2q_s16(aw,bw)));break;
    case 4: {
        int16x8_t lo=vmulq_s16(vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(x))),vmovl_s8(vget_low_s8(vreinterpretq_s8_u8(y))));
        int16x8_t hi=vmulq_s16(vreinterpretq_s16_u16(vmovl_u8(vget_high_u8(x))),vmovl_s8(vget_high_s8(vreinterpretq_s8_u8(y))));
        z=vreinterpretq_u8_s16(vcombine_s16(vqmovn_s32(vpaddlq_s16(lo)),vqmovn_s32(vpaddlq_s16(hi))));break;
    }
    case 5:z=vreinterpretq_u8_s16(vsubq_s16(vuzp1q_s16(aw,bw),vuzp2q_s16(aw,bw)));break;
    case 6:z=vreinterpretq_u8_s32(vsubq_s32(vuzp1q_s32(ad,bd),vuzp2q_s32(ad,bd)));break;
    case 7:z=vreinterpretq_u8_s16(vqsubq_s16(vuzp1q_s16(aw,bw),vuzp2q_s16(aw,bw)));break;
    case 8:z=vbslq_u8(vcltq_s8(vreinterpretq_s8_u8(y),vdupq_n_s8(0)),vreinterpretq_u8_s8(vnegq_s8(vreinterpretq_s8_u8(x))),vandq_u8(x,vmvnq_u8(vceqq_u8(y,vdupq_n_u8(0)))));break;
    case 9:z=vreinterpretq_u8_u16(vbslq_u16(vcltq_s16(bw,vdupq_n_s16(0)),vreinterpretq_u16_s16(vnegq_s16(aw)),vandq_u16(vreinterpretq_u16_u8(x),vmvnq_u16(vceqq_s16(bw,vdupq_n_s16(0))))));break;
    case 10:z=vreinterpretq_u8_u32(vbslq_u32(vcltq_s32(bd,vdupq_n_s32(0)),vreinterpretq_u32_s32(vnegq_s32(ad)),vandq_u32(vreinterpretq_u32_u8(x),vmvnq_u32(vceqq_s32(bd,vdupq_n_s32(0))))));break;
    case 11:z=vreinterpretq_u8_s16(vcombine_s16(vrshrn_n_s32(vmull_s16(vget_low_s16(aw),vget_low_s16(bw)),15),vrshrn_n_s32(vmull_s16(vget_high_s16(aw),vget_high_s16(bw)),15)));break;
    case 0x1c:z=vreinterpretq_u8_s8(vabsq_s8(vreinterpretq_s8_u8(y)));break;
    case 0x1d:z=vreinterpretq_u8_s16(vabsq_s16(bw));break;
    case 0x1e:z=vreinterpretq_u8_s32(vabsq_s32(bd));break;
    case 0xe0:z=vrhaddq_u8(x,y);break;
    case 0xe3:z=vreinterpretq_u8_u16(vrhaddq_u16(vreinterpretq_u16_u8(x),vreinterpretq_u16_u8(y)));break;
    case 0xd8:z=vqsubq_u8(x,y);break;
    case 0xd9:z=vreinterpretq_u8_u16(vqsubq_u16(vreinterpretq_u16_u8(x),vreinterpretq_u16_u8(y)));break;
    case 0xdc:z=vqaddq_u8(x,y);break;
    case 0xdd:z=vreinterpretq_u8_u16(vqaddq_u16(vreinterpretq_u16_u8(x),vreinterpretq_u16_u8(y)));break;
    case 0xe8:z=vreinterpretq_u8_s8(vqsubq_s8(vreinterpretq_s8_u8(x),vreinterpretq_s8_u8(y)));break;
    case 0xe9:z=vreinterpretq_u8_s16(vqsubq_s16(aw,bw));break;
    case 0xec:z=vreinterpretq_u8_s8(vqaddq_s8(vreinterpretq_s8_u8(x),vreinterpretq_s8_u8(y)));break;
    case 0xed:z=vreinterpretq_u8_s16(vqaddq_s16(aw,bw));break;
    case 0xd5:z=vreinterpretq_u8_s16(vmulq_s16(aw,bw));break;
    case 0xe4:z=vreinterpretq_u8_u16(vcombine_u16(vshrn_n_u32(vmull_u16(vget_low_u16(vreinterpretq_u16_u8(x)),vget_low_u16(vreinterpretq_u16_u8(y))),16),vshrn_n_u32(vmull_u16(vget_high_u16(vreinterpretq_u16_u8(x)),vget_high_u16(vreinterpretq_u16_u8(y))),16)));break;
    case 0xe5:z=vreinterpretq_u8_s16(vcombine_s16(vshrn_n_s32(vmull_s16(vget_low_s16(aw),vget_low_s16(bw)),16),vshrn_n_s32(vmull_s16(vget_high_s16(aw),vget_high_s16(bw)),16)));break;
    case 0xf5: {
        int32x4_t lo=vmull_s16(vget_low_s16(aw),vget_low_s16(bw)),hi=vmull_s16(vget_high_s16(aw),vget_high_s16(bw));
        z=vreinterpretq_u8_s32(vpaddq_s32(lo,hi));break;
    }
    case 0x0f: {
        unsigned __int128 left,right,answer=0;memcpy(&left,a.b,16);memcpy(&right,b.b,16);
        if(control==0)answer=right;else if(control<16)answer=(right>>(control*8))|(left<<(128-control*8));else if(control<32)answer=left>>((control-16)*8);
        memcpy(result.b,&answer,16);return result;
    }
    default:require(false,"reference opcode");
    }
    vst1q_u8(result.b,z);return result;
}
static program packed_program(unsigned op,vector a,vector b,unsigned control,unsigned extended,bool memory,bool alias)
{
    unsigned dst=extended?8:0;program p=setup(a,b,dst);
    if(memory) {EMIT(&p,0x48,0x83,0xec,0x18);if(extended)EMIT(&p,0x44);EMIT(&p,0x0f,0x11,alias?0x04:0x0c,0x24);}
    EMIT(&p,0x31,0xdb,0x83,0xfb,1);EMIT(&p,0x66);if(extended)EMIT(&p,memory?0x44:0x45);EMIT(&p,0x0f);
    if(op<0x80)EMIT(&p,op==0x0f?0x3a:0x38);
    EMIT(&p,op,memory?0x04:alias?0xc0:0xc1);if(memory)EMIT(&p,0x24);if(op==0x0f)EMIT(&p,control);
    if(memory)EMIT(&p,0x48,0x8d,0x64,0x24,0x18);return p;
}
static void ssse3_and_word_multiply(void)
{
    const unsigned ops[]={0,1,2,3,4,5,6,7,8,9,10,11,0x1c,0x1d,0x1e,0xd5,0xe4,0xe5,0xf5,0xe0,0xe3,0xd8,0xd9,0xdc,0xdd,0xe8,0xe9,0xec,0xed};uint64_t random=0xa87f16829bcde234;
    for(unsigned trial=0;trial<64;trial++) {
        vector a,b;for(unsigned i=0;i<16;i++){random=random*6364136223846793005ULL+1;a.b[i]=(uint8_t)(random>>32);random=random*6364136223846793005ULL+1;b.b[i]=(uint8_t)(random>>32);}
        if(trial==0)memset(a.b,0,16),memset(b.b,0,16);
        if(trial==1)memset(a.b,0x80,16),memset(b.b,0x80,16);
        if(trial==2)for(unsigned i=0;i<16;i+=2){a.b[i]=b.b[i]=0;a.b[i+1]=b.b[i+1]=0x80;}
        if(trial==3)for(unsigned i=0;i<4;i++)a.u[i]=b.u[i]=0x80000000;
        if(trial==4||trial==5)memset(a.b,255,16),memset(b.b,trial==4?127:128,16);
        if(trial==6){const int16_t av[]={32767,1,-32768,-1,32767,-32768,16384,-16384},bv[]={1,-1,32767,-32768,-16384,16384,-1,1};memcpy(a.b,av,16);memcpy(b.b,bv,16);}
        if(trial==7){a=(vector){.u={0x7fffffff,1,0x80000000,1}};b=a;}
        if(trial==8)memset(b.b,0,16);
        for(unsigned k=0;k<sizeof(ops)/sizeof(ops[0]);k++)for(unsigned ext=0;ext<2;ext++)for(unsigned mem=0;mem<2;mem++)for(unsigned alias=0;alias<2;alias++) {
            program p=packed_program(ops[k],a,b,0,ext,mem,alias);vector expected=packed_reference(ops[k],a,alias?a:b,0);
            compare_vector(p,ext?8:0,expected,"SSSE3/SSE2 integer lanes against native NEON");
            require(execute(p,ext?8:0,0,true,false)==(UINT64_C(1)|(UINT64_C(1)<<24)|(UINT64_C(1)<<32)),"SSSE3 and word multiply preserve EFLAGS");
        }
    }
    vector a={.q={0x0123456789abcdefULL,0xfedcba9876543210ULL}},b={.q={0x1020304050607080ULL,0x90a0b0c0d0e0f000ULL}};
    for(unsigned control=0;control<256;control++)for(unsigned ext=0;ext<2;ext++)for(unsigned mem=0;mem<2;mem++)for(unsigned alias=0;alias<2;alias++) {
        program p=packed_program(0x0f,a,b,control,ext,mem,alias);compare_vector(p,ext?8:0,packed_reference(0x0f,a,alias?a:b,control),"PALIGNR all byte counts, aliasing and memory");
    }
}
