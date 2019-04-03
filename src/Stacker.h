#ifndef VIPER_STACKER_H
#define VIPER_STACKER_H

#include <md/md.h>
#include <om/om.h>
#include <om/om_event.h>
#include <om/om_reject_codes.h>
#include "Common.h"
#include "ViperConfig.h"
#include "logging/Coolog.h"
#include "Queue.h"

namespace Python {

static const uint32_t NUMBER_OF_PRE_CREATED_LEVELS = 300;

inline void SetModifyTracker(const om::order_t& order, uint32_t modifyTrackerIndex) {
    om::order_t& mutableOrder = const_cast<om::order_t&>(order);
    mutableOrder.debug.i[MODIFY_TRACKER] = modifyTrackerIndex;
}

template <typename ExecutorT>
class Stacker {

public:

    struct ModifyTracker {

        void setNewOrder(hf::qty_t size) {
            _entries[_insertIndex] = size;
            _insertIndex = getNextIndex(_insertIndex);
        }

        hf::qty_t getOriginalQuantity() {
            return _entries[0];
        }

        void setQuantityToModify(hf::qty_t size) {
            _entries[_insertIndex] = size;
            _insertIndex = getNextIndex(_insertIndex);
        }

        void pop() {
            _popIndex = getNextIndex(_popIndex);
        }

        hf::qty_t getPriorQuantityToInsert() {
            uint32_t priorIndex = getPriorIndex(_insertIndex);
            return _entries[priorIndex];
        }

        hf::qty_t getPriorQuantityToPop() {
            uint32_t priorIndex = getPriorIndex(_popIndex);
            return _entries[priorIndex];
        }

        hf::qty_t getQuantityToPop() {
            return _entries[_popIndex];
        }

        uint32_t getPriorIndex(uint32_t current) {
            return current == 0 ? SIZE - 1 : current - 1;
        }

        uint32_t getNextIndex(uint32_t current) {
            return current == SIZE - 1 ? 0 : current + 1;
        }

        bool isModifying() {
            return _insertIndex != _popIndex;
        }

        void resetAcceptedQuantity() {
            uint32_t priorIndex = getPriorIndex(_popIndex);
            _entries[_popIndex] = _entries[priorIndex];
            pop();
        }

        static const uint32_t SIZE = 20;
        hf::qty_t _entries[SIZE];
        uint32_t _index;
        uint32_t _insertIndex = 0;
        uint32_t _popIndex = 1;
    };

    struct Level {

        Level(Stacker* stacker)
        :
            _stacker(stacker)
        {}

        ModifyTracker* getAvailableModifyTracker() {
            return _stacker->getAvailableModifyTracker();
        }

        ModifyTracker* getModifyTracker(uint32_t index) {
            return _stacker->getModifyTracker(index);
        }

        ModifyTracker* getModifyTracker(const om::order_t& order) {
            return getModifyTracker(static_cast<uint32_t>(order.debug.i[MODIFY_TRACKER]));
        }

        hf::qty_t getOriginalQuantity(const om::order_t& order) {
            switch (_stacker->_venue) {
                case hf::VENUE::ICE:
                    return GetQuantity(order, PRIOR_QUANTITY);
                case hf::VENUE::CME:
                    return getModifyTracker(order)->getOriginalQuantity();
                default:
                    throw std::runtime_error("unrecognized venue");
            }
        }

        hf::qty_t getPriorQuantityToInsert(const om::order_t& order) {
            switch (_stacker->_venue) {
                case hf::VENUE::ICE:
                    return GetQuantity(order, PRIOR_QUANTITY);
                case hf::VENUE::CME:
                    return getModifyTracker(order)->getPriorQuantityToInsert();
                default:
                    throw std::runtime_error("unrecognized venue");
            }
        }

        hf::qty_t getPriorQuantityToPop(const om::order_t& order) {
            switch (_stacker->_venue) {
                case hf::VENUE::ICE:
                    return GetQuantity(order, PRIOR_QUANTITY);
                case hf::VENUE::CME:
                    return getModifyTracker(order)->getPriorQuantityToPop();
                default:
                    throw std::runtime_error("unrecognized venue");
            }
        }

        hf::qty_t getQuantityToPop(const om::order_t& order) {
            switch (_stacker->_venue) {
                case hf::VENUE::ICE:
                    return GetQuantity(order, QUANTITY_TO_MODIFY);
                case hf::VENUE::CME:
                    return getModifyTracker(order)->getQuantityToPop();
                default:
                    throw std::runtime_error("unrecognized venue");
            }
        }

        void setNewOrderSize(om::order_t* order, hf::qty_t size) {
            switch (_stacker->_venue) {
                case hf::VENUE::ICE: {
                    SetQuantity(*order, size, PRIOR_QUANTITY);
                    break;
                }
                case hf::VENUE::CME: {
                    ModifyTracker* tracker = getAvailableModifyTracker();
                    tracker->setNewOrder(size);
                    SetModifyTracker(*order, tracker->_index);
                    break;
                }
                default:
                    throw std::runtime_error("unrecognized venue");
            }
        }

        void setQuantityToModify(om::order_t* order, hf::qty_t size) {
            switch (_stacker->_venue) {
                case hf::VENUE::ICE:
                    SetQuantity(*order, size, QUANTITY_TO_MODIFY);
                    break;
                case hf::VENUE::CME:
                    getModifyTracker(*order)->setQuantityToModify(size);
                    break;
                default:
                    throw std::runtime_error("unrecognized venue");
            }
        }

        void setAcceptedModifyQuantity(const om::order_t& order, hf::qty_t size) {
            switch (_stacker->_venue) {
                case hf::VENUE::ICE:
                    SetQuantity(order, size, PRIOR_QUANTITY);
                    break;
                case hf::VENUE::CME:
                    getModifyTracker(order)->pop();
                    break;
                default:
                    throw std::runtime_error("unrecognized venue");
            }
        }

