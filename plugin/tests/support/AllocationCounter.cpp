#include "AllocationCounter.h"

#include <cstdlib>
#include <new>

namespace furrow::test
{
std::atomic<long> allocations { 0 };
thread_local bool counting = false;
} // namespace furrow::test

using furrow::test::allocations;
using furrow::test::counting;

// The whole scalar family is replaced, so memory from one replaced form is never released by a
// form left to the runtime. The array forms call these.
void* operator new (std::size_t size)
{
    if (counting)
        ++allocations;
    if (void* p = std::malloc (size == 0 ? 1 : size))
        return p;
    throw std::bad_alloc();
}

void* operator new (std::size_t size, const std::nothrow_t&) noexcept
{
    if (counting)
        ++allocations;
    return std::malloc (size == 0 ? 1 : size);
}

void* operator new[] (std::size_t size) { return operator new (size); }
void* operator new[] (std::size_t size, const std::nothrow_t& tag) noexcept { return operator new (size, tag); }
void operator delete (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }
void operator delete (void* p, const std::nothrow_t&) noexcept { std::free (p); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { std::free (p); }
void operator delete[] (void* p, const std::nothrow_t&) noexcept { std::free (p); }
