#ifndef VIPER_PNL_LIMITS_CHECKER_H
#define VIPER_PNL_LIMITS_CHECKER_H

#include "boost/format.hpp"
#include "Common.h"
#include "NegativeScalpChecker.h"

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
        setPnlLimits("RV", _strategyLimits[toUInt(StrategyId::RelativeValue)], config);
        setPnlLimits("MM", _strategyLimits[toUInt(StrategyId::MarketMaker)], config);
        _pnlCalculator = calculator;
    }

    bool checkLimits(StrategyId strategyId) {
        bool limitsBreached = false;
        limitsBreached |= checkTotalLimits();
        limitsBreached |= checkStrategyLimits(strategyId);
        return limitsBreached;
    }

    bool handlePnlLimitChange(GridUpdate& update) {
        bool limitsBreached = false;
        for (auto& one : update._update.cells()) {
            PNLRows type = static_cast<PNLRows>(one.row());
            hf::price_t limit = ToRhonePrice(boost::lexical_cast<int64_t>(one.value()));
            switch (type) {
                case PNLRows::RelativeValue:
                case PNLRows::MarketMakers: {
                    StrategyId strategyId = type == PNLRows::RelativeValue ? StrategyId::RelativeValue : StrategyId::MarketMaker;
                    if (one.col() == toUInt(PNLColumns::PnlLimit)) {
                        _strategyLimits[toUInt(strategyId)]._pnlLimit = limit;
                        LOG(0, "Pnl limit changed. LimitType={i1} Value={d1}", toUInt(strategyId), ToDollars(limit))
                    } else if (one.col() == toUInt(PNLColumns::DrawdownLimit)) {
                        _strategyLimits[toUInt(strategyId)]._drawDownLimit = limit;
                        LOG(0, "Drawdown limit changed. LimitType={i1} Value={d1}", toUInt(strategyId), ToDollars(limit))
                    }
                    limitsBreached |= checkLimits(strategyId);
                }
                    break;
                case PNLRows::Total: {
                    if (one.col() == toUInt(PNLColumns::PnlLimit)) {
                        _totalLimits._pnlLimit = limit;
                        LOG(0, "Pnl limit changed. LimitType=Total Value={d1}", ToDollars(limit))
                    } else if (one.col() == toUInt(PNLColumns::DrawdownLimit)) {
                        _totalLimits._drawDownLimit = limit;
                        LOG(0, "Drawdown limit changed. LimitType=Total Value={d1}", ToDollars(limit))
                    }
                    limitsBreached |= checkTotalLimits();
                }
                    break;
                default:
                    break;
            }
        }
        return limitsBreached;
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
        auto& config = *viperConfig.getPnlConfig(name);
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
    PnlCheckData _strategyLimits[toUInt(StrategyId::MAX)];
    PnlCalculator* _pnlCalculator;
};

}

#endif //VIPER_PNL_LIMITS_CHECKER_H