        bool canModifyOrder(const om::order_t& order) {
            switch (_stacker->_venue) {
                case hf::VENUE::ICE:
                    return !order.is_modifying & order.is_active & !order.is_cancelling;
                case hf::VENUE::CME:
                    return (GetQuantity(order, IS_ACKED) == 1) & order.is_active & !order.is_cancelling;
                default:
                    throw std::runtime_error("unknown venue");
            }
        }

        hf::qty_t getLeavesQty(const om::order_t& order) {
            switch (_stacker->_venue) {
                case hf::VENUE::ICE:
                    return order.leavesQty;
                case hf::VENUE::CME:
                    return getModifyTracker(order)->getPriorQuantityToInsert() - order.executedQty;
                default:
                    throw std::runtime_error("unknown venue");
            }
        }

        bool isModifying(const om::order_t& order) {
            switch (_stacker->_venue) {
                case hf::VENUE::ICE:
                    return order.is_modifying;
                case hf::VENUE::CME:
                    return getModifyTracker(order)->isModifying();
                default:
                    throw std::runtime_error("unknown venue");
            }
        }

        void resetAcceptedQuantity(const om::order_t& order) {
            switch (_stacker->_venue) {
                case hf::VENUE::ICE:
                    break;
                case hf::VENUE::CME:
                    return getModifyTracker(order)->resetAcceptedQuantity();
                default:
                    throw std::runtime_error("unknown venue");
            }
        }

#ifdef IS_DEV
        std::string getContractName() {
            return _stacker->_name;
        }
#else
        uint32_t getContractName() {
            return _stacker->_internalId;
        }
#endif

        void updateOnlyOneLevel(hf::qty_t size, std::vector<const om::order_t*>& orders) {
            _target = size;
            hf::qty_t neededSize = size;
            for (auto it = orders.begin(); it != orders.end(); ) {
                om::order_t* order = const_cast<om::order_t*>(*it);
                if (neededSize == 0) {
                    break;
                }
                setGtcOrderProperties(order, this);
                setNewOrderSize(order, order->leavesQty);
                SetQuantity(*order, 1, IS_ACKED);
                _orders.push_back(order);
                _acknowledged += order->leavesQty;
                it = orders.erase(it);
                if (order->leavesQty <= neededSize) {
                    neededSize -= order->leavesQty;
                } else {
                    decreaseOrderSize(order, order->leavesQty - neededSize);
                    neededSize = 0;
                }
            }
            if (neededSize > 0) {
                createNewOrder(neededSize);
            }
        }

        bool updateOnlyOneLevel(hf::qty_t size) {
            LOG(0, "Stacker {C[i1]}. Updating level {d1} to size {i2}",  getContractName(), ToDollars(_price), size)
            LOG(0, "Stacker {C[i1]}. Before. Target={i2} Acked={i3} Flight={i4} Trade={i5}",  getContractName(), _target, _acknowledged, _flight, _traded)

            const bool wasZeroAndNowNo = (_target == 0) & (size > 0);

            if (!_usedByOtherStack) {
                if (size == 0) {
                    _target = 0;
                    if (isLevelFree()) {
                        LOG(0, "Stacker {C[i1]}. Setting size to 0 and has nothing to do. Just freeing level.", getContractName())
                        _acknowledged = 0;
                        _traded = 0;
                        notifyLevelFree();
                        LOG(0, "Stacker {C[i1]}. After. Target={i2} Acked={i3} Flight={i4} Trade={i5}", getContractName(), _target, _acknowledged, _flight,_traded)
                        return true;
                    }
                }

                if (isLevelFree()) {
                    notifyLevelUsed();
                }
            }

            _acknowledged -= _traded;
            _traded = 0;
            _target = size;

            const hf::qty_t change = getQuantityChange();

            if (!_usedByOtherStack) {
                synchronizeOrders(change);
            } else {
                LOG(0, "Stacker {C[i1]}. Cannot synchronize orders since level used by other side.", getContractName())
            }

            LOG(0, "Stacker {C[i1]}. After. Target={i2} Acked={i3} Flight={i4} Trade={i5}", getContractName(), _target, _acknowledged, _flight, _traded)

            return (change > 0) | wasZeroAndNowNo;
        }

        void notifyLevelUsed() {
            LOG(0, "Stacker {C[i1]}. Side={i2} Notifying level used. Price={i3}", getContractName(), _stacker->_side, _price)
            _stacker->notifyLevelUsed(_price);
        }

        hf::qty_t getTotalOrdersQuanity() {
            return _acknowledged + _flight;
        };

        hf::qty_t getQuantityChange() {
            return _target - getTotalOrdersQuanity() - _traded;
        }

        void synchronizeOrders(hf::qty_t change) {
            if (change > 0) {
                increaseSize(change);
            } else if (change < 0) {
                reduceSize(-change);
            }
        }

        void synchronizeOrders() {
            const hf::qty_t change = getQuantityChange();
            synchronizeOrders(change);
        }

        void increaseSize(hf::qty_t change) {
            createNewOrder(change);
        }

        void reduceSize(hf::qty_t change) {
            LOG(0, "Stacker {C[i1]}. Reduce size by {i2}", getContractName(), change)
            hf::qty_t totalLevelSizeDecrease = change;
            for (auto it = _orders.rbegin(); it != _orders.rend(); ++it) {
                om::order_t* order = *it;
                if (canModifyOrder(*order)) {
                    totalLevelSizeDecrease = decreaseOrderSize(order, totalLevelSizeDecrease);
                    if (totalLevelSizeDecrease == 0) {
                        break;
                    }
                } else {
                    LOG(0, "Stacker {C[i1]}. Could not change order. Id={i2}", getContractName(), order->oid)
                }
            }
        }

