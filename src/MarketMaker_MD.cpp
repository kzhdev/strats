#include "MarketMaker.h"
#include "SimpleMarketMaker.h"
#include <ven/VenueUtils.h>

using namespace Python;

void MarketMaker::onMDUpdate(const md::MDUpdateEvent& event, uint32_t internalId) {
    auto& mm = _marketMakers[internalId];
    if (!mm._contract._toMarketMake) {
        return;
    }
    copyBooks(*this, mm, event, _updatedContracts);
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
                    handleMDTrade<SimpleMarketMaker, BID>(mm, trade, internalId, _updatedContracts);
                } else if (trade.side == hf::SIDE::SELL) {
                    handleMDTrade<SimpleMarketMaker, ASK>(mm, trade, internalId, _updatedContracts);
                }
            }
        }
    }
}

void MarketMaker::onMDStatus(hf::VENUE venue, uint64_t extSeqNum, const md::MDStatusEvent& event, uint32_t internalId) {
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
                case hf::VENUE::B3: FALLTHROUGH
                case hf::VENUE::CME:
                    handle_CME_B3_MDStatus(internalId, venue, event, mm);
                    break;
                case hf::VENUE::ICE:
                    handleICEMDStatus(internalId, event, mm);
                    break;
                case hf::VENUE::EUREX:
                    handleEUREXMDStatus(internalId, event, mm);
                    break;
                default:
                    assert(false && "unhandled venue");
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

void MarketMaker::handle_CME_B3_MDStatus(uint32_t internalId, hf::VENUE venue, const md::MDStatusEvent& event, SimpleMarketMaker& mm) {
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
        case 101: FALLTHROUGH   // B3 Final Closing Call
        case 18: FALLTHROUGH
        case 4: {
            mm._contractCanTrade = false;
            mm._contractCanCancel = false;
            LOG(0, "MM {C[i1]}. market data - closed({i2}).", getContract(internalId), (uint32_t) (event.extStatus))
            break;
        }
        case 21: {
            mm._contractCanTrade = false;
            if (venue == hf::VENUE::CME) {
                mm._contractCanCancel = event.ext2.cme.securityTradingEvent != 1;
            } else {
                mm._contractCanCancel = false;
            }
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

void MarketMaker::handleEUREXMDStatus(uint32_t internalId, const md::MDStatusEvent& event, SimpleMarketMaker& mm) {

    //See notes in Executor.h handleEUREXMDStatus for more notes on state
    switch(event.extStatus) {
        case 203: //Continuous
            mm._contractCanTrade = true;
            mm._contractCanCancel = true;
            LOG(0, "MM {C[i1]}. market data - opened.", getContract(internalId));
            break;
        case 200: //Closed
            mm._contractCanTrade = false;
            mm._contractCanCancel = false;
            LOG(0, "MM {C[i1]}. market data - closed. extStatus={i2}", getContract(internalId), event.extStatus);
            break;
        case 2:   /* Halt */       FALLTHROUGH
        case 201: /* Restricted */ FALLTHROUGH
        case 216: /* Freeze */     FALLTHROUGH
        case 213: /* IPO Auction Freeze */ FALLTHROUGH
        case 214: /* Pre Call */ FALLTHROUGH
        case 215: /* Call */
            // These are states that as described we will handle as a freeze, but have never been seen in market data
            LOG(0, "MM {C[i1]}. market data - seen rare MD state we're treating as a halt  extStatus={i2}", getContract(internalId), event.extStatus);
            FALLTHROUGH;
        case 204: /* Opening Auction */ FALLTHROUGH
        case 206: /* Intraday Auction */ FALLTHROUGH
        case 208: /* Circuit Breaker Auction */ FALLTHROUGH
        case 210: /* Closing Auction */ FALLTHROUGH
        case 212: /* IPO Auction */
            mm._contractCanTrade = false;
            mm._contractCanCancel = true;
            LOG(0, "MM {C[i1]}. market data - auction.  extStatus={i2}", getContract(internalId), event.extStatus);
            break;
        case 205: /* Opening Auction Freeze */ FALLTHROUGH
        case 207: /* Intraday Auction Freeze */ FALLTHROUGH
        case 209: /* Circuit Breaker Auction Freeze */ FALLTHROUGH
        case 211: /* Closing Auction Freeze */
            mm._contractCanTrade = false;
            mm._contractCanCancel = true;
            LOG(0, "MM {C[i1]}. market data - auction freeze. extStatus={i2}", getContract(internalId), event.extStatus);
            break;
        case 202: //Book
            LOG(0, "MM {C[i1]}. market data - Book.", getContract(internalId));
            break; //Explicitly doing nothing
        default:
            LOG(0, "MM {C[i1]}. market data - unknown state.  extStatus={i2}", getContract(internalId), event.extStatus);
            break;
    }
}

void MarketMaker::onMarketDataChange(uint32_t internalId) {
    auto& mm = _marketMakers[internalId];
    assert(mm._contract._toMarketMake);
    mm.onMarketDataChange();
    mm.resetChangeIndices();
}