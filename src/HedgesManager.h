#ifndef VIPER_HEDGES_MANAGER_H
#define VIPER_HEDGES_MANAGER_H

#include "boost/format.hpp"
#include <md/md.h>
#include <om/om.h>
#include <om/om_event.h>
#include <om/om_reject_codes.h>
#include "Common.h"
#include "Logger.h"

namespace Python {

template <typename ExecutorT>
class HedgesManager {

public:
    void initialize(uint32_t internalId, uint32_t side, const std::string& name, ExecutorT* executor) {
        _internalId = internalId;
        _side = side;
        _name = name;
        _ime = executor;
    }

    bool canChangeOrder(const om::order_t& order) {
        return order.is_active & !order.is_cancelling;
    }

    void hedge(hf::price_t price, hf::qty_t size, bool payUp) {
        LOG(0, "Hedger {C[i1]} Side={i2}. Updating to price={d1} size={i3} payUp={i4}", getContractName(), _side, ToDollars(price), size, payUp)
        LOG(0, "Hedger {C[i1]} Side={i2}. Before. Target={i3} Traded={i4}", getContractName(), _side, _target, _traded)

        if (size != 0) {
            hf::qty_t change = size;
            if (_target != 0) {
                change -= getOpenSize();
            }

            _target = size;
            _traded = 0;

            if (_price == price) {
                changeWithTheSamePrice(change, payUp);
            } else {
                _price = price;
                changeToDifferentPrice(payUp);
            }
        }
        else {
            if (_target != 0) {
                LOG(0, "Hedger {C[i1]} Side={i2}. Cancelling all.", getContractName(), _side)
                _target = 0;
                _traded = 0;
                _price = price;
                cancelAll();
            }
        }

        LOG(0, "Hedger {C[i1]} Side={i2}. After. Target={i3} Trade={i4}", getContractName(), _side, _target, _traded)
    }

    void cancelAll() {
        for (auto it = _orders.rbegin(); it != _orders.rend(); ++it) {
            om::order_t* order = *it;
            if (canChangeOrder(*order)) {
                cancel(order);
            }
        }
    }

    void changeWithTheSamePrice(hf::qty_t change, bool payUp) {
        if (change > 0) {
            createNewOrder(change, payUp);
        } else if (change < 0) {
            hf::qty_t needToReduce = -change;
            for (auto it = _orders.rbegin(); it != _orders.rend(); ++it) {
                om::order_t* order = *it;
                if (canChangeOrder(*order)) {
                    needToReduce -= order->leavesQty;
                    cancel(order);
                }
                if (needToReduce <= 0) {
                    break;
                }
            }
            if (needToReduce != 0) {
                createNewOrder(-needToReduce, payUp);
            }
        }
    }

    hf::qty_t getOpenSize() {
        return  _target - _traded;
    }

    void changeToDifferentPrice(bool payUp) {
        const hf::qty_t openSize = getOpenSize();
        const std::size_t numberOfOrders = _orders.size();
        createNewOrder(openSize, payUp);
        for (std::size_t i = 0; i < numberOfOrders; ++i) {
            om::order_t* order = _orders[i];
            if (canChangeOrder(*order)) {
                cancel(order);
            }
        }
    }

    void createNewOrder(hf::qty_t orderSize, bool payUp) {
        if (orderSize > 0) {
            om::order_t* order = newOrder(_price, orderSize);
            if (order == nullptr) {
                _logger.logError(boost::str(boost::format("Hedger could not create order.")));
                _ime->_executor->onCriticalError("Hedger could not create order.");
            } else {
                order->debug.i[PAY_UP] = payUp ? PAY_UP_YES : PAY_UP_NO;
                if (!payUp) {
                    _orders.push_back(order);
                }
            }
        }
    }

    void removeOrder(const om::order_t& order) {
        for (auto it = _orders.begin(); it != _orders.end(); ++it) {
            if ((*it)->oid == order.oid) {
                _orders.erase(it);
                break;
            }
        }
    }

    void returnOrderToHedgeManager(const om::order_t& order) {
        om::order_t& mutableOrder = const_cast<om::order_t&>(order);
        mutableOrder.debug.i[PAY_UP] = PAY_UP_NO;
        _orders.push_back(&mutableOrder);
    }

    void onOrderAccepted(const om::OMOrderAcceptedEvent& event, const om::order_t& order) {
        LOG(0, "Hedger {C[i1]} Side={i2}. Order Accepted. Id={i3} Price={d1} Size={i4}", getContractName(), _side, order.oid, ToDollars(_price), event.acceptedQty)
        om::order_t& mutableOrder = const_cast<om::order_t&>(order);
        mutableOrder.leavesQty = event.acceptedQty;
        if (order.debug.i[PAY_UP] == PAY_UP_YES) {
            _ime->_executor->addCheckHedgeQuoteTimer(_internalId, order.oid);
        }
    }

