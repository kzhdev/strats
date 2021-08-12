#include "ArbieSpreader.h"

using namespace Python;

void ArbieSpreader::onMDUpdate(const md::MDUpdateEvent &event, uint32_t internalId, hf::batch_id_t batchId) {
    const md::top_t::level_t& bid = event.buy.top.l[0];
    const md::top_t::level_t& ask = event.sell.top.l[0];

    auto& contract = _contracts[internalId];
    if (!contract._usedInQuoting && !contract._usedInPath) {
        return;
    }

    if (contract._lastMDbid >= batchId) {
        logging::GetLogger().logWarning(boost::str(boost::format("AS %1%. Skip stale MD. bid=%2% lastMDbid=%3%") % contract._def._name % batchId % contract._lastMDbid));
        return;
    }

    const Market& asBid = contract._markets[BID][Market::External];
    const Market& asAsk = contract._markets[ASK][Market::External];

    const bool bidChanged = event.buy.updatedQtyNum && ((asBid.getPrice() != bid.pr) || (asBid.getUnAdjustedSize() != bid.q));
    const bool askChanged = event.sell.updatedQtyNum && ((asAsk.getPrice() != ask.pr) || (asAsk.getUnAdjustedSize() != ask.q));

    onMarketDataUpdate(contract, bidChanged, bid.pr, bid.q, askChanged, ask.pr, ask.q);
}

void ArbieSpreader::onMDTrade(const md::MDBatch& batch, const md::MDTradeEvent &event, uint32_t internalId) {
    auto& contract = _contracts[internalId];
    if (!contract._usedInQuoting && !contract._usedInPath) {
        return;
    }

    if (contract._lastMDbid >= batch.bid) {
        logging::GetLogger().logWarning(boost::str(boost::format("AS %1%. Skip stale MD. bid=%2% lastMDbid=%3%") % contract._def._name % batch.bid % contract._lastMDbid));
        return;
    }

    if (batch.venue == hf::VENUE::CME) {
        for (uint32_t i = 0; i < event.num; ++i) {
            auto& trade = event.trades[i];
            if (trade.side == hf::SIDE::BUY) {
                DLOG(0, "AS: Contract {C[i1]} Bid book adjustment due to trade. {i2}@{d1}. OurBest: {i3}@{d2}",
                     getContract(internalId),
                     trade.q, ToDollars(trade.pr), contract._markets[BID][Market::External].getUnAdjustedSize(),
                     ToDollars(contract._markets[BID][Market::External].getPrice()))
                onMarketDataUpdate(contract, true, trade.pr, trade.q, false, 0, 0);
            } else if (trade.side == hf::SIDE::SELL) {
                DLOG(0, "AS: Contract {C[i1]} Ask book adjustment due to trade. {i2}@{d1}. OurBest: {i3}@{d2}",
                     getContract(internalId),
                     trade.q, ToDollars(trade.pr), contract._markets[ASK][Market::External].getUnAdjustedSize(),
                     ToDollars(contract._markets[ASK][Market::External].getPrice()))
                onMarketDataUpdate(contract, false, 0, 0, true, trade.pr, trade.q);
            }
        }
    }
}

void ArbieSpreader::onMDStatus(hf::VENUE venue, uint64_t extSeqNum, const md::MDStatusEvent& event, uint32_t internalId) {
    auto& contract = _contracts[internalId];
    if (!contract._usedInPath && !contract._usedInQuoting) {
        return;
    }

    switch (event.statusType) {
        case md::MDStatusEvent::TYPE::RECOVERING:
        case md::MDStatusEvent::TYPE::TIMEOUT:
        case md::MDStatusEvent::TYPE::NOT_CONFIGURED: {
            if (contract._canTrade) {
                contract._canTrade = false;
                contract.markAffectedPathDirty();
            }
            LOG(0, "AS {C[i1]}. market data not working. Reason={i2}", getContract(internalId), toUInt(event.statusType))
            break;
        }
        case md::MDStatusEvent::TYPE::RECOVERED: {
            LOG(0, "AS {C[i1]}. market data recovered.", contract.name())
            contract._canTrade = true;
            contract.markAffectedPathDirty();
            break;
        }
        case md::MDStatusEvent::TYPE::TRADING_STATUS: {
            auto contractCanTrade = contract._canTrade;
            switch (venue) {
                case hf::VENUE::CME:
                case hf::VENUE::B3:
                    handle_CME_B3_MDStatus(event, contract);
                    break;
                case hf::VENUE::ICE:
                    handleICEMDStatus(event, contract);
                    break;
                case hf::VENUE::EUREX:
                    handleEUREXMDStatus(event, contract);
                    break;
                default:
                    break;
            }
            if (contractCanTrade != contract._canTrade) {
                contract.markAffectedPathDirty();
            }
            break;
        }
        case md::MDStatusEvent::TYPE::UNKNOWN: {
            std::string message = boost::str(boost::format("Contract %1% market data - received unknown md status. AS") % contract._def._name);
            sendErrorMessage( boost::str(boost::format("Contract %1% market data - received unknown md status. AS") % contract._def._name), ErrorType::Critical);
            LOG(0, "Contract {C[i1]} market data - received unknown md status. AS", contract.name())
            break;
        }
        case md::MDStatusEvent::TYPE::UNRECOVERABLE:
            break;
    }
}

