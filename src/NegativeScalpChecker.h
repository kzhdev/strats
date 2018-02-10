#ifndef VIPER_NEGATIVE_SCALP_CHECKER_H
#define VIPER_NEGATIVE_SCALP_CHECKER_H

#include <queue>
#include "boost/format.hpp"
#include "Common.h"
#include "ClockUtil.h"
#include "Logger.h"
#include "ViperConfig.h"

namespace Python {

struct Pnl {
    hf::price_t _open = 0;
    hf::price_t _closed = 0;
    hf::price_t _drawdown = 0;

    hf::price_t getTotal() { return _open + _closed; }
};

class NegativeScalpChecker : public Pnl {
public:
    const static uint32_t SIZE = 50000;

    struct Entry {
        uint64_t _time;
        hf::price_t _pnl = 0;
    };

    NegativeScalpChecker() {
        memset(_entries, 0, sizeof(_entries));
    }

    void initialize(const std::string& name, hf::price_t multiplier, uint64_t periodInCycles, hf::price_t pnlLimit, hf::qty_t countLimit) {
        _name = name;
        _periodInCycles = periodInCycles;
        _pnlLimit = pnlLimit;
        _countLimit = countLimit;
        _multiplier = multiplier;
    }

    bool match(hf::price_t buyPrice, hf::price_t sellPrice, hf::qty_t size, uint64_t now) {
        hf::price_t pnl = (sellPrice - buyPrice) * size * _multiplier;
        if (sellPrice < buyPrice) {
            return addEntry(pnl, now);
        } else {
            expireEntries(now);
        }
        _newClosedPnl += pnl;
        return true;
    }

