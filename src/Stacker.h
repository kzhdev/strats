#ifndef VIPER_STACKER_H
#define VIPER_STACKER_H

#include <md/md.h>
#include <om/om.h>
#include <om/om_event.h>
#include <om/om_reject_codes.h>
#include "Common.h"
#include "ViperConfig.h"

namespace Python {

static const uint32_t NUMBER_OF_PRE_CREATED_LEVELS = 5;

enum ice_rejects : uint8_t {
    TOO_LATE_TO_CANCEL = 0,
    UNKNOWN_ORDER = 1
};

template <typename ExecutorT>
class Stacker {

public:
    static const uint32_t LEVEL_POINTER = 1;
    static const uint32_t PRIOR_QUANTITY = 2;
    static const uint32_t QUANTITY_TO_MODIFY = 3;

    struct Level {

        Level(Stacker* stacker)
        :
            _stacker(stacker)
        {}

        bool canModifyOrder(const om::order_t& order) {
            return !order.is_modifying & order.is_acked & order.is_active & !order.is_cancelling;
        }

        void updateOnlyOneLevel(hf::qty_t size) {
            if (size != _target) {
                _acknowledged -= _traded;
                _stacker->_logger.logInfo(boost::str(boost::format("Stacker %1%. Updating level %2% to size %3%") % _stacker->_name % ToDollars(_price) % size));
                const hf::qty_t diff = size - _target + _traded;
                if (diff < 0) {
                    hf::qty_t totalLevelSizeDecrease = _target - size;
                    for (auto it = _orders.rbegin(); it != _orders.rend(); ++it) {
                        om::order_t* order = *it;
                        if (canModifyOrder(*order)) {
                            totalLevelSizeDecrease = decreaseOrderSize(order, totalLevelSizeDecrease);
                            if (totalLevelSizeDecrease == 0) {
                                break;
                            }
                        }
                    }
                } else if (diff > 0) {
                    createNewOrder(diff);
                }
                _traded = 0;
                _target = size;
            }
        }

        hf::qty_t decreaseOrderSize(om::order_t* order, hf::qty_t decreaseBy) {
            if (order->leavesQty == decreaseBy) {
                _flight -= decreaseBy;
                _stacker->cancel(order);
                return 0;
            } else if (order->leavesQty < decreaseBy){
                _flight -= order->qty;
                _stacker->cancel(order);
                return decreaseBy - order->leavesQty;
            } else {
                _flight -= decreaseBy;
                const hf::qty_t qty = order->qty - decreaseBy;
                _stacker->modify(order, _price, qty);
                setQuantity(*order, qty, QUANTITY_TO_MODIFY);
                return 0;
            }
        }

        void createNewOrder(hf::qty_t size) {
            const hf::qty_t orderSize = getOrderSize(size);
            if (orderSize > 0) {
                om::order_t* order = _stacker->newOrder(this, _price, orderSize);
                _orders.push_back(order);
                _flight += orderSize;
            }
        }

        hf::qty_t getOrderSize(hf::qty_t size) {
            return size;
        }

        int64_t getQuantity(const om::order_t& order, uint32_t key) {
            return  order.debug.i[key];
        }

        void setQuantity(const om::order_t& order, int64_t qty, uint32_t key) {
            om::order_t& mutableOrder = const_cast<om::order_t&>(order);
            mutableOrder.debug.i[key] = qty;
        }

        void adjustAfterAck(om::order_t& order) {
            const hf::qty_t diff = _target - _acknowledged - _flight;
            if (diff > 0) {
                createNewOrder(diff);
            } else if (diff < 0) {
                decreaseOrderSize(&order, std::abs(diff));
            }
        }

        void onOrderAccepted(const om::OMOrderAcceptedEvent& event, const om::order_t& order) {
            _acknowledged += event.acceptedQty;
            _flight -= event.acceptedQty;
            setQuantity(order, event.acceptedQty, PRIOR_QUANTITY);
            om::order_t& mutableOrder = const_cast<om::order_t&>(order);
            mutableOrder.leavesQty = event.acceptedQty;
            adjustAfterAck(const_cast<om::order_t&>(order));
        }

        void onOrderReplaced(const om::OMOrderModifiedEvent& event, const om::order_t& order) {
            om::order_t& mutableOrder = const_cast<om::order_t&>(order);
            const hf::qty_t priorQty = getQuantity(order, PRIOR_QUANTITY);
            const hf::qty_t diff = event.acceptedQty - priorQty;
            _acknowledged += diff;
            _flight -= diff;
            setQuantity(order, event.acceptedQty, PRIOR_QUANTITY);
            mutableOrder.is_modifying = false;
            mutableOrder.leavesQty = event.acceptedQty - order.executedQty;
            adjustAfterAck(mutableOrder);
        }

        void onOrderExecution(const om::OMExecutionEvent &event, const om::order_t &order) {
            _stacker->_logger.logInfo(boost::str(boost::format("Stacker %1%. Fill. LevelPrice=%2%. Price=%3% Size=%4%") % _stacker->_name % ToDollars(_price) % event.filledPrice % event.filledQty));
            _traded += event.filledQty;
            om::order_t& mutableOrder = const_cast<om::order_t&>(order);
            mutableOrder.leavesQty = getQuantity(order, PRIOR_QUANTITY) - event.totalExecutedQty;
            if (!order.is_active) {
                removeOrder(order);
            }
        }

