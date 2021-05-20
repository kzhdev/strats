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
        case ViperMessageType::QuoteExternallyOff: {
            const auto& msg = reinterpret_cast<const GenericMessage&> (message._data[0]);
            uint32_t internalId = msg._number_1;
            _contractQuoteExternal.setValue(internalId, false);
            sendGenericMessageToFront(ViperMessageType::QuoteExternallyOff, internalId,
                                      toUInt(WindowIds::ContractParametersRv),
                                      toUInt(ContractParameterRVColumns::QuoteExternal));
            break;
        }
        case ViperMessageType::ShiftUpdate: {
            const auto& msg = reinterpret_cast<const GenericMessage&>(message._data[0]);
            uint32_t shift = msg._number_1;
            onShiftUpdate(shift);
            break;
        }
        case ViperMessageType::EconomicNumberEnded: {
            const auto& msg = reinterpret_cast<const GenericMessage&>(message._data[0]);
            onEconomicNumberEnded((EconomicNumber::Color)msg._number_1);
            break;
        }
        case ViperMessageType::SubscribeSTFUpdate: {
            const auto& msg = reinterpret_cast<const GenericMessage&>(message._data[0]);
            handleSTFSubscription(toUInt(message._source), msg._number_1);
            break;
        }
        default:
//            DLOG(0, "RV. unhandled ViperMessage {i1}", toUInt(message._type))
            break;
    }
}

void RelativeValue::onShiftUpdate(uint32_t shift) {
    for (auto& config : _viperConfig._globalConfigs) {
        if (config._key == RV_WIDTH_MULTIPLIER) {
            _widthMultiplier = boost::lexical_cast<double>(config._shifts[shift]);
            calculateExtraContractWidths();
            LOG(0, "RV width_multiplier changed. Value={d1}", _widthMultiplier)
            calculateQuotes();
            sendQuoteSizesToGUI();
            break;
        }
    }
    auto disableTime = boost::lexical_cast<int32_t>(_viperConfig.getGlobalByShift(AUTO_RV_DISABLE_TIME, toUInt(_viperConfig._currentShift)));
    setAutoDisableTime(disableTime, clockUtil::GetCycles());
}

void RelativeValue::onEconomicNumberEnded(EconomicNumber::Color color) {
    if (!canEnable()) {
        LOG(0, "RV. on EconomicNumber ended. Cannot enabled {i1}", 0)
        sendErrorMessage("RV on EconomicNumber ended. Cannot enabled", ErrorType::Critical);
        return;
    }

    LOG(0, "RV. check quotes on EconomicNumber ended. color={i1}", toUInt(color))
    if (!doInitialCalculations()) {
        LOG(0, "RV. Not enabling due to error occurred in calculateQuotes {i1}", 0)
        sendErrorMessage("RV. On EconomicNumber ended. Not enabling due to error occurred in calculateQuotes", ErrorType::Critical);
        return;
    }

    // check cross
    bool crossed = false;
    for (uint32_t instrumentIndex = _indexOfFirstTradedContract; instrumentIndex < _numberOfAllContracts; ++instrumentIndex) {
        RvContract& contract = _contracts[instrumentIndex];
        if (contract._traded) {
            auto& bidQuote = contract._quotes[BID];
            auto mktAskPrice = contract.externalMarket<ASK>().getPrice();
            if (bidQuote._externalSize && bidQuote._externalPrice != hf::NULL_PRICE && mktAskPrice != hf::NULL_PRICE &&
                bidQuote._externalPrice >= mktAskPrice) {
                crossed = true;
                LOG(0, "RV {C[i1]}. BID crossed externalPrice={d1} marketAskPrice={d2}", getContract(contract._internalId),
                    ToDollars(bidQuote._externalPrice), ToDollars(mktAskPrice))
                sendErrorMessage(boost::str(boost::format("RV %1% BID crossed. externalPrice=%2% marketAskPrice=%3%") % contract._definition._name
                    % ToDollars(bidQuote._externalPrice) % ToDollars(mktAskPrice)), ErrorType::Critical);
            }

            auto& askQuote = contract._quotes[ASK];
            auto mktBidPrice = contract.externalMarket<BID>().getPrice();
            if (askQuote._externalSize && askQuote._externalPrice != hf::NULL_PRICE && mktBidPrice != hf::NULL_PRICE &&
                askQuote._externalPrice <= mktBidPrice) {
                crossed = true;
                LOG(0, "RV {C[i1]}. ASK crossed externalPrice={d1} marketAskPrice={d2}", getContract(contract._internalId),
                    ToDollars(bidQuote._externalPrice), ToDollars(mktAskPrice))
                sendErrorMessage(boost::str(boost::format("RV %1% ASK crossed. externalPrice=%2% marketAskPrice=%3%") % contract._definition._name
                    % ToDollars(bidQuote._externalPrice) % ToDollars(mktAskPrice)), ErrorType::Critical);
            }
        }
    }

    if (crossed) {
        LOG(0,"Not enable RV due to cross. {i1}", 0)
        sendErrorMessage("On EconomicNumber ended. Not enable RV due to cross", ErrorType::Critical);
        return;
    }

    enable();

    for (uint32_t instrumentIndex = _indexOfFirstTradedContract; instrumentIndex < _numberOfAllContracts; ++instrumentIndex) {
        RvContract& contract = _contracts[instrumentIndex];
        if (contract._traded) {
            const bool quoteExternal = canQuoteExternal(instrumentIndex, contract._definition._venue);
            sendQuoteOrder(contract, quoteExternal);
        }
    }
    LOG(0,"RV enabled {i1}", 0)
    sendErrorMessage("RV enabled on EconomicNumber ended.", ErrorType::Normal);
}

void RelativeValue::handleSTFSubscription(uint32_t /*strategyId*/, uint32_t contractId) {
    LOG(0, "RV {C[i1]}. STF subscribed by AS", contractId)
    _shortTermFairsForContracts.getRef(contractId)._subscribed = true;
}