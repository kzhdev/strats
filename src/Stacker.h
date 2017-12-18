#ifndef VIPER_STACKER_H
#define VIPER_STACKER_H

#include <md/md.h>
#include <om/om.h>
#include <om/om_event.h>
#include "Common.h"
#include "ViperConfig.h"

namespace Python {

template <typename ExecutorT>
class Stacker {

public:
    static const uint32_t LEVEL_POINTER = 1;
    static const uint32_t DIFF = 2;

    struct Level {

        bool canModifyOrder(const om::order_t& order) {
            return !order.is_modifying & order.is_acked & order.is_active & !order.is_cancelling;
        }

        void updateOnlyOneLevel(hf::qty_t size) {
            if (size < _target) {
                const hf::qty_t totalLevelSizeDecrease = _target - size;
                for(auto it = _orders.rbegin(); it != _orders.rend(); ++it) {
                    om::order_t* order = *it;
                    if (canModifyOrder(*order)) {
                        if (order->qty == totalLevelSizeDecrease) {
                            _flight -= totalLevelSizeDecrease;
                            _stacker->_downFlight += totalLevelSizeDecrease;
                            _stacker->cancel(order);
                            break;
                        } else if (order->qty < totalLevelSizeDecrease){
                            _flight -= order->qty;
                            _stacker->_downFlight += order->qty;
                            _stacker->cancel(order);
                        } else {
                            const hf::qty_t orderSizeDecrease = order->qty - totalLevelSizeDecrease;
                            _flight -= orderSizeDecrease;
                            _stacker->_downFlight += orderSizeDecrease;
                            _stacker->modify(order, _price, totalLevelSizeDecrease);
                            setDiff(*order, -orderSizeDecrease);
                            break;
                        }
                    }
                }
            } else if (size > _target) {
                const hf::qty_t  diff = size - _target;
                const hf::qty_t orderSize = std::min(diff, _stacker->_downFlight);
                om::order_t* order = _stacker->newOrder(this, _price, orderSize);
                _orders.push_back(order);
            }

            _target = size;
        }

        void setDiff(om::order_t& order, int64_t diff) {
            order.debug.i[DIFF] = diff;
        }

        int64_t getDiff(const om::order_t& order) {
            return  order.debug.i[DIFF];
        }

        void adjustAfterAck(om::order_t& order) {
            const hf::qty_t diff = _target - _acknowledged - _flight;
            if (diff > 0) {
                const hf::qty_t orderSize = std::min(diff, _stacker->_downFlight);
                om::order_t* no = _stacker->newOrder(this, _price, orderSize);
                _orders.push_back(no);
            } else if (diff < 0) {
                const hf::qty_t diffAbs = std::abs(diff);
                if (order.qty > diffAbs) {
                    const hf::qty_t orderSizeDecrease = order.qty - diffAbs;
                    _stacker->_downFlight += orderSizeDecrease;
                    _stacker->modify(&order, _price, diffAbs);
                    setDiff(order, -orderSizeDecrease);
                }
            }
        }

        void onOrderAccepted(const om::OMOrderAcceptedEvent& event, const om::order_t& order) {
            _acknowledged += event.acceptedQty;
            adjustAfterAck(const_cast<om::order_t&>(order));
        }

        void onOrderReplaced(const om::OMOrderModifiedEvent& event, const om::order_t& order) {
            om::order_t& mutableOrder = const_cast<om::order_t&>(order);
            const int64_t diff = getDiff(order);
            _acknowledged += diff;
            if (diff < 0) {
                _stacker->_downFlight -= diff;
            }
            adjustAfterAck(mutableOrder);
            mutableOrder.is_modifying = false;
        }

        void onOrderExecution(const om::OMExecutionEvent &event, const om::order_t &order) {
            _target -= event.filledQty;
            adjustAfterAck(const_cast<om::order_t&>(order));
        }

        void onOrderCanceled(const om::OMOrderCanceledEvent& event, const om::order_t& order) {
            _acknowledged -= order.leavesQty;
            _stacker->_downFlight -= order.leavesQty;
            adjustAfterAck(const_cast<om::order_t&>(order));
        }

        void onOrderRejected(const om::OMOrderRejectedEvent& event, const om::order_t& order) {
        }

        void onOrderReplaceRejected(const om::OMOrderModifyRejectedEvent& event, const om::order_t& order) {
        }

        void onOrderCancelRejected(const om::OMOrderCancelRejectedEvent &event, const om::order_t &order) {
        }

        hf::price_t _price = hf::NULL_PRICE;

        hf::qty_t _target = 0;
        hf::qty_t _acknowledged = 0;
        hf::qty_t _flight = 0;

        std::vector< om::order_t*> _orders;

        Stacker* _stacker;

        Level* before = nullptr;
        Level* after = nullptr;
    };

