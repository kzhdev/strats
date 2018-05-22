#ifndef VIPER_NEGATIVE_SCALP_CHECKER_H
#define VIPER_NEGATIVE_SCALP_CHECKER_H

#include <queue>
#include "boost/format.hpp"
#include "Common.h"
#include "ClockUtil.h"
#include "logging/Coolog.h"
#include "ViperConfig.h"

namespace Python {

class NegativeScalpChecker {
public:
    const static uint32_t SIZE = 50000;

    NegativeScalpChecker() {
        memset(_entries, 0, sizeof(_entries));
    }

    void initialize(uint64_t periodInCycles, hf::price_t pnlLimit, hf::qty_t countLimit) {
        _periodInCycles = periodInCycles;
        _pnlLimit = pnlLimit;
        _countLimit = countLimit;
    }

    void updatePnlLimit(hf::price_t limit) {
        _pnlLimit = limit;
    }

    void updateCountLimit(hf::qty_t limit) {
        _countLimit = limit;
    }

    void updatePeriodInCycles(uint64_t periodInCycles) {
        _periodInCycles = periodInCycles;
    }

    void onFillPnl(hf::price_t pnl) {
        uint64_t now = clockUtil::GetCycles();
        expireEntries(now);
        if (pnl < 0) {
            addEntry(pnl, now);
        }
    }

    bool isOverLimit() {
        if (-_runningPnl > _pnlLimit) {
            LOG(0, "Scalp checker. PNL breach. PNL={d1} Limit={d2}", ToDollars(_runningPnl), ToDollars(_pnlLimit))
            return true;
        }
        if (_runningCount > _countLimit) {
            LOG(0, "Scalp checker. Count limit breach. Count={i1} Limit={i2}", _runningCount, _countLimit)
            return true;
        }
        return false;
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
                LOG(0, "Scalp checker. Expiring entry. Entry_PNL={d1} PNL={d2} Count={i1} Time={i2} Now={i3}", ToDollars(entry._pnl), ToDollars(_runningPnl), _runningCount, entry._time, now)
                _tail = getNext(_tail);
                if (_tail == _head) {
                    break;
                }
            } else {
                break;
            }
        }
    }

