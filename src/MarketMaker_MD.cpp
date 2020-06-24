#include "MarketMaker.h"
#include "SimpleMarketMaker.h"
#include <ven/VenueUtils.h>

using namespace Python;

void MarketMaker::onMDBatch(const md::MDBatch& batch) {
    LOG(0, "MM received MD batch. bid={i1} extSeqNum={i2} venue={i3}", batch.bid, batch.extSequenceNumber,
        toUInt(batch.venue))

    const auto* data = batch.data;
    for (auto i = batch.count; i; --i) {
        const auto& mde = reinterpret_cast<const md::MDEvent&> (*data);
        data += mde.size;
        switch (mde.type) {
            case md::MDEvent::TYPE::UPDATE: {
                const auto& event = static_cast<const md::MDUpdateEvent&> (mde);
                onMDUpdate(batch.venue, event, batch.extSequenceNumber);
                break;
            }
            case md::MDEvent::TYPE::TRADE: {
                const auto& event = static_cast<const md::MDTradeEvent&> (mde);
                onMDTrade(batch.venue, event);
                break;
            }
            case md::MDEvent::TYPE::STATUS: {
                const auto& event = static_cast<const md::MDStatusEvent&> (mde);
                onMDStatus(batch.venue, event);
                break;
            }
            default:
                break;
        }
    }
}

void MarketMaker::onMDUpdate(hf::VENUE venue, const md::MDUpdateEvent& event, uint_fast64_t sequenceNumber) {
    const uint32_t internalId = _viperConfig.getInternalId(event.sid);
    if (internalId != INVALID_INTERNAL_ID) {
        auto& mm = _marketMakers[internalId];
        if (!mm._contract._toMarketMake) {
            return;
        }
        const bool bidChanged = event.buy.updatedQtyNum == 1;
        const bool askChanged = event.sell.updatedQtyNum == 1;
        auto& bid = event.buy.top.l[0];
        auto& ask = event.sell.top.l[0];
        copyBooks(mm, bid.pr, bid.q, bid.o, ask.pr, ask.q, ask.o, bidChanged, askChanged);
        if (bidChanged | askChanged) {
            _updatedContracts.insert(internalId);
        }
    }
}

void MarketMaker::copyBooks(SimpleMarketMaker& mm,
                            hf::price_t bidPrice,
                            hf::qty_t bidSize,
                            md::order_count_t bidOrderCount,
                            hf::price_t askPrice,
                            hf::qty_t askSize,
                            md::order_count_t askOrderCount,
                            bool bidChanged,
                            bool askChanged) {
    if (bidChanged) {
        copyBook(mm._books[BID], bidPrice, bidSize, bidOrderCount);
    }
    if (askChanged) {
        copyBook(mm._books[ASK], askPrice, askSize, askOrderCount);
    }
}

void MarketMaker::copyBook(Book& book, hf::price_t price, hf::qty_t size, md::order_count_t orderCount) {
    book._price = price;
    book._otherQuantity = size;
    book._otherOrderCount = orderCount;
}

void MarketMaker::onMDTrade(hf::VENUE venue, const md::MDTradeEvent& event) {
    if (venue == hf::VENUE::CME) {
        for (uint32_t i = 0; i < event.num; ++i) {
            auto& trade = event.trades[i];
            const uint32_t internalId = _viperConfig.getInternalId(event.sid);
            if (internalId != INVALID_INTERNAL_ID) {
                auto& mm = _marketMakers[internalId];
                if (!mm._contract._toMarketMake) {
                    continue;
                }
                if (trade.side == hf::SIDE::BUY) {
                    //LOG(0, "EXE: Contract {C[i1]} Bid book adjustment due to trade. {i2}@{d1}", getLoggingContract(internalId), trade.q, ToDollars(trade.pr))
                    copyBook(mm._books[BID], trade.pr, trade.q, trade.ext.cme_python_book_cleaning.orderCount);
                    _updatedContracts.insert(internalId);
                } else if (trade.side == hf::SIDE::SELL) {
                    //LOG(0, "EXE: Contract {C[i1]} Ask book adjustment due to trade. {i2}@{d1}", getLoggingContract(internalId), trade.q, ToDollars(trade.pr))
                    copyBook(mm._books[ASK], trade.pr, trade.q, trade.ext.cme_python_book_cleaning.orderCount);
                    _updatedContracts.insert(internalId);
                }
            }
        }
    }
}