        hf::qty_t decreaseOrderSize(om::order_t* order, hf::qty_t decreaseBy) {
            hf::qty_t leavesQty = getLeavesQty(*order);
            if (leavesQty == decreaseBy) {
                _flight -= decreaseBy;
                LOG(0, "Stacker {C[i1]}. After decrease. Target={i2} Acked={i3} Flight={i4} Trade={i5} OrderLeaves={i6}", getContractName(), _target, _acknowledged, _flight, _traded, leavesQty)
                _stacker->cancel(order);
                return 0;
            } else if (leavesQty < decreaseBy) {
                _flight -= leavesQty;
                _stacker->cancel(order);
                LOG(0, "Stacker {C[i1]}. After decrease. Target={i2} Acked={i3} Flight={i4} Trade={i5} OrderLeaves={i6}", getContractName(),_target, _acknowledged, _flight, _traded, leavesQty)
                return decreaseBy - leavesQty;
            } else {
                const hf::qty_t orderSize = getPriorQuantityToInsert(*order);
                const hf::qty_t qty = orderSize - decreaseBy;
                _flight -= decreaseBy;
                _stacker->modify(order, _price, qty);
                setQuantityToModify(order, qty);
                LOG(0, "Stacker {C[i1]}. After decrease. Target={i2} Acked={i3} Flight={i4} Trade={i5} OrderLeaves={i6}", getContractName(), _target, _acknowledged, _flight, _traded, leavesQty)
                return 0;
            }
        }

        void createNewOrder(hf::qty_t orderSize) {
            if (orderSize > 0) {
                om::order_t* order = _stacker->newOrder(this, _price, orderSize);
                if (order == nullptr) {
                    LOG(0, "Stacker {C[i1]}. Could not create order.", getContractName())
                    _stacker->onCriticalError("Stacker could not create order.");
                } else {
                    setNewOrderSize(order, orderSize);
                    order->leavesQty = orderSize;
                    SetQuantity(*order, 0, IS_ACKED);
                    _orders.push_back(order);
                    _flight += orderSize;
                }
            }
        }

        void setGtcOrderProperties(om::order_t* order, Level* level) {
            _stacker->setGtcOrderProperties(order, level);
        }

        void adjustAfterAck(om::order_t& order) {
            const hf::qty_t change = getQuantityChange();
            if (change > 0) {
                createNewOrder(change);
            } else if (change < 0) {
                if (canModifyOrder(order)) {
                    decreaseOrderSize(&order, std::abs(change));
                }
            }
        }

        void onOrderAccepted(const om::OMOrderAcceptedEvent& event, const om::order_t& order) {
            LOG(0, "Stacker {C[i1]}. Order Accepted. Id={i2} Price={d1} Size={i3}", getContractName(), order.oid, ToDollars(_price), event.acceptedQty)
            LOG(0, "Stacker {C[i1]}. Before. Target={i2} Acked={i3} Flight={i4} Trade={i5}",  getContractName(), _target, _acknowledged, _flight, _traded)
            SetQuantity(order, 1, IS_ACKED);
            moveQuantity(event.acceptedQty);
            adjustAfterAck(const_cast<om::order_t&>(order));
            LOG(0, "Stacker {C[i1]}. After. Target={i2} Acked={i3} Flight={i4} Trade={i5}", getContractName(), _target, _acknowledged, _flight, _traded)
        }

        void onOrderReplaced(const om::OMOrderModifiedEvent& event, const om::order_t& order) {
            LOG(0, "Stacker {C[i1]}. Order Replaced. Id={i2} Price={d1} Size={i3}", getContractName(), order.oid, ToDollars(_price), event.acceptedQty)
            LOG(0, "Stacker {C[i1]}. Before. Target={i2} Acked={i3} Flight={i4} Trade={i5}",  getContractName(), _target, _acknowledged, _flight, _traded)
            om::order_t& mutableOrder = const_cast<om::order_t&>(order);
            const hf::qty_t priorQty = getPriorQuantityToPop(order);
            const hf::qty_t diff = event.acceptedQty - priorQty;
            moveQuantity(diff);
            setAcceptedModifyQuantity(order, event.acceptedQty);
            mutableOrder.is_modifying = false;
            mutableOrder.leavesQty = event.acceptedQty - order.executedQty;
            adjustAfterAck(mutableOrder);
            notifyIfLevelFree();
            LOG(0, "Stacker {C[i1]}. After. Target={i2} Acked={i3} Flight={i4} Trade={i5}", getContractName(), _target, _acknowledged, _flight, _traded)
        }

        void onOrderExecution(const om::OMExecutionEvent &event, const om::order_t &order) {
            LOG(0, "Stacker {C[i1]}. Fill. LevelPrice={i2}. Price={d1} Size={i3} Id={i4}", getContractName(), ToDollars(_price), event.filledPrice, event.filledQty, order.oid)
            LOG(0, "Stacker {C[i1]}. Before. Target={i2} Acked={i3} Flight={i4} Trade={i5} Leaves={i6}",  getContractName(), _target, _acknowledged, _flight, _traded, order.leavesQty)
            _traded += event.filledQty;
            if (!order.is_active) {
                LOG(0, "Stacker {C[i1]}. Inflight fill.", getContractName())
                if (order.is_cancelling) {
                    _flight += order.leavesQty;
                    LOG(0, "Stacker {C[i1]}. Was cancelling. Adjusting flight by leaves", getContractName())
                }
                else if (isModifying(order)) {
                    const hf::qty_t priorQty = getPriorQuantityToPop(order);
                    const hf::qty_t modifyTo = _stacker->_venue == hf::VENUE::ICE ? getQuantityToPop(order) : getPriorQuantityToInsert(order);
                    const hf::qty_t diff = modifyTo - priorQty;
                    _flight -= diff;
                    LOG(0, "Stacker {C[i1]}. Was modifying.", getContractName())
                } else {
                    LOG(0, "Stacker {C[i1]}. Neither cancelling nor modifying.", getContractName())
                }
                removeOrder(order);
                notifyIfLevelFree();
            } else {
                if (order.is_cancelling) {
                    _flight += event.filledQty;
                }
            }
            om::order_t& mutableOrder = const_cast<om::order_t&>(order);
            mutableOrder.leavesQty = getPriorQuantityToPop(order) - event.totalExecutedQty;
            LOG(0, "Stacker {C[i1]}. After. Target={i2} Acked={i3} Flight={i4} Trade={i5} Leaves={i6}", getContractName(), _target, _acknowledged, _flight, _traded, order.leavesQty)
        }

