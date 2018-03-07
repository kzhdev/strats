#ifndef VIPER_ONE_LEVEL_ORDER_MANAGER_H
#define VIPER_ONE_LEVEL_ORDER_MANAGER_H

#include "boost/format.hpp"
#include <md/md.h>
#include <om/om.h>
#include <om/om_event.h>
#include <om/om_reject_codes.h>
#include "Common.h"
#include "Logger.h"

namespace Python {

template <typename ExecutorT>
class OneLevelOrderManager {

public:
    void initialize(uint32_t internalId, uint32_t side, const std::string& name, ExecutorT* executor) {
    }

    template <uint32_t Side>
    void quote(hf::price_t price, hf::qty_t qty) {
        _logger.logInfo(boost::str(boost::format("Hedger %1%. Updating level %2% to size %3%") % _name % ToDollars(_price) % qty));

        _logger.logInfo(boost::str(boost::format("Hedger %1%. Before. Target=%2% Acked=%3% Flight=%4% Trade=%5%") % _name % _target % _acknowledged % _flight % _traded));

        _acknowledged -= _traded;
        _traded = 0;
        _target = qty;

        const hf::qty_t change = getQuantityChange();
        synchronizeOrders(change);

        _logger.logInfo(boost::str(boost::format("Hedger %1%. After. Target=%2% Acked=%3% Flight=%4% Trade=%5%") % _name % _target % _acknowledged % _flight % _traded));
    }

    void synchronizeOrders(hf::qty_t change) {
        if (change > 0) {
            increaseSize(change);
        } else if (change < 0) {
            BOOST_ASSERT(false);
        }
    }

    hf::qty_t getTotalOrdersQuanity() {
        return _acknowledged + _flight;
    };

    hf::qty_t getQuantityChange() {
        return _target - getTotalOrdersQuanity() - _traded;
    }

    void increaseSize(hf::qty_t change) {
        createNewOrder(change);
    }

    void createNewOrder(hf::qty_t orderSize) {
        if (orderSize > 0) {
            om::order_t* order = _executor->newOrder(this, _price, orderSize);
            if (order == nullptr) {
                _logger.logError(boost::str(boost::format("Hedger could not create order.")));
                _executor->onCriticalError("Hedger could not create order.");
            } else {
                setQuantity(*order, orderSize, PRIOR_QUANTITY);
                _orders.push_back(order);
                _flight += orderSize;
            }
        }
    }

    void moveQuantity(hf::qty_t qty) {
        _acknowledged += qty;
        _flight -= qty;
    }

    void removeOrder(const om::order_t& order) {
        for (auto it = _orders.begin(); it != _orders.end(); ++it) {
            if ((*it)->oid == order.oid) {
                _orders.erase(it);
                break;
            }
        }
    }

    inline int64_t getQuantity(const om::order_t& order, uint32_t key) {
        return  order.debug.i[key];
    }

    inline void setQuantity(const om::order_t& order, int64_t qty, uint32_t key) {
        om::order_t& mutableOrder = const_cast<om::order_t&>(order);
        mutableOrder.debug.i[key] = qty;
    }

    static const uint32_t LEVEL_POINTER = 1;
    static const uint32_t PRIOR_QUANTITY = 2;

    void onOrderAccepted(const om::OMOrderAcceptedEvent& event, const om::order_t& order) {
        _logger.logInfo(boost::str(boost::format("Hedger %1%. Order Accepted. Id=%2% Price=%3% Size=%4%") % _name % order.oid % ToDollars(_price) % event.acceptedQty));
        _logger.logInfo(boost::str(boost::format("Hedger %1%. Before. Target=%2% Acked=%3% Flight=%4% Trade=%5%") % _name % _target % _acknowledged % _flight % _traded));
        moveQuantity(event.acceptedQty);
        om::order_t& mutableOrder = const_cast<om::order_t&>(order);
        mutableOrder.leavesQty = event.acceptedQty;
        _logger.logInfo(boost::str(boost::format("Hedger %1%. After. Target=%2% Acked=%3% Flight=%4% Trade=%5%") % _name % _target % _acknowledged % _flight % _traded));
    }

