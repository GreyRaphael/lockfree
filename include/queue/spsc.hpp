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

    // thread-local views
    size_t local_write_{0};
    size_t local_read_{0};

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
        size_t synced_read = read_pos_.load(std::memory_order_acquire);

        // Queue is full
        if (local_write_ >= synced_read + BufSize) return false;

        // Write data to the buffer, construct-in-place / assign
        buffer_[local_write_ & MASK] = std::forward<U>(u);
        ++local_write_;

        // Update the writer index with release semantics to ensure
        // that the write to buffer_ happens-before any subsequent reads by consumers
        write_pos_.store(local_write_, std::memory_order_release);

        return true;
    }
    // Pop method
    std::optional<T> pop() noexcept {
        // fetch the up-to-date write_pos once
        size_t synced_write = write_pos_.load(std::memory_order_acquire);

        // Queue is empty
        if (local_read_ >= synced_write) return std::nullopt;

        // Read the item from the buffer, construct the optional directly around the moved value
        std::optional<T> value{std::in_place, std::move(buffer_[local_read_ & MASK])};
        ++local_read_;

        // Update the read position
        read_pos_.store(local_read_, std::memory_order_release);

        return value;
    }

    // Non-allocating pop method
    bool pop(T& out) noexcept {
        size_t synced_write = write_pos_.load(std::memory_order_acquire);

        // Queue is empty
        if (local_read_ >= synced_write) return false;

        // Move the item to the output parameter
        out = std::move(buffer_[local_read_ & MASK]);
        ++local_read_;

        // Update the read position
        read_pos_.store(local_read_, std::memory_order_release);

        return true;
    }
};
}  // namespace lockfree