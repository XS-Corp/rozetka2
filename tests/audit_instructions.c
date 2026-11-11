/* Decode compiler-disassembled instruction boundaries without executing code. */
#include "internal.h"
#include <inttypes.h>
#include <stdio.h>
int main(void)
{
    uint64_t address;
    unsigned char length, bytes[15];
    size_t total = 0, missing = 0;
    while (fread(&address, 8, 1, stdin) == 1)
    {
        if (fread(&length, 1, 1, stdin) != 1 || length == 0 || length > 15 ||
            fread(bytes, 1, 15, stdin) != 15)
            return 2;
        r2_insn insn;
        char error[R2_ERROR_CAP] = {0};
        r2_status status = r2_decode_insn_at(bytes, length, address, &insn, error, sizeof(error));
        total++;
        if (status != R2_OK || insn.x86_len != length)
        {
            printf("%" PRIx64 " %s\n", address,
                   status != R2_OK ? error : "decoded length differs from native disassembler");
            missing++;
        }
    }
    fprintf(stderr, "Decoded %zu instructions; %zu unsupported or inconsistent\n", total, missing);
    return ferror(stdin) ? 2 : 0;
}
