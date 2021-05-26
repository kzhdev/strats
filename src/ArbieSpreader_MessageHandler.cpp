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
        markContractDirty(contract, hedgeAck._side);
    }
}

void ArbieSpreader::handleInternalHedgeDone(const InternalHedgeDone& message) {
    DLOG(0, "AS {C[i1]}. InternalHedgeDone. Side={i2} HedgeId={i3} Market={i4}",
         getContract(message._instrumentIndex), message._side, message._hedgeId, message._marketIndex)
    assert(message._marketIndex < MARKET_ID_EXTERNAL);
    auto& contract = _contracts[message._instrumentIndex];
    Market& market = contract._markets[message._side][Market::Internal];
    if (BOOST_LIKELY(market.handleInternalHedgeDone(message._marketIndex, message._hedgeId))) {
        markContractDirty(contract, message._side);
    }
}

void ArbieSpreader::handleASInternalMatchingDone(uint32_t contractId) {
    DLOG(0, "AS {C[i1]}. AS internal matching done.", getContract(contractId))
    auto& contract = _contracts[contractId];
    if (contract._markets[BID][Market::Internal].undoAsQuoteAdjustment(_effected_markets)) {
        markContractDirty(contract, BID);
    }
    if (contract._markets[ASK][Market::Internal].undoAsQuoteAdjustment(_effected_markets)) {
        markContractDirty(contract, ASK);
    }
    for (Market* market : _effected_markets) {
        auto& affected_contract = _contracts[market->getContractId()];
        markContractDirty(affected_contract, market->getSide());
    }
    _effected_markets.clear();
}

void ArbieSpreader::handleASQuoteAdjustmentACK(uint32_t contractId, uint32_t side, hf::qty_t adjSize) {
    auto& contract = _contracts[contractId];
    if (contract._markets[1 - side][Market::Type::External].ackAsQuoteAdjustment(adjSize, _effected_markets)) {
        markContractDirty(contract, side);
        for (Market* market : _effected_markets) {
            auto& affected_contract = _contracts[market->getContractId()];
            markContractDirty(affected_contract, market->getSide());
        }
        _effected_markets.clear();
    }
}

void ArbieSpreader::handleSTFUpdate(const STFUpdate& msg) {
    const STFUpdate::STF* stf = msg._data;
    for (uint32_t i = 0; i < msg._numSTF; ++i) {
        auto& configContract = _viperConfig._allContracts[stf->_contractId];
        if (configContract._legs.empty()) {
            auto range = _mapShortTermFairOffset.equal_range(stf->_contractId);
            assert(range.first != range.second);
            for (auto iter = range.first; iter != range.second; ++iter) {
                auto stfOffset = iter->second;
                stfOffset->onSTFUpdate(stf->_contractId, stf->_fair);
                _updatedSTFOffsets.emplace(stfOffset);
            }
        } else {
            Contract& contract = _contracts[stf->_contractId];
            TLOG(0, "AS {C[i1]}. stf={i2}", contract.name(), stf->_fair)
            contract._stf = stf->_fair;
        }

        ++stf;
    }

    for (auto* sftOffset : _updatedSTFOffsets) {
        sftOffset->calculateShortTermFairOffset(_updatedContracts);
    }
    _updatedSTFOffsets.clear();
}

void ArbieSpreader::ShortTermFairOffset::onSTFUpdate(uint32_t contractId, hf::price_t stf) {
    auto iter = _shortTermFairs.find(contractId);
    if (BOOST_LIKELY(iter != _shortTermFairs.end())) {
        iter->second._fair = stf;
    }
}

void ArbieSpreader::ShortTermFairOffset::calculateShortTermFairOffset(std::unordered_set<Contract*>& updatedContracts) {
    if (BOOST_UNLIKELY(!_contract)) {
        return;
    }

    hf::price_t stfOffset = 0;
    for (auto& kvp : _shortTermFairs) {
        auto& stf = kvp.second;
        if (stf._fair == hf::NULL_PRICE) {
            assert(_stfOffset == hf::NULL_PRICE);
            return;
        }
        stfOffset += stf._ratio * stf._fair;
    }

    if (_stfOffset != stfOffset) {
        _stfOffset = stfOffset;
        for (auto path : _paths) {
            path->_dirty = true;
        }
        updatedContracts.emplace(_contract);
    }
}