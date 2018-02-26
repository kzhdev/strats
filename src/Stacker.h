#ifndef VIPER_STACKER_H
#define VIPER_STACKER_H

#include <md/md.h>
#include <om/om.h>
#include <om/om_event.h>
#include <om/om_reject_codes.h>
#include "Common.h"
#include "ViperConfig.h"

namespace Python {

static const uint32_t NUMBER_OF_PRE_CREATED_LEVELS = 100;

inline int64_t GetQuantity(const om::order_t& order, uint32_t key) {
    return  order.debug.i[key];
}

inline void SetQuantity(const om::order_t& order, int64_t qty, uint32_t key) {
    om::order_t& mutableOrder = const_cast<om::order_t&>(order);
    mutableOrder.debug.i[key] = qty;
}

static const uint32_t LEVEL_POINTER = 1;
static const uint32_t PRIOR_QUANTITY = 2;
static const uint32_t QUANTITY_TO_MODIFY = 3;
static const uint32_t IS_ACKED = 4;

template <typename ExecutorT>
class Stacker {

public:
    struct Level {

        Level(Stacker* stacker)
        :
            _stacker(stacker)
        {}

        bool canModifyOrder(const om::order_t& order) {
            return !order.is_modifying & isAcked(order) & order.is_active & !order.is_cancelling;
        }

        bool isAcked(const om::order_t& order) {
            return GetQuantity(order, IS_ACKED) == 1;
        }

        bool updateOnlyOneLevel(hf::qty_t size) {
            _stacker->_logger.logInfo(boost::str(boost::format("Stacker %1%. Updating level %2% to size %3%") % _stacker->_name % ToDollars(_price) % size));

            _stacker->_logger.logInfo(boost::str(boost::format("Stacker %1%. Before. Target=%2% Acked=%3% Flight=%4% Trade=%5%") % _stacker->_name % _target % _acknowledged % _flight % _traded));

            if (!_usedByOtherStack) {
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
                _stacker->_logger.logInfo(boost::str(boost::format("Stacker %1%. Cannot synchronize orders since level used by other side.") % _stacker->_name));
            }

            _stacker->_logger.logInfo(boost::str(boost::format("Stacker %1%. After. Target=%2% Acked=%3% Flight=%4% Trade=%5%") % _stacker->_name % _target % _acknowledged % _flight % _traded));

            return change > 0;
        }

        void notifyLevelUsed() {
            _stacker->_logger.logInfo(boost::str(boost::format("Stacker %1%. Side=%2% Notifying level used. Price=%3%") % _stacker->_name % GetSideString(_stacker->_side) % _price));
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
            _stacker->_logger.logInfo(boost::str(boost::format("Stacker %1%. Reduce size by %2%") % _stacker->_name % change));
            hf::qty_t totalLevelSizeDecrease = change;
            for (auto it = _orders.rbegin(); it != _orders.rend(); ++it) {
                om::order_t* order = *it;
                if (canModifyOrder(*order)) {
                    totalLevelSizeDecrease = decreaseOrderSize(order, totalLevelSizeDecrease);
                    if (totalLevelSizeDecrease == 0) {
                        break;
                    }
                } else {
                    _stacker->_logger.logInfo(boost::str(boost::format("Stacker %1%. Could not change order - in flight. Id=%2%") % _stacker->_name % order->oid));
                }
            }
        }

        hf::qty_t decreaseOrderSize(om::order_t* order, hf::qty_t decreaseBy) {
            if (order->leavesQty == decreaseBy) {
                _flight -= decreaseBy;
                _stacker->_logger.logInfo(boost::str(boost::format("Stacker %1%. After decrease. Target=%2% Acked=%3% Flight=%4% Trade=%5% OrderLeaves=%6%") % _stacker->_name % _target % _acknowledged % _flight % _traded % order->leavesQty));
                _stacker->cancel(order);
                return 0;
            } else if (order->leavesQty < decreaseBy) {
                _flight -= order->leavesQty;
                _stacker->cancel(order);
                _stacker->_logger.logInfo(boost::str(boost::format("Stacker %1%. After decrease. Target=%2% Acked=%3% Flight=%4% Trade=%5% OrderLeaves=%6%") % _stacker->_name % _target % _acknowledged % _flight % _traded % order->leavesQty));
                return decreaseBy - order->leavesQty;
            } else {
                const hf::qty_t orderSize = GetQuantity(*order, PRIOR_QUANTITY);
                const hf::qty_t qty = orderSize - decreaseBy;
                _flight -= decreaseBy;
                _stacker->modify(order, _price, qty);
                SetQuantity(*order, qty, QUANTITY_TO_MODIFY);
                _stacker->_logger.logInfo(boost::str(boost::format("Stacker %1%. After decrease. Target=%2% Acked=%3% Flight=%4% Trade=%5% OrderLeaves=%6%") % _stacker->_name % _target % _acknowledged % _flight % _traded % order->leavesQty));
                return 0;
            }
        }

