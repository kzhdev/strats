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
            handleASInternalMatchingDone(reply._number_1, reply._number_2, reply._number_3);
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
                    LOG(0, "AS {C[i1]}. Internal book update. Side={i2} {i3}@{d1} -> {i4}@{d2}. MDbid={i5} contract._lastMDbid={i6}", contract.name(),
                        msg._side, market.getUnAdjustedSize(), ToDollars(market.getPrice()), tob.q, ToDollars(tob.pr),
                        msg._lastMDbid, contract._lastMDbid)
                    if (msg._side == BID) {
                        onMarketDataUpdate(contract, true, tob.pr, tob.q, false, otherSideMarket.getPrice(), otherSideMarket.getUnAdjustedSize());
                    } else {
                        onMarketDataUpdate(contract, false, otherSideMarket.getPrice(), otherSideMarket.getUnAdjustedSize(), true, tob.pr, tob.q);
                    }
                }
                contract._lastMDbid = msg._lastMDbid;
            } else {
                LOG(0, "AS {C[i1]}. ignore stale internal book update. Side={i2} update._lastMDbid={i3} contract._lastMDbid={i4}", contract.name(), msg._side, msg._lastMDbid, contract._lastMDbid)
            }
            break;
        }
        case ViperMessageType::EconomicNumberEnded: {
            const auto& msg = reinterpret_cast<const GenericMessage&>(message._data[0]);
            onEconomicNumberEnded((EconomicNumber::Color)msg._number_1);
            break;
        }
        case ViperMessageType::ShiftUpdate: {
            const auto& msg = reinterpret_cast<const GenericMessage&>(message._data[0]);
            uint32_t shift = msg._number_1;
            onShiftUpdate(shift);
            break;
        }
        case ViperMessageType::VectorParameterUpdate: {
            const auto& msg = reinterpret_cast<const VectorParameterUpdate&>(message._data[0]);
            handleVectorParameterUpdate(msg);
            break;
        }
        case ViperMessageType::FAKDone: {
            const auto& msg = reinterpret_cast<const GenericMessage&>(message._data[0]);
            handleFakDone(msg._number_1, msg._number_2, msg._number_3);
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
    LOG(0, "AS {C[i1]}. InternalHedgeDone. Side={i2} HedgeId={i3} Market={i4}", getContract(message._instrumentIndex),
        message._side, message._hedgeId, message._marketIndex)
    assert(message._marketIndex < MARKET_ID_EXTERNAL);
    auto& contract = _contracts[message._instrumentIndex];
    Market& market = contract._markets[message._side][Market::Internal];
    if (BOOST_LIKELY(market.handleInternalHedgeDone(message._marketIndex, message._hedgeId))) {
        markContractDirty(contract, &market);
    }
}

void ArbieSpreader::handleASInternalMatchingDone(uint32_t contractId, hf::qty_t bidFills, hf::qty_t askFills) {
    DLOG(0, "AS {C[i1]}. AS internal matching done.", getContract(contractId))
    auto& contract = _contracts[contractId];
    // If order filled internally, undoASQuoteAdjustment will be done in updateHedge
    if (!bidFills) {
        if (contract._markets[BID][Market::Internal].undoAsQuoteAdjustment(_affected_markets)) {
            markContractDirty(contract, &contract._markets[BID][Market::Internal]);
        }
    }

    if (!askFills) {
        if (contract._markets[ASK][Market::Internal].undoAsQuoteAdjustment(_affected_markets)) {
            markContractDirty(contract, &contract._markets[ASK][Market::Internal]);
        }
    }

    for (Market* market : _affected_markets) {
        auto& effected_contract = _contracts[market->getContractId()];
        markContractDirty(effected_contract, market);
    }
    _affected_markets.clear();
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
            if (contract._stf == hf::NULL_PRICE || stf->_fair == hf::NULL_PRICE) {
                logging::GetLogger().logInfo(boost::str(boost::format("AS %1%. STF=%2%") % configContract._name % ToDollars(stf->_fair)));
            }
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
    } else {
        assert(false);
    }
}

bool ArbieSpreader::ShortTermFairOffset::calculateShortTermFairOffset() {
    if (BOOST_UNLIKELY(!_contract)) {
        return false;
    }

    hf::price_t stfOffset = 0;
    for (auto& stf : _shortTermFairs) {
        if (stf._fair == hf::NULL_PRICE) {
            if (_stfOffset != hf::NULL_PRICE) {
                stfOffset = hf::NULL_PRICE;
                break;
            }
            _stfOffset = hf::NULL_PRICE;
            return false;
        }
        stfOffset += stf._ratio * stf._fair;
    }

    if (_stfOffset != stfOffset) {
        LOG(0, "AS {C[i1]}. Path {i2} STFOffset={d1} RvSTFMove={d2}", _contract->name(), (*_paths.begin())->_id,
            ToDollars(stfOffset), ToDollars(_rvSTFMove))
        if (stfOffset == hf::NULL_PRICE) {
            for (auto path : _paths) {
                path->_priceSize.reset();
                path->_resort = true;
            }
        }
        else if (_stfOffset == hf::NULL_PRICE) {
            for (auto path : _paths) {
                path->_recalc = true;
            }
        } else {
            auto diff =  stfOffset - _stfOffset;
            for (auto path : _paths) {
                if (path->_priceSize.valid()) {
                    path->_priceSize._price += diff;
                    path->_resort = true;
                }
            }
        }
        _stfOffset = stfOffset;
        return true;
    }
    return false;
}

