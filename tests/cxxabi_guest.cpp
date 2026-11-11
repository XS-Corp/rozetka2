#include <cstdio>
#include <cstdlib>
#include <new>
#include <typeinfo>
#include <cstdint>

struct Left { virtual ~Left() = default; int left = 17; };
struct Right { virtual ~Right() = default; int right = 25; };
struct Derived : Left, Right { int answer() const { return left + right; } };
struct Unrelated { virtual ~Unrelated() = default; };
struct Base { virtual ~Base() = default; int value = 42; };
struct BranchA : virtual Base {};
struct BranchB : virtual Base {};
struct Diamond : BranchA, BranchB {};
struct alignas(128) Aligned { int value = 42; };
static void require(bool ok, const char *message)
{
    if (!ok) { std::fprintf(stderr, "FAIL guest C++ ABI: %s\n", message); std::abort(); }
}
__attribute__((noinline)) static void casts(Left *left, Right *right)
{
    Derived *derived = dynamic_cast<Derived *>(left);
    require(derived && derived->answer() == 42, "downcast");
    require(dynamic_cast<Right *>(left) == right, "multiple inheritance crosscast");
    require(dynamic_cast<Unrelated *>(left) == nullptr, "failed pointer cast");
    require(dynamic_cast<void *>(right) == derived, "offset to top");
    require(typeid(*left) == typeid(Derived), "dynamic type identity");
}
__attribute__((noinline)) static void virtual_cast(Base *base)
{
    Diamond *diamond = dynamic_cast<Diamond *>(base);
    require(diamond && base->value == 42, "virtual base downcast");
    require(dynamic_cast<BranchB *>(static_cast<BranchA *>(diamond)) == static_cast<BranchB *>(diamond), "diamond crosscast");
    require(typeid(*base) == typeid(Diamond), "virtual base RTTI");
}
static int order(const void *a, const void *b)
{
    int left = *(const int *)a, right = *(const int *)b;
    return (left > right) - (left < right);
}
__attribute__((noinline)) static void check_alignment(Aligned *pointer)
{
    require(pointer && (reinterpret_cast<std::uintptr_t>(pointer) & 127) == 0 && pointer->value == 42,
            "over-aligned C++17 allocation");
}
int main()
{
    int values[] = {42, -1, 5, 0, 42, -100, 7};
    std::qsort(values, 7, sizeof(int), order);
    const int sorted[] = {-100, -1, 0, 5, 7, 42, 42};
    for (int i = 0; i < 7; i++) require(values[i] == sorted[i], "guest qsort comparator");
    Derived *derived = new Derived;
    casts(derived, derived);
    Diamond *diamond = new Diamond;
    virtual_cast(diamond);
    int *array = new (std::nothrow) int[16];
    require(array != nullptr, "nothrow array allocation");
    for (int i = 0; i < 16; i++) array[i] = i;
    require(array[15] == 15, "array memory");
    delete[] array; delete diamond; delete derived;
    Aligned *aligned = new Aligned;
    check_alignment(aligned); delete aligned;
    Aligned *aligned_array = new (std::nothrow) Aligned[3];
    check_alignment(aligned_array); check_alignment(aligned_array + 2); delete[] aligned_array;
    std::puts("R2_CXXABI x86 RTTI, multiple/virtual inheritance and allocation: OK");
    return 42;
}
