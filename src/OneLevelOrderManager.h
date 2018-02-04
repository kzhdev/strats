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

    static const uint32_t PRIOR_QUANTITY = 2;
    static const uint32_t QUANTITY_TO_MODIFY = 3;

public:
    void initialize(uint32_t internalId, uint32_t side, const std::string& name, ExecutorT* executor) {
        _internalId = internalId;
        _side = side;
        _name = name;
        _executor = executor;
    }

    void quote(hf::price_t price, hf::qty_t qty) {
        _acknowledged -= _traded;
        _traded = 0;
        _target = qty;
        if (price == _price) {
            synchronizeOrders();
        } else {
            _price = price;
            synchronizeOrdersWithPriceChange();
        }
    }
     
    int64_t getQuantity(const om::order_t& order, uint32_t key) {
        return  order.debug.i[key];
    }

    void setQuantity(const om::order_t& order, int64_t qty, uint32_t key) {
        om::order_t& mutableOrder = const_cast<om::order_t&>(order);
        mutableOrder.debug.i[key] = qty;
    }

    bool canModifyOrder(const om::order_t& order) {
        return !order.is_modifying & order.is_acked & order.is_active & !order.is_cancelling;
    }
    
    hf::qty_t getTotalOrdersQuanity() {
        return _acknowledged + _flight;
    };
	
    hf::qty_t getQuantityChange() {
        return _target - getTotalOrdersQuanity() - _traded;
    }
	    
    void synchronizeOrdersWithPriceChange() {
        const hf::qty_t change = _target - getTotalOrdersQuanity() - _traded;
        if (change < 0) {
            hf::qty_t totalLevelSizeDecrease = change;
            for (auto it = _orders.rbegin(); it != _orders.rend(); ++it) {
                om::order_t* order = *it;
                if (canModifyOrder(*order)) {
                    if (totalLevelSizeDecrease > 0) {
                        totalLevelSizeDecrease = decreaseOrderSize(order, totalLevelSizeDecrease);
                    } else {
                        const hf::qty_t orderSize = getQuantity(*order, PRIOR_QUANTITY);
                        modify(order, _price, orderSize);
                    }
                }
            }
        } else {
            for (auto it = _orders.rbegin(); it != _orders.rend(); ++it) {
                om::order_t* order = *it;
                if (canModifyOrder(*order)) {
                    const hf::qty_t orderSize = getQuantity(*order, PRIOR_QUANTITY);
                    modify(order, _price, orderSize);
                }
            }
        }
    }
	    
    void reduceSize(hf::qty_t change) {
       hf::qty_t totalLevelSizeDecrease = change;
        for (auto it = _orders.rbegin(); it != _orders.rend(); ++it) {
            om::order_t* order = *it;
            if (canModifyOrder(*order)) {
                totalLevelSizeDecrease = decreaseOrderSize(order, totalLevelSizeDecrease);
                if (totalLevelSizeDecrease == 0) {
                    break;
                }
            }
        }
    }
        
    hf::qty_t decreaseOrderSize(om::order_t* order, hf::qty_t decreaseBy) {
        if (order->leavesQty == decreaseBy) {
            _flight -= decreaseBy;
            cancel(order);
            return 0;
        } else if (order->leavesQty < decreaseBy){
            const hf::qty_t orderSize = getQuantity(*order, PRIOR_QUANTITY);
            _flight -= orderSize;
            cancel(order);
            return decreaseBy - order->leavesQty;
        } else {
            _flight -= decreaseBy;
            const hf::qty_t orderSize = getQuantity(*order, PRIOR_QUANTITY);
            const hf::qty_t qty = orderSize - decreaseBy;
            modify(order, _price, qty);
            setQuantity(*order, qty, QUANTITY_TO_MODIFY);
            return 0;
        }
    }

    void synchronizeOrders() {
        const hf::qty_t change = _target - getTotalOrdersQuanity() - _traded;
        if (change > 0) {
            increaseSize(change);
        } else if (change < 0) {
            reduceSize(-change);
        }
    }
	    
    void increaseSize(hf::qty_t change) {
        createNewOrder(change);
    }
	    
    void createNewOrder(hf::qty_t size) {
        if (size > 0) {
            om::order_t* order = newOrder(_price, size);
            if (order == nullptr) {
                std::string message = boost::str(boost::format("OLOM could not create order."));
                _logger.logError(message);
                _executor->onCriticalError(message);
            } else {
                _orders.push_back(order);
                _flight += size;
            }
        } else {
            _logger.logError(boost::str(boost::format("OLOM attempted to create new order of size 0.")));
        }
    }
        
    void cancel(om::order_t* order) {
        _logger.logInfo(boost::str(boost::format("OLOM %1%. Cancel order. Id=%2%") % _name % order->oid));
        _executor->cancelOrder(order);
    }

    void modify(const om::order_t* order, hf::price_t price, hf::qty_t size) {
        _logger.logInfo(boost::str(boost::format("OLOM %1%. Modify order. Id=%2% Price=%3% Size=%4%") % _name % order->oid % price % size));
        _executor->modifyOrder(order, price, size);
    }

    om::order_t* newOrder(hf::price_t price, hf::qty_t size) {
        om::order_t* order = _executor->newLimitOrder(_internalId, _side, price, size);
        _logger.logInfo(boost::str(boost::format("Stacker %1%. New order. Id=%2% Price=%3% Size=%4%") % _name % order->oid % price % size));
        return order;
    }
        
    void adjustAfterAck(om::order_t& order) {
        const hf::qty_t change = getQuantityChange();
        if (change > 0) {
            createNewOrder(change);
        } else if (change < 0) {
            decreaseOrderSize(&order, std::abs(change));
        }
    }

    void onOrderAccepted(const om::OMOrderAcceptedEvent& event, const om::order_t& order) {
        moveQuantity(event.acceptedQty);
        setQuantity(order, event.acceptedQty, PRIOR_QUANTITY);
        om::order_t& mutableOrder = const_cast<om::order_t&>(order);
        mutableOrder.leavesQty = event.acceptedQty;
        adjustAfterAck(const_cast<om::order_t&>(order));
    }

    void onOrderReplaced(const om::OMOrderModifiedEvent& event, const om::order_t& order) {
        om::order_t& mutableOrder = const_cast<om::order_t&>(order);
        const hf::qty_t priorQty = getQuantity(order, PRIOR_QUANTITY);
        const hf::qty_t diff = event.acceptedQty - priorQty;
        moveQuantity(diff);
        setQuantity(order, event.acceptedQty, PRIOR_QUANTITY);
        mutableOrder.is_modifying = false;
        mutableOrder.leavesQty = event.acceptedQty - order.executedQty;
        adjustAfterAck(mutableOrder);
    }

    void onOrderExecution(const om::OMExecutionEvent &event, const om::order_t &order) {
        _logger.logInfo(boost::str(boost::format("OLOM %1%. Fill. LevelPrice=%2%. Price=%3% Size=%4%") % _name % ToDollars(_price) % event.filledPrice % event.filledQty));
        _traded += event.filledQty;
        om::order_t& mutableOrder = const_cast<om::order_t&>(order);
        mutableOrder.leavesQty = getQuantity(order, PRIOR_QUANTITY) - event.totalExecutedQty;
        if (!order.is_active) {
            removeOrder(order);
        }
    }

    void onOrderCanceled(const om::OMOrderCanceledEvent& event, const om::order_t& order) {
        moveQuantity(-order.leavesQty);
        removeOrder(order);
        adjustAfterAck(const_cast<om::order_t&>(order));
    }

    void removeOrder(const om::order_t& order) {
        for (auto it = _orders.begin(); it != _orders.end(); ++it) {
            if ((*it)->oid == order.oid) {
                _orders.erase(it);
                break;
            }
        }
    }

    bool wasRejectedDueToBeingFilled(uint32_t rejectCode, hf::VENUE venue) {
        return rejectCode == ice_rejects::TOO_LATE_TO_CANCEL || rejectCode == ice_rejects::UNKNOWN_ORDER || rejectCode == om::toUType(om::OrderRejectReason::OrderNotActive);
    }

    void onOrderRejected(const om::OMOrderRejectedEvent& event, const om::order_t& order) {
        std::string message = boost::str(boost::format("OLOM New order reject. Reason=%1% %2%") % event.reasonCodeExt % event.reasonText);
        _logger.logInfo(message);
        _executor->onCriticalError(message);
    }

    void onOrderReplaceRejected(const om::OMOrderModifyRejectedEvent& event, const om::order_t& order) {
        bool rejectedDueToFill = wasRejectedDueToBeingFilled(event.reasonCodeExt, order.venue);
        if (!rejectedDueToFill) {
            std::string message = boost::str(boost::format("OLOM New order reject. Reason=%1% %2%") % event.reasonCodeExt % event.reasonText);
            _logger.logInfo(message);
            _executor->onCriticalError(message);
        } else {
            const hf::qty_t diff = getQuantity(order, QUANTITY_TO_MODIFY) - getQuantity(order, PRIOR_QUANTITY);
            _flight += diff;
            synchronizeOrdersWithPriceChange();
        }
    }

    void onOrderCancelRejected(const om::OMOrderCancelRejectedEvent &event, const om::order_t &order) {
        bool rejectedDueToFill = wasRejectedDueToBeingFilled(event.reasonCodeExt, order.venue);
        if (!rejectedDueToFill) {
            std::string message = boost::str(boost::format("OLOM New order reject. Reason=%1% %2%") % event.reasonCodeExt % event.reasonText);
            _logger.logInfo(message);
            _executor->onCriticalError(message);
        } else {
            _flight += order.leavesQty;
            synchronizeOrdersWithPriceChange();
        }
    }

    void moveQuantity(hf::qty_t qty) {
        _acknowledged += qty;
        _flight -= qty;
    }

    void getOutOfMarket() {
        quote(_price, 0);
    }

    uint32_t _internalId;
    uint32_t _side;
    std::string _name;
    hf::price_t _price;
    hf::qty_t _target = 0;
    hf::qty_t _acknowledged = 0;
    hf::qty_t _flight = 0;
    hf::qty_t _traded = 0;
    std::vector<om::order_t*> _orders;
    ExecutorT* _executor;
    Logger _logger;
};

}

#endif //VIPER_ONE_LEVEL_ORDER_MANAGER_H