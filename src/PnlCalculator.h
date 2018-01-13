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

    }
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