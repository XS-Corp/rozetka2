#include <setjmp.h>
#include <stdint.h>

static jmp_buf outer, inner, negative;
static void transfer(void) { longjmp(inner, 0); }

int main(void)
{
    int result = setjmp(outer);
    if (result == 0) {
        volatile uint64_t canary = UINT64_C(0x0123456789abcdef);
        int nested = _setjmp(inner);
        if (nested == 0) transfer();
        if (nested != 1 || canary != UINT64_C(0x0123456789abcdef)) return 10;
        int signed_result = _setjmp(negative);
        if (signed_result == 0) _longjmp(negative, -7);
        if (signed_result != -7) return 11;
        longjmp(outer, 37);
    }
    return result == 37 ? 42 : 12;
}
