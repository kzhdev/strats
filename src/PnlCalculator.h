#ifndef VIPER_PNL_CALCULATOR_H
#define VIPER_PNL_CALCULATOR_H

#include <hf/hf.h>
#include <queue>
#include "Common.h"

namespace Python {

struct Pnl {
    hf::price_t _open = 0;
    hf::price_t _closed = 0;
};

struct PnlDetail : Pnl {


    void adjustCostsAndPosition(hf::qty_t qty, hf::price_t buyPrice,hf::price_t sellPrice) {
        _openPosition  -= qty;
        _sellCost += qty * sellPrice;
        _buyCost += qty * buyPrice;
    }

    void onFill(FillMessage& fill, Pnl& pnlForType, Pnl& pnlTotal) {
        if (_openPosition == 0) {
            _openPosition = fill._qty;
            _openSide = fill._side;
            _fills.push({fill._price, fill._qty});
        }
        else if (_openSide == fill._side) {
            _openPosition += fill._qty;
            _fills.push({fill._price, fill._qty});
        } else {
            hf::qty_t remainingFillSize = fill._qty;
            while (!_fills.empty()) {
                Fill& f = _fills.front();
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
        }
    }

    struct Fill {
        hf::price_t _price;
        hf::qty_t _size;
    };

    hf::price_t _buyCost = 0;
    hf::price_t _sellCost = 0;
    hf::qty_t _openPosition = 0;
    uint32_t _openSide;
    std::queue<Fill> _fills;
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