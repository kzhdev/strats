#ifndef VIPER_HEDGES_MANAGER_H
#define VIPER_HEDGES_MANAGER_H

#include "boost/format.hpp"
#include <md/md.h>
#include <om/om.h>
#include <om/om_event.h>
#include <om/om_reject_codes.h>
#include "Common.h"
#include "logging/Coolog.h"

namespace Python {

template <typename ExecutorT>
class HedgesManager {

public:
    void initialize(uint32_t internalId, uint32_t side, const std::string& name, ExecutorT* executor, hf::VENUE venue, bool useGtc) {
        _internalId = internalId;
        _side = side;
        _name = name;
        _ime = executor;
        _venue = venue;
        _orderType = useGtc ? om::ORDER_TYPE::GTC : om::ORDER_TYPE::LMT;
    }

    bool isOrderAlive(const om::order_t& order) {
        return order.is_active & !order.is_cancelling;
    }

    bool canCancel(const om::order_t& order) {
        switch (_venue) {
            case hf::VENUE::ICE:
                return isOrderAlive(order);
            case hf::VENUE::CME:
                return (GetQuantity(order, IS_ACKED) == 1) & isOrderAlive(order);
            default:
                throw std::runtime_error("unknown venue");
        }
    }

    void hedge(hf::price_t price, hf::qty_t size, bool payUp) {
        LOG(0, "Hedger {C[i1]} Side={i2}. Updating to price={d1} size={i3} payUp={i4}", getContractName(), _side, ToDollars(price), size, payUp)
        LOG(0, "Hedger {C[i1]} Side={i2}. Before. Target={i3} Traded={i4}", getContractName(), _side, _target, _traded)

        if (size != 0) {
            _target = size;
            _traded = 0;

            if (payUp) {
                if (_price != price) {
                    _price = price;
                    changeToDifferentPrice(payUp);
                } else {
                    turnQuoteOrdersToPayupOrders();
                }
            } else {
                if (_price == price) {
                    const hf::qty_t change = getNeededChange();
                    changeWithTheSamePrice(change, payUp);
                } else {
                    const bool firstOrders = _price == hf::NULL_PRICE;
                    if (BOOST_UNLIKELY(firstOrders)) {
                        _price = price;
                        createFirstOrders(payUp);
                    } else {
                        _price = price;
                        changeToDifferentPrice(payUp);
                    }
                }
            }

            if (payUp) {
                _target = 0;
                LOG(0, "Hedger {C[i1]} Side={i2}. Reset target to 0 since payup.", getContractName(), _side)
            }
        }
        else {
            if (!_orders.empty()) {
                LOG(0, "Hedger {C[i1]} Side={i2}. Cancelling all.", getContractName(), _side)
                _target = 0;
                _traded = 0;
                _price = price;
                cancelAll();
            } else {
                LOG(0, "Hedger {C[i1]} Side={i2}. Not cancelling since no orders.", getContractName(), _side)
            }
        }

        LOG(0, "Hedger {C[i1]} Side={i2}. After. Target={i3} Trade={i4}", getContractName(), _side, _target, _traded)
    }

    void turnQuoteOrdersToPayupOrders() {
        hf::qty_t neededSize = _target;
        for (auto it = _orders.begin(); it != _orders.end(); ) {
            om::order_t* order = *it;
            if (isOrderAlive(*order) & (order->pr == _price)) {
                if (order->leavesQty <= neededSize) {
                    turnOrderIntoPayUpOrder(order);
                    neededSize -= order->leavesQty;
                    it = _orders.erase(it);
                } else {
                    ++it;
                }
            } else {
                ++it;
            }
        }
        for (auto it = _orders.begin(); it != _orders.end(); ++it) {
            om::order_t* order = *it;
            if (canCancel(*order)) {
                cancel(order);
            }
        }
        if (neededSize > 0) {
            createNewOrder(neededSize, true);
        }
    }

    void turnOrderIntoPayUpOrder( om::order_t* order) {
        order->debug.i[PAY_UP] = PAY_UP_YES;
        order->debug.i[PAY_UP_INITIAL_EXECUTED] = order->executedQty;
        if (GetQuantity(*order, IS_ACKED) == 1) {
            _ime->_executor->addCheckHedgeQuoteTimer(_internalId, order->oid);
        }
        LOG(0, "Hedger {C[i1]} Side={i2}. Turned normal order into payup. Id={i3} Initial={i4}", getContractName(), _side, order->oid, order->executedQty)
    }

    void cancelAll() {
        for (auto it = _orders.rbegin(); it != _orders.rend(); ++it) {
            om::order_t* order = *it;
            if (canCancel(*order)) {
                cancel(order);
            } else {
                if (isOrderAlive(*order)) {
                    LOG(0, "Hedger {C[i1]} Side={i2}. Could not cancel - not acked. Id={i3}", getContractName(), _side, order->oid)
                }
            }
        }
    }