        void createNewOrder(hf::qty_t size) {
            const hf::qty_t orderSize = getOrderSize(size);
            if (orderSize > 0) {
                om::order_t* order = _stacker->newOrder(this, _price, orderSize);
                if (order == nullptr) {
                    _stacker->_logger.logError(boost::str(boost::format("Stacker could not create order.")));
                    _stacker->onCriticalError("Stacker could not create order.");
                } else {
                    _orders.push_back(order);
                    _flight += orderSize;
                }
            }
        }

        hf::qty_t getOrderSize(hf::qty_t size) {
            return size;
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
            _stacker->_logger.logInfo(boost::str(boost::format("Stacker %1%. Order Accepted. Id=%2% Price=%3% Size=%4%") % _stacker->_name % order.oid % ToDollars(_price) % event.acceptedQty));
            _stacker->_logger.logInfo(boost::str(boost::format("Stacker %1%. Before. Target=%2% Acked=%3% Flight=%4% Trade=%5%") % _stacker->_name % _target % _acknowledged % _flight % _traded));
            SetQuantity(order, 1, IS_ACKED);
            moveQuantity(event.acceptedQty);
            SetQuantity(order, event.acceptedQty, PRIOR_QUANTITY);
            om::order_t& mutableOrder = const_cast<om::order_t&>(order);
            mutableOrder.leavesQty = event.acceptedQty;
            adjustAfterAck(const_cast<om::order_t&>(order));
            _stacker->_logger.logInfo(boost::str(boost::format("Stacker %1%. After. Target=%2% Acked=%3% Flight=%4% Trade=%5%") % _stacker->_name % _target % _acknowledged % _flight % _traded));
        }

        void onOrderReplaced(const om::OMOrderModifiedEvent& event, const om::order_t& order) {
            _stacker->_logger.logInfo(boost::str(boost::format("Stacker %1%. Order Replaced. Id=%2% Price=%3% Size=%4%") % _stacker->_name % order.oid % ToDollars(_price) % event.acceptedQty));
            _stacker->_logger.logInfo(boost::str(boost::format("Stacker %1%. Before. Target=%2% Acked=%3% Flight=%4% Trade=%5%") % _stacker->_name % _target % _acknowledged % _flight % _traded));
            om::order_t& mutableOrder = const_cast<om::order_t&>(order);
            const hf::qty_t priorQty = GetQuantity(order, PRIOR_QUANTITY);
            const hf::qty_t diff = event.acceptedQty - priorQty;
            moveQuantity(diff);
            SetQuantity(order, event.acceptedQty, PRIOR_QUANTITY);
            mutableOrder.is_modifying = false;
            mutableOrder.leavesQty = event.acceptedQty - order.executedQty;
            adjustAfterAck(mutableOrder);
            notifyIfLevelFree();
            _stacker->_logger.logInfo(boost::str(boost::format("Stacker %1%. After. Target=%2% Acked=%3% Flight=%4% Trade=%5%") % _stacker->_name % _target % _acknowledged % _flight % _traded));
        }

        void onOrderExecution(const om::OMExecutionEvent &event, const om::order_t &order) {
            _stacker->_logger.logInfo(boost::str(boost::format("Stacker %1%. Fill. LevelPrice=%2%. Price=%3% Size=%4%") % _stacker->_name % ToDollars(_price) % event.filledPrice % event.filledQty));
            _stacker->_logger.logInfo(boost::str(boost::format("Stacker %1%. Before. Target=%2% Acked=%3% Flight=%4% Trade=%5%") % _stacker->_name % _target % _acknowledged % _flight % _traded));
            _traded += event.filledQty;
            if (!order.is_active) {
                _stacker->_logger.logInfo(boost::str(boost::format("Stacker %1%. Inflight fill.") % _stacker->_name));
                if (order.is_cancelling) {
                    _flight += order.leavesQty;
                }
                else if (order.is_modifying) {
                    const hf::qty_t diff = GetQuantity(order, QUANTITY_TO_MODIFY) - GetQuantity(order, PRIOR_QUANTITY);
                    _flight += diff;
                }
                removeOrder(order);
                notifyIfLevelFree();
            } else {
                om::order_t& mutableOrder = const_cast<om::order_t&>(order);
                mutableOrder.leavesQty = GetQuantity(order, PRIOR_QUANTITY) - event.totalExecutedQty;
            }
            _stacker->_logger.logInfo(boost::str(boost::format("Stacker %1%. After. Target=%2% Acked=%3% Flight=%4% Trade=%5%") % _stacker->_name % _target % _acknowledged % _flight % _traded));
        }