        void onOrderCanceled(const om::OMOrderCanceledEvent& event, const om::order_t& order) {
            LOG(0, "Stacker {C[i1]}. Order Cancelled. Id={i2}", getContractName(), order.oid)
            LOG(0, "Stacker {C[i1]}. Before. Target={i2} Acked={i3} Flight={i4} Trade={i5}",  getContractName(), _target, _acknowledged, _flight, _traded)
            removeOrder(order);
            moveQuantity(-order.leavesQty);
            notifyIfLevelFree();
            LOG(0, "Stacker {C[i1]}. After. Target={i2} Acked={i3} Flight={i4} Trade={i5}", getContractName(), _target, _acknowledged, _flight, _traded)
        }

        void onOrderRejected(const om::OMOrderRejectedEvent& event, const om::order_t& order) {
            LOG(0, "Stacker {C[i1]}. New order rejected. Id={i2} Reason={i3}", getContractName(), order.oid, event.reasonCodeExt)
            LOG(0, "Stacker {C[i1]}. Before. Target={i2} Acked={i3} Flight={i4} Trade={i5}",  getContractName(), _target, _acknowledged, _flight, _traded)

            hf::qty_t diff = isModifying(order) ? getQuantityToPop(order) : getOriginalQuantity(order);
            _flight -= diff;
            removeOrder(order);

            LOG(0, "Stacker {C[i1]}. After. Target={i2} Acked={i3} Flight={i4} Trade={i5}", getContractName(), _target, _acknowledged, _flight, _traded)

            _stacker->handleNewOrderReject(order.pr, GetGatewayRejectCodeString(event.reasonCodeExt));
        }

        void onOrderUnsolicitedSMPCancel(const om::OMOrderCanceledEvent& event, const om::order_t& order) {
            LOG(0, "Stacker {C[i1]}. Order Unsolicited SMP Cancel. Id={i2}", getContractName(), order.oid);
            LOG(0, "Stacker {C[i1]}. Before. Target={i2} Acked={i3} Flight={i4} Trade={i5}",  getContractName(), _target, _acknowledged, _flight, _traded);
            removeOrder(order);
            moveQuantity(-order.leavesQty);
            notifyIfLevelFree();

            LOG(0, "Stacker {C[i1]}. After. Target={i2} Acked={i3} Flight={i4} Trade={i5}", getContractName(), _target, _acknowledged, _flight, _traded);
            _stacker->handleUnsolicitedSMPCancel(order.pr, GetGatewayRejectCodeString(event.reasonCodeExt));
        }

        void onOrderReplaceRejected(const om::OMOrderModifyRejectedEvent& event, const om::order_t& order) {
            LOG(0, "Stacker {C[i1]}. Replace order rejected. Id={i2} Reason={i3}", getContractName(), order.oid, event.reasonCodeExt)
            LOG(0, "Stacker {C[i1]}. Before. Target={i2} Acked={i3} Flight={i4} Trade={i5}",  getContractName(), _target, _acknowledged, _flight, _traded)
            om::order_t& mutableOrder = const_cast<om::order_t&>(order);
            mutableOrder.is_modifying = false;
            bool rejectedDueToFill = WasRejectedDueToBeingFilled(event.reasonCodeExt, order.venue);
            if (!rejectedDueToFill) {
                if (hasOrder(order)) {
                    _stacker->disableMarketMakerAfterStackerRejects("Stacker-Modify");
                }
            } else {
                if (order.is_active) {
                    resetAcceptedQuantity(order);
                    const hf::qty_t priorQty = getPriorQuantityToPop(order);
                    const hf::qty_t diff = getQuantityToPop(order) - priorQty;
                    moveQuantity(diff);
                }
                synchronizeOrders();
                notifyIfLevelFree();
            }
            LOG(0, "Stacker {C[i1]}. After. Target={i2} Acked={i3} Flight={i4} Trade={i5}", getContractName(), _target, _acknowledged, _flight, _traded)
        }

        void onOrderCancelRejected(const om::OMOrderCancelRejectedEvent &event, const om::order_t &order) {
            LOG(0, "Stacker {C[i1]}. Cancel order rejected. Id={i2} Reason={i3}", getContractName(), order.oid, event.reasonCodeExt)
            LOG(0, "Stacker {C[i1]}. Before. Target={i2} Acked={i3} Flight={i4} Trade={i5}",  getContractName(), _target, _acknowledged, _flight, _traded)
            bool rejectedDueToFill = WasRejectedDueToBeingFilled(event.reasonCodeExt, order.venue);
            if (!rejectedDueToFill) {
                if (hasOrder(order)) {
                    _stacker->onCriticalError("Cancel reject not due to fill and has order.");
                }
            }
            om::order_t& mutableOrder = const_cast<om::order_t&>(order);
            mutableOrder.is_cancelling = false;
            LOG(0, "Stacker {C[i1]}. After. Target={i2} Acked={i3} Flight={i4} Trade={i5}", getContractName(), _target, _acknowledged, _flight, _traded)
        }

