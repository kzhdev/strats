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
    hf::qty_t allocatedInternally = 0;
    hf::qty_t allocatedExternally = 0;
    auto& mm = _marketMakers[fill._contractId];
    mm.onFill(fill, allocatedInternally, allocatedExternally);
    sendFillToFront(fill._contractId, fill._side, fill._price, fill._qty, fill._type, fill._source, allocatedInternally, allocatedExternally);
}

hf::qty_t MarketMaker::takeOverFill(uint32_t internalId, uint32_t side, hf::price_t price, hf::qty_t size) {
    // NOTE: called from Executor thread
    if (isEnabled()) {
        auto& mm = _marketMakers[internalId];
        hf::qty_t takenSize = mm.takeOverFill(side, size);
        if (takenSize > 0) {
            sendFillToFront(internalId, side, price, takenSize, FillType::External, FillSource::Overfill, 0, takenSize);
        }
        return takenSize;
    }
    return 0;
}

void MarketMaker::sendAcceptedManualFillToFront(uint32_t internalId, hf::qty_t size, bool isInternal) {
    sendGenericMessageToFront(ViperMessageType::AcceptedManualFill, internalId, size, (isInternal ? 0 : 1));
}