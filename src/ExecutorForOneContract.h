#ifndef VIPER_EXECUTOR_FOR_ONE_CONTRACT
#define VIPER_EXECUTOR_FOR_ONE_CONTRACT

#include "boost/format.hpp"
#include "Common.h"
#include "Logger.h"
#include "InternalMatchingEngine.h"

namespace Python {

struct Messages {
    uint32_t _inserts = 0;
    uint32_t _modifies = 0;
    uint32_t _cancels = 0;
};

template <typename ExecutorT>
class ExecutorForOneContract {
public:

    void initialize(ConfigContract& contract, om::routing_id_t rid, StackerConfig& stackerConfig, ExecutorT* executor, hf::price_t rvHedgeMatchTolerance, uint64_t fillBannerPeriod, uint64_t quoteBannerPeriod) {
        _internalMatchingEngine.initialize(contract._internalId, contract._tickSize, stackerConfig, executor, contract._name, fillBannerPeriod, quoteBannerPeriod);
        _executor = executor;
        _rid = rid;
        _relativeValueHedgeMatchingTolerance = rvHedgeMatchTolerance;
    }

    void onQuoteOrderFromRelativeValue(QuoteOrderMessage& order) {
        _logger.logInfo(boost::str(boost::format("OnQuoteOrderFromRv. Name=%1% Internal: %3%@%2% External: %5%@%4% Side=%6%")
                                   % _name % order._internalMarketPrice % order._internalMarketSize % order._externalPrice % order._externalSize % order._side));
        if (order._side == BID) {
            _internalMatchingEngine.modifyBidOrder(ORDER_ID_RV_INTERNAL_QUOTE, order);
            _internalMatchingEngine.matchOrders();
        } else {
            _internalMatchingEngine.modifyAskOrder(ORDER_ID_RV_INTERNAL_QUOTE, order);
            _internalMatchingEngine.matchOrders();
        }
        _internalMatchingEngine.synchronizeOrders();
    }

    hf::qty_t onHedgeOrderFromRelativeValue(HedgeOrderMessage& order) {
        _logger.logInfo(boost::str(boost::format("onHedgeOrderFromRv. Name=%1% Side=%2% Price=%3% Size=%4%") % _name % order._side % order._price % order._size));
        const hf::qty_t unmatchedSize = _internalMatchingEngine.tryMatch(order._side, order._price, order._size, _relativeValueHedgeMatchingTolerance);
        if (order._isExternal) {
            if (unmatchedSize != 0) {
                order._size = unmatchedSize;
                _internalMatchingEngine.sendExternalRvHedge(order);
                _logger.logInfo(boost::str(boost::format("Sending RV hedge to external market. Contract=%1% Side=%2% Price=%3% Size=%4%")
                                           % _name % getSideString(order._side) % order._price % order._size));
            }
        }
        return unmatchedSize;
    }

    void modifyQuoteBidOrder(OrderId_t orderId, hf::price_t internalPrice, hf::qty_t internalSize, hf::price_t externalPrice, hf::qty_t externalSize) {
        _internalMatchingEngine.modifyBidOrder(orderId, internalPrice,internalSize, externalPrice, externalSize);
    }

    void modifyQuoteAskOrder(OrderId_t orderId, hf::price_t internalPrice, hf::qty_t internalSize, hf::price_t externalPrice, hf::qty_t externalSize) {
        _internalMatchingEngine.modifyAskOrder(orderId, internalPrice, internalSize, externalPrice, externalSize);
    }

    void modifyHedgeBidOrder(OrderId_t orderId, hf::price_t internalPrice, hf::qty_t internalSize, hf::price_t externalPrice, hf::qty_t externalSize) {
        _internalMatchingEngine.modifyBidOrder(orderId, internalPrice,internalSize, externalPrice, externalSize);
    }

    void modifyHedgeAskOrder(OrderId_t orderId, hf::price_t internalPrice, hf::qty_t internalSize, hf::price_t externalPrice, hf::qty_t externalSize) {
        _internalMatchingEngine.modifyAskOrder(orderId, internalPrice, internalSize, externalPrice, externalSize);
    }

    void removeBidOrder(OrderId_t orderId) {
        _internalMatchingEngine.removeBidOrder(orderId);
    }

    void removeAskOrder(OrderId_t orderId) {
        _internalMatchingEngine.removeAskOrder(orderId);
    }

    void synchronizeOrders() {
        _internalMatchingEngine.synchronizeOrders();
    }

    void matchOrders() {
        _internalMatchingEngine.matchOrders();
    }

    void sendGenericMessageToFront(ViperMessageType type, int64_t value1 = 0, int64_t value2 = 0) {
        _executor->sendGenericMessageToFront(type, value1, value2);
    }

    void sendErrorMessage(const std::string& s, ErrorType type) {
        _executor->sendErrorMessage(s, type);
    }

    void getOutOfMarket() {
        _internalMatchingEngine.getOutOfMarket();
    }

    void onOrderAccepted(const om::OMOrderAcceptedEvent& event, const om::order_t& order) {
        _internalMatchingEngine.onOrderAccepted(event, order);
    }

    void onOrderRejected(const om::OMOrderRejectedEvent& event, const om::order_t& order) {
        _internalMatchingEngine.onOrderRejected(event, order);
    }

    void onOrderReplaced(const om::OMOrderModifiedEvent& event, const om::order_t& order) {
        _internalMatchingEngine.onOrderReplaced(event, order);
    }

    void onOrderReplaceRejected(const om::OMOrderModifyRejectedEvent& event, const om::order_t& order) {
        _internalMatchingEngine.onOrderReplaceRejected(event, order);
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

    void expireBanner() {
        _internalMatchingEngine.expireBanners();
    }

    void handleStackerParameterUpdate(const proto::GridCell& gridCell) {
        _internalMatchingEngine.handleStackerParameterUpdate(gridCell);
    }

    void handleQuoteBannerUpdate(uint64_t period) {
        _internalMatchingEngine.handleQuoteBannerUpdate(period);
    }

    void handleFillBannerUpdate(uint64_t period) {
        _internalMatchingEngine.handleFillBannerUpdate(period);
    }

    hf::price_t _relativeValueHedgeMatchingTolerance = 0;
    ExecutorT* _executor;
    InternalMatchingEngine<ExecutorT> _internalMatchingEngine;
    std::string _name;
    om::routing_id_t _rid;
    Messages _messages;
    Logger _logger;
};

}

#endif //VIPER_EXECUTOR_FOR_ONE_CONTRACT