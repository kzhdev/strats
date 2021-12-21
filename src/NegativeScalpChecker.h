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
    NegativeScalpChecker() {
        memset(_entries, 0, sizeof(_entries));
    }

    void initialize(const std::string& name, uint32_t contractId, uint64_t periodInCycles, hf::price_t pnlLimit, hf::qty_t countLimit) {
        _name = name;
        _contractId = contractId;
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

    void onFillPnl(hf::price_t pnl, uint64_t now, StrategyId strategy) {
        if (pnl < 0) {
            addEntry(pnl, now, strategy);
        }
    }

    enum class CheckResult : uint8_t {
        OK,
        OverLimit,
        MMOverLimit,
    };

    CheckResult isOverLimit(std::string& message) {
        if (-_runningPnl > _pnlLimit) {
            message = boost::str(boost::format("%1% Scalp checker. PNL breach. PNL=%2% Limit=%3%") % _name % ToDollars(_runningPnl) % ToDollars(_pnlLimit));
            LOG(0, "{i1} Scalp checker. PNL breach. PNL={d1} Limit={d2}", getContractName(), ToDollars(_runningPnl), ToDollars(_pnlLimit))
            return CheckResult::OverLimit;
        }

        if (_runningCount > _countLimit) {
            message = boost::str(boost::format("%1% Scalp checker. Count limit breach. Count=%2% Limit=%3%") % _name % _runningCount % _countLimit);
            LOG(0, "{i1} Scalp checker. Count limit breach. Count={i2} Limit={i3}", getContractName(), _runningCount, _countLimit)
            return CheckResult::OverLimit;
        }

        if (-_mmRunningPnl > _pnlLimit) {
            message = boost::str(boost::format("%1% Scalp checker. MM PNL breach. PNL=%2% Limit=%3%") % _name % ToDollars(_mmRunningPnl) % ToDollars(_pnlLimit));
            LOG(0, "{i1} Scalp checker. PNL breach. MM PNL={d1} Limit={d2}", getContractName(), ToDollars(_mmRunningPnl), ToDollars(_pnlLimit))
            return CheckResult::MMOverLimit;
        }

        if (_mmRunningCount > _countLimit) {
            message = boost::str(boost::format("%1% Scalp checker. MM Count limit breach. Count=%2% Limit=%3%") % _name % _mmRunningCount % _countLimit);
            LOG(0, "{i1} Scalp checker. Count limit breach. MM Count={i2} Limit={i3}", getContractName(), _mmRunningCount, _countLimit)
            return CheckResult::MMOverLimit;
        }
        return CheckResult::OK;
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
                if (entry._strategy == StrategyId::MarketMaker) {
                    --_mmRunningCount;
                    _mmRunningPnl -= entry._pnl;
                }
                LOG(0, "{C[i1]} Scalp checker. Expiring entry. Entry_PNL={d1} PNL={d2} Count={i2} mmPNL={d3} mmCount={i3} Time={i4} Now={i5}", getContractName(),
                    ToDollars(entry._pnl), ToDollars(_runningPnl), _runningCount, ToDollars(_mmRunningPnl),
                    _mmRunningCount, entry._time, now)
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
        StrategyId _strategy = StrategyId::MAX;
    };

    void addEntry(hf::price_t pnl, uint64_t now, StrategyId strategy) {
        assert(strategy != StrategyId::MAX);
        Entry &entry = _entries[_head];
        entry._time = now;
        entry._pnl = pnl;
        entry._strategy = strategy;
        ++_runningCount;
        _runningPnl += pnl;
        if (strategy == StrategyId::MarketMaker) {
            ++_mmRunningCount;
            _mmRunningPnl += pnl;
        }
        LOG(0, "{C[i1]} Scalp checker. Adding entry. Entry_PNL={d1} PNL={d2} Count={i2} mmPNL={d3} mmCCount={i3} Time={i4}", getContractName(),
            ToDollars(pnl), ToDollars(_runningPnl), _runningCount, ToDollars(_mmRunningPnl), _mmRunningCount, now)
        uint32_t head = getNext(_head);
        if (head != _tail) {
            _head = head;
        } else {
            LOG(0, "{C[i1]} Scalp checker. - head would catch tail.", getContractName())
        }
    }

    uint32_t getNext(uint32_t current) {
        return ++current & _mask;
    }

#ifdef IS_DEV
    const std::string& getContractName() {
        return _name;
    }
#else
    uint32_t getContractName() {
        return _contractId;
    }
#endif

    hf::price_t _runningPnl = 0;
    hf::qty_t _runningCount = 0;

    hf::price_t _mmRunningPnl = 0;
    hf::qty_t _mmRunningCount = 0;

    hf::price_t _pnlLimit = 0;
    hf::qty_t _countLimit = 0;

    uint64_t _periodInCycles;
    uint32_t _head = 0;
    uint32_t _tail = 0;
    uint32_t _contractId;
    static constexpr uint32_t _mask = CHECKER_ENTRY_SIZE - 1;
    Entry _entries[CHECKER_ENTRY_SIZE];
    std::string _name;
};

