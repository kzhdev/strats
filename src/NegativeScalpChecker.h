#ifndef VIPER_NEGATIVE_SCALP_CHECKER_H
#define VIPER_NEGATIVE_SCALP_CHECKER_H

#include "boost/format.hpp"
#include "Common.h"
#include "ClockUtil.h"
#include "Logger.h"

namespace Python {

class NegativeScalpChecker {
public:
    const static uint32_t SIZE = 10000;

    struct Entry {
        uint64_t _time;
        hf::price_t _pnl = 0;
    };

    NegativeScalpChecker()
    :
        _head(0)
    {
        memset(_entries, 0, sizeof(_entries));
    }

    bool onFill(FillMessage& fill) {
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

    void expireEntries() {
    }

    bool checkLimits() {
        return true;
    }

    bool addEntry(hf::price_t pnl) {
        expireEntries();
        const uint64_t now = clockUtil::GetCycles();
        Entry &entry = _entries[_head];
        entry._time = now;
        entry._pnl = pnl;
        ++_runningCount;
        _runningPnl += pnl;
        _head = getNext(_head);
        return checkLimits();
    }

    uint32_t _head;
    uint32_t _size;
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