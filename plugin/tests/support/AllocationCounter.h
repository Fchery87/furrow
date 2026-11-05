#pragma once

#include <atomic>

// Counts heap allocations made on a thread while its `counting` flag is set. The test binary
// replaces the global allocation functions to do this (AllocationCounter.cpp).
namespace furrow::test
{
extern std::atomic<long> allocations;
extern thread_local bool counting;

// Counts allocations for as long as it lives.
struct CountAllocations
{
    CountAllocations()
    {
        allocations = 0;
        counting = true;
    }
    ~CountAllocations() { counting = false; }
    CountAllocations (const CountAllocations&) = delete;
    CountAllocations& operator= (const CountAllocations&) = delete;
};
} // namespace furrow::test