        bool isLevelFree() {
            return (_target == 0) & ((_acknowledged - _traded) == 0) & (_flight == 0) & _orders.empty();
        }

        void notifyIfLevelFree() {
            if (isLevelFree()) {
                notifyLevelFree();
            }
        }

        void notifyLevelFree() {
            LOG(0, "Stacker {C[i1]}. Side={i2} Notifying level free. Price={d1}", getContractName(), _stacker->_side, ToDollars(_price))
            _stacker->notifyLevelFree(_price);
        }

        void onLevelFree() {
            _usedByOtherStack = false;
            LOG(0, "Stacker {C[i1]}. Side={i2} Level freed. Price={d1}", getContractName(), _stacker->_side, ToDollars(_price))
            if (_target > 0) {
                LOG(0, "Stacker {C[i1]}. Before. Target={i2} Acked={i3} Flight={i4} Trade={i5}",  getContractName(), _target, _acknowledged, _flight, _traded)
                synchronizeOrders();
                notifyLevelUsed();
                LOG(0, "Stacker {C[i1]}. After. Target={i2} Acked={i3} Flight={i4} Trade={i5}", getContractName(), _target, _acknowledged, _flight, _traded)
            }
        }

        void onLevelUsed() {
            BOOST_ASSERT(isLevelFree());
            _usedByOtherStack = true;
            LOG(0, "Stacker {C[i1]}. Side={i2} Level used. Price={d1}", getContractName(), _stacker->_side, ToDollars(_price))
        }

        void removeOrder(const om::order_t& order) {
            for (auto it = _orders.begin(); it != _orders.end(); ++it) {
                if ((*it)->oid == order.oid) {
                    _orders.erase(it);
                    break;
                }
            }
        }

        bool hasOrder(const om::order_t& order) {
            for (auto it = _orders.begin(); it != _orders.end(); ++it) {
                if ((*it)->oid == order.oid) {
                    return true;
                }
            }
            return false;
        }

        void moveQuantity(hf::qty_t qty) {
            _acknowledged += qty;
            _flight -= qty;
        }

        hf::price_t _price = hf::NULL_PRICE;

        hf::qty_t _target = 0;
        hf::qty_t _acknowledged = 0;
        hf::qty_t _flight = 0;
        hf::qty_t _traded = 0;

        bool  _usedByOtherStack = false;

        std::vector<om::order_t*> _orders;

        std::unordered_map<om::order_id_t, ModifyTracker*> _modifyTrackers;

        Stacker* _stacker;

        Level* before = nullptr;
        Level* after = nullptr;
    };

    void initialize(uint32_t internalId, uint32_t side, hf::price_t tickSize, hf::qty_t quoteSize, uint32_t numberOfLevels, ExecutorT* ime,
                    const std::string& contractName, hf::VENUE venue, uint32_t slackSize, bool useGTC) {
        _internalId = internalId;
        _side = side;
        _tickSize = tickSize;
        _quoteSize = quoteSize;
        _nLevels = numberOfLevels;
        _ime = ime;
        _name = contractName;
        _venue = venue;
        _maxSlackSize = slackSize;
        _orderType = useGTC ? om::ORDER_TYPE::GTC : om::ORDER_TYPE::LMT;
        initializeModifyTrackers();
        LOG(0, "Stacker {C[i1]}. Side={i2} GTC={i3}", getContractName(), 0, (int) useGTC)
    }