void MarketMaker::onMDStatus(hf::VENUE venue, const md::MDStatusEvent& event) {
    const uint32_t internalId = _viperConfig.getInternalId(event.sid);
    if (internalId != INVALID_INTERNAL_ID) {
        auto& mm = _marketMakers[internalId];
        if (!mm._contract._toMarketMake) {
            return;
        }
        bool needToNotify = _oldMDStatus[static_cast<uint8_t>(venue)] != event.extStatus;
        _oldMDStatus[static_cast<uint8_t>(venue)] = event.extStatus;
        switch (event.statusType) {
            case md::MDStatusEvent::TYPE::RECOVERING:
            case md::MDStatusEvent::TYPE::TIMEOUT:
            case md::MDStatusEvent::TYPE::NOT_CONFIGURED: {
                LOG(0, "MM {C[i1]}. market data not working. Reason={i2}", getContract(internalId),
                    toUInt(event.statusType))
                mm._contractCanTrade = false;
                mm._contractCanCancel = true;
                if (needToNotify & isEnabled()) {
                    mm.disable();
                }
                break;
            }
            case md::MDStatusEvent::TYPE::RECOVERED: {
                mm._contractCanTrade = true;
                mm._contractCanCancel = true;
                LOG(0, "MM {C[i1]}. market data recovered.", getContract(internalId))
                break;
            }
            case md::MDStatusEvent::TYPE::TRADING_STATUS: {
                switch (venue) {
                    case hf::VENUE::CME:
                        handleCMEMDStatus(internalId, event, mm);
                        break;
                    case hf::VENUE::ICE:
                        handleICEMDStatus(internalId, event, mm);
                        break;
                    default:
                        break;
                }
                break;
            }
            case md::MDStatusEvent::TYPE::UNKNOWN: {
                LOG(0, "Contract {C[i1]} market data - received unknown md status.", getContract(internalId))
                sendErrorMessage(boost::str(
                        boost::format("Contract %1% market data - received unknown md status.") % mm._contract._name),
                                 ErrorType::Critical);
                break;
            }

                // remove compile warning
            case md::MDStatusEvent::TYPE::UNRECOVERABLE:
                break;
        }
    }
}

void MarketMaker::handleCMEMDStatus(uint32_t internalId, const md::MDStatusEvent& event, SimpleMarketMaker& mm) {
    switch (event.extStatus) {
        case 17: {
            mm._contractCanTrade = true;
            mm._contractCanCancel = true;
            LOG(0, "MM {C[i1]}. market data - opened.", getContract(internalId))
            break;
        }
        case 2: {
            mm._contractCanTrade = false;
            mm._contractCanCancel = true;
            LOG(0, "MM {C[i1]}. market data - halted.", getContract(internalId))
            break;
        }
        case 18:
        case 4: {
            mm._contractCanTrade = false;
            mm._contractCanCancel = false;
            LOG(0, "MM {C[i1]}. market data - closed({i2}).", getContract(internalId), (uint32_t) (event.extStatus))
            break;
        }
        case 21: {
            mm._contractCanTrade = false;
            mm._contractCanCancel = event.ext.cme.securityTradingEvent != 1;
            LOG(0, "MM {C[i1]}. market data - entered pre-open. canCancel {i2}", getContract(internalId),
                mm._contractCanCancel)
            break;
        }
        default: {
            break;
        }
    }
}

void MarketMaker::handleICEMDStatus(uint32_t internalId, const md::MDStatusEvent& event, SimpleMarketMaker& mm) {
    switch (event.extStatus) {
        case 'O': {
            mm._contractCanTrade = true;
            mm._contractCanCancel = true;
            LOG(0, "MM {C[i1]}. market data - opened.", getContract(internalId));
            break;
        }
        case 'C': {
            mm._contractCanTrade = false;
            mm._contractCanCancel = false;
            LOG(0, "MM {C[i1]}. market data - closed.", getContract(internalId))
            break;
        }
        case 'E': {
            LOG(0, "MM {C[i1]}. market data - expired.", getContract(internalId))
            mm._contractCanTrade = false;
            mm._contractCanCancel = false;

            if (isEnabled()) {
                mm.disable();
            }
            break;
        }
        case '1': {
            mm._contractCanTrade = false;
            mm._contractCanCancel = true;
            LOG(0, "MM {C[i1]}. market data - entered pre-open.", getContract(internalId))
            break;
        }

        default: {
            break;
        }
    }
}

void MarketMaker::onMarketDataChange(uint32_t internalId) {
    auto& mm = _marketMakers[internalId];
    if (mm._contract._toMarketMake) {
        mm.onChangeInOtherBooks();
    }
}