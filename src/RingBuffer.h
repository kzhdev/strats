#pragma once

#include <atomic>

namespace Python {

template<typename T>
class RingBuffer {
    T* _data = nullptr;
    uint32_t _size = 0;
    uint32_t _mask = 0;
    std::atomic_uint_fast64_t _cursor {0};

public:
    RingBuffer() = default;
    RingBuffer(uint32_t size)
        : _data(new T[size])
        , _size(size)
        , _mask(size - 1)
    {
        assert(size && !(size & (size -1)));    // size must be power of 2
    }

    RingBuffer(const RingBuffer& other) = delete;
    RingBuffer& operator=(const RingBuffer& other) = delete;

    RingBuffer(RingBuffer&& other)
        : _data{other._data}
        , _size{other._size}
        , _mask{other._mask}
        , _cursor{other._cursor.load(std::memory_order_relaxed)}
    {
        other._data = nullptr;
    }

    RingBuffer& operator=(RingBuffer&& other) = delete;

    void init(uint32_t size) {
        assert(size && !(size & (size -1)));    // size must be power of 2
        _data = new T[size];
        _size = size;
        _mask = _size - 1;
    }

    T& next(uint32_t num = 1) noexcept {
        return _data[_cursor.fetch_add(num, std::memory_order_acq_rel) & _mask];
    }

    uint64_t last() const noexcept {
        return _cursor.load(std::memory_order_relaxed);
    }

    T& operator[](uint32_t index) noexcept {
        return _data[index & _mask];
    }

    const T& operator[](uint32_t index) const noexcept {
        return _data[index & _mask];
    }
};

}