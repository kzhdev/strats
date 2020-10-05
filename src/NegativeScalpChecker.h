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

    bool isOverLimit(std::string& message) {
        if (-_runningPnl > _pnlLimit) {
            message = boost::str(boost::format("Scalp checker. PNL breach. PNL=%1% Limit=%2%") % ToDollars(_runningPnl) % ToDollars(_pnlLimit));
            LOG(0, "Scalp checker. PNL breach. PNL={d1} Limit={d2}", ToDollars(_runningPnl), ToDollars(_pnlLimit))
            return true;
        }
        if (_runningCount > _countLimit) {
            message = boost::str(boost::format("Scalp checker. Count limit breach. Count=%1% Limit=%2%") % _runningCount % _countLimit);
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
    uint32_t _size = CHECKER_ENTRY_SIZE;
    uint64_t _periodInCycles;
    Entry _entries[CHECKER_ENTRY_SIZE];

    hf::price_t _runningPnl = 0;
    hf::qty_t _runningCount = 0;

    hf::price_t _pnlLimit = 0;
    hf::qty_t _countLimit = 0;
};

struct Pnl {
    hf::price_t _open = 0;
    hf::price_t _closed = 0;
    hf::price_t _drawdown = 0;

    hf::price_t getTotal() const noexcept { return _open + _closed; }
};

class ContractPnlCalculator : public Pnl {
public:
    ContractPnlCalculator() {
    }

    void initialize(const std::string& name, uint32_t contractId, hf::price_t multiplier, NegativeScalpChecker* negativeScalpChecker) {
        _name = name;
        _contractId = contractId;
        _multiplier = multiplier;
        _negativeScalpChecker = negativeScalpChecker;
    }

    bool shouldExpireFills() {
        return _negativeScalpChecker != nullptr;
    }

    void expireFills() {
        uint64_t minTime = clockUtil::GetCycles() - _negativeScalpChecker->_periodInCycles;
        while (!_fills.empty()) {
            FillMessage& queuedFill = _fills.front();
            if (queuedFill._timestamp < minTime) {
                LOG(0, "PNL Calculator {C[i1]}. Expired fill for NSC. Side={i2} Price={d1} Size={i3}", getContractName(), queuedFill._side, ToDollars(queuedFill._price), queuedFill._qty )
                _fills.pop_front();
            } else {
                break;
            }
        }
    }

    void match(hf::price_t fillPrice, hf::price_t queuedFillPrice, hf::qty_t size) {
        hf::price_t buyPrice = _side == BID ? queuedFillPrice : fillPrice;
        hf::price_t sellPrice = _side == BID ? fillPrice : queuedFillPrice;
        hf::price_t pnl = (sellPrice - buyPrice) * size * _multiplier;
        _newClosedPnl += pnl;
        if (BOOST_LIKELY(_negativeScalpChecker != nullptr)) {
            _negativeScalpChecker->onFillPnl(pnl);
        }
    }

    void onFill(const FillMessage& fill, Pnl& pnlForType, Pnl& pnlTotal) {
        FillMessage incomingFill = fill;
        if (BOOST_LIKELY(shouldExpireFills())) {
            incomingFill._timestamp =  clockUtil::GetCycles();
            expireFills();
        }
        LOG(0, "PNL Calculator {C[i1]}. Strategy={i2} Side={i3} Price={d1} Size={i4}", getContractName(),
            strategyName(fill._strategy), incomingFill._side, ToDollars(incomingFill._price), incomingFill._qty )
        if (_fills.empty()) {
            _fills.push_back(incomingFill);
            _side = incomingFill._side;
            LOG(0, "PNL Calculator {C[i1]}. Batching Fill. Price={d1} Side={i2} Size={i3}", getContractName(), ToDollars(incomingFill._price), incomingFill._side, incomingFill._qty)
        } else {
            if (incomingFill._side == _side) {
                _fills.push_back(incomingFill);
                LOG(0, "PNL Calculator {C[i1]}. Batching Fill. Price={d1} Side={i2} Size={i3}", getContractName(), ToDollars(incomingFill._price), incomingFill._side, incomingFill._qty)
            } else {
                while (!_fills.empty()) {
                    FillMessage& queuedFill = _fills.front();
                    if (queuedFill._qty == incomingFill._qty) {
                        LOG(0, "PNL Calculator {C[i1]}. Fill. Matching equal. FillPrice={d1} QueueFillPrice={d2} Size={i2} FillSide={i3}", getContractName(), ToDollars(incomingFill._price),
                                                                                               ToDollars(queuedFill._price), incomingFill._qty, incomingFill._side)
                        match(incomingFill._price, queuedFill._price, incomingFill._qty);
                        _fills.pop_front();
                        incomingFill._qty = 0;
                        break;
                    } else if (queuedFill._qty > incomingFill._qty) {
                        LOG(0, "PNL Calculator {C[i1]}. Fill. Matching. Queued bigger. FillPrice={d1} QueueFillPrice={d2} Size={i2} FillSide={i3}", getContractName(), ToDollars(incomingFill._price),
                                                                                               ToDollars(queuedFill._price), incomingFill._qty, incomingFill._side)
                        match(incomingFill._price, queuedFill._price, incomingFill._qty);
                        queuedFill._qty -= incomingFill._qty;
                        incomingFill._qty = 0;
                        break;
                    } else {
                        LOG(0, "PNL Calculator {C[i1]}. Fill. Matching. Queued smaller. FillPrice={d1} QueueFillPrice={d2} Size={i2} FillSide={i3}", getContractName(), ToDollars(incomingFill._price),
                                                                                               ToDollars(queuedFill._price), incomingFill._qty, incomingFill._side)
                        match(incomingFill._price, queuedFill._price, queuedFill._qty);
                        incomingFill._qty -= queuedFill._qty;
                        _fills.pop_front();
                    }
                }
                if (incomingFill._qty > 0) {
                    _fills.push_back(incomingFill);
                    _side = incomingFill._side;
                }
            }
        }

        if(BOOST_LIKELY(incomingFill._type != FillType::Phantom && incomingFill._source != Python::FillSource::ViperOff)) {
            calculateOpenPnl(pnlForType, pnlTotal);
            calculateClosedPnl(pnlForType, pnlTotal);
        }

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

    std::deque<FillMessage> _fills;
    uint32_t _side;

    hf::price_t _markPrice = 0;
    hf::price_t _newClosedPnl = 0;
    hf::price_t _newOpenPnl = 0;
    hf::price_t _multiplier = 0;

    NegativeScalpChecker* _negativeScalpChecker = nullptr;
    std::string _name;
    uint32_t _contractId;
};

class PnlCalculator {
public:
    void init(std::vector<ConfigContract>& contracts) {
        for (auto& calculator : _contractPnlCalculators) {
            calculator.resize(contracts.size());
        }
        for (uint32_t i = 0; i < contracts.size(); ++i) {
            auto& contract = contracts[i];
            hf::price_t multiplier = contract._tickValue / contract._tickSize;
            for (auto& calculator : _contractPnlCalculators) {
                calculator[i].initialize(contract._name, i, multiplier, nullptr);
            }
        }
    }

    void onFill(FillMessage& fill) {
        auto& contractPnl = _contractPnlCalculators[toUInt(fill._strategy)][fill._contractId];
        auto& aggPnl = _pnl[toUInt(fill._strategy)];
        contractPnl.onFill(fill, aggPnl, _pnlTotal);
        LOG(0, "PNL: Total. Open={d1} Closed={d2} Strategy {i1}: Open={d3} Closed={d4}", ToDollars(_pnlTotal._open),
            ToDollars(_pnlTotal._closed), strategyName(fill._strategy), ToDollars(aggPnl._open), ToDollars(aggPnl._closed))
    }

    Pnl& getAggregatedPnl(StrategyId strategy) { return _pnl[toUInt(strategy)]; }

    ContractPnlCalculator& getPnlDetail(uint32_t index, StrategyId strategyId) {
        return _contractPnlCalculators[toUInt(strategyId)][index];
    }

    bool hasDifferentMarkPrice(uint32_t index, hf::price_t price) {
        bool different = false;
        for (auto& pnls : _contractPnlCalculators) {
            different |= price != pnls[index]._markPrice;
        }
        return different;
    }

    void recalculateOpenPnl(uint32_t index, hf::price_t price) {
        for (size_t i = 0; i < toUInt(StrategyId::Execution); ++i) {
            auto& pnl = _contractPnlCalculators[i][index];
            auto& aggPnl = _pnl[i];
            pnl.onMarkPriceChange(price, aggPnl, _pnlTotal);
        }
    }

    std::vector<ContractPnlCalculator> _contractPnlCalculators[toUInt(StrategyId::Execution)];
    Pnl _pnl[toUInt(StrategyId::MAX)];
    Pnl _pnlTotal;
};

class PnlCalculatorsForNegativeScalpChecker {
public:
    void init(std::vector<ConfigContract>& contracts, NegativeScalpChecker* checker) {
        _calculators.resize(contracts.size());
        for (uint32_t i = 0; i < contracts.size(); ++i) {
            auto& contract = contracts[i];
            hf::price_t multiplier = contract._tickValue / contract._tickSize;
            _calculators[i].initialize(contract._name, i, multiplier, checker);
        }
    }

    void onFill(FillMessage& fill) {
        ContractPnlCalculator& pnl = _calculators[fill._contractId];
        pnl.onFill(fill, _pnl, _pnlTotal);
    }

    Pnl _pnl;
    Pnl _pnlTotal;
    std::vector<ContractPnlCalculator> _calculators;
};


}

#endif //VIPER_NEGATIVE_SCALP_CHECKER_H