    bool onFill(FillMessage& fill, Pnl& pnlForType, Pnl& pnlTotal) {
        bool isOk = true;
        if (fill._side == BID) {
            if (!_buys.empty()) {
                _buys.push_back(fill);
                _logger.logInfo(boost::str(boost::format("Scalp checker %1% BUY. Fill but already long. Adding to queue. Price=%2% Size=%3%") % _name % fill._price % fill._qty));
            } else {
                _logger.logInfo(boost::str(boost::format("Scalp checker %1% BUY. Fill. Negative position. Price=%2% Size=%3%") % _name % fill._price % fill._qty));
                uint64_t now = clockUtil::GetCycles();
                _logger.logInfo(boost::str(boost::format("Scalp checker %1% BUY. Matching. Bigger than negative position.") % _name));
                for (auto it = _sells.begin(); it != _sells.end();) {
                    FillMessage& sell = _sells.front();
                    if (sell._qty == fill._qty) {
                        _logger.logInfo(boost::str(boost::format("Scalp checker %1% BUY. Matching equal sell fill. BuyPrice=%2% SellPrice=%3% Size=%4%") % _name % fill._price % sell._price % fill._qty));
                        isOk = match(fill._price, sell._price, fill._qty, now);
                        it = _sells.erase(it);
                        fill._qty = 0;
                        break;
                    } else if (sell._qty > fill._qty) {
                        _logger.logInfo(boost::str(boost::format("Scalp checker %1% BUY. Matching bigger sell fill. BuyPrice=%2% SellPrice=%3% Size=%4%") % _name % fill._price % sell._price % fill._qty));
                        isOk = match(fill._price, sell._price, fill._qty, now);
                        sell._qty -= fill._qty;
                        fill._qty = 0;
                        break;
                    } else {
                        _logger.logInfo(boost::str(boost::format("Scalp checker %1% BUY. Matching smaller sell fill. BuyPrice=%2% SellPrice=%3% Size=%4%") % _name % fill._price % sell._price % sell._qty));
                        isOk = match(fill._price, sell._price, sell._qty, now);
                        fill._qty -= sell._qty;
                        it = _sells.erase(it);
                    }
                }
                if (fill._qty > 0) {
                    _buys.push_back(fill);
                }
            }
        } else {
            if (!_sells.empty()) {
                _sells.push_back(fill);
                _logger.logInfo(boost::str(boost::format("Scalp checker %1% SELL. Fill but already short. Adding to queue. Price=%2% Size=%3%") % _name % fill._price % fill._qty));
            } else {
                _logger.logInfo(boost::str(boost::format("Scalp checker %1% SELL. Matching. Bigger than positive position.") % _name));
                uint64_t now = clockUtil::GetCycles();
                for (auto it = _buys.begin(); it != _buys.end();) {
                    FillMessage& buy = _buys.front();
                    if (buy._qty == fill._qty) {
                        _logger.logInfo(boost::str(boost::format("Scalp checker %1% SELL. Matching equal buy fill. SellPrice=%2% BuyPrice=%3% Size=%4%") % _name % fill._price % buy._price % fill._qty));
                        isOk = match(buy._price, fill._price, fill._qty, now);
                        it = _buys.erase(it);
                        fill._qty = 0;
                        break;
                    } else if (buy._qty > fill._qty) {
                        _logger.logInfo(boost::str(boost::format("Scalp checker %1% SELL. Matching bigger buy fill. SellPrice=%2% BuyPrice=%3% Size=%4%") % _name % fill._price % buy._price % fill._qty));
                        isOk = match(buy._price, fill._price, fill._qty, now);
                        buy._qty -= fill._qty;
                        fill._qty = 0;
                        break;
                    } else {
                        _logger.logInfo(boost::str(boost::format("Scalp checker %1% SELL. Matching smaller buy fill. SellPrice=%2% BuyPrice=%3% Size=%4%") % _name % fill._price % buy._price % buy._qty));
                        isOk = match(buy._price, fill._price, buy._qty, now);
                        fill._qty -= buy._qty;
                        it = _buys.erase(it);
                    }
                }
                if (fill._qty > 0) {
                    _sells.push_back(fill);
                }
            }
        }

        calculateOpenPnl(pnlForType, pnlTotal);

        hf::price_t closedPnlChange = _newClosedPnl - _closed;
        pnlForType._closed += closedPnlChange;
        pnlTotal._closed += closedPnlChange;
        _closed = _newClosedPnl;

        _logger.logInfo(boost::str(boost::format("PNL: %1% Open=%2% Closed=%3% MarkPrice=%4% FillPrice=%5% FillSize=%6% Side=%7%") % _name % _open % _closed % _markPrice % fill._price % fill._qty % getSideString(fill._side)));

        return isOk;
    }

    void calculateOpenPnl(Pnl& pnlForType, Pnl& pnlTotal) {
        if (_buys.size() > 0) {
            _newOpenPnl = 0;
            for (auto& fill : _buys) {
                _newOpenPnl += (_markPrice - fill._price) * fill._qty * _multiplier;
            }
        } else {
            _newOpenPnl = 0;
            for (auto& fill : _sells) {
                _newOpenPnl += (fill._price - _markPrice) * fill._qty * _multiplier;
            }
        }
        hf::price_t openPnlChange = _newOpenPnl - _open;
        pnlForType._open += openPnlChange;
        pnlTotal._open += openPnlChange;
        _open = _newOpenPnl;
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
                _logger.logInfo(boost::str(boost::format("Scalp checker %1%. Expiring entry. Entry_PNL=%2% PNL=%3% Count=%4% Time=%5% Now=%6%") % _name % entry._pnl % _runningPnl % _runningCount % entry._time % now));
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
            _logger.logInfo(boost::str(boost::format("Scalp checker %1%. PNL breach. PNL=%2% Limit=%3%") % _name % _runningPnl % _pnlLimit));
            return false;
        }
        if (_runningCount > _countLimit) {
            _logger.logInfo(boost::str(boost::format("Scalp checker %1%. Count limit breach. Count=%2% Limit=%3%") % _name % _runningCount % _countLimit));
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
        _logger.logInfo(boost::str(boost::format("Scalp checker %1%. Adding entry. Entry_PNL=%2% PNL=%3% Count=%4% Time=%5%") % _name % pnl % _runningPnl % _runningCount % now));
        uint32_t head = getNext(_head);
        if (head != _tail) {
            _head = head;
        } else {
            _logger.logInfo(boost::str(boost::format("Scalp checker %1%. - head would catch tail.") % _name));
            return false;
        }
        return checkLimits();
    }

