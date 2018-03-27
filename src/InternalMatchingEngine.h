#ifndef VIPER_INTERNAL_MATCHING_ENGINE_H
#define VIPER_INTERNAL_MATCHING_ENGINE_H

#include <map>
#include <hf/hf.h>
#include <boost/assert.hpp>
#include "Common.h"
#include "Stacker.h"
#include "ViperConfig.h"
#include "ClockUtil.h"
#include <vector>
#include "HedgesManager.h"

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
        _relativeValueTopLevels[BID] = hf::NULL_PRICE;
        _relativeValueTopLevels[ASK] = hf::NULL_PRICE;
        _marketMakerTopLevels[BID] = hf::NULL_PRICE;
        _marketMakerTopLevels[ASK] = hf::NULL_PRICE;
    }

    void initialize(uint32_t internalId,  hf::price_t tickSize, StackerConfig& stackerConfig, ExecutorT* executor, const std::string& name, uint64_t fillBannerPeriod, uint64_t quoteBannerPeriod) {
        _stacker[BID].initialize(internalId, BID, tickSize, stackerConfig._quoteSize, stackerConfig._numberOfLevels, this, name);
        _stacker[ASK].initialize(internalId, ASK, tickSize, stackerConfig._quoteSize, stackerConfig._numberOfLevels, this, name);
        _hedgeManager[BID].initialize(internalId, BID, name, this);
        _hedgeManager[ASK].initialize(internalId, ASK, name, this);
        // _stackerEnabled = stackerConfig._enabled; - not used for now - turnign off by setting levels=0
        initializeOrders();
        _executor = executor;
        _internalId = internalId;
        _name = name;
        _banner[BID]._quotePeriod = _banner[ASK]._quotePeriod = quoteBannerPeriod;
        _banner[BID]._fillPeriod = _banner[ASK]._fillPeriod = fillBannerPeriod;
    }

    void initializeOrders() {
        createOrder(ORDER_ID_RV_INTERNAL_QUOTE, BID, Order::Strategy::RelativeValue, Order::Type::Quoting, _bidOrders, 1);
        createOrder(ORDER_ID_RV_INTERNAL_QUOTE, ASK, Order::Strategy::RelativeValue, Order::Type::Quoting, _askOrders, 1);
        createOrder(ORDER_ID_MM_QUOTE, BID, Order::Strategy::MarketMaker, Order::Type::Quoting, _bidOrders, 2);
        createOrder(ORDER_ID_MM_QUOTE, ASK, Order::Strategy::MarketMaker, Order::Type::Quoting, _askOrders, 2);
        createOrder(ORDER_ID_MM_HEDGE, BID, Order::Strategy::MarketMaker, Order::Type::Hedging, _bidOrders, 0);
        createOrder(ORDER_ID_MM_HEDGE, ASK, Order::Strategy::MarketMaker, Order::Type::Hedging, _askOrders, 0);
    }

    void createOrder(OrderIds id, uint32_t side, Order::Strategy strategy, Order::Type type, Order* orders, uint32_t matchOrder) {
        Order& order = orders[id];
        order._id = id;
        order._side = side;
        order._strategy = strategy;
        order._type = type;
        order._matchOrder = matchOrder;
    }

    Order& modifyAskOrder(OrderId_t orderId, QuoteOrderMessage::Quote& message) {
        return modifyOrder<ASK>(orderId, message);
    }

    Order& modifyBidOrder(OrderId_t orderId, QuoteOrderMessage::Quote& message) {
        return modifyOrder<BID>(orderId, message);
    }

    template <uint32_t Side>
    Order& modifyOrder(OrderId_t orderId, QuoteOrderMessage::Quote& message) {
        return modifyOrder<Side>(orderId, message._internalMarketPrice, message._internalMarketSize, message._externalPrice, message._externalSize, false, false);
    }

    Order& modifyAskOrder(OrderId_t orderId, hf::price_t internalPrice, hf::qty_t internalSize, hf::price_t externalPrice, hf::qty_t externalSize, bool persistPrice, bool payUp) {
        return modifyOrder<ASK>(orderId, internalPrice, internalSize, externalPrice, externalSize, persistPrice, payUp);
    }

    Order& modifyBidOrder(OrderId_t orderId, hf::price_t internalPrice, hf::qty_t internalSize, hf::price_t externalPrice, hf::qty_t externalSize, bool persistPrice, bool payUp) {
        return modifyOrder<BID>(orderId, internalPrice, internalSize, externalPrice, externalSize, persistPrice, payUp);
    }

    template <uint32_t Side>
    Order& modifyOrder(OrderId_t orderId, hf::price_t internalPrice, hf::qty_t internalSize, hf::price_t externalPrice, hf::qty_t externalSize, bool persistPrice, bool payUp) {
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
        order._inPayUp = payUp;

        if (externalSize != 0) {
            if (orderId == OrderIds::ORDER_ID_MM_HEDGE) {
                _hedgeManager[Side].setPending(externalPrice, externalSize, payUp);
            } else {
                PriceLevel* priceLevel = ToPriceLevelAdder<Side>().addToPriceLevel(*this, order, externalPrice, externalSize);
                order.priceLevel = priceLevel;
            }
        } else {
            const hf::price_t priceToPersist = persistPrice ? order._externalPrice : hf::NULL_PRICE;
            if (orderId == OrderIds::ORDER_ID_MM_QUOTE) {
                _marketMakerTopLevels[Side] = priceToPersist;
            }
            else if (orderId == OrderIds::ORDER_ID_RV_INTERNAL_QUOTE) {
                _relativeValueTopLevels[Side] = priceToPersist;
            }
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
            level.addOrder(&order);
            return &level;
        } else {
            auto result = _bidLevels.insert(std::pair<hf::price_t, PriceLevel>(price, {}));
            PriceLevel& level = result.first->second;
            level._price = price;
            level._size = size;
            level.addOrder(&order);
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

    void removeOrderFromLevel(Order* order) {
        if (order->_side == BID) {
            removeOrderFromLevel(order, _bidLevels);
        } else {
            removeOrderFromLevel(order, _askLevels);
        }
    }

    template <typename LevelsT>
    void removeOrderFromLevel(Order* order, LevelsT& levels) {
        auto it = levels.find(order->_externalPrice);
        if (it != levels.end()) {
            PriceLevel& level = it->second;
            level.remove(*order);
            if (level._size == 0) {
                removeLevel(&level, levels);
            }
        } else {
            BOOST_ASSERT(false);
        }
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
        const hf::price_t currentTopBid = getPersistedPrice(BID);
        PriceLevel* bidTopLevel = getTopLevel(_bidLevels);
        const hf::price_t newTopBid = bidTopLevel == nullptr ? hf::NULL_PRICE : bidTopLevel->_price;
        const uint64_t now = clockUtil::GetCycles();
        if ((currentTopBid != hf::NULL_PRICE) & (newTopBid != hf::NULL_PRICE)) {
            if (newTopBid > currentTopBid) {
                synchronizeAskOrders(now);
                synchronizeBidOrders(now);
            } else {
                synchronizeBidOrders(now);
                synchronizeAskOrders(now);
            }
        } else {
            synchronizeBidOrders(now);
            synchronizeAskOrders(now);
        }
    }

    void synchronizeBidOrders(uint64_t now) {
        _hedgeManager[BID].applyPendingUpdate();

        hf::price_t price;
        hf::qty_t size;
        getPriceAndSize(_bidLevels, BID, price, size);

        _logger.logInfo(boost::str(boost::format("IME. %1% BID Synchronize orders. Price=%2% Size=%3%") % _name % price % size));

        if (price != hf::NULL_PRICE) {
            synchronizeUnderBanners<BID>(_stacker[BID], price, size, now);
        } else {
            _stacker[BID].getOutOfMarket();
        }
    }

    void synchronizeAskOrders(uint64_t now) {
        _hedgeManager[ASK].applyPendingUpdate();

        hf::price_t price;
        hf::qty_t size;
        getPriceAndSize(_askLevels, ASK, price, size);

        _logger.logInfo(boost::str(boost::format("IME. %1% ASK Synchronize orders. Price=%2% Size=%3%") % _name % price % size));

        if (price != hf::NULL_PRICE) {
            synchronizeUnderBanners<ASK>(_stacker[ASK], price, size, now);
        } else {
            _stacker[ASK].getOutOfMarket();
        }
    }

    template <uint32_t Side, typename OrderManagerT>
    void synchronizeUnderBanners(OrderManagerT& manager, hf::price_t price, hf::qty_t size, uint64_t now) {
        const hf::price_t currentPrice = manager.getPrice();
        const hf::qty_t restingSize = manager.getRestingSize();
        bool isQuoteBetter = true;
        if (BOOST_LIKELY(currentPrice != hf::NULL_PRICE)) {
            isQuoteBetter = ((IsBetterPrice<Side>(price, currentPrice)) | ((price == currentPrice) & (size > restingSize)));
            LOG(0, "IME {C[i1]} {i2} Banner. QuoteBetter={i3} Current: {i4}@{d1} New={i5}@{d2} Now={i6}", getContractName(), Side, isQuoteBetter, restingSize, ToDollars(currentPrice), size, ToDollars(price), now)
        }
        Banner& banner = _banner[Side];
        if (isQuoteBetter) {
            if (banner.isBanned(now)) {
                LOG(0, "IME {C[i1]} {i2} Banner. Quote under ongoing ban. Not updating. Price={d1} Size={i3} Now={i4}", getContractName(), Side, ToDollars(price), size, now)
            } else {
                manager.template quote<Side>(price, size);
                LOG(0, "IME {C[i1]} {i2} Banner. Quote kicked off. Now={i3} Price={d1} Qty={i4}", getContractName(), Side, now, ToDollars(price), size)
                startQuoteBanner<Side>();
            }
        } else {
            manager.template quote<Side>(price, size);
            if (banner.isBanned(now)) {
                banner.expire();
                LOG(0, "IME {C[i1]} {i2} Banner. Expired ban because worse quote. Now={i3}", getContractName(), Side, now)
            }
        }
    }

    template <uint32_t Side>
    void startQuoteBanner() {
        uint64_t now = clockUtil::GetCycles();
        uint64_t expiryTime = now + _banner[Side]._quotePeriod;
        _banner[Side]._expiryTime = expiryTime;
        _executor->addBannerTimer(expiryTime, _internalId);
        LOG(0, "IME {C[i1]} {i2} Banner. Starting ban period due to quote. Now={i3} Expiry={i4}", getContractName(), Side, now, expiryTime)
    }

    template <uint32_t Side>
    void startFillBanner() {
        uint64_t now = clockUtil::GetCycles();
        const uint64_t expiryTime = clockUtil::GetCycles() + _banner[Side]._fillPeriod;
        _banner[Side]._expiryTime = std::max(_banner[Side]._expiryTime, expiryTime);
        _executor->addBannerTimer(expiryTime, _internalId);
        LOG(0, "IME {C[i1]} {i2} Banner. Starting ban period due to fill. Now={i3} Expiry={i4}", getContractName(), Side, now, expiryTime)
    }

    void expireBanners() {
        uint64_t now = clockUtil::GetCycles();
        expireBanner<BID>(now);
        expireBanner<ASK>(now);
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
                    LOG(0, "IME {C[i1]} {i2} Banner. Putting quote back in market after ban. Now={i3} Price={d1} Size={i4}", getContractName(), Side, now, ToDollars(price), size)
                    _stacker[Side].template quote<Side>(price, size);
                } else {
                    LOG(0, "IME {C[i1]} {i2} Banner. Could not put quote back. Nothing in price level. Now={i3}", getContractName(), Side, now)
                }
                banner.expire();
                LOG(0, "IME {C[i1]} {i2} Banner. Expired ban. Now={i3}", getContractName(), Side, now)
            }
        }
    }

    template <typename T>
    void getPriceAndSize(T& levels, uint32_t side, hf::price_t& price, hf::qty_t& size) {
        PriceLevel* level = getTopLevel(levels);
        if (level != nullptr) {
            price = level->_price;
            size = level->_size;
            persistPrices(side);
        } else {
            price = getPersistedPrice(side);
            size = 0;
        }
    }

    hf::price_t getPersistedPrice(uint32_t side) {
        const hf::price_t relativeValuePrice = _relativeValueTopLevels[side];
        const hf::price_t marketMakerPrice = _marketMakerTopLevels[side];
        if ((relativeValuePrice != hf::NULL_PRICE) & (marketMakerPrice != hf::NULL_PRICE)) {
            if (side == BID) {
                return std::max(relativeValuePrice, marketMakerPrice);
            } else {
                return std::min(relativeValuePrice, marketMakerPrice);
            }
        } else {
            if (relativeValuePrice != hf::NULL_PRICE) {
                return relativeValuePrice;
            } else {
                return marketMakerPrice;
            }
        }
    }

    void persistPrices(uint32_t side) {
        if (side == BID) {
            persistPrice(_bidOrders[OrderIds::ORDER_ID_MM_QUOTE], side, _marketMakerTopLevels[side]);
            persistPrice(_bidOrders[OrderIds::ORDER_ID_RV_INTERNAL_QUOTE], side, _relativeValueTopLevels[side]);
        } else {
            persistPrice(_askOrders[OrderIds::ORDER_ID_MM_QUOTE], side, _marketMakerTopLevels[side]);
            persistPrice(_askOrders[OrderIds::ORDER_ID_RV_INTERNAL_QUOTE], side, _relativeValueTopLevels[side]);
        }
    }

    void persistPrice(Order& order, uint32_t side, hf::price_t& priceToPersistTo) {
        if (order._isInBook) {
            priceToPersistTo = order._externalPrice;
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
            return tryMatchAsk(price, size, tolerance, false);
        } else {
            return tryMatchBid(price, size, tolerance, false);
        }
    }

    hf::qty_t tryMatchAsk(hf::price_t askPrice, hf::qty_t askSize, hf::price_t tolerance, bool payUp) {
        BidOrdersMapIt bidOrdersIt = _bids.begin();
        if (bidOrdersIt == _bids.end()) {
            return askSize;
        }

        askPrice -= tolerance;

        Order* bidOrder = bidOrdersIt->second;
        hf::qty_t bidSize = bidOrder->_size;
        hf::price_t bidPrice = bidOrdersIt->first;

        while (bidPrice >= askPrice) {
            if ((bidOrder->_strategy == Order::Strategy::MarketMaker) & payUp) {
                ++bidOrdersIt;
                const bool hasBidOrder = getNextOrder(bidOrder, bidOrdersIt, _bids, bidPrice);
                if (!hasBidOrder) {
                    return askSize;
                }
                continue;
            }
            if (bidSize == askSize) {
                _executor->onInternalFill(_internalId, bidOrder, bidPrice, bidSize);
                _bids.erase(bidOrdersIt);
                removeOrderFromLevel(bidOrder);
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
                removeOrderFromLevel(bidOrder);
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

    hf::qty_t tryMatchBid(hf::price_t bidPrice, hf::qty_t bidSize, hf::price_t tolerance, bool payUp) {
        AskOrdersMapIt askOrdersIt = _asks.begin();

        if (askOrdersIt == _asks.end()) {
            return bidSize;
        }

        bidPrice += tolerance;

        hf::price_t askPrice = askOrdersIt->first;
        Order* askOrder = askOrdersIt->second;
        hf::qty_t askSize = askOrder->_size;

        while (bidPrice >= askPrice) {
            if ((askOrder->_strategy == Order::Strategy::MarketMaker) & payUp) {
                ++askOrdersIt;
                const bool hasAskOrder = getNextOrder(askOrder, askOrdersIt, _asks, askPrice);
                if (!hasAskOrder) {
                    return bidSize;
                }
                continue;
            }
            if (bidSize == askSize) {
                _executor->onInternalFill(_internalId, askOrder, askPrice, askSize);
                _asks.erase(askOrdersIt);
                removeOrderFromLevel(askOrder);
                return 0;
            }
            else if (bidSize > askSize) {
                const hf::qty_t fillSize = askSize;
                _executor->onInternalFill(_internalId, askOrder, askPrice, fillSize);
                askOrdersIt = _asks.erase(askOrdersIt);
                removeOrderFromLevel(askOrder);
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

    struct InternalFill {
        Order* _order;
        hf::price_t _price;
        hf::qty_t _size;
        bool _payUp = false;
    };

    bool matchOrders() {
        LOG(0, "IME %1% Internally Match Orders", getContractName())

        auto bidOrdersIt = _bids.begin();

        if (bidOrdersIt == _bids.end()) {
            return false;
        }

        auto askOrdersIt = _asks.begin();

        if (askOrdersIt == _asks.end()) {
            return false;
        }

        hf::price_t bidTop = bidOrdersIt->first;
        hf::price_t askTop = askOrdersIt->first;
        const bool hasMatch = bidTop >= askTop;

        if (hasMatch) {
            Order* bidOrder = bidOrdersIt->second;
            Order* askOrder = askOrdersIt->second;

            if (bidOrder->_inPayUp) {
                hf::qty_t unfilledSize = tryMatchBid(bidOrder->_price, bidOrder->_size, 0, true);
                const hf::qty_t fillSize = bidOrder->_size - unfilledSize;
                if (fillSize > 0) {
                    _internalFills.push_back({bidOrder, bidTop, fillSize, true});
                }
                _hedgeManager[BID].setPending(bidOrder->_externalPrice, unfilledSize, true);
                bidOrdersIt = _bids.erase(bidOrdersIt);
                askOrdersIt = _asks.begin();
                if ((askOrdersIt == _asks.end()) | (bidOrdersIt == _bids.end())) {
                    return true;
                }
                bidOrder = bidOrdersIt->second;
                askOrder = askOrdersIt->second;
                bidTop = bidOrdersIt->first;
                askTop = askOrdersIt->first;
            } else if (askOrder->_inPayUp) {
                hf::qty_t unfilledSize = tryMatchAsk(askOrder->_price, askOrder->_size, 0, true);
                const hf::qty_t fillSize = askOrder->_size - unfilledSize;
                if (fillSize > 0) {
                    _internalFills.push_back({askOrder, askTop, fillSize, true});
                }
                _hedgeManager[ASK].setPending(askOrder->_price, unfilledSize, true);
                askOrdersIt = _asks.erase(askOrdersIt);
                bidOrdersIt = _bids.begin();
                if ((askOrdersIt == _asks.end()) | (bidOrdersIt == _bids.end())) {
                    return true;
                }
                bidOrder = bidOrdersIt->second;
                askOrder = askOrdersIt->second;
                bidTop = bidOrdersIt->first;
                askTop = askOrdersIt->first;
            }

            hf::qty_t bidSize = bidOrder->_size;
            hf::qty_t askSize = askOrder->_size;

            while (bidTop >= askTop) {
                LOG(0, "IME {C[i1]} Internal Cross. Matching. Bid: {i2}@{d1} Id={i3} Ask={i4}@{d2} Id={i5}", getContractName(), ToDollars(bidTop), bidSize, bidOrder->_id, ToDollars(askTop), askSize, askOrder->_id)
                if (bidSize == askSize) {
                    LOG(0, "IME {C[i1]} Match. Equal size. Size={i2}", getContractName())
                    _internalFills.push_back({bidOrder, bidTop, bidSize});
                    _internalFills.push_back({askOrder, bidTop, bidSize});
                    bidOrdersIt = _bids.erase(bidOrdersIt);
                    askOrdersIt = _asks.erase(askOrdersIt);
                    removeOrderFromLevel(bidOrder);
                    removeOrderFromLevel(askOrder);
                    const bool hasBidOrder = getNextOrder(bidOrder, bidOrdersIt, _bids, bidTop);
                    const bool hasAskOrder = getNextOrder(askOrder, askOrdersIt, _asks, askTop);
                    if ((!hasAskOrder) | (!hasBidOrder)) {
                        LOG(0, "IME {C[i1]} No more orders. Exiting matching.", getContractName())
                        break;
                    }
                    bidSize = bidOrder->_size;
                    askSize = askOrder->_size;
                    LOG(0, "IME {C[i1]} Got new orders.", getContractName())
                }
                else if (bidSize > askSize) {
                    LOG(0, "IME {C[i1]} Match. Bid size > Ask size. MatchSize={i2}", getContractName(), askSize)
                    const hf::qty_t fillSize = askSize;
                    _internalFills.push_back({bidOrder, bidTop, fillSize});
                    _internalFills.push_back({askOrder, bidTop, fillSize});
                    askOrdersIt = _asks.erase(askOrdersIt);
                    removeOrderFromLevel(askOrder);
                    bidOrder->_size -= fillSize;
                    const bool hasAskOrder = getNextOrder(askOrder, askOrdersIt, _asks, askTop);
                    if (!hasAskOrder) {
                        LOG(0, "IME {C[i1]} No more ask orders. Exiting matching.", getContractName())
                        break;
                    }
                    askSize = askOrder->_size;
                    bidSize -= fillSize;
                    LOG(0, "IME {C[i1]} Got new ask order", getContractName())
                } else {
                    LOG(0, "IME {C[i1]} Match. Bid size < Ask size. MatchSize={i2}", getContractName(), bidSize)
                    const hf::qty_t fillSize = bidSize;
                    _internalFills.push_back({bidOrder, bidTop, fillSize});
                    _internalFills.push_back({askOrder, bidTop, fillSize});
                    bidOrdersIt = _bids.erase(bidOrdersIt);
                    removeOrderFromLevel(bidOrder);
                    askOrder->_size -= fillSize;
                    const bool hasBidOrder = getNextOrder(bidOrder, bidOrdersIt, _bids, bidTop);
                    if (!hasBidOrder) {
                        LOG(0, "IME {C[i1]} No more bid orders. Exiting matching.", getContractName())
                        break;
                    }
                    bidSize = bidOrder->_size;
                    askSize -= fillSize;
                    LOG(0, "IME {C[i1]} Got new bid order", getContractName())
                }
            }

            for (auto& fill : _internalFills) {
                _executor->onInternalFill(_internalId, fill._order, fill._price, fill._size, fill._payUp);
            }
            _internalFills.clear();
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
        LOG(0, "IME {C[i1]} External fill. Allocating to internal orders. Side={i2} Price={d1} Size={i3}", getContractName(), ToViperSide(order.side), event.filledPrice, event.filledQty)
        hf::qty_t filledSize = event.filledQty;
        for (auto levelIt = levels.begin(); levelIt != levels.end(); ) {
            auto& level = levelIt->second;
            auto& orders = level._orders;
            for (auto orderIt = orders.begin(); orderIt != orders.end(); ) {
                LOG(0, "IME {C[i1]} Matching on level. Price={d1} NumberOfOrders={i2}", getContractName(), ToDollars(level._price), level._orders.size())
                Order* o = *orderIt;
                if (o->_externalSize == filledSize) {
                    LOG(0, "IME {C[i1]} Match. Equal size. Order={i2} Price={d1} Size={i3}", getContractName(), o->_id, ToDollars(o->_externalPrice), filledSize)
                    _executor->onExternalFill(_internalId, o, event.filledPrice, filledSize);
                    orderIt = orders.erase(orderIt);
                    if (orderIt == orders.end()) {
                        LOG(0, "IME {C[i1]} Erasing level. No more orders. Price={d1}", getContractName(), ToDollars(level._price))
                        levels.erase(levelIt);
                    } else {
                        level._size -= filledSize;
                    }
                    removeOrderFromMap(o, map);
                    return;
                } else if (o->_externalSize < filledSize) {
                    const hf::qty_t oneFill = o->_externalSize;
                    LOG(0, "IME {C[i1]} Match. Order size < filled size. Order={i2} Price={d1} Size={i3)", getContractName(), o->_id,  ToDollars(o->_externalPrice), oneFill)
                    _executor->onExternalFill(_internalId, o, event.filledPrice, oneFill);
                    orderIt = orders.erase(orderIt);
                    removeOrderFromMap(o, map);
                    filledSize -= oneFill;
                    if (orderIt == orders.end()) {
                        LOG(0, "IME {C[i1]} Erasing level. No more orders. Price={d1}", getContractName(), ToDollars(level._price))
                        levelIt = levels.erase(levelIt);
                        break;
                    } else {
                        level._size -= filledSize;
                    }
                } else {
                    LOG(0, "IME {C[i1]} Match. Order size > filled size. Order={i2} Price={d1} Size={i3}", getContractName(), o->_id, ToDollars(o->_externalPrice), filledSize)
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
            LOG(0, "IME {C[i1]} Overfill. Price={d1} Size={i2}", getContractName(), event.filledPrice, filledSize)
            const uint32_t side = ToViperSide(order.side);
            _executor->onExternalOverFill(_internalId, side, event.filledPrice, filledSize);
        }
    }

    void getAllOutOfMarket() {
        LOG(0, "IME {C[i1]} Removing all orders from market.", getContractName())
        removeBidOrder(ORDER_ID_RV_INTERNAL_QUOTE);
        removeBidOrder(ORDER_ID_MM_QUOTE);
        removeBidOrder(ORDER_ID_MM_HEDGE);
        removeAskOrder(ORDER_ID_RV_INTERNAL_QUOTE);
        removeAskOrder(ORDER_ID_MM_QUOTE);
        removeAskOrder(ORDER_ID_MM_HEDGE);
        _marketMakerTopLevels[BID] = hf::NULL_PRICE;
        _marketMakerTopLevels[ASK] = hf::NULL_PRICE;
        _relativeValueTopLevels[BID] = hf::NULL_PRICE;
        _relativeValueTopLevels[ASK] = hf::NULL_PRICE;
        _stacker[BID].getOutOfMarket();
        _stacker[ASK].getOutOfMarket();
    }

    void getMarketMakerOutOfMarket() {
        LOG(0, "IME {C[i1]} Removing market maker orders from market.", getContractName())
        removeBidOrder(ORDER_ID_MM_QUOTE);
        removeBidOrder(ORDER_ID_MM_HEDGE);
        removeAskOrder(ORDER_ID_MM_QUOTE);
        removeAskOrder(ORDER_ID_MM_HEDGE);
        _marketMakerTopLevels[BID] = hf::NULL_PRICE;
        _marketMakerTopLevels[ASK] = hf::NULL_PRICE;
        synchronizeOrders();
    }

    void getRelativeValueOutOfMarket() {
        LOG(0, "IME {C[i1]} Removing relative value orders from market.", getContractName())
        removeBidOrder(ORDER_ID_RV_INTERNAL_QUOTE);
        removeAskOrder(ORDER_ID_RV_INTERNAL_QUOTE);
        _relativeValueTopLevels[BID] = hf::NULL_PRICE;
        _relativeValueTopLevels[ASK] = hf::NULL_PRICE;
        synchronizeOrders();
    }

    uint32_t getSide(hf::SIDE side) {
        return side == hf::SIDE::BUY ? 0 : 1;
    }

    void allocateHedgerFill(const om::OMExecutionEvent &event, const om::order_t& order, Order& internalOrder) {
        bool payUp = order.debug.i[PAY_UP] == PAY_UP_YES;
        _executor->onExternalFill(_internalId, &internalOrder, event.filledPrice, event.filledQty, payUp);
    }

    void returnOrderToHedgeManager(const om::order_t& order, uint32_t side) {
        _hedgeManager[side].returnOrderToHedgeManager(order);
    }

    void onOrderExecution(const om::OMExecutionEvent &event, const om::order_t &order) {
        int64_t orderOwner = order.debug.i[ORDER_OWNER];

        if (order.side == hf::SIDE::BUY) {
            if (orderOwner == STACKER_ORDER) {
                onOrderExecution(event, order, _bidLevels, _bids);
            } else {
                allocateHedgerFill(event, order, _bidOrders[OrderIds::ORDER_ID_MM_HEDGE]);
            }
            startFillBanner<BID>();
        } else {
            if (orderOwner == STACKER_ORDER) {
                onOrderExecution(event, order, _askLevels, _asks);
            } else {
                allocateHedgerFill(event, order,_askOrders[OrderIds::ORDER_ID_MM_HEDGE]);
            }
            startFillBanner<ASK>();
        }

        const uint32_t side = getSide(order.side);

        if (orderOwner == STACKER_ORDER) {
            _stacker[side].onOrderExecution(event, order);
        } else {
            _hedgeManager[side].onOrderExecution(event, order);
        }
        _executor->onEndOfExternalFills(_internalId, order.side);
    }

    void onOrderAccepted(const om::OMOrderAcceptedEvent& event, const om::order_t& order) {
        const uint32_t side = getSide(order.side);
        int64_t orderOwner = order.debug.i[ORDER_OWNER];
        if (orderOwner == STACKER_ORDER) {
            _stacker[side].onOrderAccepted(event, order);
        } else {
            _hedgeManager[side].onOrderAccepted(event, order);
        }
    }

    void onOrderRejected(const om::OMOrderRejectedEvent& event, const om::order_t& order) {
        const uint32_t side = getSide(order.side);
        int64_t orderOwner = order.debug.i[ORDER_OWNER];
        if (orderOwner == STACKER_ORDER) {
            _stacker[side].onOrderRejected(event, order);
        } else {
            _hedgeManager[side].onOrderRejected(event, order);
        }
    }

    void onOrderCanceled(const om::OMOrderCanceledEvent& event, const om::order_t& order) {
        const uint32_t side = getSide(order.side);
        int64_t orderOwner = order.debug.i[ORDER_OWNER];
        if (orderOwner == STACKER_ORDER) {
            _stacker[side].onOrderCanceled(event, order);
        } else {
            _hedgeManager[side].onOrderCanceled(event, order);
        }
    }

    void onOrderReplaced(const om::OMOrderModifiedEvent& event, const om::order_t& order) {
        const uint32_t side = getSide(order.side);
        _stacker[side].onOrderReplaced(event, order);
    }

    void onOrderReplaceRejected(const om::OMOrderModifyRejectedEvent& event, const om::order_t& order) {
        const uint32_t side = getSide(order.side);
        _stacker[side].onOrderReplaceRejected(event, order);
    }

    void onOrderCancelRejected(const om::OMOrderCancelRejectedEvent &event, const om::order_t &order) {
        const uint32_t side = getSide(order.side);
        int64_t orderOwner = order.debug.i[ORDER_OWNER];
        if (orderOwner == STACKER_ORDER) {
            _stacker[side].onOrderCancelRejected(event, order);
        } else {
            _hedgeManager[side].onOrderCancelRejected(event, order);
        }
    }

    void handleStackerParameterUpdate(const proto::GridCell& gridCell) {
        StackerParametersColumns columnType = static_cast<StackerParametersColumns>(gridCell.col());
        switch (columnType) {
            case StackerParametersColumns::QuoteSize: {
                hf::qty_t size = atoi(gridCell.value().c_str());
                    _stacker[BID].updateQuoteSize(size, _stackerEnabled);
                    _stacker[ASK].updateQuoteSize(size, _stackerEnabled);
                break;
            }
            case StackerParametersColumns::Enabled: {
                bool enabled = 1 == atoi(gridCell.value().c_str());
                if (enabled != _stackerEnabled) {
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

    void handleQuoteBannerUpdate(uint64_t period) {
        _banner[BID]._quotePeriod = period;
        _banner[ASK]._quotePeriod = period;
        LOG(0, "IME {C[i1]}. quote banner changed. Value={i2}", getContractName(), period)
    }

    void handleFillBannerUpdate(uint64_t period) {
        _banner[BID]._fillPeriod = period;
        _banner[ASK]._fillPeriod = period;
        LOG(0, "IME {C[i1]}. fill banner changed. Value={i2}", getContractName(), period)
    }

    void sendExternalRvHedge(HedgeOrderMessage& order) {
        _executor->sendExternalRvHedge(order);
    }

    void notifyLevelFree(uint32_t side, hf::price_t price) {
        uint32_t otherSide = GetOtherSide(side);
        _stacker[otherSide].onLevelFree(price);
    }

    void notifyLevelUsed(uint32_t side, hf::price_t price) {
        uint32_t otherSide = GetOtherSide(side);
        _stacker[otherSide].onLevelUsed(price);
    }

    Logger& getLogger() {
        return _logger;
    }

#ifdef IS_DEV
    std::string getContractName() {
        return _name;
    }
#else
    uint32_t getContractName() {
            return _internalId;
        }
#endif

    BidOrdersMap _bids;
    AskOrdersMap _asks;
    std::map<hf::price_t, PriceLevel, std::greater<hf::price_t>> _bidLevels;
    std::map<hf::price_t, PriceLevel> _askLevels;
    hf::price_t _marketMakerTopLevels[2];
    hf::price_t _relativeValueTopLevels[2];

    Order _bidOrders[3];
    Order _askOrders[3];

    bool _stackerEnabled = true;
    Stacker<MeT> _stacker[2];
    HedgesManager<MeT> _hedgeManager[2];

    ExecutorT* _executor;

    uint32_t _internalId;

    Banner _banner[2];

    std::vector<InternalFill> _internalFills;

    std::string _name;
    Logger _logger;
};

}

#endif //VIPER_INTERNAL_MATCHING_ENGINE_H
