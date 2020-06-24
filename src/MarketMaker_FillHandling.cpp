#include "MarketMaker.h"
#include "SimpleMarketMaker.h"

using namespace Python;

uint32_t MarketMaker::beginFillProcess(uint32_t internalId) {
    return _marketMakers[internalId].beginFillProcess();
}

hf::qty_t MarketMaker::allocateExternalMMHedgerFill(uint32_t internalId, hf::price_t filledPrice, hf::qty_t filledQty, uint32_t side) {
    // NOTE: This is call from Executor thread
    auto& mm = _marketMakers[internalId];
    hf::qty_t allocatedInternally = 0;
    hf::qty_t allocatedExternally = 0;
    hf::qty_t remaining = mm.allocateHedgingFill(side, filledQty, false, allocatedInternally, allocatedExternally);
    if (allocatedInternally || allocatedExternally) {
        sendFillToFront(internalId, side, filledPrice, filledQty - remaining, FillType::External, FillSource::Hedge,
                        allocatedInternally, allocatedExternally);
    }
    return remaining;
}

void MarketMaker::onFill(uint32_t internalId, Order* order, hf::price_t price, hf::qty_t size, bool isPayUp, FillType fillType) {
    // NOTE: This is called from Executor thread
    hf::qty_t allocatedInternally = 0;
    hf::qty_t allocatedExternally = 0;
    auto& mm = _marketMakers[internalId];
    mm.onFill(order, price, size, fillType == FillType::Internal, isPayUp, allocatedInternally, allocatedExternally);
    FillSource fillSource = order->_type == Order::Type::Hedging ? FillSource::Hedge : FillSource::Quote;
    sendFillToFront(internalId, order->_side, price, size, fillType, fillSource, allocatedInternally, allocatedExternally);
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

bool MarketMaker::forceTakeOverFill(uint32_t internalId, uint32_t side, hf::price_t price, hf::qty_t size, FillSource source) {
    // NOTE: called from Executor thread
    auto& mm = _marketMakers[internalId];
    mm.forceTakeOverFill(side, size);

    sendFillToFront(internalId, side, price, size, FillType::External, source, 0, size);
    return mm.isWithinPositionLimits();
}

void MarketMaker::processManualFill(const ManualFill& fill) {
    auto &mm = _marketMakers[fill._contract];
    uint32_t size = std::abs(fill._size);
    uint32_t side = fill._size > 0 ? BID : ASK;
    hf::qty_t allocatedInternally = 0;
    hf::qty_t allocatedExternally = 0;
    mm.addManualPosition(size, side, allocatedInternally, allocatedExternally);
    sendAcceptedManualFillToFront(fill._contract, allocatedInternally, true);
    sendAcceptedManualFillToFront(fill._contract, allocatedExternally, false);
}

void MarketMaker::sendAcceptedManualFillToFront(uint32_t internalId, hf::qty_t size, bool isInternal) {
    sendGenericMessageToFront(ViperMessageType::AcceptedManualFill, internalId, size, (isInternal ? 0 : 1));
}