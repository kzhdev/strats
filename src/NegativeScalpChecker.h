#ifndef VIPER_NEGATIVE_SCALP_CHECKER_H
#define VIPER_NEGATIVE_SCALP_CHECKER_H

#include <queue>
#include "boost/format.hpp"
#include "Common.h"
#include "ClockUtil.h"
#include "Logger.h"
#include "ViperConfig.h"

namespace Python {

class NegativeScalpChecker {
public:
    const static uint32_t SIZE = 50000;

    NegativeScalpChecker() {
        memset(_entries, 0, sizeof(_entries));
    }

    void initialize(const std::string& name, uint64_t periodInCycles, hf::price_t pnlLimit, hf::qty_t countLimit) {
        _name = name;
        _periodInCycles = periodInCycles;
        _pnlLimit = pnlLimit;
        _countLimit = countLimit;
    }

    bool isOverLimit() {
        return _isOverLimit;
    }

    void reset() {
        _isOverLimit = false;
        _runningCount = 0;
        _runningPnl = 0;
        _head = 0;
        _tail = 0;
    }

    void onFillPnl(hf::price_t pnl) {
        uint64_t now = clockUtil::GetCycles();
        if (pnl < 0) {
            _isOverLimit = addEntry(pnl, now);
        } else {
            expireEntries(now);
        }
    }

private:
    struct Entry {
        uint64_t _time;
        hf::price_t _pnl = 0;
    };

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
            return true;
        }
        if (_runningCount > _countLimit) {
            _logger.logInfo(boost::str(boost::format("Scalp checker %1%. Count limit breach. Count=%2% Limit=%3%") % _name % _runningCount % _countLimit));
            return true;
        }
        return false;
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
            return true;
        }
        return checkLimits();
    }

    uint32_t getNext(uint32_t current) {
        return current == _size - 1 ? 0 : current + 1;
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

    bool _isOverLimit = false;

    std::string _name;

    Logger _logger;
};

class NegativeScalpCheckers {
public:
    void initialize(std::vector<ConfigContract>& contracts, uint64_t periodInCycles, hf::price_t pnlLimit, hf::qty_t countLimit) {
        _logger.logInfo(boost::str(boost::format("Negative Scalper limits: Count=%1% Dollars=%2%") % countLimit % pnlLimit));
        _negativeScalpCheckers.resize(contracts.size());
        for (uint32_t i = 0; i < contracts.size(); ++i) {
            auto& contract = contracts[i];
            _negativeScalpCheckers[i].initialize(contract._name, periodInCycles,pnlLimit,countLimit);
        }
    }

    NegativeScalpChecker& getChecker(uint32_t index) {
        return _negativeScalpCheckers.at(index);
    }

    std::vector<NegativeScalpChecker> _negativeScalpCheckers;
    Logger _logger;
};

struct Pnl {
    hf::price_t _open = 0;
    hf::price_t _closed = 0;
    hf::price_t _drawdown = 0;

    hf::price_t getTotal() { return _open + _closed; }
};

class ContractPnlCalculator : public Pnl {
public:
    ContractPnlCalculator() {
    }

    void initialize(const std::string& name, hf::price_t multiplier, NegativeScalpChecker* negativeScalpChecker) {
        _name = name;
        _multiplier = multiplier;
    }

    void match(hf::price_t fillPrice, hf::price_t queuedFillPrice, hf::qty_t size) {
        hf::price_t buyPrice = _side == BID ? queuedFillPrice : fillPrice;
        hf::price_t sellPrice = _side == BID ? fillPrice : queuedFillPrice;
        hf::price_t pnl = (sellPrice - buyPrice) * size * _multiplier;
        _newClosedPnl += pnl;
        _negativeScalpChecker->onFillPnl(pnl);
    }

    void onFill(FillMessage& fill, Pnl& pnlForType, Pnl& pnlTotal) {
        if (_fills.empty()) {
            _fills.push_back(fill);
            _side = fill._side;
        } else {
            if (fill._side == _side) {
                _fills.push_back(fill);
            } else {
                _logger.logInfo(boost::str(boost::format("Scalp checker %1% BUY. Fill. Negative position. Price=%2% Size=%3%") % _name % fill._price % fill._qty));
                _logger.logInfo(boost::str(boost::format("Scalp checker %1% BUY. Matching. Bigger than negative position.") % _name));
                for (auto it = _fills.begin(); it != _fills.end();) {
                    FillMessage& queuedFill = _fills.front();
                    if (queuedFill._qty == fill._qty) {
                        _logger.logInfo(boost::str(boost::format("Scalp checker %1% BUY. Matching equal sell fill. BuyPrice=%2% SellPrice=%3% Size=%4%") % _name % fill._price % queuedFill._price % fill._qty));
                        match(fill._price, queuedFill._price, fill._qty);
                        _fills.erase(it);
                        fill._qty = 0;
                        break;
                    } else if (queuedFill._qty > fill._qty) {
                        _logger.logInfo(boost::str(boost::format("Scalp checker %1% BUY. Matching bigger sell fill. BuyPrice=%2% SellPrice=%3% Size=%4%") % _name % fill._price % queuedFill._price % fill._qty));
                        match(fill._price, queuedFill._price, fill._qty);
                        queuedFill._qty -= fill._qty;
                        fill._qty = 0;
                        break;
                    } else {
                        _logger.logInfo(boost::str(boost::format("Scalp checker %1% BUY. Matching smaller sell fill. BuyPrice=%2% SellPrice=%3% Size=%4%") % _name % fill._price % queuedFill._price % queuedFill._qty));
                        match(fill._price, queuedFill._price, queuedFill._qty);
                        fill._qty -= queuedFill._qty;
                        it = _fills.erase(it);
                    }
                }
                if (fill._qty > 0) {
                    _fills.push_back(fill);
                    _side = fill._side;
                }
            }
        }

        calculateOpenPnl(pnlForType, pnlTotal);
        calculateClosedPnl(pnlForType, pnlTotal);

        _logger.logInfo(boost::str(boost::format("PNL: %1% Open=%2% Closed=%3% MarkPrice=%4% FillPrice=%5% FillSize=%6% Side=%7%") % _name % _open % _closed % _markPrice % fill._price % fill._qty % getSideString(fill._side)));
    }