    void initialize(uint32_t internalId, uint32_t side, hf::price_t tickSize, hf::qty_t quoteSize, uint32_t numberOfLevels, ExecutorT* executor) {
        _internalId = internalId;
        _side = side;
        _tickSize = tickSize;
        _quoteSize = quoteSize;
        _nLevels = numberOfLevels;
        _executor = executor;
    }

    void stackBid(hf::price_t price, hf::qty_t size) {
        stack<BID>(price, size);
    }

    void stackAsk(hf::price_t price, hf::qty_t size) {
        stack<ASK>(price, size);
    }

    template <uint32_t Side>
    void stack(hf::price_t price, hf::qty_t size) {
        if (_price == hf::NULL_PRICE) {
            createFirstStack<Side>(price, size);
        }
        else if (_price == price) {
            _topLevel->updateOnlyOneLevel(size);
        }
        else if (IsPriceMovingAgainst<Side>(price, _price)) {
            Level* lastLevel = _topLevel;

            while (lastLevel->_price != price) {
                lastLevel->updateOnlyOneLevel(0);
                lastLevel = lastLevel->before;
            }

            _topLevel = lastLevel;
            _topLevel->updateOnlyOneLevel(size);

            Level* stackerLevel = _topLevel;
            for (uint32_t stackLevel = 0; stackLevel < _nLevels; ++stackLevel) {
                Level* nextLevel = stackerLevel->after;
                if (nextLevel == nullptr) {
                    nextLevel = new Level();
                    nextLevel->_price = GetPriorPrice<Side>(stackerLevel->_price, _tickSize);
                    nextLevel->before = stackerLevel;
                    stackerLevel->after = nextLevel;
                }
                stackerLevel->updateOnlyOneLevel(_quoteSize);
            }

            _bottomLevel = stackerLevel;
        }
        else {
            Level* lastLevel = _topLevel;
            while ((lastLevel->before != nullptr) & (lastLevel->_price != price)) {
                lastLevel = lastLevel->before;
            }

            if (lastLevel->_price != price) {
                for (hf::price_t levelPrice = GetNextPrice<Side>(lastLevel->_price, _tickSize); levelPrice <= price; levelPrice = GetNextPrice<Side>(levelPrice, _tickSize)) {
                    Level* level = new Level();
                    level->_price = levelPrice;
                    level->after = lastLevel;
                    lastLevel->before = level;
                    _topLevel = level;
                }
            }

            _topLevel->updateOnlyOneLevel(size);

            Level* stackerLevel = _topLevel;
            for (uint32_t stackLevel = 0; stackLevel < _nLevels; ++stackLevel) {
                stackerLevel = stackerLevel->after;
                stackerLevel->updateOnlyOneLevel(_quoteSize);
            }

            Level* newBottomLevel = stackerLevel;

            stackerLevel = stackerLevel->after;

            while (stackerLevel != _bottomLevel) {
                stackerLevel->updateOnlyOneLevel(0);
                stackerLevel = stackerLevel->after;
            }

            _bottomLevel = newBottomLevel;
        }
    }

    static const uint32_t NUMBER_OF_PRE_CREATED_LEVELS = 5;

    template <uint32_t Side>
    void createFirstStack(hf::price_t price, hf::qty_t size) {
        const uint32_t totalNumberOfLevel = _nLevels + 2 * NUMBER_OF_PRE_CREATED_LEVELS;

        hf::price_t levelPrice = price + _tickSize * NUMBER_OF_PRE_CREATED_LEVELS * (_side == BID ? 1 : -1);

        Level* level = new Level();
        level->_price = levelPrice;

        for (uint32_t levelIndex = 0; levelIndex < totalNumberOfLevel; ++levelIndex) {
            Level* nextLevel = new Level();
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
            if (levelIndex >= NUMBER_OF_PRE_CREATED_LEVELS && levelIndex < NUMBER_OF_PRE_CREATED_LEVELS + _nLevels) {
                level->updateOnlyOneLevel(_quoteSize);
            }
            if (levelIndex == NUMBER_OF_PRE_CREATED_LEVELS + _nLevels) {
                _bottomLevel = nextLevel;
            }
            if (levelIndex >= NUMBER_OF_PRE_CREATED_LEVELS + _nLevels) {
                level->updateOnlyOneLevel(0);
            }
        }
    }

    void cancel(om::order_t* order) {
        _executor->cancelOrder(order);
    }

    void modify(const om::order_t* order, hf::price_t price, hf::qty_t size) {
        _executor->modifyOrder(order, price, size);
    }

    om::order_t* newOrder(Level* level, hf::price_t price, hf::qty_t size) {
        om::order_t* order = _executor->newLimitOrder(_internalId, _side, price, size);
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


private:
    uint32_t _internalId;
    uint32_t _side;
    hf::price_t _tickSize;
    hf::qty_t _quoteSize;
    uint32_t _nLevels;

    hf::price_t _price;
    Level* _topLevel;
    Level* _bottomLevel;

    hf::qty_t _downFlight = 0;

    ExecutorT* _executor;
};

}

#endif //VIPER_STACKER_H
