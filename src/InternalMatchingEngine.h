#ifndef VIPER_INTERNAL_MATCHING_ENGINE_H
#define VIPER_INTERNAL_MATCHING_ENGINE_H

#include <map>
#include <hf/hf.h>
#include <boost/assert.hpp>
#include "Common.h"
#include "Stacker.h"
#include <vector>

namespace Python {

struct TopLevel {
    hf::price_t _price;
    hf::qty_t _qty;
};

class InternalMatchingEngine {

public:
    typedef  std::map<hf::price_t, std::vector<Order*>> Levels;
    typedef std::vector<Order*>::iterator OrdersIt;
    typedef std::map<hf::price_t, std::vector<Order*>>::iterator LevelsIt;
    typedef std::vector<Order*> Orders;

    template <uint32_t Side>
    Order& modifyOrder(OrderId_t orderId, hf::price_t price, hf::qty_t size) {
        auto it = _orders.find(orderId);
        Order& order = it->second;
        removeOrder<Side>(order);
        addOrder<Side>(order, price, size);
        return order;
    }

    template <uint32_t Side>
    void addOrder(OrderId_t orderId, hf::price_t price, hf::qty_t size) {
        auto it = _orders.find(orderId);
        Order& order = it->second;
        addOrder<Side>(order, price, size);
    }

    template <uint32_t Side>
    void cancelOrder(OrderId_t orderId) {
        auto it = _orders.find(orderId);
        Order& order = it->second;
        removeOrder<Side>(order);
    }

    template <uint32_t Side>
    void removeOrder(Order& order) {
        const auto it = _bids.find(order._price);
        auto& orders = it->second;
        for (auto orderIt = orders.begin(); orderIt != orders.end(); ++orderIt) {
            Order* foundOrder = *orderIt;
            if (foundOrder->_id == order._id) {
                orders.erase(orderIt);
                return;
            }
        }
    }

    template <uint32_t Side>
    void addOrder(Order& order, hf::price_t price, hf::qty_t size) {
        auto bidsIt = _bids.find(order._price);
        if (bidsIt == _bids.end()) {
            auto in = _bids.insert(std::pair<hf::price_t, std::vector<Order*>>(order._price, std::vector<Order*>()));
            in.first->second.push_back(&order);
        } else {
            bidsIt->second.push_back(&order);
        }
    }

    bool commit() {
        bool match = matchOrders();
        return match;
    }

    hf::qty_t tryMatch(uint32_t side, hf::price_t price, hf::qty_t size) {
        return true;
    }

    bool matchOrders() {
        LevelsIt bidLevelIt = _bids.begin();
        LevelsIt askLevelIt = _asks.begin();
        hf::price_t bidTop = bidLevelIt->first;
        hf::price_t askTop = askLevelIt->first;
        const bool hasMatch = bidTop > askTop;
        if (hasMatch) {
            std::vector<Order*>* bidOrders = &(bidLevelIt->second);
            OrdersIt bidOrderIt = bidOrders->begin();

            std::vector<Order*>* askOrders = &(askLevelIt->second);
            OrdersIt askOrderIt = askLevelIt->second.begin();

            Order* bidOrder = *bidOrderIt;
            Order* askOrder = *askOrderIt;

            hf::qty_t bidSize = bidOrder->_size;
            hf::qty_t askSize = askOrder->_size;

            while (bidTop >= askTop) {
                if (bidOrder->_strategy == askOrder->_strategy) {
                    if (bidOrder->_strategy == Order::Strategy::MarketMaker) {
                        if (bidOrder->_type == askOrder->_type) {
                            ShutdownOnError("MM crossing same type.");
                        } else {
                            if (bidOrder->_type == Order::Type::Quoting) {
                                const bool hasBidOrder = getNextOrder(bidOrder, bidOrderIt, bidLevelIt, bidOrders, _bids, bidTop);
                                if (!hasBidOrder) {
                                    break;
                                }
                                bidSize = bidOrder->_size;
                            } else {
                                const bool hasAskOrder = getNextOrder(askOrder, askOrderIt, askLevelIt, askOrders, _asks, askTop);
                                if (!hasAskOrder) {
                                    break;
                                }
                                askSize = askOrder->_size;
                            }
                        }
                    } else {
                        ShutdownOnError("Cross in Non-MM same orders.");
                    }
                }
                if (bidSize == askSize) {
                    bidOrder->onFullFill();
                    askOrder->onFullFill();
                    bidOrderIt = bidOrders->erase(bidOrderIt);
                    const bool hasBidOrder = getNextOrder(bidOrder, bidOrderIt, bidLevelIt, bidOrders, _bids, bidTop);
                    if (!hasBidOrder) {
                        break;
                    }
                    const bool hasAskOrder = getNextOrder(askOrder, askOrderIt, askLevelIt, askOrders, _asks, askTop);
                    if (!hasAskOrder) {
                        break;
                    }
                    bidSize = bidOrder->_size;
                    askSize = askOrder->_size;
                }
                else if (bidSize > askSize) {
                    const hf::qty_t fillSize = bidSize - askSize;
                    bidOrder->onPartialFill(fillSize);
                    askOrder->onFullFill();
                    const bool hasAskOrder = getNextOrder(askOrder, askOrderIt, askLevelIt, askOrders, _asks, askTop);
                    if (!hasAskOrder) {
                        break;
                    }
                    askSize = askOrder->_size;
                    bidSize -= fillSize;
                } else {
                    const hf::qty_t fillSize = askSize - bidSize;
                    askOrder->onPartialFill(fillSize);
                    bidOrder->onFullFill();
                    const bool hasBidOrder = getNextOrder(bidOrder, bidOrderIt, bidLevelIt, bidOrders, _bids, bidTop);
                    if (!hasBidOrder) {
                        break;
                    }
                    bidSize = bidOrder->_size;
                    askSize -= fillSize;
                }
            }
        }
        return hasMatch;
    }

    template <typename LevelsT>
    bool getNextOrder(Order*& order, OrdersIt& ordersIt, Levels::iterator& levelsIt, Orders*& orders, LevelsT& levels, hf::price_t& topPrice) {
        if (ordersIt == orders->end()) {
            levelsIt = levels.erase(levelsIt);
            if (levelsIt == levels.end()) {
                return false;
            } else {
                topPrice = levelsIt->first;
                orders =  &(levelsIt->second);
                ordersIt = orders->begin();
                order = *ordersIt;
            }
        } else {
            order = *ordersIt;
        }
        return true;
    }

    std::map<hf::price_t, std::vector<Order*>> _bids;
    std::map<hf::price_t, std::vector<Order*>> _asks;

    std::map<hf::price_t, PriceLevel> _bidLevels;
    std::map<hf::price_t, PriceLevel> _askLevels;
    TopLevel _topLevels[2];

    std::map<OrderId_t, Order> _orders;
};

}

#endif //VIPER_INTERNAL_MATCHING_ENGINE_H
