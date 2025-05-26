#pragma once

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>

class SharedMemory {
   public:
    /**
     * @param name   Name of the POSIX-shared-memory object (with or without leading '/').
     * @param size   Size in bytes.
     * @param create If true, creates (and truncates) the segment; otherwise opens existing.
     * @throws std::system_error on any failure.
     */
    SharedMemory(std::string_view name, std::size_t size, bool create = true)
        : name_{normalize_name(name)}, size_{size} {
        open_or_create(create);
        map();
        // once mapped, we can close the fd
        ::close(fd_);
        fd_ = -1;
    }

    ~SharedMemory() noexcept {
        // best-effort cleanup
        try {
            close();
        } catch (...) {
        }
    }

    SharedMemory(SharedMemory&& o) noexcept
        : ptr_{o.ptr_}, size_{o.size_}, name_{std::move(o.name_)}, fd_{o.fd_} {
        o.ptr_ = nullptr;
        o.fd_ = -1;
        o.size_ = 0;
    }

    SharedMemory& operator=(SharedMemory&& o) noexcept {
        if (this != &o) {
            close();
            ptr_ = o.ptr_;
            size_ = o.size_;
            name_ = std::move(o.name_);
            fd_ = o.fd_;
            o.ptr_ = nullptr;
            o.fd_ = -1;
            o.size_ = 0;
        }
        return *this;
    }

    SharedMemory(const SharedMemory&) = delete;
    SharedMemory& operator=(const SharedMemory&) = delete;

    [[nodiscard]] void* get() const noexcept { return ptr_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }

    /// Unmaps the region; does not unlink.
    void close() noexcept {
        if (ptr_) {
            munmap(ptr_, size_);
            ptr_ = nullptr;
        }
    }

    /// Closes and unlinks the shared‐memory object.
    void destroy() {
        close();
        if (!name_.empty()) {
            shm_unlink(name_.c_str());
        }
    }

   private:
    void* ptr_ = nullptr;
    std::size_t size_ = 0;
    std::string name_;
    int fd_ = -1;

    static std::string normalize_name(std::string_view raw) {
        if (raw.empty())
            throw std::invalid_argument("SharedMemory name must not be empty");
        std::string s{raw};
        if (s.front() != '/')
            s.insert(s.begin(), '/');
        return s;
    }

    void open_or_create(bool create) {
        int flags = O_RDWR;
        if (create) flags |= O_CREAT | O_TRUNC;
        // mode 0666: rw for owner, group, others
        fd_ = shm_open(name_.c_str(), flags, 0666);
        if (fd_ < 0)
            throw std::system_error(errno, std::generic_category(), "shm_open('" + name_ + "') failed");
        if (create) {
            if (ftruncate(fd_, static_cast<off_t>(size_)) < 0) {
                int saved = errno;
                ::close(fd_);
                shm_unlink(name_.c_str());
                throw std::system_error(saved, std::generic_category(), "ftruncate('" + name_ + "') failed");
            }
        }
    }

    void map() {
        ptr_ = mmap(nullptr,
                    size_,
                    PROT_READ | PROT_WRITE,
                    MAP_SHARED,
                    fd_,
                    0);
        if (ptr_ == MAP_FAILED) {
            int saved = errno;
            ::close(fd_);
            throw std::system_error(saved, std::generic_category(), "mmap('" + name_ + "') failed");
        }
    }
};
