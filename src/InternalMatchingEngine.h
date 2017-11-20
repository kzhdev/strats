#ifndef VIPER_INTERNAL_MATCHING_ENGINE_H
#define VIPER_INTERNAL_MATCHING_ENGINE_H

#include <map>
#include <hf/hf.h>
#include <boost/assert.hpp>
#include "Common.h"


namespace Python {

class InternalMatchingEngine {

public:

    hf::qty_t modifyBidOrder(OrderId_t orderId, hf::price_t price) {
        auto ordersIt = _orders.find(orderId);
        Order& order = ordersIt->second;
        const hf::price_t oldPrice = order._price;
        order._price = price;
        const hf::qty_t unmatchedSize = matchBidOrder(order);
        removeBidOrder(order._id, oldPrice);
        if (unmatchedSize > 0) {
            addOrderToBidBook(order, unmatchedSize);
            return unmatchedSize;
        }
        return 0;
    }

    void removeBidOrder(OrderId_t id, hf::price_t price) {
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

    OrderId_t addBidOrder(Order& order) {
        const hf::qty_t unmatchedSize = matchBidOrder(order);
        if (unmatchedSize == 0) {
            return NoOrderId;
        }
        return addOrderToBidBook(order, unmatchedSize);
    }

    hf::qty_t matchBidOrder(Order& order) {
        hf::qty_t unmatchedSize = order._size;
        for (auto levelIt = _asks.begin(); levelIt != _asks.end(); ++levelIt) {
            const hf::price_t levelPrice = levelIt->first;
            if (order._price > levelPrice) {
                std::vector& orders = levelIt->second;
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

    OrderId_t addOrderToBidBook(Order& order, hf::qty_t size) {
        Order o = order;
        o._size = size;
        const OrderId_t orderId = ++_orderId;
        auto it = _orders.insert(std::pair<OrderId_t, Order>(orderId, o));
        BOOST_ASSERT(it.second);
        Order& insertedOrder = it.first->second;
        _bids.insert(std::pair<hf::price_t, Order*>(order._price, &insertedOrder));
        return orderId;
    }

    std::map<hf::price_t, std::vector<Order*>> _bids;
    std::map<hf::price_t, std::vector<Order*>> _asks;
    std::map<OrderId_t, Order> _orders;
    OrderId_t _orderId = 0;
};

}

#endif //VIPER_INTERNAL_MATCHING_ENGINE_H