        void onOrderCanceled(const om::OMOrderCanceledEvent& event, const om::order_t& order) {
            _acknowledged -= order.leavesQty;
            _flight += order.leavesQty;
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
            _stacker->_logger.logInfo(boost::str(boost::format("New order reject. Reason=%1% %2%") % event.reasonCodeExt % event.reasonText));
            _stacker->onCriticalError();
        }

        void onOrderReplaceRejected(const om::OMOrderModifyRejectedEvent& event, const om::order_t& order) {
            bool rejectedDueToFill = wasRejectedDueToBeingFilled(event.reasonCodeExt, order.venue);
            if (!rejectedDueToFill) {
                _stacker->_logger.logInfo(boost::str(boost::format("New order reject. Reason=%1% %2%") % event.reasonCodeExt % event.reasonText));
                _stacker->onCriticalError();
            } else {
                const hf::qty_t diff = getQuantity(order, QUANTITY_TO_MODIFY) - getQuantity(order, PRIOR_QUANTITY);
                _flight -= diff;
                _acknowledged += diff;
            }
        }

        void onOrderCancelRejected(const om::OMOrderCancelRejectedEvent &event, const om::order_t &order) {
            bool rejectedDueToFill = wasRejectedDueToBeingFilled(event.reasonCodeExt, order.venue);
            if (!rejectedDueToFill) {
                _stacker->_logger.logInfo(boost::str(boost::format("New order reject. Reason=%1% %2%") % event.reasonCodeExt % event.reasonText));
                _stacker->onCriticalError();
            } else {
                _acknowledged -= order.leavesQty;
                _flight += order.leavesQty;
            }
        }

        hf::price_t _price = hf::NULL_PRICE;

        hf::qty_t _target = 0;
        hf::qty_t _acknowledged = 0;
        hf::qty_t _flight = 0;
        hf::qty_t _traded = 0;

        std::vector<om::order_t*> _orders;

        Stacker* _stacker;

        Level* before = nullptr;
        Level* after = nullptr;
    };

    void initialize(uint32_t internalId, uint32_t side, hf::price_t tickSize, hf::qty_t quoteSize, uint32_t numberOfLevels, ExecutorT* executor, const std::string& contractName) {
        _internalId = internalId;
        _side = side;
        _tickSize = tickSize;
        _quoteSize = quoteSize;
        _nLevels = numberOfLevels;
        _executor = executor;
        _name = contractName;
    }

    void stackBid(hf::price_t price, hf::qty_t size) {
        stack<BID>(price, size);
    }

    void stackAsk(hf::price_t price, hf::qty_t size) {
        stack<ASK>(price, size);
    }

    template <uint32_t Side>
    void stack(hf::price_t price, hf::qty_t size) {
        const constexpr char* sideString = Side == BID ? "BID" : "ASK";
        _logger.logInfo(boost::str(boost::format("Stacker %1%. Stack %2% %3% %4%") % _name % sideString % price % size));
        if (_topLevel == nullptr) {
            createFirstStack<Side>(price, size);
        }
        else if (_topLevel->_price == price) {
            _topLevel->updateOnlyOneLevel(size);
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

            _bottomLevel = newBottomLevel;
        }
    }

    template <uint32_t Side>
    void createFirstStack(hf::price_t price, hf::qty_t size) {
        const uint32_t totalNumberOfLevel = _nLevels + 2 * NUMBER_OF_PRE_CREATED_LEVELS + 1;

        hf::price_t levelPrice = price + _tickSize * NUMBER_OF_PRE_CREATED_LEVELS * (_side == BID ? 1 : -1);

        Level* level = new Level(this);
        level->_price = levelPrice;

        for (uint32_t levelIndex = 1; levelIndex < totalNumberOfLevel; ++levelIndex) {
            Level* nextLevel = new Level(this);
            level->after = nextLevel;
            nextLevel->before = level;
            levelPrice = GetPriorPrice<Side>(levelPrice, _tickSize);
            nextLevel->_price = levelPrice;
            if (levelIndex < NUMBER_OF_PRE_CREATED_LEVELS) {
                level->updateOnlyOneLevel(0);
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
                nextLevel->updateOnlyOneLevel(0);
            }
            level = nextLevel;
        }
    }

    void cancel(om::order_t* order) {
        _logger.logInfo(boost::str(boost::format("Stacker %1%. Cancel order. Id=%2% Price=%3% Size=%4%") % _name % order->oid % order->pr % order->qty));
        _executor->cancelOrder(order);
    }

    void modify(const om::order_t* order, hf::price_t price, hf::qty_t size) {
        _logger.logInfo(boost::str(boost::format("Stacker %1%. Modify order. Id=%2% Price=%3% Size=%4%") % _name % order->oid % price % size));
        _executor->modifyOrder(order, price, size);
    }

    om::order_t* newOrder(Level* level, hf::price_t price, hf::qty_t size) {
        om::order_t* order = _executor->newLimitOrder(_internalId, _side, price, size);
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

    void onCriticalError() {
        _executor->onCriticalError();
    }

    uint32_t _internalId;
    uint32_t _side;
    hf::price_t _tickSize;
    hf::qty_t _quoteSize;
    uint32_t _nLevels;

    Level* _topLevel = nullptr;
    Level* _bottomLevel = nullptr;

    ExecutorT* _executor;
    std::string _name;

    Logger _logger;
};

}

#endif //VIPER_STACKER_H
