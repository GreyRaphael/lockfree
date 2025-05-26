#include <atomic>
#include <chrono>
#include <csignal>
#include <format>
#include <iostream>
#include <memory>
#include <thread>

#include "queue/spmc.hpp"
#include "shm.hpp"

std::atomic<bool> running{true};

void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        running.store(false);  // Safe flag modification
    }
}

// Define the data structure to be sent through the ring buffer
struct MyData {
    int id;
    double value;
    char name[16];
};

// Constants for SPMC queue
#define BUFFER_CAPACITY 128  // Number of entries in the buffer
#define MAX_READERS 16
int main(int argc, char const *argv[]) {
    // Determine consumerId
    size_t consumerId = 0;  // Default consumerId
    if (argc >= 2) {
        consumerId = std::stoul(argv[1]);
        if (consumerId >= MAX_READERS) {
            std::cerr << "Invalid consumerId. Must be less than " << MAX_READERS << ".\n";
            return 1;
        }
    } else {
        std::cerr << "Usage: " << argv[0] << " <consumerId>\n";
        std::cerr << "Defaulting to consumerId = 0\n";
    }
    std::cout << "Consumer " << consumerId << " started. Press Ctrl+C to exit.\n";

    // Register signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    using T = MyData;
    static_assert(std::is_trivially_copyable<T>::value, "RingBuffer requires a trivially copyable type");

    auto shm_size = sizeof(lockfree::SPMC<T, BUFFER_CAPACITY, MAX_READERS, lockfree::trans::broadcast>);

    static constexpr auto RETRY_INTERVAL = std::chrono::milliseconds(100);
    std::unique_ptr<SharedMemory> shm;
    while (running.load(std::memory_order_relaxed)) {
        try {
            shm = std::make_unique<SharedMemory>("my_ring", shm_size, false);
            break;
        } catch (const std::system_error &e) {
            // shm_open failed because nobody has created it yet
            std::this_thread::sleep_for(RETRY_INTERVAL);
        }
    }

    auto ringBuffer = static_cast<lockfree::SPMC<T, BUFFER_CAPACITY, MAX_READERS, lockfree::trans::broadcast> *>(shm->ptr());

    // Start recving data from the ring buffer
    while (running.load(std::memory_order_relaxed)) {
        std::optional<T> value;
        // Attempt to pop data from the queue
        while (!(value = ringBuffer->pop(consumerId))) {
            // Queue is empty
            // Uncomment the following line for verbose empty notifications
            std::cout << "Queue is empty, consumer " << consumerId << " cannot pop.\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        // Successfully popped an item
        std::cout << std::format("Consumer {} got: id={}, value={}, name={}\n",
                                 consumerId, value.value().id, value.value().value, value.value().name);
    }

    // gracefully close without destroy the shared memory by signal
    shm->close();
}