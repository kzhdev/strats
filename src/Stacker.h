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

    struct Order {
        hf::qty_t _diff;
        om::order_t* _order;
    };

    struct Level {

        void updateOnlyOneLevel(hf::qty_t size) {
            if (size < _target) {
                const hf::qty_t totalLevelSizeDecrease = _target - size;
                for(auto it = _orders.rbegin(); it != _orders.rend(); ++it) {
                    Order& order = *it;
                    if (!order._order->is_modifying) {
                        if (order._order->qty == totalLevelSizeDecrease) {
                            _downFlight += totalLevelSizeDecrease;
                            _stacker->cancel(order._order);
                            break;
                        } else if (order._order->qty < totalLevelSizeDecrease){
                            _downFlight += order._order->qty;
                            _stacker->cancel(order._order);
                        } else {
                            const hf::qty_t orderSizeDecrease = order._order->qty - totalLevelSizeDecrease;
                            _downFlight += orderSizeDecrease;
                            _stacker->modify(order._order, _price, totalLevelSizeDecrease);
                            order._diff = -orderSizeDecrease;
                            break;
                        }
                    }
                }
            } else if (size > _target) {
                const hf::qty_t  diff = size - _target;
                const hf::qty_t orderSize = std::min(diff, _downFlight);
                om::order_t* order = _stacker->newOrder(_price, orderSize);
                _orders.push_back({orderSize, order});
            }

            _target = size;
        }

        void onOrderAccepted(const om::OMOrderAcceptedEvent& event, const om::order_t& order) {
            Order* o = getOrder(order);
            _acknowledged += event.acceptedQty;
        }

        void adjustAfterAck(Order* o, const om::order_t& order) {
            const hf::qty_t diff = _target - _acknowledged - _flight;
            if (diff > 0) {
                const hf::qty_t orderSize = std::min(diff, _downFlight);
                om::order_t* no = _stacker->newOrder(_price, orderSize);
                _orders.push_back({orderSize, no});
            } else if (diff < 0) {
                const hf::qty_t diffAbs = std::abs(diff);
                if (order.qty > diffAbs) {
                    const hf::qty_t orderSizeDecrease = order.qty - diffAbs;
                    _downFlight += orderSizeDecrease;
                    _stacker->modify(&order, _price, diffAbs);
                    o->_diff = -orderSizeDecrease;
                }
            }
        }

        void onOrderReplaced(const om::OMOrderModifiedEvent& event, const om::order_t& order) {
            Order* o = getOrder(order);
            _acknowledged += o->_diff;
            if (o->_diff < 0) {
                _downFlight += o->_diff;
            }
            adjustAfterAck(o, order);
        }

        void onOrderExecution(const om::OMExecutionEvent &event, const om::order_t &order) {
            Order* o = getOrder(order);
            _target -= event.filledQty;
            adjustAfterAck(o, order);
        }

        Order* getOrder(const om::order_t& order) {
            for (auto& o : _orders) {
                if (o._order->oid == order.oid) {
                    return &o;
                }
            }
            return nullptr;
        }

        hf::price_t _price = hf::NULL_PRICE;

        hf::qty_t _target = 0;
        hf::qty_t _acknowledged = 0;
        hf::qty_t _downFlight = 0;
        hf::qty_t _flight = 0;

        std::vector<Order> _orders;

        Stacker* _stacker;

        Level* before = nullptr;
        Level* after = nullptr;
    };

public:

    void initialize(ViperConfig& config, ExecutorT* executor) {
        _executor = executor;
    }

    void stackBid(hf::price_t price, hf::qty_t size) {
        if (_price == price) {
            _topLevel->updateOnlyOneLevel(size);
        }
        else if (price > _price) {
            Level* lastLevel = _topLevel;
            while (lastLevel->before != nullptr & lastLevel->_price != price) {
                lastLevel = lastLevel->before;
            }

            if (lastLevel->_price != price) {
                for (hf::price_t levelPrice = lastLevel->_price + _tickSize; levelPrice <= price; levelPrice =+ _tickSize) {
                    Level* level = new Level();
                    level->_price = levelPrice;
                    level->after = lastLevel;
                    lastLevel->before = level;
                    _topLevel = level;
                }
            }

            Level* level = _topLevel;
            level->updateOnlyOneLevel(size);

            for (uint32_t stackLevel = 0; stackLevel < _nLevels; ++stackLevel) {
                level = level->after;
                level->updateOnlyOneLevel(_quoteSize);
            }

            level = level->after;

            while (level != nullptr) {
                level->updateOnlyOneLevel(0);
                level = level->after;
            }

        } else if (_price < price) {

        }
    }

    void cancel(om::order_t* order) {
        _executor->cancelOrder(order);
    }

    void modify(const om::order_t* order, hf::price_t price, hf::qty_t size) {
        _executor->modifyOrder(order, price, size);
    }

    om::order_t* newOrder(hf::price_t price, hf::qty_t size) {
        return _executor->newLimitOrder(_internalId, _side, price, size);
    }

private:
    hf::price_t _price;
    hf::qty_t _size;
    hf::price_t _tickSize;

    hf::qty_t _quoteSize;
    uint32_t _nLevels;

    Level* _topLevel;

    ExecutorT* _executor;
    uint32_t _side;
    uint32_t _internalId;
};

}

#endif //VIPER_STACKER_H
