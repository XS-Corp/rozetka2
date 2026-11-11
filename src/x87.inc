/* Exact integer significands for x87 state, comparisons and remainders.
 * ARM's long double is binary64, so it cannot represent x87 intermediates. */
#define X87_INTEGER UINT64_C(0x8000000000000000)
#define X87_QUIET UINT64_C(0x4000000000000000)
static r2_x87_state *machine_x87(r2_machine *machine)
{
    return machine->env && machine->env->native ? r2_native_x87_state(machine->env->native) : &machine->x87;
}
static unsigned x87_top(const r2_x87_state *state) { return (state->status >> 11) & 7; }
static void x87_top_set(r2_x87_state *state,unsigned top) { state->status=(state->status&~0x3800u)|((top&7)<<11); }
static r2_x87_value x87_indefinite(void) { return (r2_x87_value){X87_INTEGER|X87_QUIET,0xffff}; }
static r2_x87_value x87_get(r2_x87_state *state,unsigned index)
{
    unsigned physical=(x87_top(state)+index)&7;
    if (!(state->valid&(1u<<physical))) {
        state->status=(state->status&~0x200u)|0x41; return x87_indefinite();
    }
    return state->registers[physical];
}
static void x87_push(r2_x87_state *state,r2_x87_value value)
{
    unsigned top=(x87_top(state)-1)&7;
    state->status&=~0x200u;
    if(state->valid&(1u<<top)) { state->status|=0x241;value=x87_indefinite(); }
    x87_top_set(state,top);state->registers[top]=value;state->valid|=1u<<top;
}
static void x87_pop(r2_x87_state *state)
{
    unsigned top=x87_top(state);state->valid&=~(1u<<top);x87_top_set(state,top+1);
}
static r2_x87_value x87_from_binary(r2_x87_state *state,uint64_t bits,unsigned fraction,unsigned exponent_bits)
{
    unsigned max_exponent=(1u<<exponent_bits)-1,bias=max_exponent>>1;
    uint16_t sign=(uint16_t)(bits>>(fraction+exponent_bits))<<15;
    uint64_t significand=bits&((UINT64_C(1)<<fraction)-1);
    unsigned exponent=(unsigned)(bits>>fraction)&max_exponent;
    if(exponent==max_exponent) {
        if(significand&&!(significand&(UINT64_C(1)<<(fraction-1))))state->status|=1;
        return (r2_x87_value){X87_INTEGER|(significand<<(63-fraction))|(significand?X87_QUIET:0),(uint16_t)(sign|0x7fff)};
    }
    if(!exponent&&!significand)return (r2_x87_value){0,sign};
    int power;
    if(exponent) { significand|=UINT64_C(1)<<fraction;power=(int)exponent-(int)bias; }
    else {state->status|=2;power=1-(int)bias;}
    unsigned highest=63-(unsigned)__builtin_clzll(significand);
    power+=(int)highest-(int)fraction;
    return (r2_x87_value){significand<<(63-highest),(uint16_t)(sign|(power+16383))};
}
static uint64_t x87_round_shift(uint64_t significand,unsigned shift,bool *inexact,bool *rounded_up)
{
    if(!shift)return significand;
    if(shift>=64) { *inexact|=significand!=0;*rounded_up=shift==64&&significand>X87_INTEGER;return *rounded_up; }
    uint64_t mask=(UINT64_C(1)<<shift)-1,discarded=significand&mask,result=significand>>shift;
    uint64_t halfway=UINT64_C(1)<<(shift-1);*inexact|=discarded!=0;
    *rounded_up=discarded>halfway||(discarded==halfway&&(result&1));return result+*rounded_up;
}
static uint64_t x87_to_binary(r2_x87_state *state,r2_x87_value value,unsigned fraction,unsigned exponent_bits)
{
    unsigned max_exponent=(1u<<exponent_bits)-1,bias=max_exponent>>1;
    uint64_t sign=(uint64_t)(value.exponent>>15)<<(fraction+exponent_bits);
    uint64_t mask=(UINT64_C(1)<<fraction)-1;
    unsigned exponent=value.exponent&0x7fff;
    if(exponent&&!(value.significand&X87_INTEGER)) {state->status|=1;value=x87_indefinite();exponent=0x7fff;sign=UINT64_C(1)<<(fraction+exponent_bits);}
    if(exponent==0x7fff) {
        uint64_t payload=value.significand&~X87_INTEGER;
        if(payload) {
            if(!(payload&X87_QUIET))state->status|=1;
            payload=(payload>>(63-fraction))|(UINT64_C(1)<<(fraction-1));
        }
        return sign|((uint64_t)max_exponent<<fraction)|(payload&mask);
    }
    if(!value.significand)return sign;
    int power=(exponent?(int)exponent:1)-16383;
    unsigned normalize=(unsigned)__builtin_clzll(value.significand);
    uint64_t significand=value.significand<<normalize;power-=(int)normalize;
    int minimum=1-(int)bias;unsigned shift=63-fraction;
    if(power<minimum)shift+=(unsigned)(minimum-power);
    bool inexact=false,rounded_up=false;uint64_t rounded=x87_round_shift(significand,shift,&inexact,&rounded_up);
    if(rounded_up)state->status|=0x200;
    if(inexact)state->status|=0x20;
    if(power<minimum) {
        if(inexact&&rounded<(UINT64_C(1)<<fraction))state->status|=0x10;
        return sign|rounded;
    }
    if(rounded==(UINT64_C(1)<<(fraction+1))) {rounded>>=1;power++;}
    if(power>(int)bias) {state->status|=0x28;return sign|((uint64_t)max_exponent<<fraction);}
    return sign|((uint64_t)(power+(int)bias)<<fraction)|(rounded&mask);
}
static r2_x87_value x87_from_remainder(uint64_t remainder,int scale,uint16_t sign)
{
    if(!remainder)return (r2_x87_value){0,sign};
    unsigned shift=(unsigned)__builtin_clzll(remainder);int power=scale+63-(int)shift;
    remainder<<=shift;
    if(power< -16382) {
        unsigned denormal_shift=(unsigned)(-16382-power);
        return (r2_x87_value){denormal_shift<64?remainder>>denormal_shift:0,sign};
    }
    return (r2_x87_value){remainder,(uint16_t)(sign|(power+16383))};
}
static bool x87_nan(r2_x87_value value) { return (value.exponent&0x7fff)==0x7fff&&(value.significand&X87_INTEGER)&&(value.significand&~X87_INTEGER); }
static void x87_compare_flags(r2_machine *machine, r2_x87_state *state, const r2_insn *instruction)
{
    /* Intel SDM 2A, FCOMI/FCOMIP/FUCOMI/FUCOMIP: all supported control
     * modes mask exceptions; these comparisons do not change C0/C2/C3. */
    r2_x87_value left=x87_get(state,0),right=x87_get(state,instruction->src);
    unsigned le=left.exponent&0x7fff,re=right.exponent&0x7fff;
    bool ln=x87_nan(left),rn=x87_nan(right);
    bool invalid=(le&&!(left.significand&X87_INTEGER))||(re&&!(right.significand&X87_INTEGER))||
        (ln&&(instruction->imm||!(left.significand&X87_QUIET)))||
        (rn&&(instruction->imm||!(right.significand&X87_QUIET)));
    bool unordered=ln||rn||invalid;
    if(invalid)state->status|=1;
    int relation=0;
    bool lzero=!le&&!left.significand,rzero=!re&&!right.significand;
    if(!unordered&&!(lzero&&rzero)) {
        bool ls=(left.exponent&0x8000)!=0,rs=(right.exponent&0x8000)!=0;
        /* Pseudo-denormals have the same value as exponent-one normals. */
        unsigned lp=le?le:1,rp=re?re:1;
        int magnitude=lzero?-1:rzero?1:lp<rp?-1:lp>rp?1:
            left.significand<right.significand?-1:left.significand>right.significand?1:0;
        relation=ls!=rs?(ls?-1:1):ls?-magnitude:magnitude;
    }
    machine->flags.zf=unordered||relation==0;
    machine->flags.pf=unordered;machine->flags.cf=unordered||relation<0;
    machine->flags.of=machine->flags.sf=machine->flags.af=false;
    state->status&=~0x200u;
    if(instruction->has_imm)x87_pop(state);
}
static void x87_remainder(r2_x87_state *state,bool nearest)
{
    r2_x87_value left=x87_get(state,0),right=x87_get(state,1),result=left;
    unsigned top=x87_top(state);uint64_t quotient=0;
    if(x87_nan(left)||x87_nan(right)) {
        result=x87_nan(left)?left:right;
        if((x87_nan(left)&&!(left.significand&X87_QUIET))||(x87_nan(right)&&!(right.significand&X87_QUIET)))state->status|=1;
        result.significand|=X87_QUIET;
    } else if(((left.exponent&0x7fff)==0x7fff)||!right.significand||
              ((left.exponent&0x7fff)&&!(left.significand&X87_INTEGER))||
              ((right.exponent&0x7fff)&&!(right.significand&X87_INTEGER))) {
        state->status|=1;result=x87_indefinite();
    } else if(!left.significand&&!(right.exponent&0x7fff)) {
        state->status|=2;
    } else if(left.significand&&(right.exponent&0x7fff)!=0x7fff) {
        int left_power=((left.exponent&0x7fff)?(left.exponent&0x7fff):1)-16383;
        int right_power=((right.exponent&0x7fff)?(right.exponent&0x7fff):1)-16383;
        if(!(left.exponent&0x7fff)||!(right.exponent&0x7fff))state->status|=2;
        unsigned lshift=(unsigned)__builtin_clzll(left.significand),rshift=(unsigned)__builtin_clzll(right.significand);
        uint64_t a=left.significand<<lshift,b=right.significand<<rshift;
        left_power-=(int)lshift;right_power-=(int)rshift;
        int difference=left_power-right_power;uint16_t sign=left.exponent&0x8000;
        if(difference>=64) {
            /* Intel permits any partial-reduction width from 32 to 63. */
            __uint128_t scaled=(__uint128_t)a<<32;
            uint64_t remainder=(uint64_t)(scaled%b);
            result=x87_from_remainder(remainder,left_power-95,sign);
            state->status|=0x400;state->registers[top]=result;return;
        }
        if(difference>=0) {
            __uint128_t scaled=(__uint128_t)a<<(unsigned)difference;
            __uint128_t q=scaled/b;uint64_t remainder=(uint64_t)(scaled%b);
            if(nearest&&((__uint128_t)remainder*2>b||((__uint128_t)remainder*2==b&&(q&1)))) {
                q++;remainder=b-remainder;sign^=0x8000;
            }
            quotient=(uint64_t)q;result=x87_from_remainder(remainder,right_power-63,sign);
        } else if(nearest&&difference== -1&&a>b) {
            quotient=1;result=x87_from_remainder((uint64_t)((__uint128_t)b*2-a),left_power-63,sign^0x8000);
        }
    }
    /* Hardware exposes the quotient magnitude; libm applies its sign itself. */
    state->status=(state->status&~0x4700u)|((quotient&4)?0x100:0)|((quotient&2)?0x4000:0)|((quotient&1)?0x200:0);
    state->registers[top]=result;state->valid|=1u<<top;
}
static void x87_save(const r2_x87_state *state,uint8_t *header,uint8_t *registers)
{
    memcpy(header,&state->control,2);memcpy(header+2,&state->status,2);header[4]=state->valid;
    for(unsigned i=0;i<8;i++) {
        const r2_x87_value *value=&state->registers[(x87_top(state)+i)&7];
        memcpy(registers+i*16,&value->significand,8);memcpy(registers+i*16+8,&value->exponent,2);
    }
}
static void x87_restore(r2_x87_state *state,const uint8_t *header,const uint8_t *registers)
{
    memcpy(&state->control,header,2);memcpy(&state->status,header+2,2);state->valid=header[4];
    for(unsigned i=0;i<8;i++) {
        r2_x87_value *value=&state->registers[(x87_top(state)+i)&7];
        memcpy(&value->significand,registers+i*16,8);memcpy(&value->exponent,registers+i*16+8,2);
    }
}
static r2_status execute_x87(r2_machine *machine,const r2_insn *instruction,char *error,size_t error_len)
{
    r2_x87_state *state=machine_x87(machine);unsigned top=x87_top(state),other=(top+instruction->src)&7;
    uint64_t address=instruction->mem.present?eval_mem_addr(instruction,machine):0,bits=0;
    r2_x87_value value;uint8_t *memory;
    switch(instruction->subop) {
    case R2_X87_LOAD:
        memory=memory_pointer(machine,address,instruction->width,false,error,error_len);if(!memory)return R2_ERR_UNSUPPORTED;
        if(instruction->width==10) {memcpy(&value.significand,memory,8);memcpy(&value.exponent,memory+8,2);}
        else {memcpy(&bits,memory,instruction->width);value=x87_from_binary(state,bits,instruction->width==8?52:23,instruction->width==8?11:8);}
        x87_push(state,value);break;
    case R2_X87_STORE:
        /* Resolve memory before changing any FP state, so a fault retries cleanly. */
        memory=memory_pointer(machine,address,instruction->width,true,error,error_len);if(!memory)return R2_ERR_UNSUPPORTED;
        state->status&=~0x200u;value=x87_get(state,0);
        if(instruction->width==10) {memcpy(memory,&value.significand,8);memcpy(memory+8,&value.exponent,2);}
        else {bits=x87_to_binary(state,value,instruction->width==8?52:23,instruction->width==8?11:8);memcpy(memory,&bits,instruction->width);}
        if(instruction->has_imm)x87_pop(state);break;
    case R2_X87_LOAD_REG:value=x87_get(state,instruction->src);x87_push(state,value);break;
    case R2_X87_STORE_REG:
        state->status&=~0x200u;value=x87_get(state,0);state->registers[other]=value;state->valid|=1u<<other;
        if(instruction->has_imm)x87_pop(state);break;
    case R2_X87_EXCHANGE: {
        value=x87_get(state,0);r2_x87_value second=x87_get(state,instruction->src);
        state->registers[top]=second;state->registers[other]=value;state->valid|=(1u<<top)|(1u<<other);state->status&=~0x200u;break;
    }
    case R2_X87_FREE:state->valid&=~(1u<<other);break;
    case R2_X87_ZERO:x87_push(state,(r2_x87_value){0,0});break;
    case R2_X87_COMPARE_FLAGS:x87_compare_flags(machine,state,instruction);break;
    case R2_X87_REMAINDER:case R2_X87_REMAINDER_NEAREST:x87_remainder(state,instruction->subop==R2_X87_REMAINDER_NEAREST);break;
    case R2_X87_STATUS:
        if(instruction->mem.present)return write_mem(machine,address,2,state->status,error,error_len);
        machine->regs[R2_X86_RAX]=(machine->regs[R2_X86_RAX]&~UINT64_C(0xffff))|state->status;break;
    case R2_X87_LOAD_CONTROL:
        if(read_mem(machine,address,2,&bits,error,error_len)!=R2_OK)return R2_ERR_UNSUPPORTED;
        if((bits&0x0c3f)!=0x003f) {r2_set_error(error,error_len,"unsupported x87 exception or rounding control mode");return R2_ERR_UNSUPPORTED;}
        state->control=(uint16_t)bits;break;
    case R2_X87_STORE_CONTROL:return write_mem(machine,address,2,state->control,error,error_len);
    case R2_X87_INIT:memset(state,0,sizeof(*state));state->control=0x037f;break;
    case R2_X87_CLEAR:state->status&=~0x80ffu;break;
    default:return R2_ERR_UNSUPPORTED;
    }
    return R2_OK;
}