    void calculateClosedPnl(Pnl& pnlForType, Pnl& pnlTotal) {
        hf::price_t closedPnlChange = _newClosedPnl - _closed;
        pnlForType._closed += closedPnlChange;
        pnlTotal._closed += closedPnlChange;
        _closed = _newClosedPnl;
    }

    void calculateOpenPnl(Pnl& pnlForType, Pnl& pnlTotal) {
        _newOpenPnl = 0;
        if (_fills.size() > 0) {
            for (auto& fill : _fills) {
                if (_side == BID) {
                    _newOpenPnl += (_markPrice - fill._price) * fill._qty * _multiplier;
                } else {
                    _newOpenPnl += (fill._price - _markPrice) * fill._qty * _multiplier;
                }
            }
        }

        hf::price_t openPnlChange = _newOpenPnl - _open;
        pnlForType._open += openPnlChange;
        pnlTotal._open += openPnlChange;
        _open = _newOpenPnl;
    }


    void onMarkPriceChange(hf::price_t price, Pnl& pnlForType, Pnl& pnlTotal) {
        _markPrice = price;
        calculateOpenPnl(pnlForType, pnlTotal);
    }

    std::vector<FillMessage> _fills;
    uint32_t _side;

    hf::price_t _markPrice = 0;
    hf::price_t _newClosedPnl = 0;
    hf::price_t _newOpenPnl = 0;
    hf::price_t _multiplier = 0;

    NegativeScalpChecker* _negativeScalpChecker;
    std::string _name;
    Logger _logger;
};

struct AggregatedPnl {
    Pnl _relativeValue;
    Pnl _marketMakers;
    Pnl _execution;
    Pnl _total;
};

class PnlCalculator {
public:
    void init(std::vector<ConfigContract>& contracts, NegativeScalpCheckers& negativeScalpCheckers) {
        _marketMakerContractPnlCalculators.resize(contracts.size());
        _relativeValueContractPnlCalculators.resize(contracts.size());
        for (uint32_t i = 0; i < contracts.size(); ++i) {
            auto& contract = contracts[i];
            NegativeScalpChecker* checker = &negativeScalpCheckers.getChecker(i);
            hf::price_t multiplier = contract._tickValue / contract._tickSize;
            _marketMakerContractPnlCalculators[i].initialize(contract._name, multiplier, checker);
            _relativeValueContractPnlCalculators[i].initialize(contract._name, multiplier,checker);
        }
    }

    void onFillFromRelativeValue(FillMessage& fill) {
        ContractPnlCalculator& pnl = _relativeValueContractPnlCalculators[fill._contractId];
        pnl.onFill(fill, _pnl._relativeValue, _pnl._total);
    }

    void onFillFromMarketMaker(FillMessage& fill) {
        ContractPnlCalculator& pnl = _marketMakerContractPnlCalculators[fill._contractId];
        pnl.onFill(fill, _pnl._marketMakers, _pnl._total);
    }

    AggregatedPnl& getAggregatedPnl() { return _pnl; }

    ContractPnlCalculator& getRelativeValuePnlDetail(uint32_t index) { return _relativeValueContractPnlCalculators[index]; }
    ContractPnlCalculator& getMarketMakerPnlDetail(uint32_t index) { return _marketMakerContractPnlCalculators[index]; }

    bool recalculateOpenPnl(uint32_t index, hf::price_t price) {
        ContractPnlCalculator& rvPnl = _relativeValueContractPnlCalculators[index];
        if (price != rvPnl._markPrice) {
            rvPnl.onMarkPriceChange(price, _pnl._relativeValue, _pnl._total);
            ContractPnlCalculator& mmPnl = _marketMakerContractPnlCalculators[index];
            mmPnl.onMarkPriceChange(price, _pnl._marketMakers, _pnl._total);
            return true;
        }
        return false;
    }

    std::vector<ContractPnlCalculator> _marketMakerContractPnlCalculators;
    std::vector<ContractPnlCalculator> _relativeValueContractPnlCalculators;

    AggregatedPnl _pnl;
};


}

#endif //VIPER_NEGATIVE_SCALP_CHECKER_H