    template <uint32_t Side>
    void quote(hf::price_t price, hf::qty_t size, bool goThruAllLevels) {
        LOG(0, "Stacker {C[i1]}. Side={i2} Price={d1} Size={i3}", getContractName(), Side, ToDollars(price), size)
        if (_topLevel == nullptr) {
            createFirstStack<Side>(price, size);
        }
        else if (_topLevel->_price == price) {
            const bool updated = _topLevel->updateOnlyOneLevel(size);
            if (updated | goThruAllLevels) {
                if (_nLevels > 0) {
                    Level* level = _topLevel->after;
                    while ((level->updateOnlyOneLevel(_quoteSize) | goThruAllLevels) & (level != _bottomLevel)) {
                        level = level->after;
                    }
                }
            }
        }
        else if (IsPriceMovingAgainst<Side>(price, _topLevel->_price)) {
            Level* lastLevel = _topLevel;

            while (lastLevel->_price != price) {
                lastLevel->updateOnlyOneLevel(0);
                lastLevel = lastLevel->after;
            }

            _topLevel = lastLevel;
            _topLevel->updateOnlyOneLevel(size);

            Level* stackerLevel = _topLevel;
            for (uint32_t stackLevel = 0; stackLevel < _nLevels; ++stackLevel) {
                Level* nextLevel = stackerLevel->after;
                if (nextLevel == nullptr) {
                    nextLevel = new Level(this);
                    nextLevel->_price = GetPriorPrice<Side>(stackerLevel->_price, _tickSize);
                    nextLevel->before = stackerLevel;
                    stackerLevel->after = nextLevel;
                }
                nextLevel->updateOnlyOneLevel(_quoteSize);
                stackerLevel = nextLevel;
            }

            if (EqualOrBelow<Side>(stackerLevel->_price, _bottomLevel->_price)) {
                _bottomLevel = stackerLevel;
            }
        }
        else {
            Level* lastLevel = _topLevel;
            while ((lastLevel->before != nullptr) & (lastLevel->_price != price)) {
                lastLevel = lastLevel->before;
            }

            if (lastLevel->_price != price) {
                hf::price_t levelPrice = GetNextPrice<Side>(lastLevel->_price, _tickSize);
                for (; EqualOrBelow<Side>(levelPrice, price);) {
                    Level* level = new Level(this);
                    level->_price = levelPrice;
                    level->after = lastLevel;
                    lastLevel->before = level;
                    lastLevel = level;
                    levelPrice = GetNextPrice<Side>(levelPrice, _tickSize);
                }
            }

            _topLevel = lastLevel;

            _topLevel->updateOnlyOneLevel(size);

            LOG(0, "Stacker {C[i1]} Side={i2}. Will update {i3} levels.", getContractName(), _side, _nLevels)

            Level* stackerLevel = _topLevel;
            for (uint32_t stackLevel = 0; stackLevel < _nLevels; ++stackLevel) {
                stackerLevel = stackerLevel->after;
                stackerLevel->updateOnlyOneLevel(_quoteSize);
            }

            Level* newBottomLevel = stackerLevel;

            if (_maxSlackSize > 0) {
                uint32_t numberOfSlackLevels = 0;
                while (true) {
                    Level* nextLevel = stackerLevel->after;
                    if ((numberOfSlackLevels < _maxSlackSize) & (nextLevel->_target == _quoteSize)) {
                        ++numberOfSlackLevels;
                        stackerLevel = nextLevel;
                        if (stackerLevel == _bottomLevel) {
                            break;
                        }
                    } else {
                        break;
                    }
                }
                newBottomLevel = stackerLevel;

                if (stackerLevel != _bottomLevel) {
                    stackerLevel = newBottomLevel->after;
                    while (stackerLevel != _bottomLevel) {
                        stackerLevel->updateOnlyOneLevel(0);
                        stackerLevel = stackerLevel->after;
                    }
                    _bottomLevel->updateOnlyOneLevel(0);
                }

                _bottomLevel = newBottomLevel;
            } else {
                stackerLevel = newBottomLevel->after;

                while (stackerLevel != _bottomLevel) {
                    stackerLevel->updateOnlyOneLevel(0);
                    stackerLevel = stackerLevel->after;
                }
                _bottomLevel->updateOnlyOneLevel(0);

                _bottomLevel = newBottomLevel;
            }
        }
        _inMarket = true;
    }

    template <uint32_t Side>
    void createFirstStack(hf::price_t price, hf::qty_t size) {
        if (_stackerStart == nullptr) {
            const uint32_t totalNumberOfLevel = _nLevels + 2 * NUMBER_OF_PRE_CREATED_LEVELS + 1;

            hf::price_t levelPrice = price + _tickSize * NUMBER_OF_PRE_CREATED_LEVELS * (_side == BID ? 1 : -1);

            Level* level = new Level(this);
            level->_price = levelPrice;
            _stackerStart = level;

            for (uint32_t levelIndex = 1; levelIndex < totalNumberOfLevel; ++levelIndex) {
            bool setBottomLevel = false;
                Level* nextLevel = new Level(this);
                level->after = nextLevel;
                nextLevel->before = level;
                levelPrice = GetPriorPrice<Side>(levelPrice, _tickSize);
                nextLevel->_price = levelPrice;
                if (_usedPrices.count(levelPrice)) {
                    nextLevel->onLevelUsed();
                }
                if (levelIndex < NUMBER_OF_PRE_CREATED_LEVELS) {
                    // nothing just pre-created
                }
                if (levelIndex == NUMBER_OF_PRE_CREATED_LEVELS) {
                    _topLevel = nextLevel;
                    auto& gtc = getGTCs(nextLevel->_price);
                    _topLevel->updateOnlyOneLevel(size, gtc);
                    setBottomLevel = true;
                }
                if (levelIndex > NUMBER_OF_PRE_CREATED_LEVELS && levelIndex <= NUMBER_OF_PRE_CREATED_LEVELS + _nLevels) {
                    auto& gtc = getGTCs(nextLevel->_price);
                    nextLevel->updateOnlyOneLevel(_quoteSize, gtc);
                    setBottomLevel = true;
                }
                if (levelIndex > NUMBER_OF_PRE_CREATED_LEVELS + _nLevels && levelIndex <= NUMBER_OF_PRE_CREATED_LEVELS + _nLevels + _maxSlackSize) {
                    auto& gtc = getGTCs(nextLevel->_price);
                    bool hasGtc = !gtc.empty();
                    if (hasGtc) {
                        nextLevel->updateOnlyOneLevel(_quoteSize, gtc);
                    }
                    setBottomLevel = hasGtc;
                }
                if (levelIndex > NUMBER_OF_PRE_CREATED_LEVELS + _nLevels + _maxSlackSize) {
                    // nothing just pre-created
                }
                if (setBottomLevel) {
                    _bottomLevel = nextLevel;
                }
                level = nextLevel;
            }
        } else {
            BOOST_ASSERT(false);
        }
    }

    void cancel(om::order_t* order) {
        LOG(0, "Stacker {C[i1]}. Cancel order. Id={i2}", getContractName(), order->oid)
        _ime->_executor->cancelOrder(_internalId, order);
    }

    void modify(const om::order_t* order, hf::price_t price, hf::qty_t size) {
        LOG(0, "Stacker {C[i1]}. Modify order. Id={i2} Price={d1} Size={i3}", getContractName(), order->oid, ToDollars(price), size)
        _ime->_executor->modifyOrder(_internalId, order, price, size);
    }

    om::order_t* newOrder(Level* level, hf::price_t price, hf::qty_t size) {
        om::order_t* order = _ime->_executor->newLimitOrder(_internalId, _side, price, size, _orderType);
        LOG(0, "Stacker {C[i1]}. New order. Id={i2} Price={d1} Size={i3}", getContractName(), order->oid, ToDollars(price), size)
        setGtcOrderProperties(order, level);
        return order;
    }

