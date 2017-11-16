#ifndef VIPER_INTERNAL_MATCHING_ENGINE_H
#define VIPER_INTERNAL_MATCHING_ENGINE_H

#include <map>
#include <hf/hf.h>
#include <boost/assert.hpp>
#include "Common.h"


namespace Python {

class InternalMatchingEngine {

public:

    OrderId_t addBidOrder(Order& order) {
        return matchBidOrder(order);
    }

    OrderId_t matchBidOrder(Order& order) {
        hf::qty_t unmatchedSize = order._size;
        auto it = _asks.begin();
        while (true) {
            Order* matchedOrder = it->second;
            if (order._price > matchedOrder->_price) {
                if (matchedOrder->_size == unmatchedSize) {
                    matchedOrder->onFullFill();
                    break;
                } else if (matchedOrder->_size > unmatchedSize) {
                    const hf::qty_t matchedSize = matchedOrder->_size - unmatchedSize;
                    matchedOrder->onPartialFill(matchedSize);
                    break;
                }
            } else {
                break;
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
                return addOrderToBidBook(order, unmatchedSize);
            } else {
                return addOrderToBidBook(order, unmatchedSize);
            }
        }
        return NoOrderId;
    }

    OrderId_t addOrderToBidBook(Order& order, hf::qty_t size) {
        Order o = order;
        o._size = size;
        const OrderId_t orderId = ++_orderId;
        auto it = _orders.insert(std::pair<OrderId_t, Order>(orderId, o));
        BOOST_ASSERT(it.second);
        Order& insertedOrder = it.first->second;
        _bids.insert(std::pair<double, Order*>(order._price, &insertedOrder));
        return orderId;
    }

    std::map<double, Order*> _bids;
    std::map<double, Order*> _asks;
    std::map<OrderId_t, Order> _orders;
    OrderId_t _orderId = 0;
};

}

#endif //VIPER_INTERNAL_MATCHING_ENGINE_H
