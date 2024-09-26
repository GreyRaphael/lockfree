#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <limits>
#include <optional>
#include <thread>

#include "def.hpp"

namespace lockfree {

// Create the general template
template <typename T, size_t BufSize, size_t MaxReaderNum, trans Ts>
class MPMC;

// Specialization for trans::broadcast
template <typename T, size_t BufSize, size_t MaxReaderNum>
class MPMC<T, BufSize, MaxReaderNum, trans::broadcast> {
    static_assert(BufSize >= 2, "Queue size must be at least 2");
    static_assert((BufSize & (BufSize - 1)) == 0, "Queue size must be a power of 2 for efficient modulo operations");
    static constexpr size_t MASK = BufSize - 1;
    static_assert(MaxReaderNum >= 1, "MaxReaderNum must be at least 1");

    std::array<T, BufSize> buffer_{};
    // Align write_pos and read_pos to separate cache lines to prevent false sharing
    alignas(64) std::atomic<size_t> write_pos_{0};
    std::array<std::atomic<size_t>, MaxReaderNum> read_positions_{};

   public:
    MPMC() noexcept = default;
    ~MPMC() noexcept = default;

    // Disable copy and move semantics
    MPMC(const MPMC&) = delete;
    MPMC& operator=(const MPMC&) = delete;
    MPMC(MPMC&&) = delete;
    MPMC& operator=(MPMC&&) = delete;

   private:
    template <typename U>
    bool do_push(U&& u) noexcept {
        while (true) {
            size_t current_write = write_pos_.load(std::memory_order_relaxed);

            // Get the minimum reader index to determine how much the buffer has been consumed
            size_t min_reader_index = std::numeric_limits<size_t>::max();  // SIZE_MAX
            for (size_t i = 0; i < MaxReaderNum; ++i) {
                size_t reader_index = read_positions_[i].load(std::memory_order_acquire);
                if (reader_index < min_reader_index) {
                    min_reader_index = reader_index;
                }
            }

            if (current_write - min_reader_index >= BufSize) {
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
    std::optional<T> pop(size_t consumerId) noexcept {
        // attention, consumerId must < MaxReaderNum
        size_t current_read = read_positions_[consumerId].load(std::memory_order_relaxed);
        size_t current_write = write_pos_.load(std::memory_order_acquire);

        // Check if there's data to read
        if (current_read >= current_write) {
            return std::nullopt;  // Queue is empty
        }

        // Read data and update reader index
        T value = std::move(buffer_[current_read & MASK]);
        read_positions_[consumerId].store(current_read + 1, std::memory_order_release);
        return value;
    }
};

// Specialization for trans::unicast
template <typename T, size_t BufSize, size_t MaxReaderNum>
class MPMC<T, BufSize, MaxReaderNum, trans::unicast> {
    static_assert(BufSize >= 2, "Queue size must be at least 2");
    static_assert((BufSize & (BufSize - 1)) == 0, "Queue size must be a power of 2 for efficient modulo operations");
    static constexpr size_t MASK = BufSize - 1;

    // alignas(std::hardware_constructive_interference_size)
    // Align the buffer to cache lines for better cache performance
    std::array<T, BufSize> buffer_{};
    // or alignas(std::hardware_destructive_interference_size)
    // Align write_pos and read_pos to separate cache lines to prevent false sharing
    alignas(64) std::atomic<size_t> write_pos_{0};
    alignas(64) std::atomic<size_t> read_pos_{0};

   public:
    MPMC() noexcept = default;
    ~MPMC() noexcept = default;

    // Disable copy and move semantics
    MPMC(const MPMC&) = delete;
    MPMC& operator=(const MPMC&) = delete;
    MPMC(MPMC&&) = delete;
    MPMC& operator=(MPMC&&) = delete;

   private:
    template <typename U>
    bool do_push(U&& u) noexcept {
        size_t current_write;
        while (true) {
            current_write = write_pos_.load(std::memory_order_relaxed);
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
                // Ensure the write to the buffer happens before any subsequent reads
                std::atomic_thread_fence(std::memory_order_release);
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
        size_t current_read;
        // Attempt to claim the next item in the buffer
        while (true) {
            current_read = read_pos_.load(std::memory_order_relaxed);
            size_t current_write = write_pos_.load(std::memory_order_acquire);
            if (current_read >= current_write) {
                return std::nullopt;  // Queue is empty
            }

            // Attempt to increment read_pos_ atomically
            // Use memory_order_acq_rel for successful operations,
            //  and memory_order_relaxed for failures to minimize overhead.
            if (read_pos_.compare_exchange_weak(
                    current_read,
                    current_read + 1,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                // Beat other consumers to  Successfully claimed the item at current_read
                T item = std::move(buffer_[current_read & MASK]);
                // Ensure that the read from the buffer happens after the load
                std::atomic_thread_fence(std::memory_order_acquire);
                return item;
            }
            std::this_thread::yield();
        }
    }
};
}  // namespace lockfree