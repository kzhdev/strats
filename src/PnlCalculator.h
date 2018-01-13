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

    Pnl& getMarketMakerPnl(uint32_t index) { return _marketMakerPnl[index]; }

    Pnl _relativeValue;
    std::vector<Pnl> _marketMakerPnl;
};

}


#endif //VIPER_PNL_CALCULATOR_H