        bool isLevelFree() {
            return (_target == 0) & (_acknowledged == 0) & (_flight == 0);
        }

        void notifyIfLevelFree() {
            if (isLevelFree()) {
                _stacker->_logger.logInfo(boost::str(boost::format("Stacker %1%. Side=%2% Notifying level free. Price=%3%") % _stacker->_name % GetSideString(_stacker->_side) % _price));
                _stacker->notifyLevelFree(_price);
            }
        }

        void onLevelFree() {
            _usedByOtherStack = false;
            _stacker->_logger.logInfo(boost::str(boost::format("Stacker %1%. Side=%2% Level freed. Price=%3%") % _stacker->_name % GetSideString(_stacker->_side) % _price));
            if (_target > 0) {
                synchronizeOrders();
            }
        }

        void onLevelUsed() {
            BOOST_ASSERT(isLevelFree());
            _usedByOtherStack = true;
            _stacker->_logger.logInfo(boost::str(boost::format("Stacker %1%. Side=%2% Level used. Price=%3%") % _stacker->_name % GetSideString(_stacker->_side) % _price));
        }

        void onOrderCanceled(const om::OMOrderCanceledEvent& event, const om::order_t& order) {
            _stacker->_logger.logInfo(boost::str(boost::format("Stacker %1%. Order Cancelled. Id=%2%") % _stacker->_name % order.oid));
            _stacker->_logger.logInfo(boost::str(boost::format("Stacker %1%. Before. Target=%2% Acked=%3% Flight=%4% Trade=%5%") % _stacker->_name % _target % _acknowledged % _flight % _traded));
            moveQuantity(-order.leavesQty);
            removeOrder(order);
            notifyIfLevelFree();
            _stacker->_logger.logInfo(boost::str(boost::format("Stacker %1%. After. Target=%2% Acked=%3% Flight=%4% Trade=%5%") % _stacker->_name % _target % _acknowledged % _flight % _traded));
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
            std::string message = boost::str(boost::format("Stacker %1% . New order rejected. Id=%2% Reason=%3% ReasonText=%4%") % _stacker->_name % order.oid % event.reasonCodeExt % event.reasonText);
            _stacker->_logger.logInfo(message);
            _stacker->onCriticalError(message);
        }

        void onOrderReplaceRejected(const om::OMOrderModifyRejectedEvent& event, const om::order_t& order) {
            std::string message = boost::str(boost::format("Stacker %1% . Replace order rejected. Id=%2% Reason=%3% ReasonText=%4%") % _stacker->_name % order.oid % event.reasonCodeExt % event.reasonText);
            _stacker->_logger.logInfo(boost::str(boost::format("Stacker %1%. Before. Target=%2% Acked=%3% Flight=%4% Trade=%5%") % _stacker->_name % _target % _acknowledged % _flight % _traded));
            bool rejectedDueToFill = wasRejectedDueToBeingFilled(event.reasonCodeExt, order.venue);
            if (!rejectedDueToFill) {
                _stacker->_logger.logInfo(message);
                _stacker->onCriticalError(message);
            }
            _stacker->_logger.logInfo(boost::str(boost::format("Stacker %1%. After. Target=%2% Acked=%3% Flight=%4% Trade=%5%") % _stacker->_name % _target % _acknowledged % _flight % _traded));

        }

        void onOrderCancelRejected(const om::OMOrderCancelRejectedEvent &event, const om::order_t &order) {
            std::string message = boost::str(boost::format("Stacker %1% . Cancel order rejected. Id=%2% Reason=%3% ReasonText=%4%") % _stacker->_name % order.oid % event.reasonCodeExt % event.reasonText);
            _stacker->_logger.logInfo(boost::str(boost::format("Stacker %1%. Before. Target=%2% Acked=%3% Flight=%4% Trade=%5%") % _stacker->_name % _target % _acknowledged % _flight % _traded));
            bool rejectedDueToFill = wasRejectedDueToBeingFilled(event.reasonCodeExt, order.venue);
            if (!rejectedDueToFill) {
                _stacker->_logger.logInfo(message);
                _stacker->onCriticalError(message);
            }
            _stacker->_logger.logInfo(boost::str(boost::format("Stacker %1%. After. Target=%2% Acked=%3% Flight=%4% Trade=%5%") % _stacker->_name % _target % _acknowledged % _flight % _traded));
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

        Stacker* _stacker;

        Level* before = nullptr;
        Level* after = nullptr;
    };

