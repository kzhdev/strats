#ifndef VIPER_INTERNAL_MATCHING_ENGINE_H
#define VIPER_INTERNAL_MATCHING_ENGINE_H

#include <map>
#include <hf/hf.h>
#include <boost/assert.hpp>
#include "Common.h"
#include "Stacker.h"
#include "ViperConfig.h"
#include <vector>

namespace Python {

struct ImConfig {
    uint32_t _internalId;
    hf::price_t _tickSize;
    hf::qty_t _quoteSize;
    uint32_t _numberOfLevels;
};

struct LastQuotedLevel {
    hf::price_t _price;
    hf::qty_t _qty;
};

typedef std::unordered_map<OrderId_t, Order> OrdersT;
typedef std::multimap<hf::price_t, Order*> BidOrdersMap;
typedef BidOrdersMap::iterator BidOrdersMapIt;
typedef std::multimap<hf::price_t, Order*, std::greater<hf::price_t>> AskOrdersMap;
typedef AskOrdersMap::iterator AskOrdersMapIt;

template <uint32_t Side>
inline OrdersT& getOrders(OrdersT& bids, OrdersT& asks);

template <>
inline OrdersT& getOrders<BID>(OrdersT& bids, OrdersT& asks) {
    return bids;
}

template <>
inline OrdersT& getOrders<ASK>(OrdersT& bids, OrdersT& asks) {
    return asks;
}

template <uint32_t Side>
struct Remover {
    template <typename T>
    void remove(T& t, Order& o);
};

template <>
template <typename T>
void Remover<BID>::remove(T& t, Order& o) {
    t.doRemoveOrder(o, t._bids);
};

template <>
template <typename T>
void Remover<ASK>::remove(T& t, Order& o) {
    t.doRemoveOrder(o, t._asks);
};

template <uint32_t Side>
struct Inserter {
    template <typename T>
    void insert(T& t, hf::price_t price, Order& o);
};

template <>
template <typename T>
void Inserter<BID>::insert(T& t, hf::price_t price, Order& o) {
    t._bids.insert(std::pair<hf::price_t, Order*>(price, &o));
};

template <>
template <typename T>
void Inserter<ASK>::insert(T& t, hf::price_t price, Order& o) {
    t._asks.insert(std::pair<hf::price_t, Order*>(price, &o));
};

template <uint32_t Size>
struct ToPriceLevelAdder {
    template <typename T>
    PriceLevel* addToPriceLevel(T& t, Order& order, hf::price_t price, hf::qty_t size);
};

template <>
template <typename T>
PriceLevel* ToPriceLevelAdder<BID>::addToPriceLevel(T& t, Order& order, hf::price_t price, hf::qty_t size) {
    auto it = t._bidLevels.find(price);
    if (it != t._bidLevels.end()) {
        PriceLevel& level = it->second;
        level._size += size;
        level._orders.push_back(&order);
        return &level;
    } else {
        auto result = t._bidLevels.insert(std::pair<hf::price_t, PriceLevel>(price, {}));
        PriceLevel& level = result.first->second;
        level._price = price;
        level._size = size;
        level._orders.push_back(&order);
        return &level;
    }
}

template <>
template <typename T>
PriceLevel* ToPriceLevelAdder<ASK>::addToPriceLevel(T& t, Order& order, hf::price_t price, hf::qty_t size) {
    auto it = t._askLevels.find(price);
    if (it != t._askLevels.end()) {
        PriceLevel& level = it->second;
        level._size += size;
        level._orders.push_back(&order);
        return &level;
    } else {
        auto result = t._askLevels.insert(std::pair<hf::price_t, PriceLevel>(price, {}));
        PriceLevel& level = result.first->second;
        level._price = price;
        level._size = size;
        level._orders.push_back(&order);
        return &level;
    }
}

template <typename ExecutorT>
class InternalMatchingEngine {

public:

    void initialize(ImConfig& config, ExecutorT* executor) {
        _bidStacker.initialize(config._internalId, BID, config._tickSize, config._quoteSize, config._numberOfLevels, executor);
        _askStacker.initialize(config._internalId, ASK, config._tickSize, config._quoteSize, config._numberOfLevels, executor);

        initializeRvQuotes();

        _executor = executor;
    }

    void initializeRvQuotes() {
        auto bidPair = _bidOrders.insert({ORDER_ID_RV_INTERNAL_QUOTE, Order()});
        Order& bidOrder = bidPair.first->second;
        bidOrder._id = ORDER_ID_RV_INTERNAL_QUOTE;
        bidOrder._side = BID;
        bidOrder._strategy = Order::Strategy::RelativeValue;
        bidOrder._type = Order::Type::Quoting;

        auto askPair = _askOrders.insert({ORDER_ID_RV_INTERNAL_QUOTE, Order()});
        Order& askOrder = askPair.first->second;
        askOrder._id = ORDER_ID_RV_INTERNAL_QUOTE;
        askOrder._side = ASK;
        askOrder._strategy = Order::Strategy::RelativeValue;
        askOrder._type = Order::Type::Quoting;
    }

