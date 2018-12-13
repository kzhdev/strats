#ifndef VIPER_EXECUTOR_FOR_ONE_CONTRACT
#define VIPER_EXECUTOR_FOR_ONE_CONTRACT

#include "boost/format.hpp"
#include "Common.h"
#include "logging/Coolog.h"
#include "InternalMatchingEngine.h"

namespace Python {

struct Messages {
    uint32_t _inserts = 0;
    uint32_t _modifies = 0;
    uint32_t _cancels = 0;
    uint32_t _cancelRejects = 0;
    uint32_t _modifyRejects = 0;
    hf::qty_t _fills = 0;

    uint32_t getTotalOutgoing() {
        return _inserts + _modifies + _cancels;
    }
};

template <typename ExecutorT>
class ExecutorForOneContract {
public:

    void initialize(ConfigContract& contract, const std::vector<om::routing_id_t>& rids, StackerConfig& stackerConfig, ExecutorT* executor, hf::price_t rvHedgeMatchTolerance,
                    uint64_t fillBannerPeriod, uint64_t quoteBannerPeriod, uint64_t hedgeabannerPeriod, hf::VENUE venue, uint64_t selfMatchBannerPeriod) {
        _internalMatchingEngine.initialize(contract._internalId, contract._tickSize, stackerConfig, executor, contract._name, fillBannerPeriod, quoteBannerPeriod, hedgeabannerPeriod, venue, selfMatchBannerPeriod);
        _executor = executor;
        for (uint32_t i = 0; i < rids.size(); ++i) {
            _rid[i] =  rids[i];
        }
        _relativeValueHedgeMatchingTolerance = rvHedgeMatchTolerance;
        _name = contract._name;
    }

    void onQuoteOrderFromRelativeValue(QuoteOrderMessage& order) {
        auto& bidQuote = order._quotes[BID];
        auto& askQuote = order._quotes[ASK];

        LOG(0, "OnQuoteOrderFromRv. Name={C[i1]} BID: Internal: {i2}@{d1} External: {i3}@{d2} ASK: Internal: {i4}@{d3} External: {i5}@{d4}",
                                   getContractName(), bidQuote._internalMarketSize, ToDollars(bidQuote._internalMarketPrice), bidQuote._externalSize, ToDollars(bidQuote._externalPrice),
                                   askQuote._internalMarketSize, ToDollars(askQuote._internalMarketPrice), askQuote._externalSize, ToDollars(askQuote._externalPrice))

        _internalMatchingEngine.modifyBidOrder(ORDER_ID_RV_INTERNAL_QUOTE, bidQuote, order._quoteExternally);
        _internalMatchingEngine.modifyAskOrder(ORDER_ID_RV_INTERNAL_QUOTE, askQuote, order._quoteExternally);
        executeChanges(Order::Strategy::RelativeValue);
    }

    hf::qty_t onHedgeOrderFromRelativeValue(HedgeOrderMessage& order) {
        LOG(0, "onHedgeOrderFromRv. Name={C[i1]} Side={i2} Price={d1} Size={i3}", getContractName(), order._side, ToDollars(order._price), order._size)
        const hf::qty_t unmatchedSize = _internalMatchingEngine.tryMatch(order._side, order._price, order._size, _relativeValueHedgeMatchingTolerance);
        const hf::qty_t fillSize = order._size - unmatchedSize;
        if (fillSize > 0) {
            const hf::qty_t sizeToSend = order._isExternal ? fillSize : order._size;
            _executor->sendRelativeValueHedgeInternalFillToFront(_internalMatchingEngine._internalId, order._side, order._price, sizeToSend, order._stf);
            _internalMatchingEngine.synchronizeOrders();
        }
        if (order._isExternal) {
            if (unmatchedSize != 0) {
                order._size = unmatchedSize;
                _executor->sendExternalRvHedge(order);
                LOG(0, "Sending RV hedge to external market. Contract={C[i1]} Side={i2} Price={d1} Size={i3}", getContractName(), order._side, ToDollars(order._price),order._size)
            } else {
                _executor->sendHedgeMarketDataAdjustmentAckWithZeroSize(order);
            }
        } else {
            _executor->onInternalHedgeDone(order._instrumentIndex, order._marketId, GetOtherSide(order._side), order._hedgeId);
        }
        return unmatchedSize;
    }

    void finishAfterHedgeOrderFromRv() {
        _internalMatchingEngine.sendInternalFills();
    }

    void updateHedgeTolerance(hf::price_t hedgeTolerance) {
        _relativeValueHedgeMatchingTolerance = hedgeTolerance;
        LOG(0, "Hedge tolerance change. Name={C[i1]} Value={d1}", getContractName(),ToDollars(hedgeTolerance))
    }

    void modifyQuoteBidOrder(OrderId_t orderId, hf::price_t internalPrice, hf::qty_t internalSize, hf::price_t externalPrice, hf::qty_t externalSize, bool persistPrice) {
        _internalMatchingEngine.modifyBidOrder(orderId, internalPrice,internalSize, externalPrice, externalSize, persistPrice, false);
    }

    void modifyQuoteAskOrder(OrderId_t orderId, hf::price_t internalPrice, hf::qty_t internalSize, hf::price_t externalPrice, hf::qty_t externalSize, bool persistPrice) {
        _internalMatchingEngine.modifyAskOrder(orderId, internalPrice, internalSize, externalPrice, externalSize, persistPrice, false);
    }

