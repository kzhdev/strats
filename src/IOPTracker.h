//
// Created by cmilutinovic on 1/29/20.
//

#ifndef RHONEDS_IOPTRACKER_H
#define RHONEDS_IOPTRACKER_H

#include "md/md_event.h"
#include "Common.h"
#include "ClockUtil.h"
#include "Throttler.h"
#include <vector>
#include <unordered_set>
#include <algorithm>
#include "ViperConfig.h"
#include <strat/om_queue_info.h>

namespace Python {

    template<typename ExecutorT>
    class IOPTracker {

        static constexpr auto GTC_ESTIMATED_HIGH_WATER_MARK_PER_SIDE = 1000;

    public:

        void init(ExecutorT *executor, int numContracts) {
            BOOST_ASSERT(executor != nullptr);
            _executor = executor;
            _scrapedGTCs.resize(numContracts);
            for(auto& set : _scrapedGTCs) {
                set.reserve(GTC_ESTIMATED_HIGH_WATER_MARK_PER_SIDE * 2);
            }
            _iopPrice.resize(numContracts);
            _lastBookUpdate.resize(numContracts);

            for(const auto& conn: _executor->context().getAllExchangeConnections()) {
                auto venue = strat::venue(conn);
                _routeToPort[toUInt(venue)].push_back(strat::port_gid(conn));
            }

            for(const auto& kvp : _executor->_viperConfig._sidToInternalId) {
                _internalIdToSid[kvp.second] = kvp.first;
            }
        }

        void stopTrackingOrder(int internalId, om::strategy_order_id_t oid) {
            auto& orders = _scrapedGTCs[internalId];
            BOOST_VERIFY(orders.erase(oid) == 1);
            sendNumGTCOrdersToFront(internalId);
        }

        void handleGTCManagementGridUpdate(const GridUpdate& gridUpdate) {
            const auto& update = gridUpdate._update;
            BOOST_ASSERT(update.updatetype() == proto::GridUpdateType::CellButtonType);
            hf::nanos_t start = getTS();

            if(BOOST_LIKELY(update.updatetype() == proto::GridUpdateType::CellButtonType)) {
                uint32_t internalId = update.cellbutton().row();
                LOG(0, "IOPTracker Received request in IOPTracker.  Contract={C[i1]} internalId={i2} command={i3}", _executor->getContract(internalId), internalId, update.cellbutton().col());
                switch(update.cellbutton().col()) {
                    case (toUInt(GTCManagementColumns::CancelAllBid)):
                        statelessCancelAll(internalId, hf::SIDE::BUY);
                        break;
                    case (toUInt(GTCManagementColumns::Cancel_1_Bid)):
                        statelessCancelLevels<BID>(internalId, 1);
                        break;
                    case (toUInt(GTCManagementColumns::Cancel_2_Bid)):
                        statelessCancelLevels<BID>(internalId, 2);
                        break;
                    case (toUInt(GTCManagementColumns::CancelAllAsk)):
                        statelessCancelAll(internalId, hf::SIDE::SELL);
                        break;
                    case (toUInt(GTCManagementColumns::Cancel_1_Ask)):
                        statelessCancelLevels<ASK>(internalId, 1);
                        break;
                    case (toUInt(GTCManagementColumns::Cancel_2_Ask)):
                        statelessCancelLevels<ASK>(internalId, 2);
                        break;
                    case (toUInt(GTCManagementColumns::CancelAll)):
                        statelessCancelAll(internalId);
                        break;
                }

                if(!contractCanTrade(internalId)) {
                    sendIOPUpdate(internalId);
                }
            }

            hf::nanos_t end = getTS();
            auto milliseconds = (end - start) / hf::util::TimeUtils::ONE_MS;
            LOG(0, "IOPTracker Request={i1} TimeToProcess={i2}ms", update.cellbutton().col(), milliseconds);
        }

        void onIOPUpdate(const md::MDPreOpenPriceEvent &event) {
            onIOPUpdate(event.sid, event.bid, event.ask);
        }

