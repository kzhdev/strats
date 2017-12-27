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
    t.doRemoveOrder(o, t._bids, t._bidLevels);
};

template <>
template <typename T>
void Remover<ASK>::remove(T& t, Order& o) {
    t.doRemoveOrder(o, t._asks, t._askLevels);
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

    void initialize(uint32_t internalId,  hf::price_t tickSize, StackerConfig& stackerConfig, ExecutorT* executor) {
        _bidStacker.initialize(internalId, BID, tickSize, stackerConfig._quoteSize, stackerConfig._numberOfLevels, executor);
        _askStacker.initialize(internalId, ASK, tickSize, stackerConfig._quoteSize, stackerConfig._numberOfLevels, executor);
        initializeRvQuotes();
        _executor = executor;
        _internalId = internalId;
    }

    void initializeRvQuotes() {
        createOrder(ORDER_ID_RV_INTERNAL_QUOTE, BID, Order::Strategy::RelativeValue, Order::Type::Quoting, _bidOrders);
        createOrder(ORDER_ID_RV_INTERNAL_QUOTE, ASK, Order::Strategy::RelativeValue, Order::Type::Quoting, _askOrders);
        createOrder(ORDER_ID_MM_QUOTE, BID, Order::Strategy::MarketMaker, Order::Type::Quoting, _bidOrders);
        createOrder(ORDER_ID_MM_QUOTE, ASK, Order::Strategy::MarketMaker, Order::Type::Quoting, _askOrders);
        createOrder(ORDER_ID_MM_HEDGE, BID, Order::Strategy::MarketMaker, Order::Type::Hedging, _bidOrders);
        createOrder(ORDER_ID_MM_HEDGE, ASK, Order::Strategy::MarketMaker, Order::Type::Hedging, _askOrders);
    }

    template <typename MapT>
    void createOrder(OrderIds id, uint32_t side, Order::Strategy strategy, Order::Type type, MapT& map) {
        auto bidPair = map.insert({id, Order()});
        Order& bidOrder = bidPair.first->second;
        bidOrder._id = id;
        bidOrder._side = side;
        bidOrder._strategy = strategy;
        bidOrder._type = type;
    }

    Order& modifyAskOrder(OrderId_t orderId, QuoteOrderMessage& message) {
        return modifyOrder<ASK>(orderId, message);
    }

    Order& modifyBidOrder(OrderId_t orderId, QuoteOrderMessage& message) {
        return modifyOrder<BID>(orderId, message);
    }

    template <uint32_t Side>
    Order& modifyOrder(OrderId_t orderId, QuoteOrderMessage& message) {
        return modifyOrder<Side>(orderId, message._internalMarketPrice, message._internalMarketSize, message._externalPrice, message._externalSize);
    }

    Order& modifyAskOrder(OrderId_t orderId, hf::price_t internalPrice, hf::qty_t internalSize, hf::price_t externalPrice, hf::qty_t externalSize) {
        return modifyOrder<ASK>(orderId, internalPrice, internalSize, externalPrice, externalSize);
    }

    Order& modifyBidOrder(OrderId_t orderId, hf::price_t internalPrice, hf::qty_t internalSize, hf::price_t externalPrice, hf::qty_t externalSize) {
        return modifyOrder<BID>(orderId, internalPrice, internalSize, externalPrice, externalSize);
    }

    template <uint32_t Side>
    Order& modifyOrder(OrderId_t orderId, hf::price_t internalPrice, hf::qty_t internalSize, hf::price_t externalPrice, hf::qty_t externalSize) {
        auto it = getOrders<Side>(_bidOrders, _askOrders).find(orderId);
        Order& order = it->second;

        if (order._isInBook) {
            removeOrder<Side>(order);
        }

        insertOrder<Side>(internalPrice, order);

        order._price = internalPrice;
        order._size = internalSize;
        order._isInBook = true;

        PriceLevel* priceLevel = ToPriceLevelAdder<Side>().addToPriceLevel(*this, order, externalPrice, externalSize);
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

    void removeBidOrder(OrderId_t orderId) {
        auto& order = _bidOrders.find(orderId)->second;
        removeOrder<BID>(order);
    }

    void removeAskOrder(OrderId_t orderId) {
        auto& order = _askOrders.find(orderId)->second;
        removeOrder<ASK>(order);
    }

    template <typename MapT, typename LevelsT>
    void doRemoveOrder(Order& order, MapT& map, LevelsT& levels) {
        auto ret = map.equal_range(order._price);
        for (auto it = ret.first; it != ret.second; ++it) {
            if (it->second->_id == order._id) {
                PriceLevel* level = order.priceLevel;
                level->remove(order);
                if (level->_size == 0) {
                    removeLevel(level, levels);
                }
                map.erase(it);
                return;
            }
        }
    }

    template <typename LevelsT>
    void removeLevel(PriceLevel* level, LevelsT levels) {
        auto it = levels.find(level->_price);
        levels.erase(it);
    }

    template <typename MapT>
    void removeOrderFromMap(Order* order, MapT& map) {
        auto ret = map.equal_range(order->_price);
        for (auto it = ret.first; it != ret.second; ++it) {
            if (it->second->_id == order->_id) {
                map.erase(it);
                return;
            }
        }
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
                    _executor->onInternalFill(_internalId, bidOrder, bidTop, bidSize);
                    _executor->onInternalFill(_internalId, askOrder, bidTop, askSize);
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
                    _executor->onInternalFill(_internalId, bidOrder, bidTop, fillSize);
                    _executor->onInternalFill(_internalId, askOrder, bidTop, fillSize);
                    const bool hasAskOrder = getNextOrder(askOrder, askOrdersIt, _asks, askTop);
                    if (!hasAskOrder) {
                        break;
                    }
                    askSize = askOrder->_size;
                    bidSize -= fillSize;
                } else {
                    const hf::qty_t fillSize = askSize - bidSize;
                    _executor->onInternalFill(_internalId, askOrder, bidTop, fillSize);
                    _executor->onInternalFill(_internalId, bidOrder, bidTop, fillSize);
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

    void onOrderExecution(const om::OMExecutionEvent &event, const om::order_t &order) {
        if (order.side == hf::SIDE::BUY) {
            onOrderExecution(event, order, _bidLevels, _bids);
        } else {
            onOrderExecution(event, order, _askLevels, _asks);
        }
        _executor->onEndOfExternalFills(_internalId);
    }

    template <typename LevelsT, typename MapT>
    void onOrderExecution(const om::OMExecutionEvent &event, const om::order_t &order, LevelsT& levels, MapT& map) {
        hf::qty_t filledSize = event.filledQty;
        for (auto levelIt = levels.begin(); levelIt != levels.end(); ++levelIt) {
            auto& level = levelIt->second;
            auto& orders = level._orders;
            for (auto orderIt = orders.begin(); orderIt != orders.end(); ++orderIt) {
                Order* o = *orderIt;
                if (o->_externalSize == filledSize) {
                    _executor->onExternalFill(_internalId, o, event.filledPrice, filledSize);
                    orders.erase(orderIt);
                    if (orders.empty()) {
                        levels.erase(levelIt);
                    } else {
                        level._size -= filledSize;
                    }
                    removeOrderFromMap(o, map);
                    return;
                } else if (o->_externalSize < filledSize) {
                    _executor->onExternalFill(_internalId, o, event.filledPrice, filledSize);
                    orders.erase(orderIt);
                    if (orders.empty()) {
                        levels.erase(levelIt);
                    } else {
                        level._size -= filledSize;
                    }
                    filledSize -= filledSize;
                    removeOrderFromMap(o, map);
                } else {
                    _executor->onExternalFill(_internalId, o, event.filledPrice, filledSize);
                    o->_size -= filledSize;
                    level._size -= event.filledQty;
                }
            }
        }
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