    void onMarkPriceChange(hf::price_t price, Pnl& pnlForType, Pnl& pnlTotal) {
        _markPrice = price;
        calculateOpenPnl(pnlForType, pnlTotal);
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

    std::vector<FillMessage> _buys;
    std::vector<FillMessage> _sells;

    hf::price_t _markPrice = 0;

    hf::price_t _newClosedPnl = 0;
    hf::price_t _newOpenPnl = 0;

    hf::price_t _multiplier = 0;

    std::string _name;

    Logger _logger;

    uint32_t getNext(uint32_t current) {
        return current == _size - 1 ? 0 : current + 1;
    }
};

struct AggregatedPnl {
    Pnl _relativeValue;
    Pnl _marketMakers;
    Pnl _execution;
    Pnl _total;
};

class PnlCalculator {
public:
    void init(std::vector<ConfigContract>& contracts, uint32_t countLimit, hf::price_t dollarLimit, uint64_t periodInCycles) {
        _marketMakerDetail.resize(contracts.size());
        _relativeValueDetail.resize(contracts.size());
        for (uint32_t i = 0; i < contracts.size(); ++i) {
            auto& contract = contracts[i];
            _marketMakerDetail[i]._name = contract._name;
            _marketMakerDetail[i]._multiplier = contract._tickValue / contract._tickSize;
            _relativeValueDetail[i]._name = contract._name;
            _relativeValueDetail[i]._multiplier = contract._tickValue / contract._tickSize;
            _relativeValueDetail[i]._countLimit = countLimit;
            _relativeValueDetail[i]._periodInCycles = periodInCycles;
            _relativeValueDetail[i]._pnlLimit = dollarLimit;
        }
    }

    void onFillFromRelativeValue(FillMessage& fill) {
        NegativeScalpChecker& pnl = _relativeValueDetail[fill._contractId];
        _negativeScalperOk = pnl.onFill(fill, _pnl._relativeValue, _pnl._total);
    }

    void onFillFromMarketMaker(FillMessage& fill) {
        NegativeScalpChecker& pnl = _marketMakerDetail[fill._contractId];
        _negativeScalperOk = pnl.onFill(fill, _pnl._marketMakers, _pnl._total);
    }

    AggregatedPnl& getAggregatedPnl() { return _pnl; }

    NegativeScalpChecker& getRelativeValuePnlDetail(uint32_t index) { return _relativeValueDetail[index]; }
    NegativeScalpChecker& getMarketMakerPnlDetail(uint32_t index) { return _marketMakerDetail[index]; }

    bool recalculateOpenPnl(uint32_t index, hf::price_t price) {
        NegativeScalpChecker& rvPnl = _relativeValueDetail[index];
        if (price != rvPnl._markPrice) {
            rvPnl.onMarkPriceChange(price, _pnl._relativeValue, _pnl._total);
            NegativeScalpChecker& mmPnl = _marketMakerDetail[index];
            mmPnl.onMarkPriceChange(price, _pnl._marketMakers, _pnl._total);
            return true;
        }
        return false;
    }

    std::vector<NegativeScalpChecker> _marketMakerDetail;
    std::vector<NegativeScalpChecker> _relativeValueDetail;

    AggregatedPnl _pnl;

    bool _negativeScalperOk = true;
};


}

#endif //VIPER_NEGATIVE_SCALP_CHECKER_H