struct Pnl {
    hf::price_t _open = 0;
    hf::price_t _closed = 0;
    hf::price_t _drawdown = 0;

    hf::price_t getTotal() const noexcept { return _open + _closed; }
    void reset() noexcept {
        _open = 0;
        _closed = 0;
        _drawdown = 0;
    }
};

enum CalculatorId : uint8_t {
    PNL_CALCULATOR = 0,
    PNL_CALCULATOR_FOR_GLOBAL_NEGATIVE_SCALP_CHECKER = 1,
    PNL_CALCULATOR_FOR_CONTRACT_NEGATIVE_SCALP_CHECKER = 2,
};

class ContractPnlCalculator : public Pnl {
public:
    ContractPnlCalculator() {
    }

    void initialize(CalculatorId id, const std::string& name, uint32_t contractId, hf::price_t multiplier,
                    NegativeScalpChecker* negativeScalpChecker) {
        _id = id;
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
                LOG(0, "PNL Calculator {i1} {C[i2]}. Expired fill for NSC. Side={i3} Price={d1} Size={i4}", toUInt(_id),
                    getContractName(), queuedFill._side, ToDollars(queuedFill._price), queuedFill._qty )
                _fills.pop_front();
            } else {
                break;
            }
        }
    }

    void match(hf::price_t fillPrice, hf::price_t queuedFillPrice, hf::qty_t size, uint64_t now, StrategyId strategy) {
        hf::price_t buyPrice = _side == BID ? queuedFillPrice : fillPrice;
        hf::price_t sellPrice = _side == BID ? fillPrice : queuedFillPrice;
        hf::price_t pnl = (sellPrice - buyPrice) * size * _multiplier;
        _newClosedPnl += pnl;
        if (BOOST_LIKELY(_negativeScalpChecker != nullptr)) {
            _negativeScalpChecker->onFillPnl(pnl, now, strategy);
        }
    }

    void onFill(const FillMessage& fill, uint64_t now) {
        FillMessage incomingFill = fill;
        if (BOOST_LIKELY(shouldExpireFills())) {
            incomingFill._timestamp =  clockUtil::GetCycles();
            expireFills();
        }
        LOG(0, "PNL Calculator {i1} {C[i2]}. Strategy={i3} Side={i4} Price={d1} Size={i5}", toUInt(_id), getContractName(),
            strategyName(fill._strategy), incomingFill._side, ToDollars(incomingFill._price), incomingFill._qty )
        if (_fills.empty()) {
            _fills.push_back(incomingFill);
            _side = incomingFill._side;
            LOG(0, "PNL Calculator {i1} {C[i2]}. Batching Fill. Price={d1} Side={i3} Size={i4}", toUInt(_id),
                getContractName(), ToDollars(incomingFill._price), incomingFill._side, incomingFill._qty)
        } else {
            if (incomingFill._side == _side) {
                _fills.push_back(incomingFill);
                LOG(0, "PNL Calculator {i1} {C[i2]}. Batching Fill. Price={d1} Side={i3} Size={i4}", toUInt(_id),
                    getContractName(), ToDollars(incomingFill._price), incomingFill._side, incomingFill._qty)
            } else {
                while (!_fills.empty()) {
                    FillMessage& queuedFill = _fills.front();
                    if (queuedFill._qty == incomingFill._qty) {
                        LOG(0, "PNL Calculator {i1} {C[i2]}. Fill. Matching equal. FillPrice={d1} QueueFillPrice={d2} Size={i3} FillSide={i4}", toUInt(_id),
                            getContractName(), ToDollars(incomingFill._price), ToDollars(queuedFill._price),
                            incomingFill._qty, incomingFill._side)
                        match(incomingFill._price, queuedFill._price, incomingFill._qty, now, incomingFill._strategy);
                        _fills.pop_front();
                        incomingFill._qty = 0;
                        break;
                    } else if (queuedFill._qty > incomingFill._qty) {
                        LOG(0, "PNL Calculator {i1} {C[i2]}. Fill. Matching. Queued bigger. FillPrice={d1} QueueFillPrice={d2} Size={i3} FillSide={i4}", toUInt(_id),
                            getContractName(), ToDollars(incomingFill._price), ToDollars(queuedFill._price),
                            incomingFill._qty, incomingFill._side)
                        match(incomingFill._price, queuedFill._price, incomingFill._qty, now, incomingFill._strategy);
                        queuedFill._qty -= incomingFill._qty;
                        incomingFill._qty = 0;
                        break;
                    } else {
                        LOG(0, "PNL Calculator {i1} {C[i2]}. Fill. Matching. Queued smaller. FillPrice={d1} QueueFillPrice={d2} Size={i3} FillSide={i4}", toUInt(_id),
                            getContractName(), ToDollars(incomingFill._price), ToDollars(queuedFill._price),
                            incomingFill._qty, incomingFill._side)
                        match(incomingFill._price, queuedFill._price, queuedFill._qty, now, incomingFill._strategy);
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
    }

    void calculateClosedPnl(Pnl& pnlForType, Pnl& pnlTotal) {
        hf::price_t closedPnlChange = _newClosedPnl - _closed;
        pnlForType._closed += closedPnlChange;
        pnlTotal._closed += closedPnlChange;
        _closed = _newClosedPnl;
        LOG(0, "PNL Calculator {i1} {C[i2]}. Closed={d1} MarkPrice={d2}", toUInt(_id), getContractName(), ToDollars(_closed), ToDollars(_markPrice))
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
        LOG(0, "PNL Calculator {i1} {C[i2]}. Open={d1} MarkPrice={d2}", toUInt(_id), getContractName(), ToDollars(_open), ToDollars(_markPrice))
    }

    void onMarkPriceChange(hf::price_t price, Pnl& pnlForType, Pnl& pnlTotal) {
        _markPrice = price;
        calculateOpenPnl(pnlForType, pnlTotal);
    }

#ifdef IS_DEV
    const std::string& getContractName() {
        return _name;
    }
#else
    uint32_t getContractName() {
        return _contractId;
    }
#endif

    CalculatorId _id;
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
                calculator[i].initialize(CalculatorId::PNL_CALCULATOR, contract._name, i, multiplier, nullptr);
            }
        }
    }

    void onFill(FillMessage& fill, uint64_t now, bool updateOpenPnl) {
        auto& contractPnl = _contractPnlCalculators[toUInt(fill._strategy)][fill._contractId];
        auto& aggPnl = _pnl[toUInt(fill._strategy)];
        contractPnl.onFill(fill, now);

        if(BOOST_LIKELY(fill._type != FillType::Phantom && fill._source != Python::FillSource::ViperOff)) {
            if (updateOpenPnl) {
                contractPnl.calculateOpenPnl(aggPnl, _pnlTotal);
            }
            contractPnl.calculateClosedPnl(aggPnl, _pnlTotal);
        }

        LOG(0, "PNL: Total. Open={d1} Closed={d2} Strategy {i1}: Open={d3} Closed={d4}", ToDollars(_pnlTotal._open),
            ToDollars(_pnlTotal._closed), strategyName(fill._strategy), ToDollars(aggPnl._open), ToDollars(aggPnl._closed))
    }

    Pnl& getAggregatedPnl(StrategyId strategy) { return _pnl[toUInt(strategy)]; }

    ContractPnlCalculator& getPnlDetail(uint32_t index, StrategyId strategyId) {
        return _contractPnlCalculators[toUInt(strategyId)][index];
    }

    hf::price_t getTotalPnl(uint32_t index) const noexcept {
        hf::price_t totalPnl = 0;
        for (auto& pnlCalculator : _contractPnlCalculators) {
            totalPnl += pnlCalculator[index].getTotal();
        }
        return totalPnl;
    }

    bool hasDifferentMarkPrice(uint32_t index, hf::price_t price) {
        bool different = false;
        for (auto& pnls : _contractPnlCalculators) {
            different |= price != pnls[index]._markPrice;
        }
        return different;
    }

    void recalculateOpenPnl(uint32_t index, hf::price_t price) {
        for (size_t i = 0; i < toUInt(StrategyId::MAX); ++i) {
            auto& pnl = _contractPnlCalculators[i][index];
            auto& aggPnl = _pnl[i];
            pnl.onMarkPriceChange(price, aggPnl, _pnlTotal);
        }
    }

    std::vector<ContractPnlCalculator> _contractPnlCalculators[toUInt(StrategyId::MAX)];
    Pnl _pnl[toUInt(StrategyId::MAX)];
    Pnl _pnlTotal;
};

