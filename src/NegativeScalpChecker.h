#ifndef VIPER_NEGATIVE_SCALP_CHECKER_H
#define VIPER_NEGATIVE_SCALP_CHECKER_H

#include "boost/format.hpp"
#include "Common.h"
#include "ClockUtil.h"
#include "Logger.h"

namespace Python {

class NegativeScalpChecker {
public:
    const static uint32_t SIZE = 50000;

    struct Entry {
        uint64_t _time;
        hf::price_t _pnl = 0;
    };

    NegativeScalpChecker() {
        memset(_entries, 0, sizeof(_entries));
    }

    void initialize(const std::string& name, uint64_t periodInCycles, hf::price_t pnlLimit, hf::qty_t countLimit) {
        _name = name;
        _periodInCycles = periodInCycles;
        _pnlLimit = pnlLimit;
        _countLimit = countLimit;
    }

    bool onFill(FillMessage& fill) {

        return true;
    }

    void expireEntries(uint64_t now) {
        if (_tail == _head) {
          return;
        }
        const uint64_t endTime = now - _periodInCycles;
        while (true) {
            Entry& entry = _entries[_tail];
            if (entry._time < endTime) {
                --_runningCount;
                _runningPnl -= entry._pnl;
                uint32_t tail = getNext(_tail);
                if (tail != _head) {
                    _tail = tail;
                } else {
                    break;
                }
            } else {
                break;
            }
        }
    }

    bool checkLimits() {
        if (_runningPnl > _pnlLimit) {
            _logger.logInfo(boost::str(boost::format("Negative scalping limit breach - PNL. Contract=%1% PNL=%2% Limit=%3%") % _name % _runningPnl % _pnlLimit));
            return false;
        }
        if (_runningCount > _countLimit) {
            _logger.logInfo(boost::str(boost::format("Negative scalping limit breach - Count. Contract=%1% Count=%2% Limit=%3%") % _name % _runningCount % _countLimit));
            return false;
        }
        return true;
    }

    bool addEntry(hf::price_t pnl, uint64_t now) {
        expireEntries(now);
        Entry &entry = _entries[_head];
        entry._time = now;
        entry._pnl = pnl;
        ++_runningCount;
        _runningPnl += pnl;
        uint32_t head = getNext(_head);
        if (head != _tail) {
            _head = head;
        } else {
            _logger.logInfo(boost::str(boost::format("Negative scalping - head would catch tail. Contract=%1%") % _name));
            return false;
        }
        return checkLimits();
    }

    uint32_t _head = 0;
    uint32_t _tail = 0;
    uint32_t _size = SIZE;
    uint64_t _periodInCycles;
    Entry _entries[SIZE];

    hf::price_t _runningPnl = 0;
    hf::qty_t _runningCount = 0;

    hf::price_t _pnlLimit = 0;
    hf::qty_t _countLimit = 0;

    std::string _name;

    Logger _logger;

    uint32_t getNext(uint32_t current) {
        return current == _size - 1 ? 0 : current + 1;
    }
};

}

#endif //VIPER_NEGATIVE_SCALP_CHECKER_H