    void onOrderExecution(const om::OMExecutionEvent &event, const om::order_t &order) {
        _logger.logInfo(boost::str(boost::format("Hedger %1%. Fill. LevelPrice=%2%. Price=%3% Size=%4%") % _name % ToDollars(_price) % event.filledPrice % event.filledQty));
        _logger.logInfo(boost::str(boost::format("Hedger %1%. Before. Target=%2% Acked=%3% Flight=%4% Trade=%5%") % _name % _target % _acknowledged % _flight % _traded));
        _traded += event.filledQty;
        if (!order.is_active) {
            _logger.logInfo(boost::str(boost::format("Hedger %1%. Inflight fill.") % _name));
            if (order.is_cancelling) {
                _flight += order.leavesQty;
            }
            removeOrder(order);
        } else {
            om::order_t& mutableOrder = const_cast<om::order_t&>(order);
            mutableOrder.leavesQty = getQuantity(order, PRIOR_QUANTITY) - event.totalExecutedQty;
        }
        _logger.logInfo(boost::str(boost::format("Hedger %1%. After. Target=%2% Acked=%3% Flight=%4% Trade=%5%") % _name % _target % _acknowledged % _flight % _traded));
    }

    void onOrderCanceled(const om::OMOrderCanceledEvent& event, const om::order_t& order) {
        _logger.logInfo(boost::str(boost::format("Hedger %1%. Order Cancelled. Id=%2%") % _name % order.oid));
        _logger.logInfo(boost::str(boost::format("Hedger %1%. Before. Target=%2% Acked=%3% Flight=%4% Trade=%5%") % _name % _target % _acknowledged % _flight % _traded));
        moveQuantity(-order.leavesQty);
        removeOrder(order);
        _logger.logInfo(boost::str(boost::format("Hedger %1%. After. Target=%2% Acked=%3% Flight=%4% Trade=%5%") % _name % _target % _acknowledged % _flight % _traded));
    }

    void onOrderRejected(const om::OMOrderRejectedEvent& event, const om::order_t& order) {
        std::string message = boost::str(boost::format("Hedger %1% . New order rejected. Id=%2% Reason=%3% ReasonText=%4%") % _name % order.oid % event.reasonCodeExt % event.reasonText);
        _logger.logInfo(message);
        _executor->onCriticalError(message);
    }

    bool wasRejectedDueToBeingFilled(uint32_t rejectCode, hf::VENUE venue) {
        return rejectCode == ice_rejects::TOO_LATE_TO_CANCEL || rejectCode == ice_rejects::UNKNOWN_ORDER || rejectCode == om::toUType(om::OrderRejectReason::OrderNotActive);
    }

    void onOrderCancelRejected(const om::OMOrderCancelRejectedEvent &event, const om::order_t &order) {
        std::string message = boost::str(boost::format("Hedger %1% . Cancel order rejected. Id=%2% Reason=%3% ReasonText=%4%") % _name % order.oid % event.reasonCodeExt % event.reasonText);
        _logger.logInfo(boost::str(boost::format("Hedger %1%. Before. Target=%2% Acked=%3% Flight=%4% Trade=%5%") % _name % _target % _acknowledged % _flight % _traded));
        bool rejectedDueToFill = wasRejectedDueToBeingFilled(event.reasonCodeExt, order.venue);
        if (!rejectedDueToFill) {
            _logger.logInfo(message);
            _executor->onCriticalError(message);
        }
        _logger.logInfo(boost::str(boost::format("Hedger %1%. After. Target=%2% Acked=%3% Flight=%4% Trade=%5%") % _name % _target % _acknowledged % _flight % _traded));
    }

    void getOutOfMarket() {
    }

    hf::qty_t getRestingSize() {
        return 0;
    }

    hf::price_t getPrice() {
        return 0;
    }

    void updateQuoteSize(hf::qty_t size) {
    }

    hf::price_t _price = hf::NULL_PRICE;

    hf::qty_t _target = 0;
    hf::qty_t _acknowledged = 0;
    hf::qty_t _flight = 0;
    hf::qty_t _traded = 0;

    std::vector<om::order_t*> _orders;

    ExecutorT* _executor;
    std::string _name;
    Logger _logger;
};

}

#endif //VIPER_ONE_LEVEL_ORDER_MANAGER_H