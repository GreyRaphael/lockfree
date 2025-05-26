#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#endif

class SharedMemory {
   public:
    SharedMemory(const std::string& name, std::size_t size, bool create = true)
        : name_{normalize_name(name)}, size_{size} {
#ifdef _WIN32
        open_or_create_win(create);
        map_win();
#else
        open_or_create_linux(create);
        map_linux();
        ::close(fd_);
        fd_ = -1;
#endif
    }

    ~SharedMemory() noexcept {
        try {
            close();
        } catch (...) {
        }
    }

    SharedMemory(SharedMemory&& o) noexcept
        : ptr_{o.ptr_}, size_{o.size_}, name_{std::move(o.name_)}
#ifdef _WIN32
          ,
          hMap_{o.hMap_}
#else
          ,
          fd_{o.fd_}
#endif
    {
        o.ptr_ = nullptr;
#ifdef _WIN32
        o.hMap_ = nullptr;
#else
        o.fd_ = -1;
#endif
        o.size_ = 0;
    }

    SharedMemory& operator=(SharedMemory&& o) noexcept {
        if (this != &o) {
            close();
            ptr_ = o.ptr_;
            size_ = o.size_;
            name_ = std::move(o.name_);
#ifdef _WIN32
            hMap_ = o.hMap_;
            o.hMap_ = nullptr;
#else
            fd_ = o.fd_;
            o.fd_ = -1;
#endif
            o.ptr_ = nullptr;
            o.size_ = 0;
        }
        return *this;
    }

    SharedMemory(const SharedMemory&) = delete;
    SharedMemory& operator=(const SharedMemory&) = delete;

    [[nodiscard]] void* get() const noexcept { return ptr_; }

    void close() noexcept {
        if (ptr_) {
#ifdef _WIN32
            UnmapViewOfFile(ptr_);
            ptr_ = nullptr;
            if (hMap_) {
                CloseHandle(hMap_);
                hMap_ = nullptr;
            }
#else
            munmap(ptr_, size_);
            ptr_ = nullptr;
#endif
        }
    }

    void destroy() {
        close();
#ifndef _WIN32
        shm_unlink(name_.c_str());
#endif
    }

   private:
    void* ptr_ = nullptr;
    std::size_t size_ = 0;
    std::string name_;

#ifdef _WIN32
    HANDLE hMap_ = nullptr;
#else
    int fd_ = -1;
#endif

    static std::string normalize_name(const std::string& raw) {
#ifdef _WIN32
        return raw;  // Windows doesn't require a leading slash
#else
        if (raw.empty()) throw std::invalid_argument("SharedMemory name must not be empty");
        return raw.front() == '/' ? raw : '/' + raw;
#endif
    }

#ifdef _WIN32
    void open_or_create_win(bool create) {
        if (create) {
            hMap_ = CreateFileMappingA(
                INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                DWORD(size_ >> 32), DWORD(size_ & 0xFFFFFFFF), name_.c_str());
            if (!hMap_) throw std::system_error(GetLastError(), std::system_category(), "CreateFileMapping failed");
        } else {
            hMap_ = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name_.c_str());
            if (!hMap_) throw std::system_error(GetLastError(), std::system_category(), "OpenFileMapping failed");
        }
    }

    void map_win() {
        ptr_ = MapViewOfFile(hMap_, FILE_MAP_ALL_ACCESS, 0, 0, size_);
        if (!ptr_) {
            CloseHandle(hMap_);
            hMap_ = nullptr;
            throw std::system_error(GetLastError(), std::system_category(), "MapViewOfFile failed");
        }
    }
#else
    void open_or_create_linux(bool create) {
        int flags = O_RDWR | (create ? O_CREAT | O_TRUNC : 0);
        fd_ = shm_open(name_.c_str(), flags, 0666);
        if (fd_ < 0) throw std::system_error(errno, std::generic_category(), "shm_open failed");

        if (create && ftruncate(fd_, size_) < 0) {
            int saved = errno;
            ::close(fd_);
            shm_unlink(name_.c_str());
            throw std::system_error(saved, std::generic_category(), "ftruncate failed");
        }
    }

    void map_linux() {
        ptr_ = mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (ptr_ == MAP_FAILED) {
            int saved = errno;
            ::close(fd_);
            throw std::system_error(saved, std::generic_category(), "mmap failed");
        }
    }
#endif
};
