#ifndef VIPER_PNL_LIMITS_CHECKER_H
#define VIPER_PNL_LIMITS_CHECKER_H

#include "boost/format.hpp"
#include "Common.h"
#include "NegativeScalpChecker.h"
#include "GUIThings.h"

namespace Python {

struct PnlCheckData {
    hf::price_t _maxPnl = 0;
    hf::price_t _pnlLimit = 0;
    hf::price_t _drawDownLimit = 0;
};

class PnlLimitsChecker {
public:

    void initialize(PnlCalculator* calculator, ViperConfig& config) {
        setPnlLimits("Total", _totalLimits, config);
        for(uint32_t i = 0; i < toUInt(StrategyId::Execution); ++i) {
            setPnlLimits(toString((StrategyId)i), _strategyLimits[i], config);
        }
        _pnlCalculator = calculator;
    }

    bool checkLimits(StrategyId strategyId) {
        bool limitsBreached = false;
        limitsBreached |= checkTotalLimits();
        limitsBreached |= checkStrategyLimits(strategyId);
        return limitsBreached;
    }

    void handlePnlLimitChange(GridUpdate& update, Shifts currentShift) {
        auto updateLimit = [currentShift](PnlCheckData& data, hf::price_t limit, Shifts shift, PNLRows type) {
            if (currentShift == shift) {
                data._pnlLimit = limit;
                LOG(0, "Pnl limit changed. LimitType={i1} Value={d1}", toUInt(type), ToDollars(limit))
            }
        };
        auto updateDrawdown = [currentShift](PnlCheckData& data, hf::price_t limit, Shifts shift, PNLRows type) {
            if (currentShift == shift) {
                data._drawDownLimit = limit;
                LOG(0, "Drawdown limit changed. LimitType={i1} Value={d1}", toUInt(type), ToDollars(limit))
            }
        };

        for (auto& one : update._update.cells()) {
            PNLRows type = static_cast<PNLRows>(one.row());
            hf::price_t limit = ToRhonePrice(boost::lexical_cast<int64_t>(one.value()));
            switch (type) {
                case PNLRows::RelativeValue:
                case PNLRows::MarketMakers:
                case PNLRows::ArbieSpreader: {
                    StrategyId strategyId = static_cast<StrategyId>(type);
                    switch(one.col()) {
                        case toUInt(PNLColumns::AsiaPnlLimit):
                            updateLimit(_strategyLimits[toUInt(strategyId)], limit, Shifts::Asia, type);
                            break;
                        case toUInt(PNLColumns::AsiaDrawdownLimit):
                            updateDrawdown(_strategyLimits[toUInt(strategyId)], limit, Shifts::Asia, type);
                            break;
                        case toUInt(PNLColumns::EuropePnlLimit):
                            updateLimit(_strategyLimits[toUInt(strategyId)], limit, Shifts::Europe, type);
                            break;
                        case toUInt(PNLColumns::EuropeDrawdownLimit):
                            updateDrawdown(_strategyLimits[toUInt(strategyId)], limit, Shifts::Europe, type);
                            break;
                        case toUInt(PNLColumns::USPnlLimit):
                            updateLimit(_strategyLimits[toUInt(strategyId)], limit, Shifts::US, type);
                            break;
                        case toUInt(PNLColumns::USDrawdownLimit):
                            updateDrawdown(_strategyLimits[toUInt(strategyId)], limit, Shifts::US, type);
                            break;
                        case toUInt(PNLColumns::SafeModePnlLimit):
                            updateLimit(_strategyLimits[toUInt(strategyId)], limit, Shifts::SafeMode, type);
                            break;
                        case toUInt(PNLColumns::SafeModeDrawdownLimit):
                            updateDrawdown(_strategyLimits[toUInt(strategyId)], limit, Shifts::SafeMode, type);
                            break;
                    }
                }
                    break;
                case PNLRows::Total: {
                    switch(one.col()) {
                        case toUInt(PNLColumns::AsiaPnlLimit):
                            updateLimit(_totalLimits, limit, Shifts::Asia, type);
                            break;
                        case toUInt(PNLColumns::AsiaDrawdownLimit):
                            updateDrawdown(_totalLimits, limit, Shifts::Asia, type);
                            break;
                        case toUInt(PNLColumns::EuropePnlLimit):
                            updateLimit(_totalLimits, limit, Shifts::Europe, type);
                            break;
                        case toUInt(PNLColumns::EuropeDrawdownLimit):
                            updateDrawdown(_totalLimits, limit, Shifts::Europe, type);
                            break;
                        case toUInt(PNLColumns::USPnlLimit):
                            updateLimit(_totalLimits, limit, Shifts::US, type);
                            break;
                        case toUInt(PNLColumns::USDrawdownLimit):
                            updateDrawdown(_totalLimits, limit, Shifts::US, type);
                            break;
                        case toUInt(PNLColumns::SafeModePnlLimit):
                            updateLimit(_totalLimits, limit, Shifts::SafeMode, type);
                            break;
                        case toUInt(PNLColumns::SafeModeDrawdownLimit):
                            updateDrawdown(_totalLimits, limit, Shifts::SafeMode, type);
                            break;
                    }
                }
                    break;
                default:
                    break;
            }
        }
    }

