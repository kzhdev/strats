#include "MarketMaker.h"
#include "SimpleMarketMaker.h"

using namespace Python;

COLD
void MarketMaker::onManualFill(ManualFill& fill) {
    if (fill._type == toUInt(ManualFillType::MarketMaker)) {
        auto &mm = _marketMakers[fill._contract];
        uint32_t size = std::abs(fill._size);
        uint32_t side = fill._size > 0 ? BID : ASK;
        hf::qty_t allocatedInternally = 0;
        hf::qty_t allocatedExternally = 0;
        mm.addManualPosition(size, side, allocatedInternally, allocatedExternally);

        hf::price_t price = GetPriceForManualFill(side, mm._books[BID]._price, mm._books[BID]._otherQuantity, mm._books[ASK]._price, mm._books[ASK]._otherQuantity);

        sendAcceptedManualFillToFront(fill._contract, allocatedInternally, price, true);
        sendAcceptedManualFillToFront(fill._contract, allocatedExternally, price, false);
        _manualFilledMarketMakers.emplace(&mm);
    }
}

void MarketMaker::onEndOfManualFills() {
    for (auto mm : _manualFilledMarketMakers) {
        mm->onEndOfFills(true);
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
    });
}