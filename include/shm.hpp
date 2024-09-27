#pragma once

#include <cstddef>
#include <cstring>
#include <string>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif
#include <stdexcept>

class SharedMemory {
    void* ptr_;
    size_t size_;
    std::string name_;
#ifdef _WIN32
    HANDLE hMapFile_;
#else
    int fd_;
#endif
   public:
    /**
     * @brief Constructs a SharedMemory object, creating or opening a shared memory segment.
     * @param name The name of the shared memory segment.
     * @param size The size of the shared memory segment.
     * @param create If true, creates a new shared memory segment; otherwise, opens an existing one.
     */
    SharedMemory(std::string name, size_t size, bool create = true)
        : ptr_(nullptr), size_(size), name_(std::move(name)) {
#ifdef _WIN32
        if (create) {
            hMapFile_ = CreateFileMapping(
                INVALID_HANDLE_VALUE,  // Use paging file
                NULL,                  // Default security
                PAGE_READWRITE,        // Read/write access
                0,                     // Maximum object size (high-order DWORD)
                size,                  // Maximum object size (low-order DWORD)
                name_.c_str());        // Name of mapping object

            if (hMapFile_ == NULL) {
                throw std::runtime_error("Could not create file mapping object (" + std::to_string(GetLastError()) + ").");
            }
        } else {
            hMapFile_ = OpenFileMapping(
                FILE_MAP_ALL_ACCESS,  // Read/write access
                FALSE,                // Do not inherit the name
                name_.c_str());       // Name of mapping object

            if (hMapFile_ == NULL) {
                throw std::runtime_error("Could not open file mapping object (" + std::to_string(GetLastError()) + ").");
            }
        }

        ptr_ = MapViewOfFile(
            hMapFile_,            // Handle to map object
            FILE_MAP_ALL_ACCESS,  // Read/write permission
            0,
            0,
            size_);

        if (ptr_ == NULL) {
            CloseHandle(hMapFile_);
            throw std::runtime_error("Could not map view of file (" + std::to_string(GetLastError()) + ").");
        }
#else
        if (create) {
            fd_ = shm_open(name_.c_str(), O_CREAT | O_RDWR, 0666);
            if (fd_ == -1) {
                throw std::runtime_error("shm_open failed: " + std::string(strerror(errno)));
            }

            if (ftruncate(fd_, size_) == -1) {
                ::close(fd_);
                shm_unlink(name_.c_str());
                throw std::runtime_error("ftruncate failed: " + std::string(strerror(errno)));
            }
        } else {
            fd_ = shm_open(name_.c_str(), O_RDWR, 0666);
            if (fd_ == -1) {
                throw std::runtime_error("shm_open failed: " + std::string(strerror(errno)));
            }
        }

        ptr_ = mmap(NULL, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (ptr_ == MAP_FAILED) {
            ::close(fd_);
            if (create) {
                shm_unlink(name_.c_str());
            }
            throw std::runtime_error("mmap failed: " + std::string(strerror(errno)));
        }

        ::close(fd_);  // File descriptor is no longer needed
        fd_ = -1;      // Invalidate file descriptor
#endif
    }

    /**
     * @brief Destructor that ensures resources are properly released.
     */
    ~SharedMemory() { close(); }

    // Disable copy constructor and assignment operator
    SharedMemory(const SharedMemory&) = delete;
    SharedMemory& operator=(const SharedMemory&) = delete;

    /**
     * @brief Gets a pointer to the shared memory.
     * @return Pointer to the shared memory.
     */
    void* get() const { return ptr_; }

    /**
     * @brief Gets the size of the shared memory.
     * @return Size of the shared memory.
     */
    size_t size() const { return size_; }

    /**
     * @brief Gets the name of the shared memory segment.
     * @return Name of the shared memory segment.
     */
    const std::string& name() const { return name_; }

    /**
     * @brief Closes the shared memory mapping without destroying it.
     */
    void close() {
        if (ptr_) {
#ifdef _WIN32
            UnmapViewOfFile(ptr_);
            ptr_ = nullptr;

            if (hMapFile_ != NULL) {
                CloseHandle(hMapFile_);
                hMapFile_ = NULL;
            }
#else
            munmap(ptr_, size_);
            ptr_ = nullptr;
#endif
        }
    }

    /**
     * @brief Destroys the shared memory object.
     */
    void destroy() {
#ifdef _WIN32
        // No equivalent of shm_unlink is needed on Windows; just close the handles
        close();
#else
        close();
        shm_unlink(name_.c_str());
#endif
    }
};