    void changeLimitsOnShiftChanged(ViperConfig& viperConfig, uint32_t shift) {
        LOG(0, "PNL Checker. Change limit to shift {i1}", shift)
        const auto& pnlConfig = viperConfig._pnlConfigs;
        for (auto i = 0u; i < toUInt(StrategyId::Execution); ++i) {
            auto& limitConfig = pnlConfig[i];
            _strategyLimits[i]._pnlLimit = ToRhonePrice(limitConfig._shiftLimit[shift]);
            LOG(0, "PNL Checker. Change strategy {i1} pnlLimit to {d1}", i, ToDollars(_strategyLimits[i]._pnlLimit))
            _strategyLimits[i]._drawDownLimit = ToRhonePrice(limitConfig._shiftDrawdown[shift]);
            LOG(0, "PNL Checker. Change strategy {i1} drawdownLimit to {d1}", i, ToDollars(_strategyLimits[i]._pnlLimit));
        }
        _totalLimits._pnlLimit = ToRhonePrice(pnlConfig[toUInt(PNLRows::Total)]._shiftLimit[shift]);
        LOG(0, "PNL Checker. Change total pnlLimit to {d1}", ToDollars(_totalLimits._pnlLimit))
        _totalLimits._drawDownLimit = ToRhonePrice(pnlConfig[toUInt(PNLRows::Total)]._shiftDrawdown[shift]);
        LOG(0, "PNL Checker. Change total drawdownLimit to {d1}", ToDollars(_totalLimits._pnlLimit))
    }

    void logState() {
        LOG(0, "PNL. Pnl: Total={d1}", ToDollars(_pnlCalculator->_pnlTotal.getTotal()))
        for (uint32_t i = 0; i < toUInt(StrategyId::Execution); ++i) {
            LOG(0, "PNL. Pnl: {i1}={d1}", strategyName((StrategyId)i), ToDollars(_pnlCalculator->_pnl[i].getTotal()))
        }
        LOG(0, "PNL. Drawdown: Total={d1}", ToDollars(_pnlCalculator->_pnlTotal._drawdown))
        for (uint32_t i = 0; i < toUInt(StrategyId::Execution); ++i) {
            LOG(0, "PNL. Drawdown: {i1}={d1}", strategyName((StrategyId)i), ToDollars(_pnlCalculator->_pnl[i]._drawdown))
        }
        LOG(0, "PNL. Limits: Total. Pnl={d1} D={d2}", //RV. Pnl={d3} D={d4} MM. Pnl={d5} D={d6}",
            ToDollars(_totalLimits._pnlLimit), ToDollars(_totalLimits._drawDownLimit))

        for (uint32_t i = 0; i < toUInt(StrategyId::Execution); ++i) {
            LOG(0, "PNL. Limits: {i1}. Pnl={d1} D={d2}", strategyName((StrategyId)i),
                ToDollars(_strategyLimits[i]._pnlLimit),
                ToDollars(_strategyLimits[i]._drawDownLimit))
        }
    }

    bool checkTotalLimits() {
        return checkPnlLimits(_pnlCalculator->_pnlTotal, _totalLimits, 99);
    }

    bool checkStrategyLimits(StrategyId strategyId) {
        auto& pnl = _pnlCalculator->getAggregatedPnl(strategyId);
        return checkPnlLimits(pnl, _strategyLimits[toUInt(strategyId)], toUInt(strategyId));
    }

private:
    void setPnlLimits(const std::string& name, PnlCheckData& data, ViperConfig& viperConfig) {
        auto& config = viperConfig._pnlConfigs[toUInt(viperConfig.getPnlRow(name))];
        data._drawDownLimit = ToRhonePrice(config._drawdown);
        data._pnlLimit = ToRhonePrice(config._limit);
    }

    bool checkPnlLimits(Pnl& pnl, PnlCheckData& limits, uint32_t limitType) {
        bool limitsBreached = checkPnlLimit(pnl, limits, limitType);
        limitsBreached |= checkDrawdownLimit(pnl, limits, limitType);
        return limitsBreached;
    }

    bool checkPnlLimit(const Pnl& pnl, PnlCheckData& limits, uint32_t limitType) {
        const hf::price_t totalPnl = pnl.getTotal();
        auto pnlLimit = limits._pnlLimit;
        if (totalPnl < -pnlLimit) {
            LOG(0, "Pnl limit breached. LimitType={i1} Limit={d1} Pnl={d2}", limitType, ToDollars(pnlLimit), ToDollars(totalPnl))
            std::cout << "PNL breach" << std::endl;
            return true;
        }
        return false;
    }

    bool checkDrawdownLimit(Pnl& pnl, PnlCheckData& limits, uint32_t limitType) {
        const hf::price_t totalPnl = pnl.getTotal();
        limits._maxPnl = std::max(limits._maxPnl, totalPnl);
        hf::price_t newDrawdown = limits._maxPnl - totalPnl;
        if (newDrawdown != pnl._drawdown) {
            bool lower = newDrawdown < pnl._drawdown;
            pnl._drawdown = newDrawdown;
            LOG(0, "PNL drawdown. LimitType={i1} Limit={d1} Drawdown={d2} MaxPnl={d3} Pnl={d4} Open={d5} Closed={d6} Lower={i2}", limitType, ToDollars(limits._drawDownLimit), ToDollars(pnl._drawdown), ToDollars(limits._maxPnl), ToDollars(totalPnl), ToDollars(pnl._open), ToDollars(pnl._closed), lower)
        }
        if (pnl._drawdown > limits._drawDownLimit) {
            LOG(0, "Total PNL drawdown breached. LimitType={i1} Limit={d1} Drawdown={d2} Open={d3} Closed={d4}", limitType, ToDollars(limits._drawDownLimit), ToDollars(pnl._drawdown), ToDollars(pnl._open), ToDollars(pnl._closed))
            std::cout << "drawdown breach" << std::endl;
            return true;
        }
        return false;
    }

    PnlCheckData _totalLimits;
    PnlCheckData _strategyLimits[toUInt(StrategyId::Execution)];
    PnlCalculator* _pnlCalculator;
};

}

#endif //VIPER_PNL_LIMITS_CHECKER_H