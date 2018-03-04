#ifndef VIPER_INTERNAL_MATCHING_ENGINE_H
#define VIPER_INTERNAL_MATCHING_ENGINE_H

#include <map>
#include <hf/hf.h>
#include <boost/assert.hpp>
#include "Common.h"
#include "Stacker.h"
#include "ViperConfig.h"
#include "OneLevelOrderManager.h"
#include "ClockUtil.h"
#include <vector>

namespace Python {

struct Banner {
    uint64_t _expiryTime = 0;
    uint64_t _fillPeriod = 0;
    uint64_t _quotePeriod = 0;

    bool isBanned(uint64_t now) {
        return _expiryTime > now;
    }

    bool wasRunning() {
        return _expiryTime != 0;
    }

    void expire() {
        _expiryTime = 0;
    }
};

typedef std::unordered_map<OrderId_t, Order> OrdersT;
typedef std::multimap<hf::price_t, Order*, std::greater<hf::price_t>> BidOrdersMap;
typedef BidOrdersMap::iterator BidOrdersMapIt;
typedef std::multimap<hf::price_t, Order*> AskOrdersMap;
typedef AskOrdersMap::iterator AskOrdersMapIt;

template <uint32_t Side>
inline Order* getOrders(Order* bids, Order* asks);

template <>
inline Order* getOrders<BID>(Order* bids, Order* asks) {
    return bids;
}

template <>
inline Order* getOrders<ASK>(Order* bids, Order* asks) {
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
        level.addOrder(&order);
        return &level;
    } else {
        auto result = t._bidLevels.insert(std::pair<hf::price_t, PriceLevel>(price, {}));
        t._logger.logInfo(boost::str(boost::format("IME %1% Adding BID level. No more orders. Price=%2%") % t._name % price));
        PriceLevel& level = result.first->second;
        level._price = price;
        level._size = size;
        level.addOrder(&order);
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
        level.addOrder(&order);
        return &level;
    } else {
        auto result = t._askLevels.insert(std::pair<hf::price_t, PriceLevel>(price, {}));
        t._logger.logInfo(boost::str(boost::format("IME %1% Adding ASK level. No more orders. Price=%2%") % t._name % price));
        PriceLevel& level = result.first->second;
        level._price = price;
        level._size = size;
        level.addOrder(&order);
        return &level;
    }
}

template <typename ExecutorT>
class InternalMatchingEngine {

public:

    typedef InternalMatchingEngine<ExecutorT> MeT;

    InternalMatchingEngine() {
        _topLevels[BID] = hf::NULL_PRICE;
        _topLevels[ASK] = hf::NULL_PRICE;
    }

    void initialize(uint32_t internalId,  hf::price_t tickSize, StackerConfig& stackerConfig, ExecutorT* executor, const std::string& name, uint64_t fillBannerPeriod, uint64_t quoteBannerPeriod) {
        _stacker[BID].initialize(internalId, BID, tickSize, stackerConfig._quoteSize, stackerConfig._numberOfLevels, this, name);
        _stacker[ASK].initialize(internalId, ASK, tickSize, stackerConfig._quoteSize, stackerConfig._numberOfLevels, this, name);
        _oneLevelManager[BID].initialize(internalId, BID, name, executor);
        _oneLevelManager[ASK].initialize(internalId, ASK, name, executor);
        _stackerEnabled = stackerConfig._enabled;
        initializeOrders();
        _executor = executor;
        _internalId = internalId;
        _name = name;
        _banner[BID]._quotePeriod = _banner[ASK]._quotePeriod = quoteBannerPeriod;
        _banner[BID]._fillPeriod = _banner[ASK]._fillPeriod = fillBannerPeriod;
    }

    void initializeOrders() {
        createOrder(ORDER_ID_RV_INTERNAL_QUOTE, BID, Order::Strategy::RelativeValue, Order::Type::Quoting, _bidOrders);
        createOrder(ORDER_ID_RV_INTERNAL_QUOTE, ASK, Order::Strategy::RelativeValue, Order::Type::Quoting, _askOrders);
        createOrder(ORDER_ID_MM_QUOTE, BID, Order::Strategy::MarketMaker, Order::Type::Quoting, _bidOrders);
        createOrder(ORDER_ID_MM_QUOTE, ASK, Order::Strategy::MarketMaker, Order::Type::Quoting, _askOrders);
        createOrder(ORDER_ID_MM_HEDGE, BID, Order::Strategy::MarketMaker, Order::Type::Hedging, _bidOrders);
        createOrder(ORDER_ID_MM_HEDGE, ASK, Order::Strategy::MarketMaker, Order::Type::Hedging, _askOrders);
    }

