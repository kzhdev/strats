#ifndef VIPER_PNL_CALCULATOR_H
#define VIPER_PNL_CALCULATOR_H

#include <hf/hf.h>
#include "Common.h"

namespace Python {

struct Pnl {
    hf::price_t _open = 0;
    hf::price_t _closed = 0;
};

class PnlCalculator {
public:
    void onFillFromRelativeValue(FillMessage& fill) {

    }

    void onFillFromMarketMaker(FillMessage& fill) {

    }

    Pnl& getRelativeValuePnl() { return _relativeValue; }
    Pnl& getMarketMakerPnl() { return _marketMakers; }

    Pnl& getRelativeValuePnlDetail(uint32_t index) { return _relativeValueDetail[index]; }
    Pnl& getMarketMakerPnlDetail(uint32_t index) { return _marketMakerDetail[index]; }

    std::vector<Pnl> _marketMakerDetail;
    std::vector<Pnl> _relativeValueDetail;
    Pnl _relativeValue;
    Pnl _marketMakers;
};

}


#endif //VIPER_PNL_CALCULATOR_H