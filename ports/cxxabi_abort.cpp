/* Fatal diagnostics for the guest LLVM guard implementation. */
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include "abort_message.h"

void abort_message(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    std::vfprintf(stderr, format, arguments);
    va_end(arguments);
    std::fputc('\n', stderr);
    std::abort();
}
