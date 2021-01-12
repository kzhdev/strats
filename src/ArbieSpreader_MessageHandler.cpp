#include "ArbieSpreader.h"

using namespace Python;

void ArbieSpreader::handleViperMessage(const ViperMessage& message) {
    switch (message._type) {
        case ViperMessageType::GridUpdate: {
            const auto& gridChange = reinterpret_cast<const GridUpdateMessage&>(message._data[0]);
            processGridChange(*gridChange._gridUpdate);
            sendGenericMessageToFront(ViperMessageType::GridUpdateAccepted, gridChange._gridUpdate->_update.gridid());
            break;
        }

        case ViperMessageType::DebugStatuses : {
            const DebugStatusEvents& dses = reinterpret_cast<const DebugStatusEvents&>(message._data[0]);
            handleDebugStatusEvents(dses);
            break;
        }

        case ViperMessageType::MarketUpdate: {
            const auto& marketUpdate = reinterpret_cast<const MarketUpdate&> (message._data[0]);
            processInternalMarketUpdate(marketUpdate);
            break;
        }
        case ViperMessageType::Fill: {
            const auto& fill = reinterpret_cast<const FillMessage&>(message._data[0]);
            handleFill(fill);
            break;
        }
        case ViperMessageType::EndOfQuotes: {
            const auto& reply = reinterpret_cast<const GenericMessage&> (message._data[0]);
            handleEndOfInternalFills(reply._number_1);
            break;
        }
        case ViperMessageType::HedgeMDAdjsutAck: {
            const auto& hedgeAck = reinterpret_cast<const HedgeMarketDataAdjustmentAck&>(message._data[0]);
            handleHedgeMarketDataAdjustmentAck(hedgeAck);
            break;
        }
        case ViperMessageType::QuoteExternallyOff: {
            const auto& msg = reinterpret_cast<const GenericMessage&> (message._data[0]);
            uint32_t internalId = msg._number_1;
            _contracts[internalId]._quoteExternally = false;
            sendGenericMessageToFront(ViperMessageType::QuoteExternallyOff, internalId,
                                      toUInt(WindowIds::ArbieSpreaderParameters),
                                      toUInt(ArbieSpreaderParameterColumns::QuoteExternal));
            break;
        }
        case ViperMessageType::InternalHedgeDone: {
            const auto& hedgeDone = reinterpret_cast<const InternalHedgeDone&> (message._data[0]);
            handleInternalHedgeDone(hedgeDone);
            break;
        }
        default:
            TLOG(0, "AS. unhandled ViperMessage {i1}", toUInt(message._type))
            break;
    }
}

void ArbieSpreader::handleHedgeMarketDataAdjustmentAck(const HedgeMarketDataAdjustmentAck& hedgeAck) {
    auto& contract = _contracts[hedgeAck._instrumentIndex];
    LOG(0, "AS {C[i1]}. Received hedge md ack. HedgeId={i2} Adj={i3} OrderId={i4}", getContract(contract._def._internalId),
        hedgeAck._hedgeId, hedgeAck._size, (uint32_t) hedgeAck._orderId)
    Market& market = contract._markets[hedgeAck._side][Market::External];
    market.ackAdjustment(hedgeAck._hedgeId, hedgeAck._size, hedgeAck._orderId);
    contract._marketNeedToSort[hedgeAck._side] = true;
    _updatedContracts.emplace(&contract);
    for (auto* path : contract._affectedPaths) {
        path->_dirty = true;
    }
}

void ArbieSpreader::handleInternalHedgeDone(const InternalHedgeDone& message) {
    DLOG(0, "AS {C[i1]}. InternalHedgeDone. Side={i2} HedgeId={i3} Market={i4}",
         getContract(message._instrumentIndex), message._side, message._hedgeId, message._marketIndex)
    assert(message._marketIndex < MARKET_ID_EXTERNAL);
    auto& contract = _contracts[message._instrumentIndex];
    Market& market = contract._markets[message._side][Market::Internal];
    market.handleInternalHedgeDone(message._hedgeId);
    contract._marketNeedToSort[message._side] = true;
    for (auto* path : contract._affectedPaths) {
        path->_dirty = true;
    }
    _updatedContracts.emplace(&contract);
}