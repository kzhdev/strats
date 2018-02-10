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
        AggregatedPnl& pnl = _pnlCalculator->getAggregatedPnl();
    }

    bool checkLimitsOnMarketMakerPnlChange() {
        AggregatedPnl& pnl = _pnlCalculator->getAggregatedPnl();
    }


    void checkPnlLimits(AggregatedPnl& pnl) {
        auto& limits = _totalLimits;
        const hf::price_t totalPnl = pnl._total.getTotal();
        auto pnlLimit = limits._pnlLimit;
        if (totalPnl < -pnlLimit) {
            _logger.logInfo(boost::str(boost::format("Pnl limit breached. Limit=%1% Pnl=%2%") % ToDollars(pnlLimit) % ToDollars(totalPnl)));
            onSystemStateChange(SystemState::Disabled);
        }
        limits._maxPnl = std::max(limits._maxPnl, totalPnl);
        hf::price_t newDrawdown = limits._maxPnl - totalPnl;
        if (newDrawdown != pnl._total._drawdown) {
            pnl._total._drawdown = newDrawdown;
            _logger.logInfo(boost::str(boost::format("Total PNL drawdown. Limit=%1% Drawdown=%2% MaxPnl=%3% Pnl=%4%")
                                       % ToDollars(limits._drawDownLimit) % ToDollars(pnl._total._drawdown) % ToDollars(limits._maxPnl) % ToDollars(totalPnl)));
        }
        if (pnl._total._drawdown > limits._drawDownLimit) {
            _logger.logInfo(boost::str(boost::format("Total PNL drawdown breached. Limit=%1% Drawdown=%2%") % ToDollars(limits._drawDownLimit) % ToDollars(pnl._total._drawdown)));
            onSystemStateChange(SystemState::Disabled);
        }
    }

    void checkRvPnlLimits(AggregatedPnl& pnl) {
        auto& limits = _strategyLimits[StrategyLimitIndexRV];
        const hf::price_t rvPnl = pnl._relativeValue.getTotal();
        if (rvPnl < -limits._pnlLimit) {
            _logger.logInfo(boost::str(boost::format("RV Pnl limit breached. Limit=%1% Pnl=%2%") % ToDollars(limits._pnlLimit) % ToDollars(rvPnl)));
            sendGenericMessageToRV(ViperMessageType::SwitchChange, static_cast<int32_t>(MainControlAction::Enabled), 0);
        }
        limits._maxPnl = std::max(limits._maxPnl, rvPnl);
        hf::price_t newDrawdown = limits._maxPnl - rvPnl;
        if (newDrawdown != pnl._relativeValue._drawdown) {
            pnl._relativeValue._drawdown = newDrawdown;
            _logger.logInfo(boost::str(boost::format("RV PNL drawdown. Limit=%1% Pnl=%2% MaxPnl=%3% Pnl=%4%")
                                       % ToDollars(limits._drawDownLimit) % ToDollars(pnl._relativeValue._drawdown) % ToDollars(limits._maxPnl) % ToDollars(rvPnl)));
        }
        if (pnl._relativeValue._drawdown > limits._drawDownLimit) {
            _logger.logInfo(boost::str(boost::format("RV PNL drawdown breached. Limit=%1% Pnl=%2%") % ToDollars(limits._drawDownLimit) % ToDollars(pnl._relativeValue._drawdown)));
            sendGenericMessageToRV(ViperMessageType::SwitchChange, static_cast<int32_t>(MainControlAction::Enabled), 0);
        }
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