class PnlCalculatorsForGlobalNegativeScalpChecker {
public:
    void init(std::vector<ConfigContract>& contracts, NegativeScalpChecker* checker) {
        _calculators.resize(contracts.size());
        for (uint32_t i = 0; i < contracts.size(); ++i) {
            auto& contract = contracts[i];
            hf::price_t multiplier = contract._tickValue / contract._tickSize;
            _calculators[i].initialize(CalculatorId::PNL_CALCULATOR_FOR_GLOBAL_NEGATIVE_SCALP_CHECKER, contract._name, i, multiplier, checker);
        }
    }

    void onFill(FillMessage& fill, uint64_t now) {
        ContractPnlCalculator& pnl = _calculators[fill._contractId];
        pnl.onFill(fill, now);
    }
    std::vector<ContractPnlCalculator> _calculators;
};

class PnlCalculatorsForContractNegativeScalpChecker {
public:
    void init(const ViperConfig& viperConfig, const std::vector<NegativeScalpCheckerConfig>& configs, double frequency) {
        _negativeScalpCheckers.resize(viperConfig._allContracts.size());
        _calculators.resize(viperConfig._allContracts.size());
        for (uint32_t i = 0; i < configs.size(); ++i) {
            auto& config = configs[i];
            auto contract = viperConfig.getContract(config._contract);
            if (BOOST_LIKELY(contract != nullptr) && contract->_toMarketMake) {
                auto& checker = _negativeScalpCheckers[contract->_internalId];
                checker.initialize(contract->_name, contract->_internalId, GetCyclesFromMilliSeconds(config._period, frequency), ToRhonePrice(config._pnlLimit), config._countLimit);
                hf::price_t multiplier = contract->_tickValue / contract->_tickSize;
                _calculators[contract->_internalId].initialize(CalculatorId::PNL_CALCULATOR_FOR_CONTRACT_NEGATIVE_SCALP_CHECKER, contract->_name, contract->_internalId, multiplier, &checker);
            }
        }
    }

    NegativeScalpChecker::CheckResult onFill(FillMessage& fill, uint64_t now, std::string& message) {
        ContractPnlCalculator& pnl = _calculators[fill._contractId];
        if (BOOST_UNLIKELY(pnl._negativeScalpChecker == nullptr)) {
            // negativeScalpChecker not set, it is must be a not toMarketMake contract
            assert(false && "should not come here");
            return NegativeScalpChecker::CheckResult::OK;
        }
        auto& checker = _negativeScalpCheckers[fill._contractId];
        checker.expireEntries(now);
        pnl.onFill(fill, now);
        return checker.isOverLimit(message);
    }

    NegativeScalpChecker& getNegativeScalpChecker(uint32_t contractId) noexcept {
        return _negativeScalpCheckers[contractId];
    }

    std::vector<NegativeScalpChecker> _negativeScalpCheckers;
    std::vector<ContractPnlCalculator> _calculators;
};

}

#endif //VIPER_NEGATIVE_SCALP_CHECKER_H