    void changeWithTheSamePrice(hf::qty_t change, bool payUp) {
        if (change > 0) {
            createNewOrder(change, payUp);
        } else if (change < 0) {
            hf::qty_t needToReduce = -change;
            reduceSize(needToReduce, payUp);
        }
    }

    void reduceSize(hf::qty_t needToReduce, bool payUp) {
        for (auto it = _orders.rbegin(); it != _orders.rend(); ++it) {
            om::order_t* order = *it;
            if (canCancel(*order)) {
                needToReduce -= order->leavesQty;
                cancel(order);
            } else {
                if (isOrderAlive(*order)) {
                    LOG(0, "Hedger {C[i1]} Side={i2}. Could not cancel - not acked. Id={i3}", getContractName(), _side, order->oid)
                }
            }
            if (needToReduce <= 0) {
                break;
            }
        }
        if (needToReduce < 0) {
            createNewOrder(-needToReduce, payUp);
        }
    }

    hf::qty_t getOpenSize() {
        return  _target - _traded;
    }

    void createFirstOrders(bool payUp) {
        hf::qty_t neededSize = getOpenSize();
        auto& gtcs = _ime->getGTCs(_price, _side);
        for (auto it = gtcs.begin(); it != gtcs.end(); ) {
            if (neededSize == 0) {
                break;
            }
            om::order_t* order = const_cast<om::order_t*>(*it);
            if (order->leavesQty <= neededSize) {
                neededSize -= order->leavesQty;
                setGtcProperties(order, order->leavesQty, payUp);
                SetQuantity(*order, 1, IS_ACKED);
                it = gtcs.erase(it);
            } else {
                ++it;
            }
        }
        if (neededSize > 0) {
            createNewOrder(neededSize, payUp);
        }
    }

    void changeToDifferentPrice(bool payUp) {
        const hf::qty_t openSize = getOpenSize();
        const std::size_t numberOfOrders = _orders.size();
        createNewOrder(openSize, payUp);
        for (std::size_t i = 0; i < numberOfOrders; ++i) {
            om::order_t* order = _orders[i];
            if (canCancel(*order)) {
                cancel(order);
            } else {
                if (isOrderAlive(*order)) {
                    LOG(0, "Hedger {C[i1]} Side={i2}. Could not cancel - not acked. Id={i3}", getContractName(), _side, order->oid)
                }
            }
        }
    }

    void createNewOrder(hf::qty_t orderSize, bool payUp) {
        if (orderSize > 0) {
            om::order_t* order = newOrder(_price, orderSize);
            if (order == nullptr) {
                logging::GetLogger().logError(boost::str(boost::format("Hedger could not create order.")));
                _ime->_executor->onCriticalError("Hedger could not create order.");
            } else {
                setGtcProperties(order, orderSize, payUp);
            }
        }
    }

    void setGtcProperties(om::order_t* order, hf::qty_t orderSize, bool payUp) {
        order->debug.i[PAY_UP] = payUp ? PAY_UP_YES : PAY_UP_NO;
        order->debug.i[PAY_UP_INITIAL_EXECUTED] = 0;
        order->debug.i[ORDER_OWNER] = HEDGER_ORDER;
        if (!payUp) {
            _orders.push_back(order);
            LOG(0, "Hedger {C[i1]} Side={i2}. Added order.", getContractName(), _side)
        } else {
            LOG(0, "Hedger {C[i1]} Side={i2}. Not adding order since payup.", getContractName(), _side)
        }
        order->leavesQty = orderSize;
        SetQuantity(*order, 0, IS_ACKED);
    }

    void removeOrder(const om::order_t& order) {
        for (auto it = _orders.begin(); it != _orders.end(); ++it) {
            if ((*it)->oid == order.oid) {
                _orders.erase(it);
                LOG(0, "Hedger {C[i1]} Side={i2} Id={i3}. Removed order.", getContractName(), _side, order.oid)
                break;
            }
        }
    }

    void returnOrderToHedgeManager(const om::order_t& order) {
        LOG(0, "Hedger {C[i1]} Side={i2}. Returning order to hedger. HedgerPrice={d1} Id={i3} OrderPrice={d2} Size={i4} Leaves={i5}", getContractName(), _side, ToDollars(_price), order.oid, ToDollars(order.pr), order.qty, order.leavesQty)
        LOG(0, "Hedger {C[i1]} Side={i2}. Before. Target={i3} Traded={i4}", getContractName(), _side, _target, _traded)
        if (isOrderAlive(order)) {
            om::order_t& mutableOrder = const_cast<om::order_t&>(order);
            mutableOrder.debug.i[PAY_UP] = PAY_UP_NO;
            if (order.pr == _price) {
                _orders.push_back(&mutableOrder);
                LOG(0, "Hedger {C[i1]} Side={i2} Order price and hedger price same. Added.", getContractName(), _side)
            } else {
                LOG(0, "Hedger {C[i1]} Side={i2} Order price and hedger price different. Cancelling.", getContractName(), _side)
                cancel(&order);
            }
        } else {
            LOG(0, "Hedger {C[i1]} Side={i2} Did not add order since not active or being cancelled.", getContractName(), _side)
        }
        LOG(0, "Hedger {C[i1]} Side={i2}. After. Target={i3} Trade={i4}", getContractName(), _side, _target, _traded)
    }

