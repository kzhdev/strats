#ifndef VIPER_PNL_CALCULATOR_H
#define VIPER_PNL_CALCULATOR_H

/*
#include "boost/format.hpp"
#include <hf/hf.h>
#include <queue>
#include "Common.h"
#include "Logger.h"
#include "ViperConfig.h"

namespace Python {

struct Pnl {
    hf::price_t _open = 0;
    hf::price_t _closed = 0;
    hf::price_t _drawdown = 0;

    hf::price_t getTotal() { return _open + _closed; }
};

struct PnlDetail : Pnl {

    void adjustCostsAndPosition(hf::qty_t qty, hf::price_t buyPrice, hf::price_t sellPrice) {
        _openPosition  -= qty;
        _sellCost += qty * sellPrice;
        _buyCost += qty * buyPrice;
    }

    void onFill(FillMessage& fill, Pnl& pnlForType, Pnl& pnlTotal) {
        if (_openPosition == 0) {
            _openPosition = fill._qty;
            _openSide = fill._side;
            _openPrice = fill._price;
        }
        else if (_openSide == fill._side) {
            _openPosition += fill._qty;
            _fills.push({fill._price, fill._qty});
        } else {
            hf::qty_t remainingFillSize = fill._qty;
            hf::qty_t matchSize = std::min(remainingFillSize, _openPosition);
            if (fill._side == BID) {
                adjustCostsAndPosition(matchSize, fill._price, _openPrice);
            } else {
                adjustCostsAndPosition(matchSize, _openPrice, fill._price);
            }
            remainingFillSize -= matchSize;

            bool hasOtherFillsToMatch = !_fills.empty();

            if (hasOtherFillsToMatch) {
                while (!_fills.empty()) {
                    Fill & f = _fills.front();
                    if (f._size <= remainingFillSize) {
                        remainingFillSize -= f._size;
                        adjustCostsAndPosition(f._size, fill._price, f._price);
                        _fills.pop();
                    } else {
                        f._size -= remainingFillSize;
                        adjustCostsAndPosition(remainingFillSize, fill._price, f._price);
                        remainingFillSize = 0;
                    }
                    if (remainingFillSize > 0) {
                        _fills.push({fill._price, remainingFillSize});
                        _openPosition = remainingFillSize;
                        _openSide = fill._side;
                    }
                }
            } else {
                _openSide = GetOtherSide(_openSide);
                _openPosition = remainingFillSize;
                _openPrice = fill._price;
            }
        }

        calculateClosedPnl(pnlForType, pnlTotal);
        calculateOpenPnl(pnlForType, pnlTotal);

        _logger.logInfo(boost::str(boost::format("PNL: %1%. Fill. Price=%2% Qty=%3% Open=%4% OpenCost=%5% OpenPosition=%6% OpenSide=%7% Closed=%8% ClosedBuyCost=%9% ClosedSellCost=%10% MarkPrice=%11%")
                                   % _name % ToDollars(fill._price) % fill._qty % ToDollars(_open) % ToDollars(_openPrice * _openPosition) % _openPosition % getSideString(_openSide)
                                   % ToDollars(_closed) % ToDollars(_buyCost) % ToDollars(_sellCost) % ToDollars(_markPrice)));

        _logger.logInfo(boost::str(boost::format("PNL Strategy: Open=%1% Closed=%2% ") % ToDollars(pnlForType._open) % ToDollars(pnlForType._closed)));
        _logger.logInfo(boost::str(boost::format("PNL Total: Open=%1% Closed=%2% ") % ToDollars(pnlTotal._open) % ToDollars(pnlTotal._closed)));
    }

    void calculateClosedPnl(Pnl& pnlForType, Pnl& pnlTotal) {
        hf::price_t newClosed = (_sellCost - _buyCost) * _multiplier;
        hf::price_t closedDiff = newClosed - _closed;
        pnlForType._closed += closedDiff;
        pnlTotal._closed += closedDiff;
        _closed = newClosed;
    }

    void calculateOpenPnl(Pnl& pnlForType, Pnl& pnlTotal) {
        hf::price_t newOpen;
        if (_openSide == BID) {
            newOpen = (_openPosition * _markPrice - _openPrice * _openPosition) * _multiplier;
        } else {
            newOpen = (_openPrice * _openPosition - _openPosition * _markPrice) * _multiplier;
        }
        hf::price_t openDiff = newOpen - _open;
        pnlForType._open += openDiff;
        pnlTotal._open += openDiff;
        _open = newOpen;
    }

    void onMarkPriceChange(hf::price_t price, Pnl& pnlForType, Pnl& pnlTotal) {
        _markPrice = price;
        calculateOpenPnl(pnlForType, pnlTotal);
    }

    struct Fill {
        hf::price_t _price;
        hf::qty_t _size;
    };

    hf::price_t _buyCost = 0;
    hf::price_t _sellCost = 0;
    hf::qty_t _openPosition = 0;
    uint32_t _openSide;
    hf::price_t _openPrice;
    hf::price_t _markPrice;
    hf::price_t _multiplier = 0;
    std::queue<Fill> _fills;

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
    void init(std::vector<ConfigContract>& contracts) {
        _marketMakerDetail.resize(contracts.size());
        _relativeValueDetail.resize(contracts.size());
        for (uint32_t i = 0; i < contracts.size(); ++i) {
            auto& contract = contracts[i];
            _marketMakerDetail[i]._name = contract._name;
            _marketMakerDetail[i]._multiplier = contract._tickValue / contract._tickSize;
            _relativeValueDetail[i]._name = contract._name;
            _relativeValueDetail[i]._multiplier = contract._tickValue / contract._tickSize;
        }
    }

    void onFillFromRelativeValue(FillMessage& fill) {
        PnlDetail& pnl = _relativeValueDetail[fill._contractId];
        pnl.onFill(fill, _pnl._relativeValue, _pnl._total);
    }

    void onFillFromMarketMaker(FillMessage& fill) {
        PnlDetail& pnl = _marketMakerDetail[fill._contractId];
        pnl.onFill(fill, _pnl._marketMakers, _pnl._total);
    }

    AggregatedPnl& getAggregatedPnl() { return _pnl; }

    Pnl& getRelativeValuePnlDetail(uint32_t index) { return _relativeValueDetail[index]; }
    Pnl& getMarketMakerPnlDetail(uint32_t index) { return _marketMakerDetail[index]; }

    bool recalculateOpenPnl(uint32_t index, hf::price_t price) {
        PnlDetail& rvPnl = _relativeValueDetail[index];
        if (price != rvPnl._markPrice) {
            rvPnl.onMarkPriceChange(price, _pnl._relativeValue, _pnl._total);
            PnlDetail& mmPnl = _marketMakerDetail[index];
            mmPnl.onMarkPriceChange(price, _pnl._marketMakers, _pnl._total);
            return true;
        }
        return false;
    }

    std::vector<PnlDetail> _marketMakerDetail;
    std::vector<PnlDetail> _relativeValueDetail;

    AggregatedPnl _pnl;
};

}
*/

#endif //VIPER_PNL_CALCULATOR_H