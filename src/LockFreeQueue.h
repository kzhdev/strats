#pragma once

#include <hf/hf.h>
#include <hf/mem/DataBlock.h>
#include <atomic>
#include "RingBufferQueue.h"
#include <hf/log.h>
#include <boost/format.hpp>

namespace hf {
namespace queue {

LOCAL_EXPLICIT_LOG_DECLARE("LockFreeQueue.h", LockFreeQueueSpace)

template<typename T = Python::CacheLineBuf>
class LockFreeQueue : public hf::mem::DataBlock {
    struct slot {
        uint_fast64_t _num;
        std::atomic_uint_fast64_t _id {0};
        T _data;
    };

    std::atomic_uint_fast64_t* _reserved_cursor;
    uint_fast64_t _size;
    uint_fast64_t _mask;
    slot* _data = nullptr;

public:
    bool createQueue(const std::string &id, uint32_t size, bool shm = false, const std::string &name = "",
                    DataBlock::ShmExitMode shmExitMode = DataBlock::ShmExitMode::SHMDT) {
        assert(size && !(size & (size -1))); // size must be power of 2

        if (!size) return false;
        _size = size;
        _mask = size - 1;
        const ssize_t sizeWithBuffer = CACHE_LINE_SZ + sizeof(slot) * size + 2000 * 1024ull;
        //
        auto result = createBlock(id, sizeWithBuffer, shm, name, shmExitMode);
        if (result == DataBlock::Result::FAILED) return false;
        //
        _data = reinterpret_cast<slot*>((reinterpret_cast<uint8_t*>(getP()) + CACHE_LINE_SZ));

        if (result == DataBlock::Result::SUCCESS) {
            _reserved_cursor = new (getP()) std::atomic_uint_fast64_t(0);
            // invalidate the first slot
            _data[0]._id.store(1, std::memory_order_relaxed);
        } else {
            _reserved_cursor = reinterpret_cast<std::atomic_uint_fast64_t*>(getP());
        }

        return true;
    }

    uint32_t capacity() const noexcept {
        return _size;
    }

    void clear() noexcept {
        DataBlock::clear();
        // invalidate the first slot
        _data[0]._id.store(1, std::memory_order_relaxed);
        _reserved_cursor->store(0, std::memory_order_release);
    }

    uint_fast64_t reserve(uint_fast64_t num = 1) noexcept {
        return _reserved_cursor->fetch_add(num);
    }

    T* operator[](uint_fast64_t index) noexcept {
        return &_data[index & _mask]._data;
    }

    const T* operator[](uint_fast64_t index) const noexcept {
        return &_data[index & _mask]._data;
    }

    void publish(uint_fast64_t index, uint_fast64_t num = 1) const noexcept {
        _data[index & _mask]._num = num;
        _data[index & _mask]._id.store(index, std::memory_order_release);
    }

    /**
     * Get next available data
     * @param The reader's cursor
     * @return A valid pointer to the data if new data available, otherwise null_ptr
     *
     * NOTE: If a data available, the cursor will be advanced
     */
    T* next(uint_fast64_t& cursor) noexcept {
        auto reserved = _reserved_cursor->load(std::memory_order_relaxed);
        if (BOOST_UNLIKELY((reserved - cursor) > _size)) {
            using LockFreeQueueSpace::s_log;
            if (reserved < cursor) {
                LOG_INFO(boost::format("%1% has been reset") % getID());
                cursor = 0;
            } else {
                LOG_ERROR(boost::format("!!!! %1% is falling behind. reserved=%2% cursor=%3%") % getID() % reserved % cursor);
                cursor = reserved - _size + 100;
            }
        }

        auto& slot = _data[cursor & _mask];
        if (slot._id.load(std::memory_order_relaxed) != cursor) {
            // no new data available
            return nullptr;
        }

        cursor += slot._num;
        return &slot._data;
    }
};

}}