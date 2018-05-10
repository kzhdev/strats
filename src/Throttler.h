#ifndef VIPER_THROTTLER_H
#define VIPER_THROTTLER_H

#include <stdint.h>
#include <vector>
#include <string.h>
#include "ClockUtil.h"

namespace Python {

struct MessageTracker {

    MessageTracker()
    :
        _head(0)
    {}

    struct Entry {
        uint64_t m_time = 0;
    };

    bool addMessage(uint64_t now) {
        Entry &entry = _entries[_head];
        const uint64_t diff = now - entry.m_time;
        const bool inDangerZone = diff > _periodInCycles;
        entry.m_time = now;
        _head = getNext(_head);
        return inDangerZone;
    }

    uint32_t getNext(uint32_t current) {
        return current == _size - 1 ? 0 : current + 1;
    }

    uint32_t _head;
    uint32_t _size;
    uint64_t _periodInCycles;
    std::vector<Entry> _entries;
};

class Throttler {
public:
    bool addMessage(uint32_t route, uint64_t now) {
        return _messageTrackers[route].addMessage(now);
    }

    bool isBanOn() {
        return _throttlingEnd != 0;
    }

    void startBanPeriod(uint64_t now) {
        _throttlingEnd = now + _banPeriod;
    }

    bool canGoFullSpeed(uint64_t now) {
        const bool can = now > _throttlingEnd;
        if (can) {
            _throttlingEnd = 0;
        }
        return can;
    }

    uint32_t getRoute() {
        if (_route == _numberOfRoutes - 1) {
            _route = 0;
        } else {
            ++_route;
        }
        return _route;
    }

    uint32_t getHedgeRoute() {
        return _hedgeRoute;
    }

private:
    uint32_t _hedgeRoute;
    uint32_t _route = 0;
    uint32_t _numberOfRoutes = 0;
    std::vector<MessageTracker> _messageTrackers;
    uint64_t _throttlingEnd;
    uint64_t _banPeriod;
};

}

#endif //VIPER_THROTTLER_H
