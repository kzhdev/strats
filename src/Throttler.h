#ifndef VIPER_THROTTLER_H
#define VIPER_THROTTLER_H

#include <stdint.h>
#include <vector>
#include <string.h>
#include "ClockUtil.h"
#include "Common.h"

namespace Python {

struct MessageTracker {

    MessageTracker() noexcept : _head(0)
    {}

    void initialize(uint64_t periodInCycles, uint32_t size) noexcept {
        _periodInCycles = periodInCycles;
        _size = size;
        _entries.resize(size);
    }

    struct Entry {
        uint64_t m_time = 0;
    };

    bool addMessage(uint64_t now) noexcept {
        Entry &entry = _entries[_head];
        const bool withinLimit = isWithinLimit(now, entry);
        entry.m_time = now;
        _head = getNext(_head);
        return !withinLimit;
    }

    uint32_t getNext(uint32_t current) const noexcept {
        return current == _size - 1 ? 0 : current + 1;
    }

    bool isWithinLimit(uint64_t now) const noexcept {
        return isWithinLimit(now, _entries[_head]);
    }

    bool isWithinLimit(uint64_t now, const Entry &entry) const noexcept {
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
    void initialize(double frequency, hf::VENUE venue, const om::port_gid_t& port_gid, uint32_t messagePeriod,
                    uint32_t messageCount, double dangerPercentage, double hedgePercentage, uint64_t dangerZonePeriod,
                    uint64_t atCapacityPeriod) {
        _messageCount = messageCount;
        _hedgePercentage = hedgePercentage;
        _dangerZonePercentage = dangerPercentage;
        _dangerZonePeriod = dangerZonePeriod;
        _atCapacityPeriod = atCapacityPeriod;
        _port_gid = port_gid;
        _venue = venue;

        uint32_t dangerThreshold;
        uint32_t atCapacityThreshold;
        const bool allGood = calculateThresholds(dangerPercentage, hedgePercentage, dangerThreshold, atCapacityThreshold);
        if (!allGood) {
            logging::GetLogger().logErrorAndThrow("Reserved for hedges and danger zone are more than full capacity");
        }

        LOG(0, "Throttler. Initialized. VENUE={i1} port_gid={i2} DangerThreshold={i3} AtCapacityThreshold={i4}", toUInt(_venue),
            _port_gid.value(), dangerThreshold, atCapacityThreshold)

        uint64_t periodInCycles = GetCyclesFromSeconds(messagePeriod, frequency);
        _dangerZoneTracker.initialize(periodInCycles, dangerThreshold);
        _fullCapacityTracker.initialize(periodInCycles, atCapacityThreshold);
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

    void addMessage(uint64_t now, bool& isInDangerZone, bool& isAtCapacity) {
        isInDangerZone = _dangerZoneTracker.addMessage(now);
        isAtCapacity = _fullCapacityTracker.addMessage(now);
    }

    void setInDangerZone(uint64_t now) noexcept {
        _banEnd = now + _dangerZonePeriod;
        _mode = THROTTLING_MODE_DANGER_ZONE;
        LOG(0, "Throttler Setting mode: DANGER_ZONE venue={i1} port_gid={i2}", toUInt(_venue), _port_gid.value())
    }

    void setAtCapacity(uint64_t now) noexcept {
        _banEnd = now + _atCapacityPeriod;
        _mode = THROTTLING_MODE_AT_CAPACITY;
        LOG(0, "Throttler Setting mode: AT_CAPACITY venue={i1} port_gid={i2}", toUInt(_venue), _port_gid.value())
    }

    void setInSafeZone() {
        _mode = THROTTLING_MODE_NONE;
        _banEnd = 0;
        LOG(0, "Throttler Setting mode: NONE venue={i1} port_gid={i2}", toUInt(_venue), _port_gid.value())
    }

    bool wasInDangerZone() {
        return _mode == THROTTLING_MODE_DANGER_ZONE;
    }

    bool didBanExpire(uint64_t now) const noexcept {
        return now > _banEnd;
    }

    THROTTLING_MODE getNewMode(uint64_t now) {
        if (!_dangerZoneTracker.isWithinLimit(now)) {
            setInDangerZone(now);
        } else {
            setInSafeZone();
        }
        return _mode;
    }

    bool isBanOn() const noexcept {
        return _mode != THROTTLING_MODE_NONE;
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

    THROTTLING_MODE getMode() const noexcept {
        return _mode;
    }

    om::port_gid_t getPortGid() const noexcept {
        return _port_gid;
    }

    hf::VENUE getVenue() const noexcept {
        return _venue;
    }

private:
    MessageTracker _dangerZoneTracker;
    MessageTracker _fullCapacityTracker;
    uint64_t _dangerZonePeriod = 0;
    uint64_t _atCapacityPeriod = 0;
    uint64_t _banEnd = 0;
    uint32_t _messageCount;
    double _dangerZonePercentage;
    double _hedgePercentage;
    om::port_gid_t _port_gid;
    hf::VENUE _venue;
    THROTTLING_MODE _mode = THROTTLING_MODE_NONE;
};

}

#endif //VIPER_THROTTLER_H