    void createOrder(OrderIds id, uint32_t side, Order::Strategy strategy, Order::Type type, Order* orders) {
        Order& order = orders[id];
        order._id = id;
        order._side = side;
        order._strategy = strategy;
        order._type = type;
    }

    Order& modifyAskOrder(OrderId_t orderId, QuoteOrderMessage::Quote& message) {
        return modifyOrder<ASK>(orderId, message);
    }

    Order& modifyBidOrder(OrderId_t orderId, QuoteOrderMessage::Quote& message) {
        return modifyOrder<BID>(orderId, message);
    }

    template <uint32_t Side>
    Order& modifyOrder(OrderId_t orderId, QuoteOrderMessage::Quote& message) {
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
        Order& order = getOrders<Side>(_bidOrders, _askOrders)[orderId];

        if (order._isInBook) {
            removeOrder<Side>(order);
        }

        insertOrder<Side>(internalPrice, order);

        order._price = internalPrice;
        order._size = internalSize;
        order._externalPrice = externalPrice;
        order._externalSize = externalSize;
        order._isInBook = true;

        if (externalSize != 0) {
            PriceLevel* priceLevel = ToPriceLevelAdder<Side>().addToPriceLevel(*this, order, externalPrice, externalSize);
            order.priceLevel = priceLevel;
        } else {
            _topLevels[Side] = hf::NULL_PRICE;
            order.priceLevel = nullptr;
            _logger.logInfo(boost::str(boost::format("IME %1% %2% Modified order. external_size=0. Just pulling order. ID=%3%") % _name % GetSideString(Side) % orderId));
        }

        PriceLevel* topLevel = Side == BID ? getTopLevel(_bidLevels) : getTopLevel(_askLevels);
        if (topLevel != nullptr) {
            _logger.logInfo(boost::str(boost::format("IME %1% %2% Modified order. ID=%3% TopLevel=%4%@%5%") % _name % GetSideString(Side) % orderId % topLevel->_price % topLevel->_size));
        } else {
            _logger.logInfo(boost::str(boost::format("IME %1% %2% Modified order. Have no top level.") % _name % GetSideString(Side)));
        }

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
        auto& order = _bidOrders[orderId];
        removeOrder<BID>(order);
    }

    void removeAskOrder(OrderId_t orderId) {
        auto& order = _askOrders[orderId];
        removeOrder<ASK>(order);
    }

    template <typename MapT, typename LevelsT>
    void doRemoveOrder(Order& order, MapT& map, LevelsT& levels) {
        auto ret = map.equal_range(order._price);
        for (auto it = ret.first; it != ret.second; ++it) {
            if (it->second->_id == order._id) {
                PriceLevel* level = order.priceLevel;
                if (level != nullptr) {
                    level->remove(order);
                    if (level->_size == 0) {
                        removeLevel(level, levels);
                    }
                } else {
                    BOOST_ASSERT(order._externalPrice == hf::NULL_PRICE);
                }
                map.erase(it);
                return;
            }
        }
        order._isInBook = false;
    }

    template <typename LevelsT>
    void removeLevel(PriceLevel* level, LevelsT& levels) {
        auto size = levels.erase(level->_price);
        BOOST_ASSERT(size == 1);
    }

    template <typename MapT>
    void removeOrderFromMap(Order* order, MapT& map) {
        auto ret = map.equal_range(order->_price);
        for (auto it = ret.first; it != ret.second; ) {
            if (it->second->_id == order->_id) {
                it = map.erase(it);
                order->_isInBook = false;
                return;
            } else {
                ++it;
            }
        }
    }

    void synchronizeOrders() {
        const hf::price_t currentTopBid = _topLevels[BID];
        PriceLevel* level = getTopLevel(_bidLevels);
        const hf::price_t newTopBid = level == nullptr ? currentTopBid : level->_price;
        const uint64_t now = clockUtil::GetCycles();
        if (newTopBid > currentTopBid) {
            synchronizeBidOrders(now);
            synchronizeAskOrders(now);
        } else {
            synchronizeAskOrders(now);
            synchronizeBidOrders(now);
        }
    }

