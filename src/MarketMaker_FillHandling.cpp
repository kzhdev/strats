#include "MarketMaker.h"
#include "SimpleMarketMaker.h"

using namespace Python;

COLD
void MarketMaker::onManualFill(ManualFill& fill) {
    auto &mm = _marketMakers[fill._contract];
    uint32_t size = std::abs(fill._size);
    uint32_t side = fill._size > 0 ? BID : ASK;
    hf::qty_t allocatedInternally = 0;
    hf::qty_t allocatedExternally = 0;
    mm.addManualPosition(size, side, allocatedInternally, allocatedExternally);

    auto& bidBook = mm.topBook<BID>();
    auto& askBook = mm.topBook<ASK>();

    hf::price_t price;
    auto [bidPrice, bidQty, askPrice, askQty] = std::make_tuple(bidBook.pr, bidBook.q, askBook.pr, askBook.q);
    if(bidPrice != hf::NULL_PRICE && bidQty > 0 && askPrice != hf::NULL_PRICE && askQty > 0 && mm._establishedLevel > 0) {
        price = _marketMakers[fill._contract].calculateMidPoint(bidBook, askBook);
    }
    else {
        price = GetPriceForManualFill(side, bidPrice, bidQty, askPrice, askQty);
    }

    sendAcceptedManualFillToFront(fill._contract, allocatedInternally, price, true);
    sendAcceptedManualFillToFront(fill._contract, allocatedExternally, price, false);
    _manualFilledMarketMakers.emplace(&mm);

    if (!mm._internallyInitiatedPosition && !mm._externallyInitiatedPosition) {
        mm._refFillPrice = hf::NULL_PRICE;
    } else if (mm._refFillPrice == hf::NULL_PRICE) {
        mm._refFillPrice = price;
    }
}

COLD
void MarketMaker::onEndOfManualFills() {
    for (auto mm : _manualFilledMarketMakers) {
        mm->onEndOfFills(mm->_orderExecutionToken, true);
    }
    _manualFilledMarketMakers.clear();
}

void MarketMaker::onFill(const FillMessage& fill) {
    auto& mm = _marketMakers[fill._contractId];
    mm.onFill(fill);
}

COLD
void MarketMaker::sendAcceptedManualFillToFront(uint32_t internalId, hf::qty_t size, hf::price_t price, bool isInternal) {
    sendMessageToFront<AcceptedManualFillMessage>(ViperMessageType::AcceptedManualFill, [&](AcceptedManualFillMessage& msg) {
        msg._contractId = internalId;
        msg._isInternal = isInternal;
        msg._size = size;
        msg._price = price;
        msg._stf = hf::NULL_PRICE;
    });
}