    void initialize(uint32_t internalId, uint32_t side, hf::price_t tickSize, hf::qty_t quoteSize, uint32_t numberOfLevels, ExecutorT* ime, const std::string& contractName) {
        _internalId = internalId;
        _side = side;
        _tickSize = tickSize;
        _quoteSize = quoteSize;
        _nLevels = numberOfLevels;
        _ime = ime;
        _name = contractName;
    }

    template <uint32_t Side>
    void quote(hf::price_t price, hf::qty_t size) {
        const constexpr char* sideString = Side == BID ? "BID" : "ASK";
        _logger.logInfo(boost::str(boost::format("Stacker %1%. Stack %2% Price=%3% Size=%4%") % _name % sideString % price % size));
        if (_topLevel == nullptr) {
            createFirstStack<Side>(price, size);
        }
        else if (_topLevel->_price == price) {
            const bool updated = _topLevel->updateOnlyOneLevel(size);
            if (updated) {
                Level* level = _topLevel->after;
                while (level->updateOnlyOneLevel(_quoteSize)) {
                    level = level->after;
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

            _bottomLevel = stackerLevel;
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

            _logger.logInfo(boost::str(boost::format("Stacker %1% %2%. Will update %3% levels.") % _name % GetSideString(_side) % _nLevels));

            Level* stackerLevel = _topLevel;
            for (uint32_t stackLevel = 0; stackLevel < _nLevels; ++stackLevel) {
                stackerLevel = stackerLevel->after;
                stackerLevel->updateOnlyOneLevel(_quoteSize);
            }

            Level* newBottomLevel = stackerLevel;

            stackerLevel = newBottomLevel->after;

            stackerLevel->updateOnlyOneLevel(0);

            while (stackerLevel != _bottomLevel) {
                stackerLevel->updateOnlyOneLevel(0);
                stackerLevel = stackerLevel->after;
            }
            _bottomLevel->updateOnlyOneLevel(0);

            _bottomLevel = newBottomLevel;
        }
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
                    _topLevel->updateOnlyOneLevel(size);
                }
                if (levelIndex > NUMBER_OF_PRE_CREATED_LEVELS && levelIndex <= NUMBER_OF_PRE_CREATED_LEVELS + _nLevels) {
                    nextLevel->updateOnlyOneLevel(_quoteSize);
                }
                if (levelIndex == NUMBER_OF_PRE_CREATED_LEVELS + _nLevels) {
                    _bottomLevel = nextLevel;
                }
                if (levelIndex > NUMBER_OF_PRE_CREATED_LEVELS + _nLevels) {
                    // nothing just pre-created
                }
                level = nextLevel;
            }
        } else {
            _logger.logInfo(boost::str(boost::format("Stacker %1% %2%. Repopulate after re-enable.") % _name % GetSideString(_side)));
            Level* level = _stackerStart;
            while (level != nullptr) {
                if (_usedPrices.count(level->_price)) {
                    level->onLevelUsed();
                }
                if (level->_price == price) {
                    _topLevel = level;
                    _topLevel->updateOnlyOneLevel(size);
                }
                else {
                    bool shouldStack;
                    hf::price_t bottomLevelPrice;
                    if (_side == BID) {
                        bottomLevelPrice = price - _tickSize * _nLevels;
                        shouldStack = (price > level->_price) && (level->_price >= bottomLevelPrice);
                    } else {
                        bottomLevelPrice = price + _tickSize * _nLevels;
                        shouldStack = (price < level->_price) && (level->_price <= bottomLevelPrice);
                    }
                    if (shouldStack) {
                        level->updateOnlyOneLevel(_quoteSize);
                    } else {
                        BOOST_ASSERT(level->_target == 0);
                    }
                    if (level->_price == bottomLevelPrice) {
                        _bottomLevel = level;
                    }
                }
                level = level->after;
            }
            _logger.logInfo(boost::str(boost::format("Stacker %1% %2%. Done repopulating.") % _name % GetSideString(_side)));
        }
    }

    void cancel(om::order_t* order) {
        _logger.logInfo(boost::str(boost::format("Stacker %1%. Cancel order. Id=%2%") % _name % order->oid));
        _ime->_executor->cancelOrder(_internalId, order);
    }