private:
    struct Entry {
        uint64_t _time;
        hf::price_t _pnl = 0;
    };

    void addEntry(hf::price_t pnl, uint64_t now) {
        Entry &entry = _entries[_head];
        entry._time = now;
        entry._pnl = pnl;
        ++_runningCount;
        _runningPnl += pnl;
        LOG(0, "Scalp checker. Adding entry. Entry_PNL={d1} PNL={d2} Count={i1} Time={i2}", ToDollars(pnl), ToDollars(_runningPnl), _runningCount, now)
        uint32_t head = getNext(_head);
        if (head != _tail) {
            _head = head;
        } else {
            LOG(0, "Scalp checker. - head would catch tail. {i1}", 0)
        }
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
        _negativeScalpChecker = negativeScalpChecker;
    }

    void match(hf::price_t fillPrice, hf::price_t queuedFillPrice, hf::qty_t size) {
        hf::price_t buyPrice = _side == BID ? queuedFillPrice : fillPrice;
        hf::price_t sellPrice = _side == BID ? fillPrice : queuedFillPrice;
        hf::price_t pnl = (sellPrice - buyPrice) * size * _multiplier;
        _newClosedPnl += pnl;
        if (_negativeScalpChecker != nullptr) {
            _negativeScalpChecker->onFillPnl(pnl);
        }
    }

    void onFill(FillMessage& fill, Pnl& pnlForType, Pnl& pnlTotal) {
        LOG(0, "PNL Calculator {C[i1]}. Side={i2} Price={d1} Size={i3}", getContractName(), fill._side, ToDollars(fill._price), fill._qty )
        if (_fills.empty()) {
            _fills.push_back(fill);
            _side = fill._side;
            LOG(0, "PNL Calculator {C[i1]}. Batching Fill. Price={d1} Side={i2} Size={i3}", getContractName(), ToDollars(fill._price), fill._side, fill._qty)
        } else {
            if (fill._side == _side) {
                _fills.push_back(fill);
                LOG(0, "PNL Calculator {C[i1]}. Batching Fill. Price={d1} Side={i2} Size={i3}", getContractName(), ToDollars(fill._price), fill._side, fill._qty)
            } else {
                for (auto it = _fills.begin(); it != _fills.end();) {
                    FillMessage& queuedFill = _fills.front();
                    if (queuedFill._qty == fill._qty) {
                        LOG(0, "PNL Calculator {C[i1]}. Fill. Matching equal. FillPrice={d1} QueueFillPrice={d2} Size={i2} FillSide={i3}", getContractName(), ToDollars(fill._price), ToDollars(queuedFill._price), fill._qty, fill._side)
                        match(fill._price, queuedFill._price, fill._qty);
                        _fills.erase(it);
                        fill._qty = 0;
                        break;
                    } else if (queuedFill._qty > fill._qty) {
                        LOG(0, "PNL Calculator {C[i1]}. Fill. Matching. Queued bigger. FillPrice={d1} QueueFillPrice={d2} Size={i2} FillSide={i3}", getContractName(), ToDollars(fill._price), ToDollars(queuedFill._price), fill._qty, fill._side)
                        match(fill._price, queuedFill._price, fill._qty);
                        queuedFill._qty -= fill._qty;
                        fill._qty = 0;
                        break;
                    } else {
                        LOG(0, "PNL Calculator {C[i1]}. Fill. Matching. Queued smaller. FillPrice={d1} QueueFillPrice={d2} Size={i2} FillSide={i3}", getContractName(), ToDollars(fill._price), ToDollars(queuedFill._price), fill._qty, fill._side)
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

        LOG(0,"PNL Calculator {C[i1]}. Open={d1} Closed={d2} MarkPrice={d3}", getContractName(), ToDollars(_open), ToDollars(_closed), ToDollars(_markPrice))
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

#ifdef IS_DEV
        std::string getContractName() {
            return _name;
        }
#else
        uint32_t getContractName() {
            return _contractId;
        }
#endif

    std::vector<FillMessage> _fills;
    uint32_t _side;

    hf::price_t _markPrice = 0;
    hf::price_t _newClosedPnl = 0;
    hf::price_t _newOpenPnl = 0;
    hf::price_t _multiplier = 0;

    NegativeScalpChecker* _negativeScalpChecker = nullptr;
    std::string _name;
    uint32_t _contractId;
};

struct AggregatedPnl {
    Pnl _relativeValue;
    Pnl _marketMakers;
    Pnl _execution;
    Pnl _total;
};

class PnlCalculator {
public:
    void init(std::vector<ConfigContract>& contracts) {
        _marketMakerContractPnlCalculators.resize(contracts.size());
        _relativeValueContractPnlCalculators.resize(contracts.size());
        for (uint32_t i = 0; i < contracts.size(); ++i) {
            auto& contract = contracts[i];
            hf::price_t multiplier = contract._tickValue / contract._tickSize;
            _marketMakerContractPnlCalculators[i].initialize(contract._name, multiplier, nullptr);
            _relativeValueContractPnlCalculators[i].initialize(contract._name, multiplier, nullptr);
        }
    }

    void onFill(FillMessage& fill) {
        if (fill._strategy == StrategyId::RelativeValue) {
            ContractPnlCalculator& pnl = _relativeValueContractPnlCalculators[fill._contractId];
            pnl.onFill(fill, _pnl._relativeValue, _pnl._total);
            LOG(0, "PNL: Total. Open={d1} Closed={d2} RV: Open={d3} Closed={d4}", ToDollars(_pnl._total._open), ToDollars(_pnl._total._closed), ToDollars(_pnl._relativeValue._open), ToDollars(_pnl._relativeValue._closed))
        }
        if (fill._strategy == StrategyId::MarketMaker) {
            ContractPnlCalculator& pnl = _marketMakerContractPnlCalculators[fill._contractId];
            pnl.onFill(fill, _pnl._marketMakers, _pnl._total);
            LOG(0, "PNL: Total. Open={d1} Closed={d2} MM: Open={d3} Closed={d4}", ToDollars(_pnl._total._open), ToDollars(_pnl._total._closed), ToDollars(_pnl._marketMakers._open), ToDollars(_pnl._marketMakers._closed))
        }
    }

    AggregatedPnl& getAggregatedPnl() { return _pnl; }

    ContractPnlCalculator& getPnlDetail(uint32_t index, StrategyId strategyId) {
        if (strategyId == StrategyId::RelativeValue) {
            return _relativeValueContractPnlCalculators[index];
        }
        if (strategyId == StrategyId::MarketMaker) {
            return _marketMakerContractPnlCalculators[index];
        }
        BOOST_ASSERT(false);
    }

    bool hasDifferentMarkPrice(uint32_t index, hf::price_t price) {
        ContractPnlCalculator& rvPnl = _relativeValueContractPnlCalculators[index];
        return price != rvPnl._markPrice;
    }

    void recalculateOpenPnl(uint32_t index, hf::price_t price) {
        ContractPnlCalculator& rvPnl = _relativeValueContractPnlCalculators[index];
        rvPnl.onMarkPriceChange(price, _pnl._relativeValue, _pnl._total);
        ContractPnlCalculator& mmPnl = _marketMakerContractPnlCalculators[index];
        mmPnl.onMarkPriceChange(price, _pnl._marketMakers, _pnl._total);
    }

    std::vector<ContractPnlCalculator> _marketMakerContractPnlCalculators;
    std::vector<ContractPnlCalculator> _relativeValueContractPnlCalculators;

    AggregatedPnl _pnl;
};

class PnlCalculatorsForNegativeScalpChecker {
public:
    void init(std::vector<ConfigContract>& contracts, NegativeScalpChecker* checker) {
        _calculators.resize(contracts.size());
        for (uint32_t i = 0; i < contracts.size(); ++i) {
            auto& contract = contracts[i];
            hf::price_t multiplier = contract._tickValue / contract._tickSize;
            _calculators[i].initialize(contract._name, multiplier, checker);
        }
    }

    void onFill(FillMessage& fill) {
        ContractPnlCalculator& pnl = _calculators[fill._contractId];
        pnl.onFill(fill, _pnl._relativeValue, _pnl._total);
    }

    AggregatedPnl _pnl;
    std::vector<ContractPnlCalculator> _calculators;
};


}

#endif //VIPER_NEGATIVE_SCALP_CHECKER_H