void ArbieSpreader::handleICEMDStatus(const md::MDStatusEvent& event, Contract& contract) {
    switch (event.extStatus) {
        case 'O': {
            contract._canTrade = true;
            LOG(0, "AS {C[i1]}. market data - open.", contract.name())
            break;
        }
        case 'C': {
            contract._canTrade = false;
            LOG(0, "AS {C[i1]}. market data - closed. Used in pricing={i2}", contract.name(), contract._enabled)
//            disableRelativeValue(boost::str(boost::format("Contract %1% market data - closed. Used in pricing=%2%") % contract._def._name % contract._enabled));
            break;
        }
        case 'E': {
            contract._canTrade = false;
            LOG(0, "AS {C[i1]}. market data - expired. Used in pricing={i2}", contract.name(), contract._enabled)
//            disableRelativeValue(boost::str(boost::format("Contract %1% market data - expired. Used in pricing=%2%") % contract._def._name % contract._enabled));
            break;
        }
        case '1': {
            contract._canTrade = false;
            LOG(0, "AS {C[i1]}. market data - entered pre-open. Used in pricing={i2}", contract.name(), contract._enabled)
//            disableRelativeValue(boost::str(boost::format("Contract %1% market data - entered pre-open.  Used in pricing=%2%") % contract._def._name % contract._enabled));
            break;
        }
        default:
            break;
    }
}

void ArbieSpreader::handle_CME_B3_MDStatus(const md::MDStatusEvent& event, Contract& contract) {
    switch (event.extStatus) {
        case 17: {
            contract._canTrade = true;
            LOG(0, "AS {C[i1]}. market data - opened.", contract.name())
            break;
        }
        case 2: {
            contract._canTrade = false;
            LOG(0, "AS {C[i1]}. market data - halted. Used in pricing={i2}", contract.name(), contract._enabled)
//            disableRelativeValue(boost::str(boost::format("Contract %1% market data - halted. Used in pricing=%2%.") % contract._def._name % contract._enabled));
            break;
        }
        case 18:
        case 4: {
            contract._canTrade = false;
            LOG(0, "AS {C[i1]} market data - closed({i2}). Used in pricing={i3}", contract.name(), (uint32_t)(event.extStatus), contract._enabled)
//            disableRelativeValue(boost::str(boost::format("Contract %1% market data - closed(%2%). Used in pricing=%3%.") % contract._def._name % event.extStatus % contract._enabled));
            break;
        }
        case 21: {
            contract._canTrade = false;
            LOG(0, "AS {C[i1]} market data - entered pre-open. Used in pricing={i2}", contract.name(), contract._enabled)
//            disableRelativeValue(boost::str(boost::format("Contract %1% market data - entered pre-open. Used in pricing=%2%") % contract._def._name % contract._enabled));
            break;
        }
        default: {
            break;
        }
    }
}

void ArbieSpreader::handleEUREXMDStatus(const md::MDStatusEvent& event, Contract& contract) {

    //See notes in Executor.h handleEUREXMDStatus for more notes on state
    switch(event.extStatus) {
        case 203: //Continuous
            contract._canTrade = true;
            LOG(0, "AS {C[i1]}. market data - opened.", contract.name())
            break;
        case 200: //Closed
            contract._canTrade = false;
            LOG(0, "AS {C[i1]}. market data - closed. extStatus={i2}", contract.name(), event.extStatus);
            break;
        case 2:   /* Halt */
        case 201: /* Restricted */
        case 216: /* Freeze */
        case 213: /* IPO Auction Freeze */
        case 214: /* Pre Call */
        case 215: /* Call */
            // These are states that as described we will handle as a freeze, but have never been seen in market data
            LOG(0, "AS {C[i1]}. market data - seen rare MD state we're treating as a halt  extStatus={i2}", contract.name(), event.extStatus);
            FALLTHROUGH;
        case 204: /* Opening Auction */
        case 206: /* Intraday Auction */
        case 208: /* Circuit Breaker Auction */
        case 210: /* Closing Auction */
        case 212: /* IPO Auction */
            contract._canTrade = false;
            LOG(0, "AS {C[i1]}. market data - auction. extStatus={i2}", contract.name(), event.extStatus);
            break;
        case 205: /* Opening Auction Freeze */
        case 207: /* Intraday Auction Freeze */
        case 209: /* Circuit Breaker Auction Freeze */
        case 211: /* Closing Auction Freeze */
            contract._canTrade = false;
            LOG(0, "AS {C[i1]}. market data - auction freeze. extStatus={i2}", contract.name(), event.extStatus);
            break;
        case 202: //Book
            LOG(0, "AS {C[i1]}. market data - Book.", contract.name());
            break; //Explicitly doing nothing
        default:
            LOG(0, "AS {C[i1]}. market data - unknown state.  extStatus={i2}", contract.name(), event.extStatus);
            break;
    }
}

