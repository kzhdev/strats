#pragma once

//#include <hf/hf.h>
#include <hf/mem/DataBlock.h>
#include <atomic>
#include "RingBufferQueue.h"

namespace hf {
namespace queue {

template<typename T = Python::CacheLineBuf>
class LockFreeQueue : public hf::mem::DataBlock {
    std::atomic_uint_fast64_t* _reserved_cursor;
    std::atomic_uint_fast64_t* _published_cursor;
    uint_fast64_t _size;
    uint_fast64_t _mask;
    T* _data = nullptr;

public:
    bool createQueue(const std::string &id, uint32_t size, bool shm = false, const std::string &name = "",
                    DataBlock::ShmExitMode shmExitMode = DataBlock::ShmExitMode::SHMDT) {
        assert(size && !(size & (size -1))); // size must be power of 2

        if (!size) return false;
        _size = size;
        const ssize_t sizeWithBuffer = 2 * CACHE_LINE_SZ + sizeof(T) * size + 2000 * 1024ull + CACHE_LINE_SZ;
        //
        auto result = createBlock(id, sizeWithBuffer, shm, name, shmExitMode);
        if (result == DataBlock::Result::FAILED) return false;
        //
        _data = reinterpret_cast<T*>((reinterpret_cast<uint8_t*>(getP()) + 2 * CACHE_LINE_SZ));
        _reserved_cursor = reinterpret_cast<std::atomic_uint_fast64_t*>(getP());
        _published_cursor = reinterpret_cast<std::atomic_uint_fast64_t*>((reinterpret_cast<uint8_t*>(getP()) + CACHE_LINE_SZ));

        if (result == DataBlock::Result::SUCCESS) {
            _reserved_cursor->store(0, std::memory_order_relaxed);
            _published_cursor->store(0, std::memory_order_relaxed);
        }

        printf("recerved: %p\n", _reserved_cursor);
        return true;
    }

    uint32_t capacity() const noexcept {
        return _size;
    }

    void clear() noexcept {
        DataBlock::clear();
    }

    uint_fast64_t reserve(uint_fast64_t num = 1) noexcept {
        return _reserved_cursor->fetch_add(num);
    }

    T& operator[](uint_fast64_t index) noexcept {
        return _data[index & _mask];
    }

    const T& operator[](uint_fast64_t index) const noexcept {
        return _data[index & _mask];
    }

    void publish(uint_fast64_t index, uint_fast64_t num = 1) const noexcept {
        while (_published_cursor->load(std::memory_order_relaxed) != index);
        _published_cursor->fetch_add(num, std::memory_order_acq_rel);
    }

    uint64_t end() const noexcept { return _published_cursor->load(std::memory_order_relaxed); }
};

}}