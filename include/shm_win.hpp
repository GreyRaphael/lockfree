#pragma once

#include <windows.h>

#include <string>
#include <system_error>
#include <utility>

class SharedMemoryWindows {
   public:
    /**
     * @param name   Name of the memory mapping object.
     * @param size   Size in bytes.
     * @param create If true, creates (or replaces) the mapping; otherwise opens an existing one.
     * @throws std::system_error on any Win32 failure.
     */
    SharedMemoryWindows(std::wstring name, std::size_t size, bool create = true)
        : name_{std::move(name)}, size_{size} {
        open_or_create(create);
        map();
    }

    ~SharedMemoryWindows() noexcept {
        // best‐effort cleanup
        try {
            close();
        } catch (...) {
        }
    }

    SharedMemoryWindows(SharedMemoryWindows&& o) noexcept
        : ptr_{o.ptr_}, size_{o.size_}, name_{std::move(o.name_)}, hMap_{o.hMap_} {
        o.ptr_ = nullptr;
        o.hMap_ = nullptr;
        o.size_ = 0;
    }

    SharedMemoryWindows& operator=(SharedMemoryWindows&& o) noexcept {
        if (this != &o) {
            close();
            ptr_ = o.ptr_;
            size_ = o.size_;
            name_ = std::move(o.name_);
            hMap_ = o.hMap_;
            o.ptr_ = nullptr;
            o.hMap_ = nullptr;
            o.size_ = 0;
        }
        return *this;
    }

    SharedMemoryWindows(const SharedMemoryWindows&) = delete;
    SharedMemoryWindows& operator=(const SharedMemoryWindows&) = delete;

    [[nodiscard]] void* get() const noexcept { return ptr_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] const std::wstring& name() const noexcept { return name_; }

    /// Unmaps and closes the handle.
    void close() noexcept {
        if (ptr_) {
            UnmapViewOfFile(ptr_);
            ptr_ = nullptr;
        }
        if (hMap_) {
            CloseHandle(hMap_);
            hMap_ = nullptr;
        }
    }

    /// Alias for close() — no separate unlink on Win32.
    void destroy() { close(); }

   private:
    void* ptr_ = nullptr;
    std::size_t size_ = 0;
    std::wstring name_;
    HANDLE hMap_ = nullptr;

    void open_or_create(bool create) {
        if (create) {
            hMap_ = CreateFileMappingW(
                INVALID_HANDLE_VALUE,       // paging file
                nullptr,                    // default security
                PAGE_READWRITE,             // read/write
                DWORD(size_ >> 32),         // high-order DWORD of max size
                DWORD(size_ & 0xFFFFFFFF),  // low-order DWORD
                name_.c_str());
            if (!hMap_)
                throw std::system_error(
                    std::error_code(GetLastError(), std::system_category()),
                    "CreateFileMappingW failed for \"" + std::string(name_.begin(), name_.end()) + '"');
        } else {
            hMap_ = OpenFileMappingW(
                FILE_MAP_ALL_ACCESS,  // read/write
                FALSE,                // do not inherit
                name_.c_str());
            if (!hMap_)
                throw std::system_error(
                    std::error_code(GetLastError(), std::system_category()),
                    "OpenFileMappingW failed for \"" + std::string(name_.begin(), name_.end()) + '"');
        }
    }

    void map() {
        ptr_ = MapViewOfFile(
            hMap_,                // handle
            FILE_MAP_ALL_ACCESS,  // read/write
            0, 0, size_);
        if (!ptr_) {
            auto ec = std::error_code(GetLastError(), std::system_category());
            CloseHandle(hMap_);
            hMap_ = nullptr;
            throw std::system_error(ec,
                                    "MapViewOfFile failed for \"" + std::string(name_.begin(), name_.end()) + '"');
        }
    }
};