    void setGtcOrderProperties(om::order_t* order, Level* level) {
        order->debug.i[LEVEL_POINTER] = reinterpret_cast<uint64_t>(level);
        order->debug.i[ORDER_OWNER] = STACKER_ORDER;
    }

    void onOrderAccepted(const om::OMOrderAcceptedEvent& event, const om::order_t& order) {
        Level* level = reinterpret_cast<Level*>(order.debug.i[LEVEL_POINTER]);
        level->onOrderAccepted(event, order);
    }

    void onOrderReplaced(const om::OMOrderModifiedEvent& event, const om::order_t& order) {
        Level* level = reinterpret_cast<Level*>(order.debug.i[LEVEL_POINTER]);
        level->onOrderReplaced(event, order);
    }

    void onOrderExecution(const om::OMExecutionEvent &event, const om::order_t &order) {
        Level* level = reinterpret_cast<Level*>(order.debug.i[LEVEL_POINTER]);
        level->onOrderExecution(event, order);
    }

    void onOrderCanceled(const om::OMOrderCanceledEvent& event, const om::order_t& order) {
        Level* level = reinterpret_cast<Level*>(order.debug.i[LEVEL_POINTER]);
        level->onOrderCanceled(event, order);
    }

    void onOrderRejected(const om::OMOrderRejectedEvent& event, const om::order_t& order) {
        Level* level = reinterpret_cast<Level*>(order.debug.i[LEVEL_POINTER]);
        level->onOrderRejected(event, order);
    }

    void onOrderUnsolicitedSMPCancel(const om::OMOrderCanceledEvent& event, const om::order_t& order) {
        Level* level = reinterpret_cast<Level*>(order.debug.i[LEVEL_POINTER]);
        level->onOrderUnsolicitedSMPCancel(event, order);
    }

    void onOrderReplaceRejected(const om::OMOrderModifyRejectedEvent& event, const om::order_t& order) {
        Level* level = reinterpret_cast<Level*>(order.debug.i[LEVEL_POINTER]);
        level->onOrderReplaceRejected(event, order);
    }

    void onOrderCancelRejected(const om::OMOrderCancelRejectedEvent &event, const om::order_t &order) {
        Level* level = reinterpret_cast<Level*>(order.debug.i[LEVEL_POINTER]);
        level->onOrderCancelRejected(event, order);
    }

    void onCriticalError(const std::string& message) {
        _ime->_executor->onCriticalError(message);
    }

    void getOutOfMarket(bool force = false) {
        if (_inMarket) {
            if (!useGTC() | force) {
                Level* level = _topLevel;
                while (level != _bottomLevel) {
                    level->updateOnlyOneLevel(0);
                    level = level->after;
                }
                _bottomLevel->updateOnlyOneLevel(0);
                LOG(0, "Stacker {C[i1]} Side={i2}. Got out of market.", getContractName(), _side)
                _inMarket = false;
            }
        }
    }

    hf::qty_t getRestingSize() {
        if (!_inMarket) {
            return 0;
        } else {
            return _topLevel->_target - _topLevel->_traded;
        }
    }

    hf::price_t getPrice() {
        if (!_inMarket) {
            return hf::NULL_PRICE;
        } else {
            return _topLevel->_price;
        }
    }

    void updateQuoteSize(hf::qty_t size, bool enabled) {
        _quoteSize = size;
        if (_nLevels > 0) {
            if (_topLevel != nullptr) {
                Level* level = _topLevel->after;
                while (level != _bottomLevel) {
                    if (enabled) {
                        level->updateOnlyOneLevel(_quoteSize);
                    } else {
                        level->_target = _quoteSize;
                    }
                    level = level->after;
                }
                if (enabled) {
                    _bottomLevel->updateOnlyOneLevel(_quoteSize);
                } else {
                    _bottomLevel->_target = _quoteSize;
                }
            }
        }
    }

    template <uint32_t Side>
    void updateMaxSlackSize(uint32_t slackSize) {
        LOG(0, "Stacker {C[i1]} Side={i2}. Update number slack size. N={i3}", getContractName(), _side, slackSize)
        if (slackSize < _maxSlackSize) {
            if (_topLevel != nullptr) {
                Level* stackerLevel = _topLevel;
                for (uint32_t n = 0; n < _nLevels; ++n) {
                    stackerLevel = stackerLevel->after;
                }
                Level* newBottom = stackerLevel;

                while (stackerLevel != _bottomLevel) {
                    stackerLevel->updateOnlyOneLevel(0);
                    stackerLevel = stackerLevel->after;
                }
                _bottomLevel->updateOnlyOneLevel(0);

                _bottomLevel = newBottom;
                LOG(0, "Stacker {C[i1]} Side={i2}. BottomPrice={d1}", getContractName(), _side, ToDollars(_bottomLevel->_price))
            }
        }
        _maxSlackSize = slackSize;
    }

    template <uint32_t Side>
    void updateNumberOfLevels(uint32_t nLevels, bool enabled) {
        LOG(0, "Stacker {C[i1]} Side={i2}. Update number of levels. N={i3}", getContractName(), _side, nLevels)
        if (nLevels != _nLevels) {
            if (_topLevel != nullptr) {
                Level* stackLevel = _topLevel;
                for (uint64_t i = 0; i < nLevels; ++i) {
                    stackLevel = stackLevel->after;
                    if (enabled) {
                        stackLevel->updateOnlyOneLevel(_quoteSize);
                    } else {
                        stackLevel->_target = _quoteSize;
                    }
                }
                if (nLevels > _nLevels) {
                    if (EqualOrBelow<Side>(stackLevel->_price, _bottomLevel->_price)) {
                        _bottomLevel = stackLevel;
                    }
                }
                else {
                    Level* newBottomLevel = stackLevel;

                    stackLevel = stackLevel->after;
                    while (stackLevel != _bottomLevel) {
                        if (enabled) {
                            stackLevel->updateOnlyOneLevel(0);
                        } else {
                            stackLevel->_target = 0;
                        }
                        stackLevel = stackLevel->after;
                    }
                    if (enabled) {
                        _bottomLevel->updateOnlyOneLevel(0);
                    } else {
                        _bottomLevel->_target = 0;
                    }

                    _bottomLevel = newBottomLevel;
                }
                LOG(0, "Stacker {C[i1]} Side={i2}. BottomPrice={d1}", getContractName(), _side, ToDollars(_bottomLevel->_price))
            }
            _nLevels = nLevels;
        }
    }

