#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <thread>

namespace lockfree {

template <typename T, size_t BufSize>
class MPSC {
    static_assert(BufSize >= 2, "Queue size must be at least 2");
    static_assert((BufSize & (BufSize - 1)) == 0, "Queue size must be a power of 2 for efficient modulo operations");
    static constexpr size_t MASK = BufSize - 1;

    std::array<T, BufSize> buffer_{};
    // Align write_pos and read_pos to separate cache lines to prevent false sharing
    alignas(64) std::atomic<size_t> write_pos_{0};
    alignas(64) std::atomic<size_t> read_pos_{0};

   public:
    MPSC() noexcept = default;
    ~MPSC() noexcept = default;

    // Disable copy and move semantics
    MPSC(const MPSC&) = delete;
    MPSC& operator=(const MPSC&) = delete;
    MPSC(MPSC&&) = delete;
    MPSC& operator=(MPSC&&) = delete;

   private:
    template <typename U>
    bool do_push(U&& u) noexcept {
        while (true) {
            size_t current_write = write_pos_.load(std::memory_order_relaxed);
            size_t current_read = read_pos_.load(std::memory_order_acquire);

            if (current_write - current_read >= BufSize) {
                return false;  // Queue is full
            }

            // Attempt to reserve the current_write position
            if (write_pos_.compare_exchange_weak(
                    current_write,
                    current_write + 1,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                // Successfully reserved the slot, perform the write
                buffer_[current_write & MASK] = std::forward<U>(u);
                return true;
            }
            // If compare_exchange_weak fails, another producer has updated write_pos_, retry
            // Optional: Add a pause or yield to reduce contention
            std::this_thread::yield();
        }
    }

   public:
    // Push method for lvalue references
    bool push(const T& t) noexcept { return do_push(t); }

    // Push method for rvalue references
    bool push(T&& t) noexcept { return do_push(std::move(t)); }
    // Pop method
    std::optional<T> pop() noexcept {
        size_t current_read = read_pos_.load(std::memory_order_relaxed);
        size_t current_write = write_pos_.load(std::memory_order_acquire);

        if (current_read >= current_write) {
            return std::nullopt;  // Queue is empty
        }

        // Read the item from the buffer
        T value = std::move(buffer_[current_read & MASK]);

        // Update the read position
        read_pos_.store(current_read + 1, std::memory_order_release);

        return value;
    }
};
}  // namespace lockfree