        void onMDUpdateDuringAuction(uint32_t internal_id, hf::price_t bid, hf::price_t ask) {
            auto& bbbo = _lastBookUpdate[internal_id];
            bool updateRequired = (bid != bbbo._bid) | (ask != bbbo._ask);
            IOP_LOG(0, "IOPTracker onMDUpdateDuringAuction contract={C[i1]} updateRequired={i2} bid={i3} ask={i4}", internal_id, updateRequired, bid, ask);
            if(updateRequired) {
                bbbo._bid = bid;
                bbbo._ask = ask;

                //On CME, when the IOP uncrosses, no IOP update is sent!  We'll assume this is true for all exchanges
                if((bbbo._bid == hf::NULL_PRICE) | (bbbo._ask == hf::NULL_PRICE) | (bbbo._bid < bbbo._ask)) {
                    _iopPrice[internal_id] = {};
                    sendIOPUpdate(internal_id);
                }
                _executor->sendAuctionMDUpdateToFront(internal_id, bbbo._bid, bbbo._ask);
            }
        }

        void onIOPUpdate(hf::symbol_id_t sid, hf::price_t bid, hf::price_t ask) {
            BOOST_ASSERT(bid == ask);

            auto internalId = _executor->_viperConfig.getInternalId(sid);
            if(BOOST_UNLIKELY(internalId == INVALID_INTERNAL_ID)) {
                LOG(0, "IOPTracker Could not find _internalId for SymbolID.  sid={i1}", sid);
                _executor->sendErrorMessage("Could not find _internalId for SymbolID.  sid=" + std::to_string(sid), ErrorType::Critical);
                return;
            }

            IOP_LOG(0, "IOPTracker onIOPUpdate contract={C[i1]} bid={i2} ask={i3}", internalId, bid, ask);
            sendIOPUpdate(internalId, bid, ask);
        }

        void onOrderCancelled(uint32_t internalId, om::strategy_order_id_t oid) {
            _scrapedGTCs[internalId].erase(oid);
            IOP_LOG(0, "IOPTracker onOrderCancelled contract={C[i1]} oid={i2}", internalId, oid.value());
            sendIOPUpdate(internalId);
        }

        void cancelAllGTCs() {
            IOP_LOG(0, "IOPTracker cancelAllGTCs {i1}", 0);
            statelessCancelAll();
        }

        int cancelGTCs(uint32_t internalID) {
            IOP_LOG(0, "IOPTracker cancelGTCs for internalId={i1}", internalID);
            return statelessCancelAll(internalID);
        }

        void forceReconcileGTCs(bool sendNumGTCUpdateToGui = false) {
            IOP_LOG(0, "IOPTracker forceReconcileGTCs sendNumGTCUpdateToGui={i1}", sendNumGTCUpdateToGui);
            gatherGTCs();
            if(sendNumGTCUpdateToGui) {
                //        std::vector<std::unordered_set<om::order_id_t>> _scrapedGTCs; //_contractID => set<oid>
                for(unsigned i = 0; i < _scrapedGTCs.size(); ++i) {
                    sendNumGTCOrdersToFront(i);
                }
            }
        }

        void sendNumGTCOrdersToFront(int32_t internalId) {
            const auto& orders = _scrapedGTCs[internalId];
            int bid = 0;
            int offer = 0;
            for(const auto& oid : orders) {
                const auto& order = _executor->context().getOrder(oid);
                BOOST_ASSERT(order.side == hf::SIDE::BUY || order.side == hf::SIDE::SELL);
                if(order.side == hf::SIDE::BUY) {
                    ++bid;
                }
                else if(order.side == hf::SIDE::SELL) {
                    ++offer;
                }
            }
            IOP_LOG(0, "IOPTracker sendNumGTCOrdersToFront contract={C[i1]} bids={i2} offers={i3}", internalId, bid, offer);
            if(bid | offer) {
                _executor->sendNumGTCOrdersToFront(internalId, bid, offer);
            }
        }

    private:

        //Discovers GTC state
        void gatherGTCs() {

            hf::nanos_t startTS = getTS();

            for(auto& gtcSet : _scrapedGTCs) {
                gtcSet.clear();
            }

            auto& orders = _executor->context().getOrders();
            for (auto it = orders.begin(); it != orders.end(); ++it) {
                const om::order_t& order = *it;
                if((order.action_flags.is_active) & (!order.action_flags.is_cancelling) & (order.ot == om::ORDER_TYPE::GTC)) {
                    auto internalId = _executor->_viperConfig.getInternalId(order.sid);
                    if(BOOST_UNLIKELY(internalId == INVALID_INTERNAL_ID)) {
                        LOG(0, "IOPTracker Could not find _internalId for oid.  oid={i1}", order.oid.value());
                        _executor->sendErrorMessage("Could not find _internalId for oid.  oid=" + std::to_string(order.oid.value()), ErrorType::Critical);
                    }
                    else {
                        _scrapedGTCs[internalId].insert(order.oid);
                    }
                }
            }

            hf::nanos_t endTS = getTS();

            LOG(0, "IOPTracker Gather GTC elapsed time: {i1}ms",((endTS - startTS) / hf::util::TimeUtils::ONE_MS));
        }

        void sendIOPUpdate(uint32_t internalId) {
            const auto& contract = _executor->_contractStates[internalId]._contract;
            const auto& iop = _iopPrice[internalId];
            const auto& lastBookUpdate = _lastBookUpdate[internalId];

            int numBidGTCs = 0;
            int numOfferGTCs = 0;

            if(!contract._toMarketMake) {
                return;
            }

            //If there is an IOP, we will use the IOP.  Otherwise we will use _lastBookUpdate which could be hf::NULL_PRICE
            const auto bestBid = iop._bid != hf::NULL_PRICE ? iop._bid : lastBookUpdate._bid;
            const auto bestOffer = iop._ask != hf::NULL_PRICE ? iop._ask : lastBookUpdate._ask;

            const om::order_t* bestBidOrder = nullptr;
            const om::order_t* bestAskOrder = nullptr;
            const auto& orders = _scrapedGTCs[internalId];
            for(const auto& oid : orders) {
                const om::order_t& order = _executor->context().getOrder(oid);
                if(order.action_flags.is_active & (!order.action_flags.is_cancelling)) {
                    order.side == hf::SIDE::BUY ? ++numBidGTCs : ++numOfferGTCs;
                    //Update best bid if necessary
                    if (order.side == hf::SIDE::BUY) {
                        if (bestBidOrder == nullptr || order.pr >= bestBidOrder->pr) {
                            bestBidOrder = &order;
                        }
                    } else if (order.side == hf::SIDE::SELL) {
                        if (bestAskOrder == nullptr || order.pr <= bestAskOrder->pr) {
                            bestAskOrder = &order;
                        }
                    }
                }
            }

            int ticksThroughBid = (bestAskOrder && bestBid != hf::NULL_PRICE) ? (bestBid - bestAskOrder->pr) / contract._tickSize : INT32_MIN;
            int ticksThroughOffer = (bestBidOrder && bestOffer != hf::NULL_PRICE) ? (bestBidOrder->pr - bestOffer) / contract._tickSize : INT32_MIN;

            bool forceCrossed = (!orders.empty()) & ((bestBid == hf::NULL_PRICE) | (bestOffer == hf::NULL_PRICE));

            IOP_LOG(0, "IOPTracker sendIOPUpdate contract={C[i1]} bestBid={i2} bestAsk={i3} iop={i4} numBidGTCs={i5} numAskGTCs={i6} ticksThroughBid={i7} ticksThroughAsk={i8} froceCrossed={i9}",
                    internalId, ((bestBidOrder == nullptr) ? -1111 : bestBidOrder->pr), ((bestAskOrder==nullptr) ? -1111 : bestAskOrder->pr), iop._bid, numBidGTCs, numOfferGTCs,
                    ticksThroughBid, ticksThroughOffer, forceCrossed);

            //We assume bid IOP is the actual IOP
            _executor->sendIOPUpdateToFront(internalId, iop._bid, numBidGTCs, numOfferGTCs, ticksThroughBid, ticksThroughOffer, forceCrossed);
        }

        void sendIOPUpdate(uint32_t internalId, hf::price_t bestBid, hf::price_t bestOffer) {
            auto& bbbo = _iopPrice[internalId];
            bbbo._bid = bestBid;
            bbbo._ask = bestOffer;
            sendIOPUpdate(internalId);
        }

