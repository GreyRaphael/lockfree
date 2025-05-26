#pragma once
#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <optional>
#include <utility>

namespace lockfree {

template <typename T, size_t BufSize>
    requires(BufSize >= 2) && ((BufSize & (BufSize - 1)) == 0)
class SPSC {
    static constexpr size_t MASK = BufSize - 1;

    std::array<T, BufSize> buffer_{};
    // Align write_pos and read_pos to separate cache lines to prevent false sharing
    alignas(64) std::atomic<size_t> write_pos_{0};
    alignas(64) std::atomic<size_t> read_pos_{0};

   public:
    SPSC() noexcept = default;
    ~SPSC() noexcept = default;

    // Disable copy and move semantics
    SPSC(const SPSC&) = delete;
    SPSC& operator=(const SPSC&) = delete;
    SPSC(SPSC&&) = delete;
    SPSC& operator=(SPSC&&) = delete;

    // push method
    template <typename U>
        requires std::constructible_from<T, U&&>
    bool push(U&& u) noexcept {
        // fetch the up-to-date read_pos once
        size_t current_write = write_pos_.load(std::memory_order_relaxed);
        size_t current_read = read_pos_.load(std::memory_order_acquire);

        // Queue is full
        if (current_write >= current_read + BufSize) return false;

        // Write data to the buffer, construct-in-place / assign
        buffer_[current_write & MASK] = std::forward<U>(u);

        // Update the writer index with release semantics to ensure
        // that the write to buffer_ happens-before any subsequent reads by consumers
        write_pos_.store(current_write + 1, std::memory_order_release);

        return true;
    }
    // Pop method
    std::optional<T> pop() noexcept {
        // fetch the up-to-date write_pos once
        size_t current_read = read_pos_.load(std::memory_order_relaxed);
        size_t current_write = write_pos_.load(std::memory_order_acquire);

        // Queue is empty
        if (current_read >= current_write) return std::nullopt;

        // Read the item from the buffer, construct the optional directly around the moved value
        std::optional<T> value{std::in_place, std::move(buffer_[current_read & MASK])};

        // Update the read position
        read_pos_.store(current_read + 1, std::memory_order_release);

        return value;
    }

    // Non-allocating pop method
    bool pop(T& out) noexcept {
        size_t current_read = read_pos_.load(std::memory_order_relaxed);
        size_t current_write = write_pos_.load(std::memory_order_acquire);

        if (current_read >= current_write) return false;

        // Move the item to the output parameter
        out = std::move(buffer_[current_read & MASK]);

        read_pos_.store(current_read + 1, std::memory_order_release);

        return true;
    }
};
}  // namespace lockfree