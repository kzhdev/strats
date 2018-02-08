#ifndef VIPER_NEGATIVE_SCALP_CHECKER_H
#define VIPER_NEGATIVE_SCALP_CHECKER_H

#include <queue>
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

    bool matchBuy(hf::price_t price, hf::qty_t size, uint64_t now, hf::price_t openPrice) {
        if (price > openPrice) {
            hf::price_t pnl = size * price - openPrice * size;
            return addEntry(pnl, now);
        } else {
            expireEntries(now);
        }
        return true;
    }

    bool matchSell(hf::price_t price, hf::qty_t size, uint64_t now, hf::price_t openPrice) {
        if (price < openPrice) {
            hf::price_t pnl = openPrice * size - price * size;
            return addEntry(pnl, now);
        } else {
            expireEntries(now);
        }
        return true;
    }

    bool onFill(FillMessage& fill) {
        if (fill._side == BID) {
            if (_unmatchedPosition > 0) {
                _buys.push(fill);
            }
            else if (_unmatchedPosition < 0) {
                uint64_t now = clockUtil::GetCycles();
                hf::qty_t unmatchedPosition = -_unmatchedPosition;
                if (fill._qty == unmatchedPosition) {
                    const bool isOk = matchBuy(fill._price, fill._qty, now, _openPrice);
                    _unmatchedPosition = 0;
                    return isOk;
                }
                else if (fill._qty > unmatchedPosition) {
                    if (!matchBuy(fill._price, unmatchedPosition, now, _openPrice)) {
                        return false;
                    }
                    hf::qty_t remainingBuy = fill._qty - unmatchedPosition;
                    while (true) {
                        if (!_sells.empty()) {
                            const FillMessage& sell = _sells.front();
                            if (sell._qty == remainingBuy) {
                                const bool isOk = matchBuy(fill._price, remainingBuy, now, sell._price);
                                _sells.pop();
                                return isOk;
                            } else if (sell._qty > remainingBuy) {
                                const bool isOk = matchBuy(fill._price, remainingBuy, now, sell._price);
                                _sells.pop();
                                _unmatchedPosition = remainingBuy - sell._qty;
                                return isOk;
                            } else {
                                if (!matchBuy(fill._price, sell._qty, now, sell._price)) {
                                    return false;
                                }
                                _sells.pop();
                                remainingBuy -= sell._qty;
                            }
                        } else {
                            break;
                        }
                    }
                }
                else {
                    bool isOk = matchBuy(fill._price, fill._qty, now, _openPrice);
                    _unmatchedPosition += fill._qty;
                    return isOk;
                }
            }
            else {
                _unmatchedPosition = fill._qty;
                _openPrice = fill._price;
                return true;
            }
        } else {
            if (_unmatchedPosition < 0) {
                _sells.push(fill);
            }
            else if (_unmatchedPosition == 0) {
                _unmatchedPosition = -fill._qty;
                _openPrice = fill._price;
            }
            else {
                uint64_t now = clockUtil::GetCycles();
                hf::qty_t unmatchedPosition = _unmatchedPosition;
                if (fill._qty == unmatchedPosition) {
                    const bool isOk = matchSell(fill._price, fill._qty, now, _openPrice);
                    _unmatchedPosition = 0;
                    return isOk;
                }
                else if (fill._qty > unmatchedPosition) {
                    if (!matchSell(fill._price, unmatchedPosition, now, _openPrice)) {
                        return false;
                    }
                    hf::qty_t remainingSell = fill._qty - unmatchedPosition;
                    while (true) {
                        if (!_buys.empty()) {
                            const FillMessage& buy = _buys.front();
                            if (buy._qty == remainingSell) {
                                const bool isOk = matchSell(fill._price, remainingSell, now, buy._price);
                                _sells.pop();
                                return isOk;
                            } else if (buy._qty > remainingSell) {
                                const bool isOk = matchSell(fill._price, remainingSell, now, buy._price);
                                _sells.pop();
                                _unmatchedPosition = buy._qty - remainingSell;
                                return isOk;
                            } else {
                                if (!matchSell(fill._price, buy._qty, now, buy._price)) {
                                    return false;
                                }
                                _sells.pop();
                                remainingSell -= buy._qty;
                            }
                        } else {
                            break;
                        }
                    }
                } else {
                    bool isOk = matchSell(fill._price, fill._qty, now, _openPrice);
                    _unmatchedPosition -= fill._qty;
                    return isOk;
                }
            }
        }
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

    std::queue<FillMessage> _buys;
    std::queue<FillMessage> _sells;

    hf::qty_t _unmatchedPosition = 0;
    hf::price_t _openPrice = 0;

    std::string _name;

    Logger _logger;

    uint32_t getNext(uint32_t current) {
        return current == _size - 1 ? 0 : current + 1;
    }
};

}

#endif //VIPER_NEGATIVE_SCALP_CHECKER_H