    Order& modifyAskOrder(OrderId_t orderId, QuoteOrderMessage& message) {
        return modifyOrder<ASK>(orderId, message);
    }

    Order& modifyBidOrder(OrderId_t orderId, QuoteOrderMessage& message) {
        return modifyOrder<BID>(orderId, message);
    }

    template <uint32_t Side>
    Order& modifyOrder(OrderId_t orderId, QuoteOrderMessage& message) {
        auto it = getOrders<Side>(_bidOrders, _askOrders).find(orderId);
        Order& order = it->second;

        if (order._isInBook) {
            removeOrder<Side>(order);
        }

        insertOrder<Side>(message._internalMarketPrice, order);

        order._price = message._internalMarketPrice;
        order._size = message._internalMarketSize;
        order._isInBook = true;

        PriceLevel* priceLevel = ToPriceLevelAdder<Side>().addToPriceLevel(*this, order, message._externalPrice, message._externalSize);
        order.priceLevel = priceLevel;

        return order;
    }

    template <uint32_t Side>
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

    template <uint32_t Side>
    void insertOrder(hf::price_t price, Order& order) {
        Inserter<Side> inserter;
        inserter.insert(*this, price, order);
    }

    template <uint32_t Side>
    void removeOrder(Order& order) {
        Remover<Side> remover;
        remover.remove(*this, order);
    }

    void removeBidOrder(Order& order) {
        removeOrder<BID>(order);
    }

    void removeAskOrder(Order& order) {
        removeOrder<ASK>(order);
    }

    template <typename MapT>
    void doRemoveOrder(Order& order, MapT& map) {
        auto ret = map.equal_range(order._price);
        for (auto it = ret.first; it != ret.second; ++it) {
            if (it->second->_id == order._id) {
                PriceLevel* level = order.priceLevel;
                level->remove(order);
                if (level->_size == 0) {
                    removeLevel(level);
                }
                map.erase(it);
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
        hf::price_t price;
        hf::qty_t size;
        getPriceAndSize(_bidLevels, BID, price, size);
        if (size != 0) {
            _bidStacker.stackBid(price, size);
        }
    }

    void synchronizeAskOrders() {
        hf::price_t price;
        hf::qty_t size;
        getPriceAndSize(_askLevels, ASK, price, size);
        if (size != 0) {
            _askStacker.stackAsk(price, size);
        }
    }

    template <typename T>
    void getPriceAndSize(T& levels, uint32_t side, hf::price_t& price, hf::qty_t& size) {
        PriceLevel* level = getTopLevel(levels);
        if (level != nullptr) {
            price = level->_price;
            size = level->_size;
        } else {
            LastQuotedLevel& lastQuotedLevel = _topLevels[side];
            price = lastQuotedLevel._price;
            size = 0;
        }
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
        BidOrdersMapIt bidOrdersIt = _bids.begin();

        if (bidOrdersIt == _bids.end()) {
            return false;
        }

        AskOrdersMapIt askOrdersIt = _asks.begin();

        if (askOrdersIt == _asks.end()) {
            return false;
        }

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
                    switch (bidOrder->_strategy) {
                        case Order::Strategy::MarketMaker: {
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
                                    const bool hasAskOrder = getNextOrder(askOrder, askOrdersIt, _asks, askTop);
                                    if (!hasAskOrder) {
                                        break;
                                    }
                                    askSize = askOrder->_size;
                                }
                            }
                        }
                        case Order::Strategy::RelativeValue:
                            break;
                        default:
                            break;
                    }
                }
                if (bidSize == askSize) {
                    _executor->onFullFill(bidOrder, bidTop);
                    _executor->onFullFill(askOrder, bidTop);
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
                    _executor->onPartialFill(bidOrder, bidTop, fillSize);
                    _executor->onFullFill(askOrder, bidTop);
                    const bool hasAskOrder = getNextOrder(askOrder, askOrdersIt, _asks, askTop);
                    if (!hasAskOrder) {
                        break;
                    }
                    askSize = askOrder->_size;
                    bidSize -= fillSize;
                } else {
                    const hf::qty_t fillSize = askSize - bidSize;
                    _executor->onPartialFill(askOrder, bidTop, fillSize);
                    _executor->onFullFill(bidOrder, bidTop);
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

    template <typename OrdersMapItT, typename OrdersMapT>
    bool getNextOrder(Order*& order, OrdersMapItT& ordersIt, OrdersMapT& orders, hf::price_t& topPrice) {
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
    std::multimap<hf::price_t, Order*, std::greater<hf::price_t>> _asks;

    std::map<hf::price_t, PriceLevel> _bidLevels;
    std::map<hf::price_t, PriceLevel, std::greater<hf::price_t>> _askLevels;
    LastQuotedLevel _topLevels[2];

    std::unordered_map<OrderId_t, Order> _bidOrders;
    std::unordered_map<OrderId_t, Order> _askOrders;

    Stacker<ExecutorT> _bidStacker;
    Stacker<ExecutorT> _askStacker;

    ExecutorT* _executor;

    uint32_t _internalId;
};

}

#endif //VIPER_INTERNAL_MATCHING_ENGINE_H