        template<uint32_t Side>
        void statelessCancelLevels(uint32_t internalId, int numLevels) {

            if(BOOST_UNLIKELY(numLevels < 1)) {
                LOG(0, "IOPTracker Contract={C[i1]} Number of levels requested to cancel={i2} cannot handle negative numlevels, aborting", _executor->getContract(internalId), numLevels);
                _executor->sendErrorMessage("Contract=" + _executor->getContractName(internalId) + " Number of levels requested to cancel="
                     + std::to_string(numLevels) + " cannot handle negative numlevels, aborting", ErrorType::Critical);
                return;
            }

            gatherGTCs();
            hf::price_t priceOffset = (numLevels-1) * _executor->_viperConfig._allContracts[internalId]._tickSize;

            static_assert(Side == BID || Side == ASK, "unknown side");
            if(Side == BID) {
                hf::price_t bestBid = hf::NULL_PRICE;
                for(const auto& oid : _scrapedGTCs[internalId]) {
                    const auto& order = _executor->context().getOrder(oid);
                    if(order.side == hf::SIDE::BUY) {
                        bestBid = (bestBid == hf::NULL_PRICE) ? order.pr : std::max(bestBid, order.pr);
                    }
                }
                if(BOOST_UNLIKELY(bestBid == hf::NULL_PRICE)) {
                    LOG(0, "IOPTracker Contract={C[i1]} Request to cancel {i2} bid GTC levels, but no bid GTC orders", _executor->getContract(internalId), numLevels);
                    _executor->sendErrorMessage("Contract=" + _executor->getContractName(internalId) + " Request to cancel " + std::to_string(numLevels) + "bid GTC levels, but no bid GTC orders", ErrorType::Critical);
                    return;
                }
                statelessCancel(internalId, false, hf::SIDE::BUY, bestBid, bestBid - priceOffset);
            }
            else if (Side == ASK) {
                hf::price_t bestOffer = hf::NULL_PRICE;
                for(const auto& oid : _scrapedGTCs[internalId]) {
                    const auto& order = _executor->context().getOrder(oid);
                    if(order.side == hf::SIDE::SELL) {
                        bestOffer = (bestOffer == hf::NULL_PRICE) ? order.pr : std::min(bestOffer, order.pr);
                    }
                }
                if(BOOST_UNLIKELY(bestOffer == hf::NULL_PRICE)) {
                    LOG(0, "IOPTracker Contract={C[i1]} Request to cancel {i2} ask GTC levels, but no ask GTC orders", _executor->getContract(internalId), numLevels);
                    _executor->sendErrorMessage("Contract=" + _executor->getContractName(internalId) + " Request to cancel " + std::to_string(numLevels) + "ask GTC levels, but no ask GTC orders", ErrorType::Critical);
                    return;
                }
                statelessCancel(internalId, false, hf::SIDE::SELL, bestOffer, bestOffer + priceOffset);
            }
        }

        //Note *does not* follow C++ iter conventions.  First and last are inclusive.
        void statelessCancel(uint32_t internalId, bool shouldGatherGTCs, hf::SIDE side, hf::price_t firstPrice, hf::price_t lastPriceInclusive) {
            if(firstPrice > lastPriceInclusive) {
                std::swap(firstPrice, lastPriceInclusive);
            }

            cancelMatching(internalId, shouldGatherGTCs, [side, firstPrice, lastPriceInclusive](const om::order_t& order) {
                return (order.side == side) & (order.pr >= firstPrice) & (order.pr <= lastPriceInclusive);
            });
        }

        void statelessCancelAll(uint32_t internalId, hf::SIDE side) {
            cancelMatching(internalId, true, [side](const om::order_t& order) {
                return (order.side == side);
            });
        }

        int statelessCancelAll(uint32_t internalId) {
            return cancelMatching(internalId, true, [](const om::order_t& order) {
                return (order.side == hf::SIDE::BUY) | (order.side == hf::SIDE::SELL);
            });
        }

        void statelessCancelAll() {
            gatherGTCs();
            for(unsigned i = 0; i < _scrapedGTCs.size(); ++i) {
                cancelMatching(i, false, [](const om::order_t& order) { return true; } );
            }
        }

