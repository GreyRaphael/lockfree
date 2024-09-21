#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <optional>

namespace lockfree {
template <class T, size_t BufSize>
class LockFreeQueue {
    static_assert(BufSize >= 2, "Queue size must be at least 2");
    static_assert((BufSize & (BufSize - 1)) == 0, "Queue size must be a power of 2 for efficient modulo operations");

    std::array<T, BufSize> buffer_{};
    // Align write_pos and read_pos to separate cache lines to prevent false sharing
    alignas(64) std::atomic<size_t> write_pos_{0};
    alignas(64) std::atomic<size_t> read_pos_{0};

   public:
    LockFreeQueue() noexcept = default;
    ~LockFreeQueue() noexcept = default;

    // Disable copy and move semantics
    LockFreeQueue(const LockFreeQueue&) = delete;
    LockFreeQueue& operator=(const LockFreeQueue&) = delete;
    LockFreeQueue(LockFreeQueue&&) = delete;
    LockFreeQueue& operator=(LockFreeQueue&&) = delete;

   private:
    template <typename U>
    bool do_push(U&& u) noexcept {
        size_t current_write = write_pos_.load(std::memory_order_relaxed);
        size_t current_read = read_pos_.load(std::memory_order_acquire);

        if (current_write - current_read >= BufSize) {
            return false;  // Queue is full
        }

        // Write data to the buffer
        buffer_[current_write % BufSize] = std::forward<U>(u);

        // Update the writer index with release semantics to ensure
        // that the write to buffer_ happens-before any subsequent reads by consumers
        write_pos_.store(current_write + 1, std::memory_order_release);

        return true;
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
        T value = std::move(buffer_[current_read % BufSize]);

        // Update the read position
        read_pos_.store(current_read + 1, std::memory_order_release);

        return value;
    }
};
}  // namespace lockfree