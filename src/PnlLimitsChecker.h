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
    }

    bool checkLimitsOnPnlChange(StrategyId strategyId) {
        bool limitsBreached = false;
        AggregatedPnl& pnl = _pnlCalculator->getAggregatedPnl();
        limitsBreached |= checkPnlLimits(pnl._total, _totalLimits, "Total");
        if (strategyId == StrategyId::RelativeValue) {
            limitsBreached |= checkPnlLimits(pnl._relativeValue, _strategyLimits[toUInt(strategyId)], "RelativeValue");
        }
        if (strategyId == StrategyId::MarketMaker) {
            limitsBreached |= checkPnlLimits(pnl._marketMakers, _strategyLimits[toUInt(strategyId)], "MarketMaker");
        }
        return limitsBreached;
    }

private:
    void setPnlLimits(const std::string& name, PnlCheckData& data, ViperConfig& vipoerConfig) {
        auto& config = *vipoerConfig.getPnlConfig(name);
        data._drawDownLimit = ToRhonePrice(config._drawdown);
        data._pnlLimit = ToRhonePrice(config._limit);
    }

    bool checkPnlLimits(Pnl& pnl, PnlCheckData& limits, const std::string& limitType) {
        bool limitsBreached = false;
        const hf::price_t totalPnl = pnl.getTotal();
        auto pnlLimit = limits._pnlLimit;
        if (totalPnl < -pnlLimit) {
            limitsBreached = true;
            _logger.logInfo(boost::str(boost::format("Pnl limit breached. LimitType=%1% Limit=%2% Pnl=%2% %3%") % limitType % ToDollars(pnlLimit) % ToDollars(totalPnl)));
        }
        limits._maxPnl = std::max(limits._maxPnl, totalPnl);
        hf::price_t newDrawdown = limits._maxPnl - totalPnl;
        if (newDrawdown != pnl._drawdown) {
            pnl._drawdown = newDrawdown;
            _logger.logInfo(boost::str(boost::format("PNL drawdown. LimitType=%1% Limit=%2% Drawdown=%3% MaxPnl=%4% Pnl=%5%")
                                       % limitType % ToDollars(limits._drawDownLimit) % ToDollars(pnl._drawdown) % ToDollars(limits._maxPnl) % ToDollars(totalPnl)));
        }
        if (pnl._drawdown > limits._drawDownLimit) {
            _logger.logInfo(boost::str(boost::format("Total PNL drawdown breached. LimitType=%1% Limit=%2% Drawdown=%3%") % limitType % ToDollars(limits._drawDownLimit) % ToDollars(pnl._drawdown)));
            limitsBreached = true;
        }
        return limitsBreached;
    }

    PnlCheckData _totalLimits;
    PnlCheckData _strategyLimits[3];
    PnlCalculator* _pnlCalculator;

    Logger _logger;
};

}

#endif //VIPER_PNL_LIMITS_CHECKER_H