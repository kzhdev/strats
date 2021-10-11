#pragma once

#include <atomic>
#include <cassert>

namespace Python {

static constexpr auto CACHE_LINE_SZ = 64;

struct CacheLineBuf {
    __attribute__((aligned(CACHE_LINE_SZ))) uint64_t data[8];

    static constexpr uint32_t calculateReserveSize(uint32_t size) {
        return 1 + (size - 1) / sizeof(CacheLineBuf);
    }
};

template<typename T = CacheLineBuf>
class RingBufferQueue final {
    __attribute__((aligned(CACHE_LINE_SZ))) std::atomic_uint_fast64_t _ready_cursor {0};
    T* _data = nullptr;
    uint32_t _size = 0;
    uint32_t _mask = 0;
    __attribute__((aligned(CACHE_LINE_SZ))) std::atomic_uint_fast64_t _reserve_cursor {0};

public:
    RingBufferQueue() = default;

    RingBufferQueue(uint32_t size)
        : _data(new T[size + (1'000'000 / sizeof(T))])
        , _size(size)
        , _mask(size - 1) {
    }

    ~RingBufferQueue() {
        if(_data) {
            delete[] _data;
            _data = nullptr;
        }
    }

    void init(uint32_t size) {
        assert(size && !(size & (size-1))); // size must be power of 2
        assert(_data == nullptr);
        _data = new T[size + (1'000'000 / sizeof(T))]; //Need some buffer at the end for overflow on write
        _size = size;
        _mask = size - 1;
    }

    RingBufferQueue(const RingBufferQueue& other) = delete;
    RingBufferQueue& operator=(const RingBufferQueue& other) = delete;

    RingBufferQueue(RingBufferQueue&& other)
        : _ready_cursor{other._ready_cursor.load(std::memory_order_relaxed)}
        , _data{other._data}
        , _size{other._size}
        , _mask{other._mask}
        , _reserve_cursor{other._ready_cursor.load(std::memory_order_relaxed)}
    {
        other._data = nullptr;
    }

    RingBufferQueue& operator=(RingBufferQueue&& other) = delete;

    uint32_t size() const noexcept { return _size; }

    uint64_t reserve(uint32_t num = 1) noexcept {
        return _reserve_cursor.fetch_add(num, std::memory_order_acq_rel);
    }

    T& operator[](uint64_t index) noexcept {
        return _data[index & _mask];
    }

    const T& operator[](uint64_t index) const noexcept {
        return _data[index & _mask];
    }

    void publish(uint64_t index, uint32_t num = 1) noexcept {
        while (_ready_cursor.load(std::memory_order_relaxed) != index);
        _ready_cursor.fetch_add(num, std::memory_order_acq_rel);
    }

    uint64_t end() const noexcept { return _ready_cursor.load(std::memory_order_relaxed); }
};

}