void ArbieSpreader::onMarketDataUpdate(Contract& contract,
                                       bool bidChanged,
                                       hf::price_t bidPrice,
                                       hf::qty_t bidSize,
                                       bool askChanged,
                                       hf::price_t askPrice,
                                       hf::qty_t askSize) {
    bool hasChange = false;
    if (bidChanged) {
        hasChange = contract.updateMarket<BID>(MarketIds::MARKET_ID_EXTERNAL, bidPrice, bidSize, _effected_markets);
    }
    if (askChanged) {
        hasChange = contract.updateMarket<ASK>(MarketIds::MARKET_ID_EXTERNAL, askPrice, askSize, _effected_markets);
    }

    if (hasChange) {
        addUpdatedContract(contract);
    }

    for (Market* market : _effected_markets) {
        markContractDirty(_contracts[market->getContractId()], market);
    }
    _effected_markets.clear();
}

void ArbieSpreader::processInternalMarketUpdate(const MarketUpdate& marketUpdate) {
    auto& contract = _contracts[marketUpdate._instrumentIndex];
    auto& bidMarket = contract._markets[BID][Market::Internal];
    bidMarket.setId(marketUpdate._bidMarketId);
    auto bidPrice = marketUpdate._bidSize == 0 ? hf::NULL_PRICE : marketUpdate._bidPrice;
    if (bidMarket.getPrice() != bidPrice || bidMarket.getUnAdjustedSize() != marketUpdate._bidSize) {
        if (contract.updateMarket<BID>((MarketIds)marketUpdate._bidMarketId, bidPrice, marketUpdate._bidSize, _effected_markets)) {
            addUpdatedContract(contract);
        }
    }
    auto& askMarket = contract._markets[ASK][Market::Internal];
    askMarket.setId(marketUpdate._askMarketId);
    auto askPrice = marketUpdate._askSize == 0 ? hf::NULL_PRICE : marketUpdate._askPrice;
    if (askMarket.getPrice() != askPrice || askMarket.getUnAdjustedSize() != marketUpdate._askSize) {
        if (contract.updateMarket<ASK>((MarketIds)marketUpdate._askMarketId, askPrice, marketUpdate._askSize, _effected_markets)) {
            addUpdatedContract(contract);
        }
    }

    for (Market* market : _effected_markets) {
        markContractDirty(_contracts[market->getContractId()], market);
    }
    _effected_markets.clear();
}

void ArbieSpreader::handleUncommonMDEvent(const md::MDEvent &mde, const md::MDBatch& batch, uint32_t internalId) {
    switch (mde.type) {
        case md::MDEvent::TYPE::FAK_DONE: {
            const auto &event = static_cast<const md::MDFakDonenEvent &> (mde);
            handleFakDone(batch.venue, event, internalId);
            break;
        }
        default:
            break;
    }
}

void ArbieSpreader::handleFakDone(hf::VENUE venue, const md::MDFakDonenEvent& event, uint32_t internalId) {
    const uint32_t side = ToViperSide(event._side);
    LOG(0, "AS {C[i1]}. Side={i2} Received FAK done. OID={i3}", internalId, side, event._oid.value())

    auto& contract = _contracts[internalId];
    Market& market = contract._markets[side][Market::External];
    if (BOOST_LIKELY(market.handleFakDone(event._oid.value()))) {
        markContractDirty(contract, &market);
    }
}

void ArbieSpreader::onEndMDBatch(hf::batch_id_t bid) {
    for (auto contractId : _mdUpdated) {
        auto& contract = _contracts[contractId];
        contract._lastMDbid = bid;
    }
}