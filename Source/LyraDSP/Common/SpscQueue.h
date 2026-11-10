#pragma once
#include <array>
#include <atomic>
#include <cstddef>

namespace lyra
{
// Bounded single-producer/single-consumer FIFO for trivially copyable items.
// Producer (audio thread) never blocks or allocates: push() drops the item when full.
template <typename T, size_t Capacity>
class SpscQueue
{
    static_assert ((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    static_assert (std::atomic<size_t>::is_always_lock_free);

public:
    bool push (const T& item) noexcept
    {
        const size_t w = write.load (std::memory_order_relaxed);
        if (w - read.load (std::memory_order_acquire) == Capacity)
            return false;
        items[w & (Capacity - 1)] = item;
        write.store (w + 1, std::memory_order_release);
        return true;
    }

    bool pop (T& out) noexcept
    {
        const size_t r = read.load (std::memory_order_relaxed);
        if (r == write.load (std::memory_order_acquire))
            return false;
        out = items[r & (Capacity - 1)];
        read.store (r + 1, std::memory_order_release);
        return true;
    }

private:
    std::array<T, Capacity> items {};
    alignas (64) std::atomic<size_t> write { 0 };
    alignas (64) std::atomic<size_t> read { 0 };
};
} // namespace lyra
