#ifndef VIPER_EXECUTOR_FOR_ONE_CONTRACT
#define VIPER_EXECUTOR_FOR_ONE_CONTRACT

#include "boost/format.hpp"
#include "Common.h"
#include "Logger.h"
#include "InternalMatchingEngine.h"

namespace Python {

template <typename ExecutorT>
class ExecutorForOneContract {
public:

    void initialize(ConfigContract& contract, om::routing_id_t rid, StackerConfig& stackerConfig, ExecutorT* executor) {
        _internalMatchingEngine.initialize(contract._internalId, contract._tickSize, stackerConfig, executor, contract._name);
        _executor = executor;
        _rid = rid;
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
        return _internalMatchingEngine.tryMatch(order._side, order._price, order._size);
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

    ExecutorT* _executor;
    InternalMatchingEngine<ExecutorT> _internalMatchingEngine;
    std::string _name;
    om::routing_id_t _rid;
    Logger _logger;
};

}

#endif //VIPER_EXECUTOR_FOR_ONE_CONTRACT