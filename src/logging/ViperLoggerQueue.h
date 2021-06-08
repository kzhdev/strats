#pragma once

#include <util/Resource.h>
#include "../LockFreeQueue.h"
#include <om/orders/strategy_order.h>

namespace hf {
namespace queue {

class ViperLoggerQueueResource final : public rhone::util::Resource {
    hf::queue::LockFreeQueue<om::debug_t> _logQueue;
    uint_fast64_t  _logQueueCursor = 0;

public:
    virtual void onCreate(const libconfig::Setting &config) override;

    void clear() noexcept {
        _logQueue.clear();
    }
    uint_fast64_t reserve(uint_fast64_t num = 1) noexcept {
        return _logQueue.reserve(num);
    }
    om::debug_t* operator[](uint_fast64_t index) noexcept {
        return _logQueue[index];
    }
    const om::debug_t* operator[](uint_fast64_t index) const noexcept {
        return _logQueue[index];
    }
    void publish(uint_fast64_t index, uint_fast64_t num = 1) const noexcept {
        _logQueue.publish(index, num);
    }
    om::debug_t* read() noexcept {
        return _logQueue.next(_logQueueCursor);
    }
};

}}