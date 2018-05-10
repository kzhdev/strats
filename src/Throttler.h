#ifndef VIPER_THROTTLER_H
#define VIPER_THROTTLER_H

#include <stdint.h>
#include <vector>
#include <string.h>
#include "ClockUtil.h"
#include "Common.h"

namespace Python {

struct MessageTracker {

    MessageTracker()
    :
        _head(0)
    {}

    void initialize(uint64_t periodInCycles, uint32_t size) {
        _periodInCycles = periodInCycles;
        _size = size;
        _entries.resize(size);
    }

    struct Entry {
        uint64_t m_time = 0;
    };

    bool addMessage(uint64_t now) {
        Entry &entry = _entries[_head];
        const bool withinLimit = isWithinLimit(now, entry);
        entry.m_time = now;
        _head = getNext(_head);
        return !withinLimit;
    }

    uint32_t getNext(uint32_t current) {
        return current == _size - 1 ? 0 : current + 1;
    }

    bool isWithinLimit(uint64_t now) {
        return isWithinLimit(now, _entries[_head]);
    }

    bool isWithinLimit(uint64_t now, Entry &entry) {
        const uint64_t diff = now - entry.m_time;
        return diff > _periodInCycles;
    }

    uint32_t _head;
    uint32_t _size;
    uint64_t _periodInCycles;
    std::vector<Entry> _entries;
};

class Throttler {
public:
    void initialize(double frequency, uint32_t nRoutes, uint32_t messagePeriod, uint32_t messageCount, double dangerPercentage, double hedgePercentage, uint64_t dangerZonePeriod, uint64_t atCapacityPeriod) {
        _messageCount = messageCount;
        _hedgePercentage = hedgePercentage;
        _dangerZonePercentage = dangerPercentage;
        _dangerZonePeriod = dangerZonePeriod;
        _atCapacityPeriod = atCapacityPeriod;
        _numberOfRoutes = nRoutes;
        _dangerZoneTrackers.resize(nRoutes);
        _fullCapacityTrackers.resize(nRoutes);

        uint32_t dangerThreshold;
        uint32_t atCapacityThreshold;
        const bool allGood = calculateThresholds(dangerPercentage, hedgePercentage, dangerThreshold, atCapacityThreshold);
        if (!allGood) {
            GetLogger().logErrorAndThrow("Reserved for hedges and danger zone are more than full capacity");
        }

        uint64_t periodInCycles = GetCyclesFromSeconds(messagePeriod, frequency);
        for (uint32_t i = 0; i < nRoutes; ++i) {
            _dangerZoneTrackers[i].initialize(periodInCycles, dangerThreshold);
            _fullCapacityTrackers[i].initialize(periodInCycles, atCapacityThreshold);
        }
    }

    bool calculateThresholds(double dangerPercentage, double hedgePercentage, uint32_t& dangerThreshold, uint32_t& atCapacityThreshold) {
        const uint32_t reservedForHedges = static_cast<uint32_t>(_messageCount * hedgePercentage);
        const uint32_t dangerZone = static_cast<uint32_t>(_messageCount * dangerPercentage);
        if (reservedForHedges + dangerZone > _messageCount) {
            return false;
        }
        dangerThreshold = _messageCount - reservedForHedges - dangerZone;
        atCapacityThreshold = _messageCount - reservedForHedges;
        return true;
    }

    void addMessage(uint32_t route, uint64_t now, bool& isInDangerZone, bool& isAtCapacity) {
        isInDangerZone = _dangerZoneTrackers[route].addMessage(now);
        isAtCapacity = _fullCapacityTrackers[route].addMessage(now);
    }

    void setInDangerZone(uint64_t now) {
        _banEnd = now + _dangerZonePeriod;
        _mode = THROTTLING_MODE_DANGER_ZONE;
    }

    void setAtCapacity(uint64_t now) {
        _banEnd = now + _atCapacityPeriod;
        _mode = THROTTLING_MODE_AT_CAPACITY;
    }

    void setInSafeZone() {
        _mode = THROTTLING_MODE_NONE;
        _banEnd = 0;
    }

    bool wasInDangerZone() {
        return _mode == THROTTLING_MODE_DANGER_ZONE;
    }

    bool didBanExpire(uint64_t now) {
        return now > _banEnd;
    }

    THROTTLING_MODE getNewMode(uint64_t now) {
        if (!_dangerZoneTrackers[0].isWithinLimit(now)) {
            setInDangerZone(now);
        } else {
            setInSafeZone();
        }
        return _mode;
    }

    bool isBanOn() {
        return _mode != THROTTLING_MODE_NONE;
    }

    uint32_t getRoute() {
        if (_route == _numberOfRoutes - 1) {
            _route = 0;
        } else {
            ++_route;
        }
        return _route;
    }

    void updateInDangerZonePeriod(uint64_t period) {
        _dangerZonePeriod = period;
    }

    void updateAtCapacityPeriod(uint64_t period) {
        _atCapacityPeriod = period;
    }

    void updateHedgePercentage(double hedgePercentage) {
    }

    void updateDangerZonePercentage(double dangerZonePercentage) {
    }

private:
    uint32_t _route = 0;
    uint32_t _numberOfRoutes = 0;
    std::vector<MessageTracker> _dangerZoneTrackers;
    std::vector<MessageTracker> _fullCapacityTrackers;
    uint64_t _dangerZonePeriod = 0;
    uint64_t _atCapacityPeriod = 0;
    uint64_t _banEnd = 0;
    uint32_t _messageCount;
    double _dangerZonePercentage;
    double _hedgePercentage;
    THROTTLING_MODE _mode = THROTTLING_MODE_NONE;
};

}

#endif //VIPER_THROTTLER_H
