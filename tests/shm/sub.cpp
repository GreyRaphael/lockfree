// reader.cpp
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <optional>
#include <thread>
#include <type_traits>

#include "queue/spmc.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

// Define the data structure to be received through the ring buffer
struct MyData {
    int id;
    double value;
    char name[16];
};

// Constants for shared memory
#define SHARED_MEMORY_NAME "my_ring_buffer"
#define BUFFER_CAPACITY 128  // Number of entries in the buffer
#define MAX_READERS 16

#ifdef _WIN32
HANDLE hMapFile = NULL;  // Global handle for the shared memory
#else
int shm_fd = -1;  // Global file descriptor for shared memory
#endif

// Global pointers for cleanup in signal handlers
void* shared_ptr = nullptr;
size_t shm_size = 0;

// Function to open shared memory
void* open_shared_memory(size_t size) {
    void* ptr = nullptr;
#ifdef _WIN32
    hMapFile = OpenFileMapping(
        FILE_MAP_ALL_ACCESS,  // Read/write access
        FALSE,                // Do not inherit the name
        SHARED_MEMORY_NAME);  // Name of mapping object

    if (hMapFile == NULL) {
        std::cerr << "Could not open file mapping object (" << GetLastError() << ").\n";
        exit(EXIT_FAILURE);
    }

    ptr = MapViewOfFile(
        hMapFile,             // Handle to map object
        FILE_MAP_ALL_ACCESS,  // Read/write permission
        0,
        0,
        size);

    if (ptr == NULL) {
        std::cerr << "Could not map view of file (" << GetLastError() << ").\n";
        CloseHandle(hMapFile);
        exit(EXIT_FAILURE);
    }
#else
    shm_fd = shm_open(SHARED_MEMORY_NAME, O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("shm_open");
        exit(EXIT_FAILURE);
    }

    ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (ptr == MAP_FAILED) {
        perror("mmap");
        close(shm_fd);
        exit(EXIT_FAILURE);
    }

    // No need to close(fd) here as we use a global shm_fd for cleanup
#endif
    return ptr;
}

// Function to close shared memory
void close_shared_memory() {
#ifdef _WIN32
    if (shared_ptr) {
        UnmapViewOfFile(shared_ptr);
        shared_ptr = nullptr;
    }
    if (hMapFile != NULL) {
        CloseHandle(hMapFile);
        hMapFile = NULL;
    }
#else
    if (shared_ptr) {
        munmap(shared_ptr, shm_size);
        shared_ptr = nullptr;
    }
    if (shm_fd != -1) {
        close(shm_fd);
        shm_fd = -1;
    }
    // Do not unlink; the writer handles that
#endif
}

#ifdef _WIN32
// Windows console control handler
BOOL WINAPI console_handler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_CLOSE_EVENT ||
        signal == CTRL_BREAK_EVENT || signal == CTRL_LOGOFF_EVENT ||
        signal == CTRL_SHUTDOWN_EVENT) {
        std::cout << "\nTermination signal received. Cleaning up shared memory...\n";
        close_shared_memory();
        ExitProcess(0);
    }
    return TRUE;
}
#else
// POSIX signal handler
void signal_handler(int signum) {
    std::cout << "\nSignal (" << signum << ") received. Cleaning up shared memory...\n";
    close_shared_memory();
    exit(signum);
}
#endif

int main(int argc, char* argv[]) {
    using T = MyData;  // Type of data to read from the ring buffer

    // Ensure T is trivially copyable
    static_assert(std::is_trivially_copyable<T>::value, "RingBuffer requires a trivially copyable type");

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

    // Calculate the size needed for the SPMC queue
    shm_size = sizeof(lockfree::SPMC<T, BUFFER_CAPACITY, MAX_READERS, lockfree::trans::broadcast>);

    // Register signal handlers before opening shared memory
#ifdef _WIN32
    if (!SetConsoleCtrlHandler(console_handler, TRUE)) {
        std::cerr << "Could not set control handler\n";
        return 1;
    }
#else
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    // Handle SIGINT and SIGTERM
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }
#endif

    // Open and map shared memory
    shared_ptr = open_shared_memory(shm_size);

    // Access the SPMC queue in shared memory
    auto ringBuffer = static_cast<lockfree::SPMC<T, BUFFER_CAPACITY, MAX_READERS, lockfree::trans::broadcast>*>(shared_ptr);

    // Validate consumerId
    // In a more robust implementation, you might have a mechanism to assign unique consumerIds dynamically.

    std::cout << "Consumer " << consumerId << " started. Press Ctrl+C to exit.\n";

    // Start reading data from the ring buffer
    while (true) {
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

    // Cleanup (unreachable in this infinite loop, but good practice)
    close_shared_memory();
    return 0;
}
