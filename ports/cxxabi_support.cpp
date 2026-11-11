/* Allocation for the guest ABI subset, built with exceptions disabled.
 * Allocation failure terminates in guest code; C++ unwinding is not provided. */
#include <cstdlib>
#include <cstddef>
#include <new>

#ifndef R2_FULL_LIBCXX
namespace std { const nothrow_t nothrow; }
#endif

__attribute__((weak)) void *operator new(std::size_t size)
{
    void *pointer = std::malloc(size ? size : 1);
    if (pointer == nullptr) std::abort();
    return pointer;
}
__attribute__((weak)) void *operator new[](std::size_t size) { return ::operator new(size); }
__attribute__((weak)) void operator delete(void *pointer) noexcept { std::free(pointer); }
__attribute__((weak)) void operator delete[](void *pointer) noexcept { std::free(pointer); }
__attribute__((weak)) void *operator new(std::size_t size, const std::nothrow_t &) noexcept
{ return std::malloc(size ? size : 1); }
__attribute__((weak)) void *operator new[](std::size_t size, const std::nothrow_t &) noexcept
{ return std::malloc(size ? size : 1); }
__attribute__((weak)) void operator delete(void *pointer, const std::nothrow_t &) noexcept { std::free(pointer); }
__attribute__((weak)) void operator delete[](void *pointer, const std::nothrow_t &) noexcept { std::free(pointer); }
static void *aligned_allocate(std::size_t size, std::align_val_t alignment) noexcept
{
    std::size_t bytes = size ? size : 1, boundary = static_cast<std::size_t>(alignment);
    if (boundary <= alignof(std::max_align_t)) return std::malloc(bytes);
    void *pointer = nullptr;
    return posix_memalign(&pointer, boundary, bytes) == 0 ? pointer : nullptr;
}
__attribute__((weak)) void *operator new(std::size_t size, std::align_val_t alignment)
{
    void *pointer = aligned_allocate(size, alignment);
    if (pointer == nullptr) std::abort();
    return pointer;
}
__attribute__((weak)) void *operator new[](std::size_t size, std::align_val_t alignment) { return ::operator new(size, alignment); }
__attribute__((weak)) void *operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t &) noexcept
{ return aligned_allocate(size, alignment); }
__attribute__((weak)) void *operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t &) noexcept
{ return aligned_allocate(size, alignment); }
__attribute__((weak)) void operator delete(void *pointer, std::align_val_t) noexcept { std::free(pointer); }
__attribute__((weak)) void operator delete[](void *pointer, std::align_val_t) noexcept { std::free(pointer); }
__attribute__((weak)) void operator delete(void *pointer, std::size_t, std::align_val_t) noexcept { std::free(pointer); }
__attribute__((weak)) void operator delete[](void *pointer, std::size_t, std::align_val_t) noexcept { std::free(pointer); }
__attribute__((weak)) void operator delete(void *pointer, std::align_val_t, const std::nothrow_t &) noexcept { std::free(pointer); }
__attribute__((weak)) void operator delete[](void *pointer, std::align_val_t, const std::nothrow_t &) noexcept { std::free(pointer); }
#ifndef R2_FULL_LIBCXX
namespace std { void terminate() noexcept { std::abort(); } }
#endif
extern "C" void __cxa_pure_virtual() { std::abort(); }
extern "C" [[noreturn]] void __cxa_bad_cast() { std::abort(); }
extern "C" [[noreturn]] void __cxa_bad_typeid() { std::abort(); }
extern "C" [[noreturn]] void __cxa_throw_bad_array_new_length() { std::abort(); }