    void onOrderAccepted(const om::OMOrderAcceptedEvent& event, const om::order_t& order) {
        LOG(0, "Hedger {C[i1]} Side={i2}. Order Accepted. Id={i3} Price={d1} Size={i4}", getContractName(), _side, order.oid, ToDollars(_price), event.acceptedQty)
        SetQuantity(order, 1, IS_ACKED);
        if (order.debug.i[PAY_UP] == PAY_UP_YES) {
            _ime->_executor->addCheckHedgeQuoteTimer(_internalId, order.oid);
        } else {
            if (_venue == hf::VENUE::CME) {
                if (_target == 0) {
                    LOG(0, "Hedger {C[i1]} Side={i2}. Target 0 => cancelling.. Id={i3}", getContractName(), _side, order.oid)
                    cancel(&order);
                } else {
                    if (order.pr != _price) {
                        LOG(0, "Hedger {C[i1]} Side={i2}. Target price different => cancelling. Id={i3}", getContractName(), _side, order.oid)
                        cancel(&order);
                    } else {
                        hf::qty_t diff = getNeededChange();
                        changeWithTheSamePrice(diff, false);
                    }
                }
            }
        }
    }

    void onOrderExecution(const om::OMExecutionEvent &event, const om::order_t &order) {
        LOG(0, "Hedger {C[i1]} Side={i2}. Fill. LevelPrice={d1}. Price={d2} Size={i3} Id={i4}", getContractName(), _side, ToDollars(_price), ToDollars(event.filledPrice), event.filledQty, order.oid)
        LOG(0, "Hedger {C[i1]} Side={i2}. Before. Target={i3} Traded={i4}", getContractName(), _side, _target, _traded)
        if (order.debug.i[PAY_UP] != PAY_UP_YES) {
            _traded += event.filledQty;
        }
        if (!order.is_active) {
            removeOrder(order);
        } else {
            om::order_t& mutableOrder = const_cast<om::order_t&>(order);
            mutableOrder.leavesQty = order.qty - event.totalExecutedQty;
        }
        LOG(0, "Hedger {C[i1]} Side={i2}. After. Target={i3} Trade={i4}", getContractName(), _side, _target, _traded)
    }

    hf::qty_t getNeededChange() {
        hf::qty_t neededSize = _target - _traded;
        hf::qty_t hasSize = 0;
        for (auto myOrder : _orders) {
            if (isOrderAlive(*myOrder) & (myOrder->pr == _price)) {
                hasSize += myOrder->leavesQty;
            }
        }
        LOG(0, "Hedger {C[i1]} Side={i2}. Needed change. Target={i3} Traded={i4} Need={i5} Has={i6}", getContractName(), _side, _target, _traded, neededSize, hasSize)
        return neededSize - hasSize;
    }

    void onOrderCanceled(const om::OMOrderCanceledEvent& event, const om::order_t& order) {
        LOG(0, "Hedger {C[i1]} Side={i2}. Order Cancelled. Id={i3}", getContractName(), _side, order.oid)
        LOG(0, "Hedger {C[i1]} Side={i2}. Before. Target={i3} Traded={i4}", getContractName(), _side, _target, _traded)
        removeOrder(order);
        if (!order.is_cancelling) {
            if (_target != 0) {
                 hf::qty_t diff = getNeededChange();
                changeWithTheSamePrice(diff, false);
            }
        }
        LOG(0, "Hedger {C[i1]} Side={i2}. After. Target={i3} Trade={i4}", getContractName(), _side, _target, _traded)
    }

    void onOrderRejected(const om::OMOrderRejectedEvent& event, const om::order_t& order) {
        std::string message = boost::str(boost::format("Hedger %1% . New order rejected. Id=%2% Reason=%3% ReasonText=%4% Text=%5%") 
                                         % _name % order.oid % event.reasonCodeExt % event.reasonText % GetGatewayRejectCodeString(event.reasonCodeExt));
        const bool inPayUp = order.debug.i[PAY_UP] == PAY_UP_YES;
        if (inPayUp) {
            _ime->_executor->handlePayUpSelfMatch(_internalId, order, message);
        } else {
            logging::GetLogger().logError(message);
            _ime->_executor->disableMarketMakerAfterStackerRejects(_internalId, message);
        }
    }

