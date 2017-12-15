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
    typedef std::multimap<hf::price_t, Order*> OrdersMap;
    typedef std::multimap<hf::price_t, Order*>::iterator OrdersMapIt;

    Order& modifyBidOrder(OrderId_t orderId, QuoteOrderMessage& message) {
        auto it = _orders.find(orderId);
        Order& order = it->second;

        if (order._isInBook) {
            removeBidOrder(order);
        }

        _bids.insert(std::pair<hf::price_t, Order*>(message._internalMarketPrice, &order));
        order._price = message._internalMarketPrice;
        order._size = message._internalMarketSize;
        order._isInBook = true;

        PriceLevel* priceLevel = addToPriceLevel(order, message._externalPrice, message._externalSize);
        order.priceLevel = priceLevel;

        return order;
    }

    PriceLevel* addToPriceLevel(Order& order, hf::price_t price, hf::qty_t size) {
        auto it = _bidLevels.find(price);
        if (it != _bidLevels.end()) {
            PriceLevel& level = it->second;
            level._size += size;
            level._orders.push_back(&order);
            return &level;
        } else {
            auto result = _bidLevels.insert(std::pair<hf::price_t, PriceLevel>(price, {}));
            PriceLevel& level = result.first->second;
            level._price = price;
            level._size = size;
            level._orders.push_back(&order);
            return &level;
        }
    }

    void removeBidOrder(Order& order) {
        auto ret = _bids.equal_range(order._price);
        for (auto it = ret.first; it != ret.second; ++it) {
            if (it->second->_id == order._id) {
                PriceLevel* level = order.priceLevel;
                level->remove(order);
                if (level->_size == 0) {
                    removeLevel(level);
                }
                _bids.erase(it);
                return;
            }
        }
    }

    void removeLevel(PriceLevel* level) {
        auto it =_bidLevels.find(level->_price);
        _bidLevels.erase(it);
    }

    void synchronizeOrders() {
        const hf::price_t currentTopBid = getCurrentTopBid();
        const hf::price_t newTopBid = getNewTopBid();

        if (newTopBid > currentTopBid) {
            synchronizeBidOrders();
            synchronizeAskOrders();
        } else {
            synchronizeAskOrders();
            synchronizeBidOrders();
        }
    }

    hf::price_t getCurrentTopBid() {
        return 0;
    }

    hf::price_t getNewTopBid() {
        return 0;
    }

    void synchronizeBidOrders() {

    }

    void synchronizeAskOrders() {

    }

    template <typename T>
    PriceLevel* getTopLevel(T& levels) {
        auto it = levels.begin();
        if (it != levels.end()) {
            PriceLevel& level = it->second;
            return &level;
        }
        return nullptr;
    }

    hf::qty_t tryMatch(uint32_t side, hf::price_t price, hf::qty_t size) {
        return true;
    }

    bool matchOrders() {
        OrdersMapIt bidOrdersIt = _bids.begin();
        OrdersMapIt askOrdersIt = _asks.begin();
        hf::price_t bidTop = bidOrdersIt->first;
        hf::price_t askTop = askOrdersIt->first;
        const bool hasMatch = bidTop > askTop;
        if (hasMatch) {
            Order* bidOrder = bidOrdersIt->second;
            Order* askOrder = askOrdersIt->second;

            hf::qty_t bidSize = bidOrder->_size;
            hf::qty_t askSize = askOrder->_size;

            while (bidTop >= askTop) {
                if (bidOrder->_strategy == askOrder->_strategy) {
                    if (bidOrder->_strategy == Order::Strategy::MarketMaker) {
                        if (bidOrder->_type == askOrder->_type) {
                            ShutdownOnError("MM crossing same type.");
                        } else {
                            if (bidOrder->_type == Order::Type::Quoting) {
                                const bool hasBidOrder = getNextOrder(bidOrder, bidOrdersIt, _bids, bidTop);
                                if (!hasBidOrder) {
                                    break;
                                }
                                bidSize = bidOrder->_size;
                            } else {
                                const bool hasAskOrder = getNextOrder(askOrder,  askOrdersIt, _asks, askTop);
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
                    bidOrdersIt = _bids.erase(bidOrdersIt);
                    const bool hasBidOrder = getNextOrder(bidOrder, bidOrdersIt, _bids, bidTop);
                    if (!hasBidOrder) {
                        break;
                    }
                    const bool hasAskOrder = getNextOrder(askOrder, askOrdersIt, _asks, askTop);
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
                    const bool hasAskOrder = getNextOrder(askOrder, askOrdersIt, _asks, askTop);
                    if (!hasAskOrder) {
                        break;
                    }
                    askSize = askOrder->_size;
                    bidSize -= fillSize;
                } else {
                    const hf::qty_t fillSize = askSize - bidSize;
                    askOrder->onPartialFill(fillSize);
                    bidOrder->onFullFill();
                    const bool hasBidOrder = getNextOrder(bidOrder, bidOrdersIt, _bids, bidTop);
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

    bool getNextOrder(Order*& order, OrdersMapIt& ordersIt, OrdersMap& orders, hf::price_t& topPrice) {
        const bool hasMore = ordersIt == orders.end();
        if (hasMore) {
            ++ordersIt;
            topPrice = ordersIt->first;
            ordersIt = ordersIt;
            order = ordersIt->second;
        }
        return hasMore;
    }

    std::multimap<hf::price_t, Order*> _bids;
    std::multimap<hf::price_t, Order*> _asks;

    std::map<hf::price_t, PriceLevel> _bidLevels;
    std::map<hf::price_t, PriceLevel> _askLevels;
    TopLevel _topLevels[2];

    std::map<OrderId_t, Order> _orders;

    Stacker _bidStacker;
    Stacker _askStacker;
};

}

#endif //VIPER_INTERNAL_MATCHING_ENGINE_H
