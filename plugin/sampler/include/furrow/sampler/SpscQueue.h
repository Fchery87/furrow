#pragma once

#include <array>
#include <atomic>
#include <cstddef>

namespace furrow::sampler
{

// A bounded queue for one producer thread and one consumer thread. Neither side waits, locks or
// allocates: push fails when the queue is full and pop fails when it is empty.
template <typename T, std::size_t Capacity>
class SpscQueue
{
public:
    bool push (const T& item)
    {
        const auto tail = tail_.load (std::memory_order_relaxed);
        const auto next = (tail + 1) % slots;
        if (next == head_.load (std::memory_order_acquire))
            return false;
        items_[tail] = item;
        tail_.store (next, std::memory_order_release);
        return true;
    }

    bool pop (T& item)
    {
        const auto head = head_.load (std::memory_order_relaxed);
        if (head == tail_.load (std::memory_order_acquire))
            return false;
        item = items_[head];
        head_.store ((head + 1) % slots, std::memory_order_release);
        return true;
    }

private:
    // One slot stays empty to tell a full queue from an empty one.
    static constexpr std::size_t slots = Capacity + 1;
    std::array<T, slots> items_ {};
    std::atomic<std::size_t> head_ { 0 };
    std::atomic<std::size_t> tail_ { 0 };
};

} // namespace furrow::sampler
