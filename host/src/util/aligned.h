#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <malloc.h>

namespace twin {
namespace detail {

/* 32-byte aligned, zero-initialized heap buffer. Hardware encoder APIs
 * (e.g. mfxBitstream.Data) require stronger alignment than the C++ default
 * allocator provides. */
class AlignedBytes {
public:
    AlignedBytes() = default;
    ~AlignedBytes() { Release(); }

    AlignedBytes(const AlignedBytes&) = delete;
    AlignedBytes& operator=(const AlignedBytes&) = delete;

    bool Allocate(size_t bytes) {
        Release();
        if (!bytes) return false;
        ptr_ = static_cast<uint8_t*>(_aligned_malloc(bytes, 32));
        if (!ptr_) return false;
        size_ = bytes;
        std::memset(ptr_, 0, size_);
        return true;
    }
    uint8_t* data() { return ptr_; }
    size_t size() const { return size_; }

private:
    void Release() {
        if (ptr_) {
            _aligned_free(ptr_);
            ptr_ = nullptr;
            size_ = 0;
        }
    }
    uint8_t* ptr_ = nullptr;
    size_t size_ = 0;
};

}  // namespace detail
}  // namespace twin
