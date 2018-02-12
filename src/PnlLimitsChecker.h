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
        AggregatedPnl& pnl = _pnlCalculator->getAggregatedPnl();
        limitsBreached |= checkTotalLimits(pnl);
        limitsBreached |= checkStrategyLimits(pnl, strategyId);
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
                    if (one.col() == 1) {
                        _strategyLimits[toUInt(strategyId)]._pnlLimit = limit;
                    } else {
                        _strategyLimits[toUInt(strategyId)]._drawDownLimit = limit;
                    }
                    limitsBreached |= checkLimits(strategyId);
                }
                case PNLRows::Total: {
                    if (one.col() == 1) {
                        _totalLimits._pnlLimit = limit;
                    } else {
                        _totalLimits._drawDownLimit = limit;
                    }
                    limitsBreached |= checkTotalLimits(_pnlCalculator->getAggregatedPnl());
                    break;
                }
                default:
                    break;
            }
        }
        return limitsBreached;
    }

private:
    bool checkTotalLimits(AggregatedPnl& pnl) {
        return checkPnlLimits(pnl._total, _totalLimits, "Total");
    }

    bool checkStrategyLimits(AggregatedPnl& pnl, StrategyId strategyId) {
        if (strategyId == StrategyId::RelativeValue) {
            return checkPnlLimits(pnl._relativeValue, _strategyLimits[toUInt(strategyId)], "RelativeValue");
        }
        if (strategyId == StrategyId::MarketMaker) {
            return checkPnlLimits(pnl._marketMakers, _strategyLimits[toUInt(strategyId)], "MarketMaker");
        }
        BOOST_ASSERT(false);
        return false;
    }

    void setPnlLimits(const std::string& name, PnlCheckData& data, ViperConfig& vipoerConfig) {
        auto& config = *vipoerConfig.getPnlConfig(name);
        data._drawDownLimit = ToRhonePrice(config._drawdown);
        data._pnlLimit = ToRhonePrice(config._limit);
    }

    bool checkPnlLimits(Pnl& pnl, PnlCheckData& limits, const std::string& limitType) {
        bool limitsBreached = checkPnlLimit(pnl, limits, limitType);
        limitsBreached |= checkDrawdownLimit(pnl, limits, limitType);
        return limitsBreached;
    }

    bool checkPnlLimit(Pnl& pnl, PnlCheckData& limits, const std::string& limitType) {
        const hf::price_t totalPnl = pnl.getTotal();
        auto pnlLimit = limits._pnlLimit;
        if (totalPnl < -pnlLimit) {
            _logger.logInfo(boost::str(boost::format("Pnl limit breached. LimitType=%1% Limit=%2% Pnl=%2% %3%") % limitType % ToDollars(pnlLimit) % ToDollars(totalPnl)));
            return true;
        }
        return false;
    }

    bool checkDrawdownLimit(Pnl& pnl, PnlCheckData& limits, const std::string& limitType) {
        const hf::price_t totalPnl = pnl.getTotal();
        limits._maxPnl = std::max(limits._maxPnl, totalPnl);
        hf::price_t newDrawdown = limits._maxPnl - totalPnl;
        if (newDrawdown != pnl._drawdown) {
            pnl._drawdown = newDrawdown;
            _logger.logInfo(boost::str(boost::format("PNL drawdown. LimitType=%1% Limit=%2% Drawdown=%3% MaxPnl=%4% Pnl=%5%")
                                       % limitType % ToDollars(limits._drawDownLimit) % ToDollars(pnl._drawdown) % ToDollars(limits._maxPnl) % ToDollars(totalPnl)));
        }
        if (pnl._drawdown > limits._drawDownLimit) {
            _logger.logInfo(boost::str(boost::format("Total PNL drawdown breached. LimitType=%1% Limit=%2% Drawdown=%3%") % limitType % ToDollars(limits._drawDownLimit) % ToDollars(pnl._drawdown)));
            return true;
        }
        return false;
    }

    PnlCheckData _totalLimits;
    PnlCheckData _strategyLimits[3];
    PnlCalculator* _pnlCalculator;

    Logger _logger;
};

}

#endif //VIPER_PNL_LIMITS_CHECKER_H