#ifndef VIPER_PNL_LIMITS_CHECKER_H
#define VIPER_PNL_LIMITS_CHECKER_H

#include "boost/format.hpp"
#include "Common.h"
#include "NegativeScalpChecker.h"

namespace Python {

enum StrategyLimitIndex {StrategyLimitIndexMM, StrategyLimitIndexRV};

struct PnlCheckData {
    hf::price_t _maxPnl = 0;
    hf::price_t _pnlLimit = 0;
    hf::price_t _drawDownLimit = 0;
};

class PnlLimitsChecker {
public:
    bool checkLimitsOnRelativeValuePnlChange() {
        bool limitsBreached = false;
        AggregatedPnl& pnl = _pnlCalculator->getAggregatedPnl();
        limitsBreached |= checkPnlLimits(pnl._total);
        limitsBreached |= checkRvPnlLimits(pnl._relativeValue);
        return limitsBreached;
    }

    bool checkLimitsOnMarketMakerPnlChange() {
        AggregatedPnl& pnl = _pnlCalculator->getAggregatedPnl();
    }


    bool checkPnlLimits(Pnl& pnl) {
        bool limitsBreached = false;
        auto& limits = _totalLimits;
        const hf::price_t totalPnl = pnl.getTotal();
        auto pnlLimit = limits._pnlLimit;
        if (totalPnl < -pnlLimit) {
            limitsBreached = true;
            _logger.logInfo(boost::str(boost::format("Pnl limit breached. Limit=%1% Pnl=%2%") % ToDollars(pnlLimit) % ToDollars(totalPnl)));
        }
        limits._maxPnl = std::max(limits._maxPnl, totalPnl);
        hf::price_t newDrawdown = limits._maxPnl - totalPnl;
        if (newDrawdown != pnl._drawdown) {
            pnl._drawdown = newDrawdown;
            _logger.logInfo(boost::str(boost::format("Total PNL drawdown. Limit=%1% Drawdown=%2% MaxPnl=%3% Pnl=%4%")
                                       % ToDollars(limits._drawDownLimit) % ToDollars(pnl._drawdown) % ToDollars(limits._maxPnl) % ToDollars(totalPnl)));
        }
        if (pnl._drawdown > limits._drawDownLimit) {
            _logger.logInfo(boost::str(boost::format("Total PNL drawdown breached. Limit=%1% Drawdown=%2%") % ToDollars(limits._drawDownLimit) % ToDollars(pnl._drawdown)));
            limitsBreached = true;
        }
        return limitsBreached;
    }

    bool checkRvPnlLimits(Pnl& pnl) {
        bool limitsBreached = false;
        auto& limits = _strategyLimits[StrategyLimitIndexRV];
        const hf::price_t rvPnl = pnl.getTotal();
        if (rvPnl < -limits._pnlLimit) {
            _logger.logInfo(boost::str(boost::format("RV Pnl limit breached. Limit=%1% Pnl=%2%") % ToDollars(limits._pnlLimit) % ToDollars(rvPnl)));
            limitsBreached = true;
        }
        limits._maxPnl = std::max(limits._maxPnl, rvPnl);
        hf::price_t newDrawdown = limits._maxPnl - rvPnl;
        if (newDrawdown != pnl._drawdown) {
            pnl._drawdown = newDrawdown;
            _logger.logInfo(boost::str(boost::format("RV PNL drawdown. Limit=%1% Pnl=%2% MaxPnl=%3% Pnl=%4%")
                                       % ToDollars(limits._drawDownLimit) % ToDollars(pnl._drawdown) % ToDollars(limits._maxPnl) % ToDollars(rvPnl)));
        }
        if (pnl._drawdown > limits._drawDownLimit) {
            _logger.logInfo(boost::str(boost::format("RV PNL drawdown breached. Limit=%1% Pnl=%2%") % ToDollars(limits._drawDownLimit) % ToDollars(pnl._drawdown)));
            limitsBreached = true;
        }
        return limitsBreached;
    }

    void checkMMPnlLimits(AggregatedPnl& pnl) {
        auto& limits = _strategyLimits[StrategyLimitIndexMM];
        const hf::price_t totalPnl = pnl._marketMakers.getTotal();
        if (totalPnl < -limits._pnlLimit) {
            _logger.logInfo(boost::str(boost::format("MM Pnl limit breached. Limit=%1% Pnl=%2%") % ToDollars(limits._pnlLimit) % ToDollars(totalPnl)));
            sendGenericMessageToMM(ViperMessageType::SwitchChange, static_cast<int32_t>(MainControlAction::Enabled), 0);
        }
        limits._maxPnl = std::max(limits._maxPnl, totalPnl);
        hf::price_t newDrawdown = limits._maxPnl - totalPnl;
        if (newDrawdown !=  pnl._marketMakers._drawdown) {
            pnl._marketMakers._drawdown = newDrawdown;
            _logger.logInfo(boost::str(boost::format("MM PNL drawdown. Limit=%1% Pnl=%2% MaxPnl=%3% Pnl=%4%")
                                       % ToDollars(limits._drawDownLimit) % ToDollars(pnl._marketMakers._drawdown) % ToDollars(limits._maxPnl) % ToDollars(pnl._marketMakers._drawdown)));
        }
        if (pnl._marketMakers._drawdown > limits._drawDownLimit) {
            _logger.logInfo(boost::str(boost::format("MM PNL drawdown breached. Limit=%1% Pnl=%2%") % ToDollars(limits._drawDownLimit) % ToDollars(pnl._marketMakers._drawdown)));
            sendGenericMessageToMM(ViperMessageType::SwitchChange, static_cast<int32_t>(MainControlAction::Enabled), 0);
        }
    }


    PnlCheckData _totalLimits;
    PnlCheckData _strategyLimits[2];
    PnlCalculator* _pnlCalculator;

    Logger _logger;
};

}

#endif //VIPER_PNL_LIMITS_CHECKER_H