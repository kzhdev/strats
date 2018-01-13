#ifndef VIPER_PNL_CALCULATOR_H
#define VIPER_PNL_CALCULATOR_H

#include <hf/hf.h>
#include "Common.h"

namespace Python {

struct Pnl {
    hf::price_t _open = 0;
    hf::price_t _closed = 0;
};

struct PnlDetail : Pnl {
    void onFill(FillMessage& fill, Pnl& pnlForType, Pnl& pnlTotal) {
        if (_open == 0) {
            _openPosition = fill._qty * getSideMultiplier(fill._side);
            _avgPriceForOpenPosition = fill._price;
        }
        else if (_openPosition < 0) {
            if (fill._side == ASK) {
                _avgPriceForOpenPosition = (_avgPriceForOpenPosition * -_openPosition + fill._qty * fill._price) / (-_openPosition + fill._qty);
                _openPosition -= fill._qty;
            } else {
                if (-_openPosition >= fill._qty) {
                    _buyCost += fill._qty * fill._price;
                    _sellCost += fill._qty * _avgPriceForOpenPosition;
                    _openPosition += fill._qty;
                } else {
                    _buyCost += -_openPosition * fill._price;
                    _sellCost += -_openPosition * _avgPriceForOpenPosition;
                    _openPosition = fill._qty + _openPosition;
                    _avgPriceForOpenPosition = fill._price;
                }
            }
        }
        else {
            if (fill._side == BID) {
                _avgPriceForOpenPosition = (_avgPriceForOpenPosition * _openPosition + fill._qty * fill._price) / (_openPosition + fill._qty);
                _openPosition += fill._qty;
            } else {
                if (_openPosition >= fill._qty) {

                }
            }
        }
    }

    hf::qty_t getSideMultiplier(uint32_t side) {
        return side == BID ? 1 : -1;
    }

    hf::price_t _buyCost = 0;
    hf::price_t _sellCost = 0;
    hf::qty_t _openPosition = 0;
    hf::price_t _avgPriceForOpenPosition = 0;
};

struct AggregatedPnl {
    Pnl _relativeValue;
    Pnl _marketMakers;
    Pnl _execution;
    Pnl _total;
};

class PnlCalculator {
public:
    void init(uint32_t numberOfContracts) {
        _marketMakerDetail.resize(numberOfContracts);
        _relativeValueDetail.resize(numberOfContracts);
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

    std::vector<PnlDetail> _marketMakerDetail;
    std::vector<PnlDetail> _relativeValueDetail;
    AggregatedPnl _pnl;
};

}


#endif //VIPER_PNL_CALCULATOR_H