        hf::symbol_id_t getSIDFromInternalId(uint32_t internalId) {
            const auto iter = _internalIdToSid.find(internalId);
            if(BOOST_LIKELY(iter != std::end(_internalIdToSid))) {
                return iter->second;
            }
            return hf::NULL_SYMBOLID;
        }

        template<typename ConditionT>
        int cancelMatching(uint32_t internalId, bool shouldGatherGTCs, ConditionT&& condition) {
            int numCancelled = 0;
            hf::symbol_id_t sid = getSIDFromInternalId(internalId);
            if(BOOST_UNLIKELY(sid == hf::NULL_SYMBOLID)) {
                LOG(0, "IOPTracker Could not find sid for _internalID.  _internalId={i1}}", internalId);
                _executor->sendErrorMessage("Could not find sid for _internalID.  oid=" + std::to_string(internalId), ErrorType::Critical);
                return numCancelled;
            }
            if (shouldGatherGTCs) {
                gatherGTCs();
            }
            auto& gtcSet = _scrapedGTCs[internalId];
            auto orderIter = _scrapedGTCs[internalId].begin();
            while(orderIter != gtcSet.end()) {
                const om::order_t& order = _executor->context().getOrder(*orderIter);
                if(condition(order)) {
                    if(order.debug.i[ORDER_OWNER] != OM_OWNER || CanModifyOrCancelOrder(order.venue, order)) {
                        SetBit(order, STACKER_ORDER_FLAGS, EXPLICIT_OUT_OF_BAND_CANCEL);
                        _executor->cancelOrder(internalId, &order);
                        orderIter = gtcSet.erase(orderIter);
                        LOG(0, "IOPTracker Cancelling GTC symbol={C[i1]} price={d1} oid={i2}", _executor->getContract(internalId), ToDollars(order.pr), order.oid.value());
                        _executor->sendErrorMessage(boost::str(boost::format("Cancelling GTC.  symbol=%1% price=%2% oid=%3%") % _executor->getContractName(internalId) % ToDollars(order.pr)
                                % static_cast<int>(order.oid.value())), ErrorType::Normal);
                        ++numCancelled;
                        continue;
                    }
                    else {
                        LOG(0, "IOPTracker Attempted to cancel GTC.  In-flight state prevented cancel.  Will cancel when possible.  symbol={C[i1]} price={d1} oid={i2}", _executor->getContract(internalId), ToDollars(order.pr), order.oid.value())
                        _executor->sendErrorMessage(boost::str(boost::format("Attempted to cancel GTC.  In-flight state prevented cancel.  Will cancel when possible.  symbol=%1% price=%2% oid=%3%") %
                            _executor->getContractName(internalId) % ToDollars(order.pr) % static_cast<int>(order.oid.value())), ErrorType::Normal);
                        SetBit(order, STACKER_ORDER_FLAGS, SOF_IOP_ORDER_SHOULD_BE_CANCELLED);
                    }
                }
                ++orderIter;
            }
            return numCancelled;
        }

        bool contractCanTrade(uint32_t internalId) const {
            return _executor->_contractStates[internalId]._ime._contractCanTrade;
        }

        hf::nanos_t getTS() {
            timespec ts;
            asm volatile("" ::: "memory");
            clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
            asm volatile("" ::: "memory");
            return (ts.tv_sec * hf::util::TimeUtils::ONE_SECOND) + ts.tv_nsec;
        }

        struct bbbo { hf::price_t _bid = hf::NULL_PRICE; hf::price_t _ask = hf::NULL_PRICE; };

        ExecutorT *_executor = nullptr;
        std::vector<std::unordered_set<om::strategy_order_id_t, om::strategy_order_id_hash_t>> _scrapedGTCs; //_contractID => set<oid>
        std::vector<bbbo> _iopPrice;
        std::vector<bbbo> _lastBookUpdate;

        //Exchange -> Pid, preserving order, where order corresponds to the PID
        std::vector<om::port_gid_t> _routeToPort[static_cast<int>(hf::VENUE::LAST)];
        std::unordered_map<uint32_t, hf::symbol_id_t> _internalIdToSid;
    };
}


#endif //RHONEDS_IOPTRACKER_H