    void modify(const om::order_t* order, hf::price_t price, hf::qty_t size) {
        _logger.logInfo(boost::str(boost::format("Stacker %1%. Modify order. Id=%2% Price=%3% Size=%4%") % _name % order->oid % price % size));
        _ime->_executor->modifyOrder(_internalId, order, price, size);
    }

    om::order_t* newOrder(Level* level, hf::price_t price, hf::qty_t size) {
        om::order_t* order = _ime->_executor->newLimitOrder(_internalId, _side, price, size);
        _logger.logInfo(boost::str(boost::format("Stacker %1%. New order. Id=%2% Price=%3% Size=%4%") % _name % order->oid % price % size));
        order->debug.i[LEVEL_POINTER] = reinterpret_cast<uint64_t>(level);
        return order;
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

    void getOutOfMarket() {
        if (_topLevel != nullptr) {
            Level* level = _topLevel;
            while (level != _bottomLevel) {
                level->updateOnlyOneLevel(0);
                level = level->after;
            }
            _bottomLevel->updateOnlyOneLevel(0);
            _topLevel = nullptr;
            _logger.logInfo(boost::str(boost::format("Stacker %1% %2%. Got out of market.") % _name % GetSideString(_side)));
        }
    }

    hf::qty_t getRestingSize() {
        if (_topLevel == nullptr) {
            return 0;
        } else {
            return _topLevel->_target - _topLevel->_traded;
        }
    }

    hf::price_t getPrice() {
        if (_topLevel == nullptr) {
            return hf::NULL_PRICE;
        } else {
            return _topLevel->_price;
        }
    }

    void updateQuoteSize(hf::qty_t size, bool enabled) {
        _quoteSize = size;
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

    template <uint32_t Side>
    void updateNumberOfLevels(uint32_t nLevels, bool enabled) {
        if (nLevels != _nLevels) {
            if (_topLevel != nullptr) {
                if (nLevels > _nLevels) {
                    uint64_t nMissingLevels = nLevels - _nLevels;
                    Level* level = _bottomLevel;
                    for (uint64_t i = 0; i < nMissingLevels; ++i) {
                        if (level->after == nullptr) {
                            Level* newtLevel = new Level(this);
                            level->after = newtLevel;
                            newtLevel->before = level;
                            newtLevel->_price = GetPriorPrice<Side>(level->_price, _tickSize);
                        }
                        if (enabled) {
                            level->after->updateOnlyOneLevel(_quoteSize);
                        } else {
                            level->after->_target = _quoteSize;
                        }
                        level = level->after;
                    }
                    _bottomLevel = level;
                }
                else {
                    uint64_t nLevelsToRemove = _nLevels - nLevels;
                    Level* level = _bottomLevel;
                    for (uint64_t i = 0; i < nLevelsToRemove; ++i) {
                        if (enabled) {
                            level->updateOnlyOneLevel(0);
                        } else {
                            level->_target = 0;
                        }
                        level = level->before;
                    }
                    _bottomLevel = level;
                }
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
                _logger.logInfo(boost::str(boost::format("Stacker %1% %2%. onLevelFree. Did not find level. Price=%3%") % _name % GetSideString(_side) % price));
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
                _logger.logInfo(boost::str(boost::format("Stacker %1% %2%. onLevelUsed. Did not find level. Price=%3%") % _name % GetSideString(_side) % price));
                BOOST_ASSERT(false);
            }
        } else {
            _logger.logInfo(boost::str(boost::format("Stacker %1% %2%. onLevelUsed. Does not have top level. Storing. Price=%3%") % _name % GetSideString(_side) % price));
            _usedPrices.insert(price);
        }
    }

    bool wouldSelfMatch(hf::price_t price) {
        if (_topLevel == nullptr) {
            return false;
        }
        if (_side == BID) {
            if (price <= _topLevel->_price) {
                return true;
            }
        } else {
            if (price >= _topLevel->_price) {
                return true;
            }
        }
        Level* level = _topLevel->before;
        while (level != nullptr && level->_price != price) {
            level = level->before;
        }
        if (level == nullptr) {
            return false;
        }
        return !level->isLevelFree();
    }

    std::set<hf::price_t> _usedPrices;

    uint32_t _internalId;
    uint32_t _side;
    hf::price_t _tickSize;
    hf::qty_t _quoteSize;
    uint32_t _nLevels;

    Level* _stackerStart = nullptr;
    Level* _topLevel = nullptr;
    Level* _bottomLevel = nullptr;

    ExecutorT* _ime;
    std::string _name;

    Logger _logger;
};

}

#endif //VIPER_STACKER_H
