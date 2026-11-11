#ifndef R2_X86_SIGNAL_H
#define R2_X86_SIGNAL_H
/* Public Darwin x86_64 ABI. Keep these separate from the ARM host's headers.
 * Verified against an x86_64 SDK build; tests also use its actual ucontext_t. */
#define R2_X86_UC_SIZE 56u
#define R2_X86_MC_SIZE 712u
#define R2_X86_UC_MASK 4u
#define R2_X86_UC_STACK 8u
#define R2_X86_UC_MCSIZE 40u
#define R2_X86_UC_MCONTEXT 48u
#define R2_X86_MC_THREAD 16u
#define R2_X86_MC_RIP 144u
#define R2_X86_MC_RFLAGS 152u
#define R2_X86_MC_CS 160u
#define R2_X86_MC_FLOAT 184u
#define R2_X86_MC_FCW 192u
#define R2_X86_MC_FSW 194u
#define R2_X86_MC_FTW 196u
#define R2_X86_MC_MXCSR 216u
#define R2_X86_MC_MXCSR_MASK 220u
#define R2_X86_MC_XMM 352u
#define R2_X86_MC_STMM 224u
#endif