    void synchronizeBidOrders(uint64_t now) {
        hf::price_t price;
        hf::qty_t size;
        getPriceAndSize(_bidLevels, BID, price, size);
        if (price != hf::NULL_PRICE) {
            if (_stackerEnabled) {
                synchronizeUnderBanners<BID>(_stacker[BID], price, size, now);
            } else {
                synchronizeUnderBanners<BID>(_oneLevelManager[BID], price, size, now);
            }
            _topLevels[BID] = price;
        } else {
            if (_stackerEnabled) {
                _stacker[BID].getOutOfMarket();
            } else {
                _oneLevelManager[BID].getOutOfMarket();
            }
        }
    }

    void synchronizeAskOrders(uint64_t now) {
        hf::price_t price;
        hf::qty_t size;
        getPriceAndSize(_askLevels, ASK, price, size);
        if (price != hf::NULL_PRICE) {
            if (_stackerEnabled) {
                synchronizeUnderBanners<ASK>(_stacker[ASK], price, size, now);
            } else {
                synchronizeUnderBanners<ASK>(_oneLevelManager[ASK], price, size, now);
            }
            _topLevels[ASK] = price;
        } else {
            if (_stackerEnabled) {
                _stacker[ASK].getOutOfMarket();
            } else {
                _oneLevelManager[ASK].getOutOfMarket();
            }
        }
    }

    template <uint32_t Side, typename OrderManagerT>
    void synchronizeUnderBanners(OrderManagerT& manager, hf::price_t price, hf::qty_t size, uint64_t now) {
        const hf::price_t currentPrice = manager.getPrice();
        const hf::qty_t restingSize = manager.getRestingSize();
        bool isQuoteBetter = true;
        if (BOOST_LIKELY(currentPrice != hf::NULL_PRICE)) {
            isQuoteBetter = ((IsBetterPrice<Side>(price, currentPrice)) | ((price == currentPrice) & (size > restingSize)));
        }
        Banner& banner = _banner[Side];
        if (isQuoteBetter) {
            if (banner.isBanned(now)) {
                _logger.logInfo(boost::str(boost::format("Better quote for %1% %2% Not synchronizing") % _name % (Side == 1 ? "BID" : "ASK")));
                if (currentPrice != price) {
                    manager.template quote<Side>(price, 0);
                }
            } else {
                manager.template quote<Side>(price, size);
                startQuoteBanner<Side>();
            }
        } else {
            manager.template quote<Side>(price, size);
            banner.expire();
        }
    }

    template <uint32_t Side>
    void startQuoteBanner() {
        _banner[Side]._expiryTime = clockUtil::GetCycles() + _banner[Side]._quotePeriod;
    }

    template <uint32_t Side>
    void startFillBanner() {
        const uint64_t expiryTime = clockUtil::GetCycles() + _banner[Side]._fillPeriod;
        _banner[Side]._expiryTime = std::max(_banner[Side]._expiryTime, expiryTime);
    }

    void expireBanners() {
        uint64_t now = clockUtil::GetCycles();
        expireBanner<BID>(now);
        expireBanner<BID>(now);
    }

    template <uint32_t Side>
    void expireBanner(uint64_t now) {
        Banner& banner = _banner[Side];
        if (banner.wasRunning()) {
            if (!_banner[Side].isBanned(now)) {
                PriceLevel* level = Side == BID ? getTopLevel(_bidLevels) : getTopLevel(_askLevels);
                if (level != nullptr) {
                    hf::price_t price = level->_price;
                    hf::qty_t size = level->_size;
                    if (_stackerEnabled) {
                        if ((_stacker[Side]._topLevel->_target == 0) & (size != 0)) {
                            _stacker[Side].template quote<Side>(price, size);
                        }
                    } else {
                        if ((_oneLevelManager[Side]._target == 0) & (size != 0)) {
                            _oneLevelManager[Side].template quote<Side>(price, size);
                        }
                    }
                }
                banner.expire();
            }
        }
    }

