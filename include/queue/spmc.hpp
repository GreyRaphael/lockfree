#pragma once
#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <optional>
#include <thread>

#include "def.hpp"

namespace lockfree {

static constexpr size_t UPDATE_INTERVAL = 64;

// Create the general template
template <typename T, size_t BufSize, size_t MaxReaders, trans Ts>
class SPMC;

// Specialization for trans::broadcast
template <typename T, size_t BufSize, size_t MaxReaders>
    requires(BufSize > UPDATE_INTERVAL) && ((BufSize & (BufSize - 1)) == 0) && (MaxReaders >= 1)
class SPMC<T, BufSize, MaxReaders, trans::broadcast> {
    static constexpr size_t MASK = BufSize - 1;

    std::array<T, BufSize> buffer_{};
    // Align write_pos and read_pos to separate cache lines to prevent false sharing
    alignas(64) std::atomic<size_t> write_pos_{0};
    std::array<std::atomic<size_t>, MaxReaders> read_positions_{};

    // Optional: cache a “min reader” to avoid full scans every push
    size_t min_read_cache_{0};

   public:
    SPMC() noexcept = default;
    ~SPMC() noexcept = default;

    // Disable copy and move semantics
    SPMC(const SPMC&) = delete;
    SPMC& operator=(const SPMC&) = delete;
    SPMC(SPMC&&) = delete;
    SPMC& operator=(SPMC&&) = delete;

    template <typename U>
        requires std::constructible_from<T, U&&>
    bool push(U&& u) noexcept {
        size_t current_write = write_pos_.load(std::memory_order_relaxed);
        // occasionally refresh min_read_cache_
        if ((current_write & (UPDATE_INTERVAL - 1)) == 0) {
            // Get the minimum reader index to determine how much the buffer has been consumed
            size_t min_reader_index = SIZE_MAX;
            for (size_t i = 0; i < MaxReaders; ++i) {
                size_t reader_index = read_positions_[i].load(std::memory_order_acquire);
                if (reader_index < min_reader_index) {
                    min_reader_index = reader_index;
                }
            }
            min_read_cache_ = min_reader_index;
        }

        // Queue is full
        if (current_write - min_read_cache_ >= BufSize) return false;

        // Write data to the buffer
        buffer_[current_write & MASK] = std::forward<U>(u);

        // Update the writer index with release semantics to ensure
        // that the write to buffer_ happens-before any subsequent reads by consumers
        write_pos_.store(current_write + 1, std::memory_order_release);

        return true;
    }

    template <typename U>
        requires std::constructible_from<T, U&&>
    bool push_overwrite(U&& u) noexcept {
        size_t current_write = write_pos_.load(std::memory_order_relaxed);

        // Overwrite data in the buffer
        buffer_[current_write & MASK] = std::forward<U>(u);

        // Update the writer index with release semantics
        write_pos_.store(current_write + 1, std::memory_order_release);

        return true;
    }

    // Pop method
    std::optional<T> pop(size_t consumerId) noexcept {
        // attention, consumerId must < MaxReaders
        size_t current_read = read_positions_[consumerId].load(std::memory_order_relaxed);
        size_t current_write = write_pos_.load(std::memory_order_acquire);

        // Check if there's data to read, Queue is empty
        if (current_read >= current_write) return std::nullopt;

        // Read data and update reader index
        // as there are many reader, it cannot use std::move()
        T value = buffer_[current_read & MASK];
        read_positions_[consumerId].store(current_read + 1, std::memory_order_release);
        return value;
    }

    // Pop method with overwrite
    std::optional<T> pop_overwrite(size_t consumerId) noexcept {
        size_t current_read = read_positions_[consumerId].load(std::memory_order_relaxed);
        size_t current_write = write_pos_.load(std::memory_order_acquire);

        // Check if the consumer has missed data
        if (current_read + BufSize <= current_write) {
            // Data has been overwritten
            current_read = current_write - BufSize + 1;
            read_positions_[consumerId].store(current_read, std::memory_order_release);
            return std::nullopt;  // Indicate data loss
        }

        // Check if there's data to read, Queue is empty
        if (current_read >= current_write) return std::nullopt;
        // Read data and update reader index
        T value = buffer_[current_read & MASK];
        read_positions_[consumerId].store(current_read + 1, std::memory_order_release);
        return value;
    }

    size_t get_read_pos(size_t consumerId) noexcept {
        return read_positions_[consumerId].load(std::memory_order_acquire);
    }

    void set_read_pos(size_t consumerId, size_t pos) noexcept {
        read_positions_[consumerId].store(pos, std::memory_order_release);
    }

    void fetch_sub_read_pos(size_t consumerId, size_t val) {
        read_positions_[consumerId].fetch_sub(val, std::memory_order_acq_rel);
    }

    void fetch_add_read_pos(size_t consumerId, size_t val) {
        read_positions_[consumerId].fetch_add(val, std::memory_order_acq_rel);
    }
};

// Specialization for trans::unicast
template <typename T, size_t BufSize, size_t MaxReaders>
    requires(BufSize >= 2) && ((BufSize & (BufSize - 1)) == 0)
class SPMC<T, BufSize, MaxReaders, trans::unicast> {
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

    template <typename U>
        requires std::constructible_from<T, U&&>
    bool push(U&& u) noexcept {
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