    void modifyHedgeBidOrder(OrderId_t orderId, hf::price_t internalPrice, hf::qty_t internalSize, hf::price_t externalPrice, hf::qty_t externalSize, bool payUp) {
        _internalMatchingEngine.modifyBidOrder(orderId, internalPrice,internalSize, externalPrice, externalSize, false, payUp);
    }

    void modifyHedgeAskOrder(OrderId_t orderId, hf::price_t internalPrice, hf::qty_t internalSize, hf::price_t externalPrice, hf::qty_t externalSize, bool payUp) {
        _internalMatchingEngine.modifyAskOrder(orderId, internalPrice, internalSize, externalPrice, externalSize, false, payUp);
    }

    void removeBidOrder(OrderId_t orderId) {
        _internalMatchingEngine.removeBidOrder(orderId);
    }

    void removeAskOrder(OrderId_t orderId) {
        _internalMatchingEngine.removeAskOrder(orderId);
    }

    bool executeChanges(Order::Strategy strategy) {
        const bool matchOk = _internalMatchingEngine.matchOrders(strategy);
        if (matchOk) {
            _internalMatchingEngine.synchronizeOrders();
            _internalMatchingEngine.sendInternalFills();
        } else {
            _executor->disableMarketMakerAfterStackerRejects(_internalMatchingEngine._internalId, "Match orders failed");
        }
        return matchOk;
    }

    void sendGenericMessageToFront(ViperMessageType type, int64_t value1 = 0, int64_t value2 = 0) {
        _executor->sendGenericMessageToFront(type, value1, value2);
    }

    void sendErrorMessage(const std::string& s, ErrorType type) {
        _executor->sendErrorMessage(s, type);
    }

    void getAllOutOfMarket() {
        _internalMatchingEngine.getAllOutOfMarket();
    }

    void getMarketMakerOutOfMarket() {
        _internalMatchingEngine.getMarketMakerOutOfMarket();
    }

    void getRelativeValueOutOfMarket() {
        _internalMatchingEngine.getRelativeValueOutOfMarket();
    }

    void onOrderAccepted(const om::OMOrderAcceptedEvent& event, const om::order_t& order) {
        _internalMatchingEngine.onOrderAccepted(event, order);
    }

    void onOrderRejected(const om::OMOrderRejectedEvent& event, const om::order_t& order) {
        _internalMatchingEngine.onOrderRejected(event, order);
    }

    void onOrderCanceled(const om::OMOrderCanceledEvent& event, const om::order_t& order) {
        _internalMatchingEngine.onOrderCanceled(event, order);
    }

    void onOrderCancelRejected(const om::OMOrderCancelRejectedEvent &event, const om::order_t &order) {
        _internalMatchingEngine.onOrderCancelRejected(event, order);
    }

    void onOrderExecution(const om::OMExecutionEvent &event, const om::order_t &order) {
        _internalMatchingEngine.onOrderExecution(event, order);
    }

    void onOrderReplaced(const om::OMOrderModifiedEvent& event, const om::order_t& order) {
        _internalMatchingEngine.onOrderReplaced(event, order);
    }

    void onOrderReplaceRejected(const om::OMOrderModifyRejectedEvent& event, const om::order_t& order) {
        _internalMatchingEngine.onOrderReplaceRejected(event, order);
    }

    void expireFillBanner(uint32_t side) {
        _internalMatchingEngine.expireFillBanner(side);
    }

    void expireQuoteBanner(uint32_t side) {
        _internalMatchingEngine.expireQuoteBanner(side);
    }

    void expireHedgeQuoteBanner(uint32_t side) {
        _internalMatchingEngine.expireHedgeQuoteBanner(side);
    }

    void handleStackerParameterUpdate(const proto::GridCell& gridCell) {
        _internalMatchingEngine.handleStackerParameterUpdate(gridCell);
    }

    void handleQuoteBannerUpdate(uint64_t period) {
        _internalMatchingEngine.handleQuoteBannerUpdate(period);
    }

    void handleHedgeQuoteBannerUpdate(uint64_t period) {
        _internalMatchingEngine.handleHedgeQuoteBannerUpdate(period);
    }

    void handleRejectBannerUpdate(uint64_t period) {
        _internalMatchingEngine.handleRejectBannerUpdate(period);
    }

    void handleFillBannerUpdate(uint64_t period) {
        _internalMatchingEngine.handleFillBannerUpdate(period);
    }

    void returnOrderToHedgeManager(const om::order_t& order, uint32_t side) {
        _internalMatchingEngine.returnOrderToHedgeManager(order, side);
    }

    void resetThrottlingBanner() {
        _internalMatchingEngine.resetThrottlingBanner();
    }

    void setThrottlingBanner(THROTTLING_MODE mode) {
        _internalMatchingEngine.setThrottlingBanner(mode);
    }

    void forceDeleteHedgerOrders() {
        _internalMatchingEngine.forceDeleteHedgerOrders();
    }

#ifdef IS_DEV
    std::string getContractName() {
        return _name;
    }
#else
    uint32_t getContractName() {
            return _internalMatchingEngine._internalId;
        }
#endif

    hf::price_t _relativeValueHedgeMatchingTolerance = 0;
    ExecutorT* _executor;
    InternalMatchingEngine<ExecutorT> _internalMatchingEngine;
    std::string _name;
    om::routing_id_t _rid[10];
    bool _contractCanTrade = false;
    Messages _messages;
};

}

#endif //VIPER_EXECUTOR_FOR_ONE_CONTRACT