#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <format>
#include <iostream>
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
    // Register signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    using T = MyData;  // Type of data to store in the ring buffer
    static_assert(std::is_trivially_copyable_v<T>, "RingBuffer requires a trivially copyable type");

    // Calculate the size needed for the SPMC queue
    auto shm_size = sizeof(lockfree::SPMC<T, BUFFER_CAPACITY, MAX_READERS, lockfree::trans::broadcast>);
    // Create and map shared memory
    auto shm = SharedMemory("my_ring", shm_size, true);
    auto shm_ptr = shm.get();
    // Construct the SPMC queue in shared memory using placement new
    auto ringBuffer = new (shm_ptr) lockfree::SPMC<T, BUFFER_CAPACITY, MAX_READERS, lockfree::trans::broadcast>();

    // Start writing data into the ring buffer
    size_t index = 0;
    while (running.load(std::memory_order_relaxed)) {
        // Prepare data to write
        T myData;
        myData.id = static_cast<int>(index);
        myData.value = index * 0.1;
        snprintf(myData.name, sizeof(myData.name), "Data%zu", index);

        // Attempt to push data into the ring buffer
        while (!ringBuffer->push(myData)) {
            std::cout << "Queue is full, cannot push. Retrying...\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::cout << std::format("Writer wrote: id={}, value={}, name={}\n", myData.id, myData.value, myData.name);
        ++index;

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    // gracefully cleanup with signal
    shm.destroy();
}