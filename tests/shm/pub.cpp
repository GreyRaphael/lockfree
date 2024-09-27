// writer.cpp
#include <chrono>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
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

// Define the data structure to be sent through the ring buffer
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

// Function to create shared memory
void* create_shared_memory(size_t size) {
    void* ptr = nullptr;
#ifdef _WIN32
    hMapFile = CreateFileMappingA(
        INVALID_HANDLE_VALUE,      // Use paging file
        NULL,                      // Default security
        PAGE_READWRITE,            // Read/write access
        0,                         // Maximum object size (high-order DWORD)
        static_cast<DWORD>(size),  // Maximum object size (low-order DWORD)
        SHARED_MEMORY_NAME);       // Name of mapping object

    if (hMapFile == NULL) {
        std::cerr << "Could not create file mapping object (" << GetLastError() << ").\n";
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
    shm_fd = shm_open(SHARED_MEMORY_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("shm_open");
        exit(EXIT_FAILURE);
    }

    if (ftruncate(shm_fd, size) == -1) {
        perror("ftruncate");
        shm_unlink(SHARED_MEMORY_NAME);
        exit(EXIT_FAILURE);
    }

    ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (ptr == MAP_FAILED) {
        perror("mmap");
        shm_unlink(SHARED_MEMORY_NAME);
        exit(EXIT_FAILURE);
    }

    // No need to close(fd) here as we use a global shm_fd for cleanup
#endif
    return ptr;
}

// Function to destroy shared memory
void destroy_shared_memory() {
#ifdef _WIN32
    if (shared_ptr) {
        UnmapViewOfFile(shared_ptr);
        shared_ptr = nullptr;
    }
    if (hMapFile != NULL) {
        CloseHandle(hMapFile);
        hMapFile = NULL;
    }
    // Optionally, you can use UnlinkOrphanedFileMapping, but it's not required here
#else
    if (shared_ptr) {
        // Assuming you have access to the ringBuffer pointer
        // Replace 'ringBuffer' with the actual pointer if needed
        // ringBuffer->~SPMC<T, BUFFER_CAPACITY, MAX_READERS, trans::broadcast>();
        munmap(shared_ptr, shm_size);
        shared_ptr = nullptr;
    }
    if (shm_fd != -1) {
        close(shm_fd);
        shm_fd = -1;
    }
    shm_unlink(SHARED_MEMORY_NAME);
#endif
}

#ifdef _WIN32
// Windows console control handler
BOOL WINAPI console_handler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_CLOSE_EVENT ||
        signal == CTRL_BREAK_EVENT || signal == CTRL_LOGOFF_EVENT ||
        signal == CTRL_SHUTDOWN_EVENT) {
        std::cout << "Ctrl+C pressed. Cleaning up shared memory...\n";
        destroy_shared_memory();
        ExitProcess(0);
    }
    return TRUE;
}
#else
// POSIX signal handler
void signal_handler(int signum) {
    std::cout << "\nSignal (" << signum << ") received. Cleaning up shared memory...\n";
    destroy_shared_memory();
    exit(signum);
}
#endif

int main() {
    using T = MyData;  // Type of data to store in the ring buffer

    // Ensure T is trivially copyable
    static_assert(std::is_trivially_copyable<T>::value, "RingBuffer requires a trivially copyable type");

    // Calculate the size needed for the SPMC queue
    shm_size = sizeof(lockfree::SPMC<T, BUFFER_CAPACITY, MAX_READERS, lockfree::trans::broadcast>);

    // Register signal handlers before creating shared memory
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

    // Create and map shared memory
    shared_ptr = create_shared_memory(shm_size);

    // Construct the SPMC queue in shared memory using placement new
    auto ringBuffer = new (shared_ptr) lockfree::SPMC<T, BUFFER_CAPACITY, MAX_READERS, lockfree::trans::broadcast>();

    // Start writing data into the ring buffer
    size_t index = 0;
    while (true) {
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

    // Cleanup (unreachable in this infinite loop, but good practice)
    destroy_shared_memory();
    return 0;
}