void ArbieSpreader::onEconomicNumberEnded(EconomicNumber::Color color) {
    if (!canEnable()) {
        LOG(0, "AS. on EconomicNumber ended. Cannot enabled {i1}", 0)
        sendErrorMessage("AS on EconomicNumber ended. Cannot enabled", ErrorType::Critical);
        return;
    }

    LOG(0, "AS. check cross on EconomicNumber ended. color={i1}", toUInt(color))

    // check cross
    bool crossed = false;
    for (auto& contract : _contracts) {
        if (contract._usedInQuoting) {
            auto& bidQuote = contract._quote[BID];
            auto mktAskPrice = contract._markets[ASK][Market::External].getPrice();
            if (bidQuote._externalSize && bidQuote._externalPrice != hf::NULL_PRICE && mktAskPrice != hf::NULL_PRICE &&
                bidQuote._externalPrice >= mktAskPrice) {
                crossed = true;
                LOG(0, "AS {C[i1]}. BID crossed externalPrice={d1} marketAskPrice={d2}", contract.name(),
                    ToDollars(bidQuote._externalPrice), ToDollars(mktAskPrice))
                sendErrorMessage(boost::str(boost::format("AS %1% BID crossed. externalPrice=%2% marketAskPrice=%3%") % contract._def._name
                                            % ToDollars(bidQuote._externalPrice) % ToDollars(mktAskPrice)), ErrorType::Critical);
            }

            auto& askQuote = contract._quote[ASK];
            auto mktBidPrice = contract._markets[BID][Market::External].getPrice();
            if (askQuote._externalSize && askQuote._externalPrice != hf::NULL_PRICE && mktBidPrice != hf::NULL_PRICE &&
                askQuote._externalPrice <= mktBidPrice) {
                crossed = true;
                LOG(0, "AS {C[i1]}. ASK crossed externalPrice={d1} marketAskPrice={d2}", contract.name(),
                    ToDollars(bidQuote._externalPrice), ToDollars(mktAskPrice))
                sendErrorMessage(boost::str(boost::format("AS %1% ASK crossed. externalPrice=%2% marketBidPrice=%3%") % contract._def._name
                                            % ToDollars(bidQuote._externalPrice) % ToDollars(mktAskPrice)), ErrorType::Critical);
            }
        }
    }

    if (crossed) {
        LOG(0,"Not enable AS due to cross. {i1}", 0)
        sendErrorMessage("On EconomicNumber ended. Not enable AS due to cross", ErrorType::Critical);
        return;
    }

    enable();
    updateQuotes(true);
    LOG(0,"AS enabled {i1}", 0)
    sendErrorMessage("AS enabled on EconomicNumber ended.", ErrorType::Normal);
}

void ArbieSpreader::onShiftUpdate(uint32_t shift) {
    for (auto& config : _viperConfig._globalConfigs) {
        if (config._key == AS_EDGE_MULTIPLIER) {
            auto multiplier = boost::lexical_cast<double>(config._shifts[shift]);
            if (Compare(multiplier, _edgeMultiplier) != 0) {
                LOG(0, "AS edge_multiplier changed. Value={d1}", _edgeMultiplier)
                for (auto& contract : _contracts) {
                    if (contract._usedInQuoting) {
                        contract._edge = contract._edge / _edgeMultiplier * multiplier;
                    }
                }
                _edgeMultiplier = multiplier;
                updateQuotes(true);
            }
            break;
        }
    }
    auto disableTime = boost::lexical_cast<int32_t>(_viperConfig.getGlobalByShift(AUTO_AS_DISABLE_TIME, toUInt(_viperConfig._currentShift)));
    setAutoDisableTime(disableTime, clockUtil::GetCycles());
}

void ArbieSpreader::handleVectorParameterUpdate(const VectorParameterUpdate& msg) {
    const VectorParameterUpdate::Update* update = msg._data;
    for (uint32_t i = 0; i < msg._numUpdate; ++i) {
        if (update->_type == VectorParametersColumns::Range) {
            _vectorsRange[update->_vectorIndex] = update->_value;
        } else {
            _vectorsMaxPosition[update->_vectorIndex] = update->_value;
        }
        ++update;
    }

    for (auto& contract : _contracts) {
        if (contract._usedInQuoting) {
            for (auto& stfOffset : contract._shortTermFairOffsets) {
                calculateRVSTFMove(contract, stfOffset, true);
            }
        }
    }
}