    void onOrderCancelRejected(const om::OMOrderCancelRejectedEvent &event, const om::order_t &order) {
        std::string message = boost::str(boost::format("Hedger %1% . Cancel order rejected. Id=%2% Reason=%3% ReasonText=%4%") % _name % order.oid % event.reasonCodeExt % event.reasonText);
        LOG(0, "Hedger {C[i1]} Side={i2}. Before. Target={i3} Traded={i4}", getContractName(), _side, _target, _traded)
        bool rejectedDueToFill = WasRejectedDueToBeingFilled(event.reasonCodeExt, order.venue);
        if (!rejectedDueToFill) {
            logging::GetLogger().logError(message);
            _ime->_executor->onCriticalError(message);
        }
        LOG(0, "Hedger {C[i1]} Side={i2}. After. Target={i3} Trade={i4}", getContractName(), _side, _target, _traded)
    }

    void onOrderUnsolicitedSMPCancel(const om::OMOrderCanceledEvent& event, const om::order_t& order) {
        std::string message = boost::str(boost::format("Hedger %1% . Unsolicited SMP Cancel. Id=%2% Reason=%3% ReasonText=%4% Text=%5%")
                                         % _name % order.oid % event.reasonCodeExt % event.reasonText % GetGatewayRejectCodeString(event.reasonCodeExt));

        removeOrder(order);
        if (!order.is_cancelling) {
            if (_target != 0) {
                hf::qty_t diff = getNeededChange();
                changeWithTheSamePrice(diff, false);
            }
        }

        const bool inPayUp = order.debug.i[PAY_UP] == PAY_UP_YES;
        if (inPayUp) {
            _ime->_executor->handlePayUpSelfMatch(_internalId, order, message);
        } else {
            logging::GetLogger().logError(message);
            _ime->_executor->disableMarketMakerAfterStackerRejects(_internalId, message);
        }
    }

    void setPending(hf::price_t price, hf::qty_t size, bool payUp) {
        LOG(0, "Hedger {C[i1]} Side={i2}. Setting pending to. Price={d1} Size={i3} PayUp={4}", getContractName(), _side, ToDollars(price), size, payUp)
        _pendingPrice = price;
        _pendingSize = size;
        _pendingPayUp = payUp;
        _hasPending = true;
    }

    void applyPendingUpdate() {
        if (_hasPending) {
            LOG(0, "Hedger {C[i1]} Side={i2}. Applying pending", getContractName(), _side)
            hedge(_pendingPrice, _pendingSize, _pendingPayUp);
            _pendingPrice = hf::NULL_PRICE;
            _pendingSize = 0;
            _pendingPayUp = false;
            _hasPending = false;
        }
    }

    void cancel(const om::order_t* order) {
        LOG(0, "Hedger {C[i1]} Side={i2}. Cancel order. Id={i3}", getContractName(), _side, order->oid)
        _ime->_executor->cancelOrder(_internalId, order);
    }

    om::order_t* newOrder(hf::price_t price, hf::qty_t size) {
        om::order_t* order = _ime->_executor->newLimitOrder(_internalId, _side, price, size, _orderType, true);
        LOG(0, "Hedger {C[i1]} Side={i2}. New order. Id={i3} Price={d1} Size={i4}", getContractName(), _side, order->oid, ToDollars(price), size)
        return order;
    }

    void updateUseGtc(bool useGtc, bool onlyDelete) {
        LOG(0, "Hedger {C[i1]} Side={i2}. Update useGTC use={i3} onlyDelete={i4} size={i5} price={d1}", getContractName(), _side, (int) useGtc, (int) onlyDelete, _target, ToDollars(_price))
        hf::qty_t size = _target - _traded;
        _orderType = useGtc ? om::ORDER_TYPE::GTC : om::ORDER_TYPE::LMT;
        forceDeleteOrders(true);
        if (size != 0 && !onlyDelete) {
            hedge(_price, size, false);
        }
    }

    bool useGTC() {
        return _orderType == om::ORDER_TYPE::GTC;
    }

    void getOutOfMarket(bool force = false) {
        if (_target != 0) {
            if (!useGTC() | force) {
                hedge(_price, 0, false);
            }
        }
    }

    void forceDeleteOrders(bool force = false) {
        if (!useGTC() | force) {
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
    bool _hasPending = false;

    hf::price_t _price = hf::NULL_PRICE;
    hf::qty_t _target = 0;
    hf::qty_t _traded = 0;

    hf::VENUE _venue;
    om::ORDER_TYPE _orderType = om::ORDER_TYPE::LMT;

    std::vector<om::order_t*> _orders;

    ExecutorT* _ime;
    std::string _name;
};

}

#endif //VIPER_HEDGES_MANAGER_H