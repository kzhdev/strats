#include "MarketMaker.h"
#include "SimpleMarketMaker.h"

using namespace Python;

void MarketMaker::onManualFill(ManualFill& fill) {
    if (fill._type == toUInt(ManualFillType::MarketMaker)) {
        auto &mm = _marketMakers[fill._contract];
        uint32_t size = std::abs(fill._size);
        uint32_t side = fill._size > 0 ? BID : ASK;
        hf::qty_t allocatedInternally = 0;
        hf::qty_t allocatedExternally = 0;
        mm.addManualPosition(size, side, allocatedInternally, allocatedExternally);
        sendAcceptedManualFillToFront(fill._contract, allocatedInternally, true);
        sendAcceptedManualFillToFront(fill._contract, allocatedExternally, false);
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

void MarketMaker::sendAcceptedManualFillToFront(uint32_t internalId, hf::qty_t size, bool isInternal) {
    sendGenericMessageToFront(ViperMessageType::AcceptedManualFill, internalId, size, (isInternal ? 0 : 1));
}