    void notifyLevelFree(hf::price_t price) {
        _ime->notifyLevelFree(_side, price);
    }

    void notifyLevelUsed(hf::price_t price) {
        _ime->notifyLevelUsed(_side, price);
    }

    void onLevelFree(hf::price_t price) {
        Level* level = _topLevel;
        if (level != nullptr) {
            const bool betterPrice = IsBetter(price, level->_price, _side);
            if (betterPrice) {
                while (level != nullptr && level->_price != price) {
                    level = level->before;
                }
            } else {
                while (level != nullptr && level->_price != price) {
                    level = level->after;
                }
            }
            if (level != nullptr) {
                level->onLevelFree();
            } else {
                LOG(0, "Stacker {C[i1]} Side={i2}. onLevelFree. Did not find level. Price={d1}", getContractName(), _side, ToDollars(price))
                BOOST_ASSERT(false);
            }
        } else {
            auto it = _usedPrices.find(price);
            if (it != _usedPrices.end()) {
                _usedPrices.erase(it);
            }
        }
    }

    void onLevelUsed(hf::price_t price) {
        Level* level = _topLevel;
        if (level != nullptr) {
            const bool betterPrice = IsBetter(price, level->_price, _side);
            if (betterPrice) {
                while (level != nullptr && level->_price != price) {
                    level = level->before;
                }
            } else {
                while (level != nullptr && level->_price != price) {
                    level = level->after;
                }
            }
            if (level != nullptr) {
                level->onLevelUsed();
            } else {
                LOG(0, "Stacker {C[i1]} Side={i2}. onLevelUsed. Did not find level. Price={d1}", getContractName(), _side, ToDollars(price))
                BOOST_ASSERT(false);
            }
        } else {
            LOG(0, "Stacker {C[i1]} Side={i2}. onLevelUsed. Does not have top level. Storing. Price={d1}", getContractName(), _side, ToDollars(price))
            _usedPrices.insert(price);
        }
    }

    ModifyTracker* getAvailableModifyTracker() {
        uint32_t index;
        const bool hasValue = _availableModifyTrackers.pop(index);
        if (hasValue) {
            return &_modifyTrackers[index];
        }
        throw std::runtime_error("does not have available modify tracker");
    }

    void returnModifyTracker(uint32_t index) {
        _availableModifyTrackers.push(index);
        ModifyTracker& tracker = _modifyTrackers[index];
        tracker._popIndex = 1;
        tracker._insertIndex = 0;
    }

    ModifyTracker* getModifyTracker(uint32_t index) {
        return &_modifyTrackers[index];
    }

    void initializeModifyTrackers() {
        if (_venue == hf::VENUE::CME) {
            for (uint32_t i = 0; i < NUMBER_OF_MODIFY_TRACKERS; ++i) {
                _availableModifyTrackers.push(i);
                _modifyTrackers[i]._index = i;
            }
        }
    }

    void disableMarketMakerAfterStackerRejects(const std::string& reason) {
        _ime->_executor->disableMarketMakerAfterStackerRejects(_internalId, reason);
    }

    void handleNewOrderReject(hf::price_t price, const std::string& reason) {
        _ime->handleStackerNewOrderReject(_side, price, reason);
    }

    void handleUnsolicitedSMPCancel(hf::price_t price, const std::string& reason) {
        _ime->handleStackerUnsolicitedSMPCancel(_side, price, reason);
    }

    std::vector<const om::order_t*>& getGTCs(hf::price_t price) {
        return _ime->getGTCs(price, _side);
    }

    bool useGTC() {
        return _orderType == om::ORDER_TYPE::GTC;
    }

    void updateUseGtc(bool useGtc) {
        _orderType = useGtc ? om::ORDER_TYPE::GTC : om::ORDER_TYPE::LMT;
        if (_inMarket) {
            hf::price_t price = _topLevel->_price;
            hf::qty_t size = _topLevel->_target;
            getOutOfMarket(true);
            if (_side == BID) {
                quote<BID>(price, size, true);
            } else {
                quote<ASK>(price, size, true);
            }
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

    std::set<hf::price_t> _usedPrices;

    uint32_t _internalId;
    uint32_t _side;
    hf::price_t _tickSize;
    hf::qty_t _quoteSize;
    uint32_t _nLevels;
    uint32_t _maxSlackSize;

    Level* _stackerStart = nullptr;
    Level* _topLevel = nullptr;
    Level* _bottomLevel = nullptr;
    bool _inMarket = false;

    static const uint32_t NUMBER_OF_MODIFY_TRACKERS = 90000;
    ModifyTracker _modifyTrackers[NUMBER_OF_MODIFY_TRACKERS];
    Queue<uint32_t, NUMBER_OF_MODIFY_TRACKERS> _availableModifyTrackers;

    hf::VENUE _venue;

    ExecutorT* _ime;
    std::string _name;

    om::ORDER_TYPE _orderType = om::ORDER_TYPE::LMT;
};

}

#endif //VIPER_STACKER_H
