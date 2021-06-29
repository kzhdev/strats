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
        case ViperMessageType::ASInternalMatchingDone: {
            const auto& reply = reinterpret_cast<const GenericMessage&> (message._data[0]);
            handleASInternalMatchingDone(reply._number_1);
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
        case ViperMessageType::ASQuoteAdjustmentACK: {
            const auto& msg = reinterpret_cast<const GenericMessage&> (message._data[0]);
            handleASQuoteAdjustmentACK(msg._number_1, msg._number_2, msg._number_3);
            break;
        }
        case ViperMessageType::STFUpdate: {
            const auto& msg = reinterpret_cast<const STFUpdate&> (message._data[0]);
            handleSTFUpdate(msg);
            break;
        }
        case ViperMessageType::InternalBookUpdate: {
            const auto& msg = reinterpret_cast<const InternalBookUpdate&>(message._data[0]);
            auto& contract = _contracts[msg._contractId];
            if (msg._lastMDbid >= contract._lastMDbid) {
                auto& market = contract._markets[msg._side][Market::External];
                auto& otherSideMarket = contract._markets[1 - msg._side][Market::External];
                auto& tob = msg._book.l[0];
                if (market.getPrice() != tob.pr || market.getUnAdjustedSize() != tob.q) {
                    if (msg._side == BID) {
                        onMarketDataUpdate(contract, true, tob.pr, tob.q, false, otherSideMarket.getPrice(), otherSideMarket.getUnAdjustedSize());
                    } else {
                        onMarketDataUpdate(contract, false, otherSideMarket.getPrice(), otherSideMarket.getUnAdjustedSize(), true, tob.pr, tob.q);
                    }
                }
                contract._lastMDbid = msg._lastMDbid;
            }
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
    if (BOOST_LIKELY(market.ackFAKAdjustment(hedgeAck._hedgeId, hedgeAck._size, hedgeAck._orderId))) {
        markContractDirty(contract, &market);
    }
}

void ArbieSpreader::handleInternalHedgeDone(const InternalHedgeDone& message) {
    DLOG(0, "AS {C[i1]}. InternalHedgeDone. Side={i2} HedgeId={i3} Market={i4}",
         getContract(message._instrumentIndex), message._side, message._hedgeId, message._marketIndex)
    assert(message._marketIndex < MARKET_ID_EXTERNAL);
    auto& contract = _contracts[message._instrumentIndex];
    Market& market = contract._markets[message._side][Market::Internal];
    if (BOOST_LIKELY(market.handleInternalHedgeDone(message._marketIndex, message._hedgeId))) {
        markContractDirty(contract, &market);
    }
}

void ArbieSpreader::handleASInternalMatchingDone(uint32_t contractId) {
    DLOG(0, "AS {C[i1]}. AS internal matching done.", getContract(contractId))
    auto& contract = _contracts[contractId];
    if (contract._markets[BID][Market::Internal].undoAsQuoteAdjustment(_effected_markets)) {
        markContractDirty(contract, &contract._markets[BID][Market::Internal]);
    }
    if (contract._markets[ASK][Market::Internal].undoAsQuoteAdjustment(_effected_markets)) {
        markContractDirty(contract, &contract._markets[ASK][Market::Internal]);
    }
    for (Market* market : _effected_markets) {
        auto& effected_contract = _contracts[market->getContractId()];
        markContractDirty(effected_contract, market);
    }
    _effected_markets.clear();
}

void ArbieSpreader::handleASQuoteAdjustmentACK(uint32_t contractId, uint32_t side, hf::qty_t adjSize) {
    auto& contract = _contracts[contractId];
    auto otherSide = GetOtherSide(side);
    if (contract._markets[otherSide][Market::Type::External].ackAsQuoteAdjustment(adjSize, _effected_markets)) {
        markContractDirty(contract, &contract._markets[otherSide][Market::Type::External]);
        for (Market* market : _effected_markets) {
            auto& effected_contract = _contracts[market->getContractId()];
            markContractDirty(effected_contract, market);
        }
        _effected_markets.clear();
    }
}

void ArbieSpreader::handleSTFUpdate(const STFUpdate& msg) {
    const STFUpdate::STF* stf = msg._data;
    for (uint32_t i = 0; i < msg._numSTF; ++i) {
//#ifdef IS_DEV
//        TLOG(0, "AS {C[i1]}. stf={i2}", _viperConfig._allContracts[stf->_contractId]._name, stf->_fair)
//#endif

        auto& configContract = _viperConfig._allContracts[stf->_contractId];
        auto range = _mapShortTermFairOffset.equal_range(stf->_contractId);
        if(range.first != range.second) {
            for (auto iter = range.first; iter != range.second; ++iter) {
                auto stfOffset = iter->second;
                stfOffset->onSTFUpdate(stf->_contractId, stf->_fair);
                _updatedSTFOffsets.emplace(stfOffset);
            }
        } else {
            logging::GetLogger().logInfo(boost::str(boost::format("AS %1%. Not in ShortTermFairOffset map. contractId=%2%") % configContract._name % configContract._internalId));
        }

        if (!configContract._legs.empty()) {
            Contract& contract = _contracts[stf->_contractId];
            contract._stf = stf->_fair;
        }
        ++stf;
    }

    for (auto* stfOffset : _updatedSTFOffsets) {
        if (stfOffset->calculateShortTermFairOffset()) {
            pushUpdatedContract(stfOffset->_contract);
        }
    }
    _updatedSTFOffsets.clear();
}

void ArbieSpreader::ShortTermFairOffset::onSTFUpdate(uint32_t contractId, hf::price_t stf) {
    auto iter = findSTF(contractId);
    if (BOOST_LIKELY(iter != _shortTermFairs.end())) {
        iter->_fair = stf;
    }
}

bool ArbieSpreader::ShortTermFairOffset::calculateShortTermFairOffset() {
    if (BOOST_UNLIKELY(!_contract)) {
        return false;
    }

    hf::price_t stfOffset = 0;
    for (auto& stf : _shortTermFairs) {
        if (stf._fair == hf::NULL_PRICE) {
            assert(_stfOffset == hf::NULL_PRICE);
            return false;
        }
        stfOffset += stf._ratio * stf._fair;
    }

    if (_stfOffset != stfOffset) {
        _stfOffset = stfOffset;
        for (auto path : _paths) {
            path->_dirty = true;
        }
        return false;
    }
    return true;
}