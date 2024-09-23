#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <thread>

#include "def.hpp"
namespace lockfree {

// Create the general template
template <typename T, size_t BufSize, size_t MaxReaderNum, trans Ts>
class SPMC;

// Specialization for trans::broadcast
template <typename T, size_t BufSize, size_t MaxReaderNum>
class SPMC<T, BufSize, MaxReaderNum, trans::broadcast> {
};

// Specialization for trans::unicast
template <typename T, size_t BufSize, size_t MaxReaderNum>
class SPMC<T, BufSize, MaxReaderNum, trans::unicast> {
    static_assert(BufSize >= 2, "Queue size must be at least 2");
    static_assert((BufSize & (BufSize - 1)) == 0, "Queue size must be a power of 2 for efficient modulo operations");
    static constexpr size_t MASK = BufSize - 1;

    std::array<T, BufSize> buffer_{};
    // Align write_pos and read_pos to separate cache lines to prevent false sharing
    alignas(64) std::atomic<size_t> write_pos_{0};
    alignas(64) std::atomic<size_t> read_pos_{0};

   public:
    SPMC() noexcept = default;
    ~SPMC() noexcept = default;

    // Disable copy and move semantics
    SPMC(const SPMC&) = delete;
    SPMC& operator=(const SPMC&) = delete;
    SPMC(SPMC&&) = delete;
    SPMC& operator=(SPMC&&) = delete;

   private:
    template <typename U>
    bool do_push(U&& u) noexcept {
        size_t current_write = write_pos_.load(std::memory_order_relaxed);
        size_t current_read = read_pos_.load(std::memory_order_acquire);

        if (current_write - current_read >= BufSize) {
            return false;  // Queue is full
        }

        // Write data to the buffer
        buffer_[current_write & MASK] = std::forward<U>(u);

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
        size_t current_read;
        size_t current_write;
        // Attempt to claim the next item in the buffer
        while (true) {
            current_read = read_pos_.load(std::memory_order_relaxed);
            current_write = write_pos_.load(std::memory_order_acquire);
            if (current_read >= current_write) {
                return std::nullopt;  // Queue is empty
            }

            if (read_pos_.compare_exchange_weak(
                    current_read,
                    current_read + 1,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                // Beat other consumers to  Successfully claimed the item at current_read
                T item = std::move(buffer_[current_read & MASK]);
                return item;
            }
            std::this_thread::yield();
        }
    }
};
}  // namespace lockfree