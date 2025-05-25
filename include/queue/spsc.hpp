#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <utility>

namespace lockfree {
template <typename T, size_t BufSize>
class SPSC {
    static_assert(BufSize >= 2, "Queue size must be at least 2");
    static_assert((BufSize & (BufSize - 1)) == 0, "Queue size must be a power of 2 for efficient modulo operations");
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

   private:
    template <typename U>
    bool do_push(U&& u) noexcept {
        size_t current_write = write_pos_.load(std::memory_order_relaxed);
        size_t current_read = read_pos_.load(std::memory_order_acquire);

        // Queue is full
        if (current_write - current_read >= BufSize) return false;

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

        // Queue is empty
        if (current_read >= current_write) return false;

        // Move the item to the output parameter
        out = std::move(buffer_[current_read & MASK]);

        // Update the read position
        read_pos_.store(current_read + 1, std::memory_order_release);

        return true;
    }
};
}  // namespace lockfree