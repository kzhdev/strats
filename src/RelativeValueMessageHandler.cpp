#include "RelativeValue.h"

using namespace Python;

void RelativeValue::handleViperMessage(const ViperMessage& message) {
    switch (message._type) {
        case ViperMessageType::GridUpdate: {
            const auto& gridChange = reinterpret_cast<const GridUpdateMessage&>(message._data[0]);
            processGridChange(*gridChange._gridUpdate);
            sendGenericMessageToFront(ViperMessageType::GridUpdateAccepted, gridChange._gridUpdate->_update.gridid());
            _calculateDueToFront = true;
            break;
        }

        case ViperMessageType::DebugStatuses : {
            const DebugStatusEvents& dses = reinterpret_cast<const DebugStatusEvents&>(message._data[0]);
            handleDebugStatusEvents(dses);
            _calculateDueToFront = true;
            break;
        }

        case ViperMessageType::MarketUpdate: {
            const auto& marketUpdate = reinterpret_cast<const MarketUpdate&> (message._data[0]);
            processInternalMarketUpdate(marketUpdate);
            _hadInternalMarketData = true;
            break;
        }
        case ViperMessageType::Fill: {
            const auto& fill = reinterpret_cast<const FillMessage&>(message._data[0]);
            handleFill(fill);
            ++_numberOfFills;
            break;
        }
        case ViperMessageType::HedgeMDAdjsutAck: {
            const auto& hedgeAck = reinterpret_cast<const HedgeMarketDataAdjustmentAck&>(message._data[0]);
            handleHedgeMarketDataAdjustmentAck(hedgeAck);
            break;
        }
        case ViperMessageType::InternalHedgeDone: {
            const auto& hedgeDone = reinterpret_cast<const InternalHedgeDone&> (message._data[0]);
            handleInternalHedgeDone(hedgeDone);
            break;
        }
        case ViperMessageType::EndOfQuotes: {
            const auto& reply = reinterpret_cast<const GenericMessage&> (message._data[0]);
            handleEndOfInternalFills(reply._number_1);
            _endOfQuotes = true;
            break;
        }
        case ViperMessageType::SetRhoneLimits: {
            const auto& limits = reinterpret_cast<const SetRhoneLimits&>(message._data[0]);
            handleSetRhoneLimits(limits);
            break;
        }
        case ViperMessageType::RVQuoteExternallyOff: {
            const auto& msg = reinterpret_cast<const GenericMessage&> (message._data[0]);
            uint32_t internalId = msg._number_1;
            _quoteExternal.setValue(internalId, false);
            sendGenericMessageToFront(ViperMessageType::RVQuoteExternallyOff, internalId);
            break;
        }
        default:
//            DLOG(0, "RV. unhandled ViperMessage {i1}", toUInt(message._type))
            break;
    }
}