    template <typename T>
    void getPriceAndSize(T& levels, uint32_t side, hf::price_t& price, hf::qty_t& size) {
        PriceLevel* level = getTopLevel(levels);
        if (level != nullptr) {
            price = level->_price;
            size = level->_size;
        } else {
            price = _topLevels[side];
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

    hf::qty_t tryMatch(uint32_t side, hf::price_t price, hf::qty_t size, hf::price_t tolerance) {
        if (side == ASK) {
            return tryMatchAsk(price, size, tolerance);
        } else {
            return tryMatchBid(price, size, tolerance);
        }
    }

    hf::qty_t tryMatchAsk(hf::price_t askPrice, hf::qty_t askSize, hf::price_t tolerance) {
        BidOrdersMapIt bidOrdersIt = _bids.begin();
        if (bidOrdersIt == _bids.end()) {
            return askSize;
        }

        askPrice -= tolerance;

        Order* bidOrder = bidOrdersIt->second;
        hf::qty_t bidSize = bidOrder->_size;
        hf::price_t bidPrice = bidOrdersIt->first;

        while (bidPrice >= askPrice) {
            if (bidSize == askSize) {
                _executor->onInternalFill(_internalId, bidOrder, bidPrice, bidSize);
                _bids.erase(bidOrdersIt);
                return 0;
            }
            else if (bidSize > askSize) {
                const hf::qty_t fillSize = askSize;
                _executor->onInternalFill(_internalId, bidOrder, bidPrice, fillSize);
                bidOrder->_size -= fillSize;
                return 0;
            } else {
                const hf::qty_t fillSize = bidSize;
                _executor->onInternalFill(_internalId, bidOrder, bidPrice, fillSize);
                bidOrdersIt = _bids.erase(bidOrdersIt);
                const bool hasBidOrder = getNextOrder(bidOrder, bidOrdersIt, _bids, bidPrice);
                askSize -= fillSize;
                if (!hasBidOrder) {
                    break;
                }
                bidSize = bidOrder->_size;
            }
        }

        return askSize;
    }

    hf::qty_t tryMatchBid(hf::price_t bidPrice, hf::qty_t bidSize, hf::price_t tolerance) {
        AskOrdersMapIt askOrdersIt = _asks.begin();

        if (askOrdersIt == _asks.end()) {
            return bidSize;
        }

        bidPrice += tolerance;

        hf::price_t askPrice = askOrdersIt->first;
        Order* askOrder = askOrdersIt->second;
        hf::qty_t askSize = askOrder->_size;

        while (bidPrice >= askPrice) {
            if (bidSize == askSize) {
                _executor->onInternalFill(_internalId, askOrder, askPrice, askSize);
                _asks.erase(askOrdersIt);
                return 0;
            }
            else if (bidSize > askSize) {
                const hf::qty_t fillSize = askSize;
                _executor->onInternalFill(_internalId, askOrder, askPrice, fillSize);
                askOrdersIt = _asks.erase(askOrdersIt);
                const bool hasAskOrder = getNextOrder(askOrder, askOrdersIt, _asks, askPrice);
                bidSize -= fillSize;
                if (!hasAskOrder) {
                    break;
                }
                askSize = askOrder->_size;
            } else {
                const hf::qty_t fillSize = bidSize;
                _executor->onInternalFill(_internalId, askOrder, askPrice, fillSize);
                askOrder->_size -= fillSize;
                return 0;
            }
        }

        return bidSize;
    }

    bool matchOrders() {
        _logger.logInfo(boost::str(boost::format("IME %1% Internally Match Orders") % _name));

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
        const bool hasMatch = bidTop >= askTop;
        if (hasMatch) {
            Order* bidOrder = bidOrdersIt->second;
            Order* askOrder = askOrdersIt->second;

            hf::qty_t bidSize = bidOrder->_size;
            hf::qty_t askSize = askOrder->_size;

            while (bidTop >= askTop) {
                _logger.logInfo(boost::str(boost::format("IME %1% Internal Cross. Matching. Bid: %2%@%3% Id=%4% Ask=%5%@%6% Id=%7%")
                                                                       % _name % bidTop % bidSize % bidOrder->_id % askTop % askSize % askOrder->_id));
                if (bidOrder->_strategy == askOrder->_strategy) {
                    switch (bidOrder->_strategy) {
                        case Order::Strategy::MarketMaker: {
                            if (bidOrder->_type == askOrder->_type) {
                                ShutdownOnError("MM crossing same type.");
                            } else {
                                if (bidOrder->_type == Order::Type::Quoting) {
                                    ++bidOrdersIt;
                                    const bool hasBidOrder = getNextOrder(bidOrder, bidOrdersIt, _bids, bidTop);
                                    if (!hasBidOrder) {
                                        break;
                                    }
                                    bidSize = bidOrder->_size;
                                } else {
                                    ++askOrdersIt;
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
                    _logger.logInfo(boost::str(boost::format("IME %1% Match. Equal size. Size=%2%") % _name % bidSize));
                    _executor->onInternalFill(_internalId, bidOrder, bidTop, bidSize);
                    _executor->onInternalFill(_internalId, askOrder, bidTop, askSize);
                    bidOrdersIt = _bids.erase(bidOrdersIt);
                    const bool hasBidOrder = getNextOrder(bidOrder, bidOrdersIt, _bids, bidTop);
                    askOrdersIt = _asks.erase(askOrdersIt);
                    const bool hasAskOrder = getNextOrder(askOrder, askOrdersIt, _asks, askTop);
                    if ((!hasAskOrder) | (!hasBidOrder)) {
                        _logger.logInfo(boost::str(boost::format("IME %1% No more orders. Exiting matching.") % _name));
                        break;
                    }
                    bidSize = bidOrder->_size;
                    askSize = askOrder->_size;
                    _logger.logInfo(boost::str(boost::format("IME %1% Got new orders.") % _name));
                }
                else if (bidSize > askSize) {
                    _logger.logInfo(boost::str(boost::format("IME %1% Match. Bid size > Ask size. MatchSize=%2%") % _name % askSize));
                    const hf::qty_t fillSize = askSize;
                    _executor->onInternalFill(_internalId, bidOrder, bidTop, fillSize);
                    _executor->onInternalFill(_internalId, askOrder, bidTop, fillSize);
                    askOrdersIt = _asks.erase(askOrdersIt);
                    bidOrder->_size -= fillSize;
                    const bool hasAskOrder = getNextOrder(askOrder, askOrdersIt, _asks, askTop);
                    if (!hasAskOrder) {
                        _logger.logInfo(boost::str(boost::format("IME %1% No more ask orders. Exiting matching.") % _name));
                        break;
                    }
                    askSize = askOrder->_size;
                    bidSize -= fillSize;
                    _logger.logInfo(boost::str(boost::format("IME %1% Got new ask order") % _name));
                } else {
                    _logger.logInfo(boost::str(boost::format("IME %1% Match. Bid size < Ask size. MatchSize=%2%") % _name % bidSize));
                    const hf::qty_t fillSize = bidSize;
                    _executor->onInternalFill(_internalId, askOrder, bidTop, fillSize);
                    _executor->onInternalFill(_internalId, bidOrder, bidTop, fillSize);
                    bidOrdersIt = _bids.erase(bidOrdersIt);
                    askOrder->_size -= fillSize;
                    const bool hasBidOrder = getNextOrder(bidOrder, bidOrdersIt, _bids, bidTop);
                    if (!hasBidOrder) {
                        _logger.logInfo(boost::str(boost::format("IME %1% No more bid orders. Exiting matching.") % _name));
                        break;
                    }
                    BOOST_ASSERT(bidOrdersIt != _bids.end());
                    bidSize = bidOrder->_size;
                    askSize -= fillSize;
                    _logger.logInfo(boost::str(boost::format("IME %1% Got new bid order") % _name));
                }
            }
        }
        return hasMatch;
    }

    template <typename OrdersMapItT, typename OrdersMapT>
    bool getNextOrder(Order*& order, OrdersMapItT& ordersIt, OrdersMapT& orders, hf::price_t& topPrice) {
        const bool hasMore = ordersIt != orders.end();
        if (hasMore) {
            topPrice = ordersIt->first;
            order = ordersIt->second;
        }
        return hasMore;
    }

    template <typename LevelsT, typename MapT>
    void onOrderExecution(const om::OMExecutionEvent &event, const om::order_t &order, LevelsT& levels, MapT& map) {
        _logger.logInfo(boost::str(boost::format("IME %1% External fill. Allocating to internal orders. Side=%2% Price=%3% Size=%4%")
                                                        % _name % GetSideString(ToViperSide(order.side)) % event.filledPrice % event.filledQty));
        hf::qty_t filledSize = event.filledQty;
        for (auto levelIt = levels.begin(); levelIt != levels.end(); ) {
            auto& level = levelIt->second;
            auto& orders = level._orders;
            for (auto orderIt = orders.begin(); orderIt != orders.end(); ) {
                _logger.logInfo(boost::str(boost::format("IME %1% Matching on level. Price=%2% NumberOfOrders=%3%") % _name % level._price % level._orders.size()));
                Order* o = *orderIt;
                if (o->_externalSize == filledSize) {
                    _logger.logInfo(boost::str(boost::format("IME %1% Match. Equal size. Order=%2% Price=%3% Size=%4%") % _name % o->_id % o->_externalPrice % filledSize));
                    _executor->onExternalFill(_internalId, o, event.filledPrice, filledSize);
                    orderIt = orders.erase(orderIt);
                    if (orderIt == orders.end()) {
                        _logger.logInfo(boost::str(boost::format("IME %1% Erasing level. No more orders. Price=%2%") % _name % level._price));
                        levels.erase(levelIt);
                    } else {
                        level._size -= filledSize;
                    }
                    removeOrderFromMap(o, map);
                    return;
                } else if (o->_externalSize < filledSize) {
                    const hf::qty_t oneFill = o->_externalSize;
                    _logger.logInfo(boost::str(boost::format("IME %1% Match. Order size < filled size. Order=%2% Price=%3% Size=%4%") % _name % o->_id % o->_externalPrice % oneFill));
                    _executor->onExternalFill(_internalId, o, event.filledPrice, oneFill);
                    orderIt = orders.erase(orderIt);
                    removeOrderFromMap(o, map);
                    filledSize -= oneFill;
                    if (orderIt == orders.end()) {
                        _logger.logInfo(boost::str(boost::format("IME %1% Erasing level. No more orders. Price=%2%") % _name % level._price));
                        levelIt = levels.erase(levelIt);
                        break;
                    } else {
                        level._size -= filledSize;
                    }
                } else {
                    _logger.logInfo(boost::str(boost::format("IME %1% Match. Order size > filled size. Order=%2% Price=%3% Size=%4%") % _name % o->_id % o->_externalPrice % filledSize));
                    _executor->onExternalFill(_internalId, o, event.filledPrice, filledSize);
                    const double ratio = static_cast<double>(o->_size) / static_cast<double>(o->_externalSize);
                    const hf::qty_t internalReduction = static_cast<hf::qty_t>(ratio * filledSize);
                    o->_externalSize -= filledSize;
                    o->_size =- internalReduction;
                    level._size -= filledSize;
                    return;
                }
            }
        }
        if (filledSize > 0) {
            _logger.logInfo(boost::str(boost::format("IME %1% Overfill. Price=%2% Size=%3%") % _name % event.filledPrice % filledSize));
            const uint32_t side = ToViperSide(order.side);
            _executor->onExternalOverFill(_internalId, side, event.filledPrice, filledSize);
        }
    }

    void getAllOutOfMarket() {
        _logger.logInfo(boost::str(boost::format("IME %1% Removing all orders from market.") % _name));
        removeBidOrder(ORDER_ID_RV_INTERNAL_QUOTE);
        removeBidOrder(ORDER_ID_MM_QUOTE);
        removeBidOrder(ORDER_ID_MM_HEDGE);
        removeAskOrder(ORDER_ID_RV_INTERNAL_QUOTE);
        removeAskOrder(ORDER_ID_MM_QUOTE);
        removeAskOrder(ORDER_ID_MM_HEDGE);
        if (_stackerEnabled) {
            _stacker[BID].getOutOfMarket();
            _stacker[ASK].getOutOfMarket();
        } else {
            _oneLevelManager[BID].getOutOfMarket();
            _oneLevelManager[ASK].getOutOfMarket();
        }
    }

    void getMarketMakerOutOfMarket() {
        _logger.logInfo(boost::str(boost::format("IME %1% Removing market maker orders from market.") % _name));
        removeBidOrder(ORDER_ID_MM_QUOTE);
        removeBidOrder(ORDER_ID_MM_HEDGE);
        removeAskOrder(ORDER_ID_MM_QUOTE);
        removeAskOrder(ORDER_ID_MM_HEDGE);
        synchronizeOrders();
    }

    void getRelativeValueOutOfMarket() {
        _logger.logInfo(boost::str(boost::format("IME %1% Removing relative value orders from market.") % _name));
        removeBidOrder(ORDER_ID_RV_INTERNAL_QUOTE);
        removeAskOrder(ORDER_ID_RV_INTERNAL_QUOTE);
        synchronizeOrders();
    }

    uint32_t getSide(hf::SIDE side) {
        return side == hf::SIDE::BUY ? 0 : 1;
    }

    void onOrderExecution(const om::OMExecutionEvent &event, const om::order_t &order) {
        if (order.side == hf::SIDE::BUY) {
            onOrderExecution(event, order, _bidLevels, _bids);
            startFillBanner<BID>();
        } else {
            onOrderExecution(event, order, _askLevels, _asks);
            startFillBanner<ASK>();
        }
        _executor->onEndOfExternalFills(_internalId, order.side);

        const uint32_t side = getSide(order.side);
        if (_stackerEnabled) {
            _stacker[side].onOrderExecution(event, order);
        } else {
            _oneLevelManager[side].onOrderExecution(event, order);
        }
    }

    void onOrderAccepted(const om::OMOrderAcceptedEvent& event, const om::order_t& order) {
        const uint32_t side = getSide(order.side);
        if (_stackerEnabled) {
            _stacker[side].onOrderAccepted(event, order);
        } else {
            _oneLevelManager[side].onOrderAccepted(event, order);
        }
    }

    void onOrderRejected(const om::OMOrderRejectedEvent& event, const om::order_t& order) {
        const uint32_t side = getSide(order.side);
        if (_stackerEnabled) {
            _stacker[side].onOrderRejected(event, order);
        } else {
            _oneLevelManager[side].onOrderRejected(event, order);
        }
    }

    void onOrderReplaced(const om::OMOrderModifiedEvent& event, const om::order_t& order) {
        const uint32_t side = getSide(order.side);
        if (_stackerEnabled) {
            _stacker[side].onOrderReplaced(event, order);
        } else {
            _oneLevelManager[side].onOrderReplaced(event, order);
        }
    }

    void onOrderReplaceRejected(const om::OMOrderModifyRejectedEvent& event, const om::order_t& order) {
        const uint32_t side = getSide(order.side);
        if (_stackerEnabled) {
            _stacker[side].onOrderReplaceRejected(event, order);
        } else {
            _oneLevelManager[side].onOrderReplaceRejected(event, order);
        }
    }

    void onOrderCanceled(const om::OMOrderCanceledEvent& event, const om::order_t& order) {
        const uint32_t side = getSide(order.side);
        if (_stackerEnabled) {
            _stacker[side].onOrderCanceled(event, order);
        } else {
            _oneLevelManager[side].onOrderCanceled(event, order);
        }
    }

    void onOrderCancelRejected(const om::OMOrderCancelRejectedEvent &event, const om::order_t &order) {
        const uint32_t side = getSide(order.side);
        if (_stackerEnabled) {
            _stacker[side].onOrderCancelRejected(event, order);
        } else {
            _oneLevelManager[side].onOrderCancelRejected(event, order);
        }
    }

    void handleStackerParameterUpdate(const proto::GridCell& gridCell) {
        StackerParametersColumns columnType = static_cast<StackerParametersColumns>(gridCell.col());
        switch (columnType) {
            case StackerParametersColumns::QuoteSize: {
                hf::qty_t size = atoi(gridCell.value().c_str());
                if (_stackerEnabled) {
                    _stacker[BID].updateQuoteSize(size, _stackerEnabled);
                    _stacker[ASK].updateQuoteSize(size, _stackerEnabled);
                } else {
                    _oneLevelManager[BID].updateQuoteSize(size);
                    _oneLevelManager[ASK].updateQuoteSize(size);
                }
                break;
            }
            case StackerParametersColumns::Enabled: {
                bool enabled = 1 == atoi(gridCell.value().c_str());
                if (enabled != _stackerEnabled) {
                    if (enabled) {
                        switchFromOlomToStacker();
                    } else {
                        switchFromStackerToOlom();
                    }
                    _stackerEnabled = enabled;
                }
                break;
            }
            case StackerParametersColumns::StackLevels: {
                const uint32_t levels = atoi(gridCell.value().c_str());
                _stacker[BID].template updateNumberOfLevels<BID>(levels, _stackerEnabled);
                _stacker[ASK].template updateNumberOfLevels<ASK>(levels, _stackerEnabled);
                break;
            }
        }
    }

    void switchFromOlomToStacker() {
        // TODO
    }

    void switchFromStackerToOlom() {
        // TODO
    }

    void handleQuoteBannerUpdate(uint64_t period) {
        _banner[BID]._quotePeriod = period;
        _banner[ASK]._quotePeriod = period;
        _logger.logInfo(boost::str(boost::format("IME %1%. quote banner changed. Value=%2%") % _name % period));
    }

    void handleFillBannerUpdate(uint64_t period) {
        _banner[BID]._fillPeriod = period;
        _banner[ASK]._fillPeriod = period;
        _logger.logInfo(boost::str(boost::format("IME %1%. fill banner changed. Value=%2%") % _name % period));
    }

    void notifyLevelFree(uint32_t side, hf::price_t price) {
        uint32_t otherSide = GetOtherSide(side);
        if (_stackerEnabled) {
            _stacker[otherSide].onLevelFree(price);
            if (side == BID) {
                if (!_askPendingHedgeOrders.empty()) {
                    auto ret = _askPendingHedgeOrders.equal_range(price);
                    for (auto it = ret.first; it != ret.second; ) {
                        _executor->sendExternalRvHedge(it->second);
                        it = _askPendingHedgeOrders.erase(it);
                    }
                }
            } else {
                if (!_bidPendingHedgeOrders.empty()) {
                    auto ret = _bidPendingHedgeOrders.equal_range(price);
                    for (auto it = ret.first; it != ret.second; ) {
                        _executor->sendExternalRvHedge(it->second);
                        it = _bidPendingHedgeOrders.erase(it);
                    }
                }
            }
        }
    }

    void notifyLevelUsed(uint32_t side, hf::price_t price) {
        uint32_t otherSide = GetOtherSide(side);
        if (_stackerEnabled) {
            _stacker[otherSide].onLevelUsed(price);
        }
    }

    void sendExternalRvHedge(HedgeOrderMessage& order) {
        if (order._side == BID) {
            if (!_stacker[ASK].wouldSelfMatch(order._price)) {
                _executor->sendExternalRvHedge(order);
            } else {
                _logger.logInfo(boost::str(boost::format("IME %1%. Storing hedge because would cross. Price=%2% Size=%3% Side=%4%") % _name % order._price % order._size % GetSideString(order._side)));
                _bidPendingHedgeOrders.insert(std::pair<hf::price_t, HedgeOrderMessage>(order._price, order));
            }
        } else {
            if (!_stacker[BID].wouldSelfMatch(order._price)) {
                _executor->sendExternalRvHedge(order);
            } else {
                _askPendingHedgeOrders.insert(std::pair<hf::price_t, HedgeOrderMessage>(order._price, order));
                _logger.logInfo(boost::str(boost::format("IME %1%. Storing hedge because would cross. Price=%2% Size=%3% Side=%4%") % _name % order._price % order._size % GetSideString(order._side)));
            }
        }
    }

    std::multimap<hf::price_t, HedgeOrderMessage> _bidPendingHedgeOrders;
    std::multimap<hf::price_t, HedgeOrderMessage> _askPendingHedgeOrders;

    BidOrdersMap _bids;
    AskOrdersMap _asks;
    std::map<hf::price_t, PriceLevel, std::greater<hf::price_t>> _bidLevels;
    std::map<hf::price_t, PriceLevel> _askLevels;
    hf::price_t _topLevels[2];

    Order _bidOrders[3];
    Order _askOrders[3];

    bool _stackerEnabled;
    Stacker<MeT> _stacker[2];
    OneLevelOrderManager<ExecutorT> _oneLevelManager[2];

    ExecutorT* _executor;

    uint32_t _internalId;

    Banner _banner[2];

    std::string _name;
    Logger _logger;
};

}

#endif //VIPER_INTERNAL_MATCHING_ENGINE_H
