#include "MarketMaker.h"
#include "SimpleMarketMaker.h"

using namespace Python;

void MarketMaker::handleViperMessage(const ViperMessage& message) {
    switch(message._type) {
        case ViperMessageType::GridUpdate: {
            const auto& gridChange = reinterpret_cast<const GridUpdateMessage &>(message._data[0]);
            processGridChange(*gridChange._gridUpdate);
            sendGenericMessageToFront(ViperMessageType::GridUpdateAccepted, gridChange._gridUpdate->_update.gridid());
            break;
        }

        case ViperMessageType::Fill: {
            const auto& fill = reinterpret_cast<const FillMessage&>(message._data[0]);
            onFill(fill);
            break;
        }

        case ViperMessageType::EndOfFill: {
            const auto& endFill = reinterpret_cast<const EndOfFill&>(message._data[0]);
            auto& mm = _marketMakers[endFill._internalId];
            if (endFill._isInternal) {
                mm.onEndOfInternalFills(endFill._execToken);
            } else {
                mm.onEndOfExternalFills(endFill._side, endFill._execToken);
            }
            break;
        }

        case ViperMessageType::QuoteExternallyOff: {
            const auto& msg = reinterpret_cast<const GenericMessage&>(message._data[0]);
            uint32_t internalId = msg._number_1;
            disableMMExternalQuoting(internalId);
            break;
        }
        case ViperMessageType::DisableMarketMakerInstrument: {
            const auto& msg = reinterpret_cast<const GenericMessage&>(message._data[0]);
            uint32_t internalId = msg._number_1;
            disableMarketMaker(internalId);
            break;
        }
        case ViperMessageType::DisableMMInstrumentAndGTCAndExternalQuote: {
            const auto& msg = reinterpret_cast<const GenericMessage&>(message._data[0]);
            uint32_t internalId = msg._number_1;
            disableMarketMaker(internalId, true);
            disableMMExternalQuoting(internalId);
            break;
        }
        case ViperMessageType::ShiftUpdate: {
            const auto& msg = reinterpret_cast<const GenericMessage&>(message._data[0]);
            uint32_t shift = msg._number_1;
            onShiftUpdate(shift);
            break;
        }
        case ViperMessageType::OrderUpdate: {
            const auto& msg = reinterpret_cast<const OrderUpdate&>(message._data[0]);
            onOrderUpdate(msg);
            break;
        }
        case ViperMessageType::QueuePositionUpdate: {
            const auto& msg = reinterpret_cast<const QueuePositionUpdate&>(message._data[0]);
            onQueuePositionUpdate(msg);
            break;
        }
        default:
            BOOST_ASSERT_MSG(false, [&message]() { return (new std::string(
                    std::string("Unhandled ViperMessage of type=") + std::to_string(static_cast<int>(message._type))))->c_str();}());
//            DLOG(0, "MM. unhandled ViperMessage {i1}", toUInt(message._type))
            break;
    }
}

void MarketMaker::disableMMExternalQuoting(uint32_t id) {
    _marketMakers[id].disableExternalQuoting();
    notifyFrontMMExternalQuoteDisabled(id);
}

void MarketMaker::onShiftUpdate(uint32_t shift) {
    for (uint32_t i = 0; i < _viperConfig._marketMakerConfigs.size(); ++i) {
        uint32_t contractId = i + _viperConfig._indexOfFirstTradedContract;
        auto& mm = _marketMakers[contractId];
        if (mm._contract._toMarketMake) {
            mm.updateWidthPercent(_viperConfig._marketMakerConfigs[i]._widthPercentage[shift]);
        }
    }
    sendGenericMessageToFront(ViperMessageType::MMWidthPercentageUpdate);
    auto disableTime = boost::lexical_cast<int32_t>(_viperConfig.getGlobalByShift(AUTO_MM_DISABLE_TIME, toUInt(_viperConfig._currentShift)));
    setAutoDisableTime(disableTime, clockUtil::GetCycles());
}

void MarketMaker::onOrderUpdate(const OrderUpdate& msg) {
    auto& mm = _marketMakers[msg.contractId];
    mm.onOrderUpdate(msg);
}

void MarketMaker::onQueuePositionUpdate(const QueuePositionUpdate& msg) {
    auto& mm = _marketMakers[msg.contractId];
    mm.onQueuePositionUpdate(msg);
}