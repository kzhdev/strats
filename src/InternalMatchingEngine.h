#ifndef VIPER_INTERNAL_MATCHING_ENGINE_H
#define VIPER_INTERNAL_MATCHING_ENGINE_H

#include <map>
#include <hf/hf.h>
#include <boost/assert.hpp>
#include "Common.h"

namespace Python {

class InternalMatchingEngine {

public:

    template <uint32_t Side>
    void cancelOrder(OrderId_t orderId) {
    }

    template <uint32_t Side>
    void modifyOrder(OrderId_t orderId, hf::price_t price, bool crossAttempt) {
        auto ordersIt = _orders.find(orderId);
        Order& order = ordersIt->second;
        const hf::price_t oldPrice = order._price;
        order._price = price;
        const hf::qty_t unmatchedSize = matchOrder<Side>(order);
        removeOrder<Side>(order._id, oldPrice);
        if (unmatchedSize > 0 && !crossAttempt) {
            addOrderToBook<Side>(order, unmatchedSize);
        }
    }

    template <uint32_t Side>
    void removeOrder(OrderId_t id, hf::price_t price) {
        const auto it = _bids.find(price);
        auto& orders = it->second;
        for (auto orderIt = orders.begin(); orderIt != orders.end(); ++orderIt) {
            Order* order = *orderIt;
            if (order->_id == id) {
                orders.erase(orderIt);
                return;
            }
        }
    }

    template <uint32_t Side>
    OrderId_t addOrder(Order& order) {
        const hf::qty_t unmatchedSize = matchOrder<Side>(order);
        if (unmatchedSize == 0) {
            return NoOrderId;
        }
        return addOrderToBook<Side>(order, unmatchedSize);
    }

    template <uint32_t Side>
    hf::qty_t matchOrder(Order& order) {
        hf::qty_t unmatchedSize = order._size;
        for (auto levelIt = _asks.begin(); levelIt != _asks.end(); ++levelIt) {
            const hf::price_t levelPrice = levelIt->first;
            if (order._price > levelPrice) {
                std::vector<Order*>& orders = levelIt->second;
                for (auto orderIt = orders.begin(); orderIt != orders.end();) {
                    Order* matchedOrder = *orderIt;
                    if (matchedOrder->_size == unmatchedSize) {
                        matchedOrder->onFullFill();
                        orders.erase(orderIt);
                        unmatchedSize = 0;
                        break;
                    } else if (matchedOrder->_size > unmatchedSize) {
                        const hf::qty_t matchedSize = matchedOrder->_size - unmatchedSize;
                        matchedOrder->onPartialFill(matchedSize);
                        unmatchedSize -= matchedSize;
                        break;
                    } else {
                        matchedOrder->onFullFill();
                        unmatchedSize -= matchedOrder->_size;
                    }
                }
                if (orders.size() == 0) {
                    _asks.erase(levelIt);
                }
                if (unmatchedSize > 0) {
                    break;
                }
            }
        }

        if (unmatchedSize == 0) {
            order.onFullFill();
        } else {
            const hf::qty_t fillSize = order._size - unmatchedSize;
            if (fillSize == order._size) {
                order.onFullFill();
            } else if (fillSize > 0) {
                order.onPartialFill(fillSize);
            }
        }

        return unmatchedSize;
    }

    template <uint32_t Side>
    OrderId_t addOrderToBook(Order& order, hf::qty_t size) {
        Order o = order;
        o._size = size;
        const OrderId_t orderId = ++_orderId;
        auto it = _orders.insert(std::pair<OrderId_t, Order>(orderId, o));
        BOOST_ASSERT(it.second);
        Order& insertedOrder = it.first->second;
        auto bidsIt = _bids.find(order._price);
        if (bidsIt == _bids.end()) {
            auto in = _bids.insert(std::pair<hf::price_t, std::vector<Order*>>(order._price, std::vector<Order*>()));
            in.first->second.push_back(&insertedOrder);
        } else {
            bidsIt->second.push_back(&insertedOrder);
        }
        return orderId;
    }

    std::map<hf::price_t, std::vector<Order*>> _bids;
    std::map<hf::price_t, std::vector<Order*>> _asks;
    std::map<OrderId_t, Order> _orders;
    OrderId_t _orderId = 0;
};

}

#endif //VIPER_INTERNAL_MATCHING_ENGINE_H