    void onOrderExecution(const om::OMExecutionEvent &event, const om::order_t &order) {
        LOG(0, "Hedger {C[i1]} Side={i2}. Fill. LevelPrice={d1}. Price={d2} Size={i3}", getContractName(), _side, ToDollars(_price), ToDollars(event.filledPrice), event.filledQty)
        LOG(0, "Hedger {C[i1]} Side={i2}. Before. Target={i3} Traded={i4}", getContractName(), _side, _target, _traded)
        _traded += event.filledQty;
        if (!order.is_active) {
            removeOrder(order);
        } else {
            om::order_t& mutableOrder = const_cast<om::order_t&>(order);
            mutableOrder.leavesQty = order.qty - event.totalExecutedQty;
        }
        LOG(0, "Hedger {C[i1]} Side={i2}. After. Target={i3} Trade={i4}", getContractName(), _side, _target, _traded)
    }

    void onOrderCanceled(const om::OMOrderCanceledEvent& event, const om::order_t& order) {
        LOG(0, "Hedger {C[i1]} Side={i2}. Order Cancelled. Id={i3}", getContractName(), _side, order.oid)
        LOG(0, "Hedger {C[i1]} Side={i2}. Before. Target={i3} Traded={i4}", getContractName(), _side, _target, _traded)
        removeOrder(order);
        LOG(0, "Hedger {C[i1]} Side={i2}. After. Target={i3} Trade={i4}", getContractName(), _side, _target, _traded)
    }

    void onOrderRejected(const om::OMOrderRejectedEvent& event, const om::order_t& order) {
        std::string message = boost::str(boost::format("Hedger %1% . New order rejected. Id=%2% Reason=%3% ReasonText=%4%") % _name % order.oid % event.reasonCodeExt % event.reasonText);
        _logger.logError(message);
        _ime->_executor->onCriticalError(message);
    }

    bool wasRejectedDueToBeingFilled(uint32_t rejectCode, hf::VENUE venue) {
        return rejectCode == ice_rejects::TOO_LATE_TO_CANCEL || rejectCode == ice_rejects::UNKNOWN_ORDER || rejectCode == om::toUType(om::OrderRejectReason::OrderNotActive);
    }

    void onOrderCancelRejected(const om::OMOrderCancelRejectedEvent &event, const om::order_t &order) {
        std::string message = boost::str(boost::format("Hedger %1% . Cancel order rejected. Id=%2% Reason=%3% ReasonText=%4%") % _name % order.oid % event.reasonCodeExt % event.reasonText);
        LOG(0, "Hedger {C[i1]} Side={i2}. Before. Target={i3} Traded={i4}", getContractName(), _side, _target, _traded)
        bool rejectedDueToFill = wasRejectedDueToBeingFilled(event.reasonCodeExt, order.venue);
        if (!rejectedDueToFill) {
            _logger.logError(message);
            _ime->_executor->onCriticalError(message);
        }
        LOG(0, "Hedger {C[i1]} Side={i2}. After. Target={i3} Trade={i4}", getContractName(), _side, _target, _traded)
    }

    void setPending(hf::price_t price, hf::qty_t size, bool payUp) {
        LOG(0, "Hedger {C[i1]} Side={i2}. Setting pending to. Price={d1} Size={i3} PayUp={4}", getContractName(), _side, ToDollars(price), size, payUp)
        _pendingPrice = price;
        _pendingSize = size;
        _pendingPayUp = payUp;
    }

    void applyPendingUpdate() {
        LOG(0, "Hedger {C[i1]} Side={i2}. Applying pending", getContractName(), _side)
        hedge(_pendingPrice, _pendingSize, _pendingPayUp);
        _pendingPrice = hf::NULL_PRICE;
        _pendingSize = 0;
        _pendingPayUp = false;
    }

    void cancel(om::order_t* order) {
        LOG(0, "Hedger {C[i1]} Side={i2}. Cancel order. Id={i3}", getContractName(), _side, order->oid)
        _ime->_executor->cancelOrder(_internalId, order);
    }

    om::order_t* newOrder(hf::price_t price, hf::qty_t size) {
        om::order_t* order = _ime->_executor->newLimitOrder(_internalId, _side, price, size);
        LOG(0, "Hedger {C[i1]} Side={i2}. New order. Id={i3} Price={d1} Size={i4}", getContractName(), _side, order->oid, ToDollars(price), size)
        order->debug.i[ORDER_OWNER] = HEDGER_ORDER;
        return order;
    }

    void getOutOfMarket() {
        if (_target != 0) {
            hedge(_price, 0, false);
        }
    }

#ifdef IS_DEV
    std::string getContractName() {
        return _name;
    }
#else
    uint32_t getContractName() {
            return _internalId;
        }
#endif

    uint32_t _internalId;
    uint32_t _side;

    hf::price_t _pendingPrice = hf::NULL_PRICE;
    hf::qty_t _pendingSize = 0;
    bool _pendingPayUp = false;

    hf::price_t _price = hf::NULL_PRICE;
    hf::qty_t _target = 0;
    hf::qty_t _traded = 0;

    std::vector<om::order_t*> _orders;

    ExecutorT* _ime;
    std::string _name;
    Logger _logger;
};

}

#endif //VIPER_HEDGES_MANAGER_H