#ifndef VIPER_INTERNAL_MATCHING_ENGINE_H
#define VIPER_INTERNAL_MATCHING_ENGINE_H

#include <map>
#include <hf/hf.h>
#include <boost/assert.hpp>
#include "Common.h"
#include "OrderManager.h"
#include "ViperConfig.h"
#include "ClockUtil.h"
#include <vector>
#include "OrderRequestManager.h"
#include "SimpleMarketMaker.h"
#include "MarketMaker.h"
#include <unordered_set>

namespace Python {

struct Messages {
    uint32_t _inserts = 0;
    uint32_t _modifies = 0;
    uint32_t _cancels = 0;
    uint32_t _cancelRejects = 0;
    uint32_t _modifyRejects = 0;
    hf::qty_t _fills = 0;

    uint32_t getTotalOutgoing() {
        return _inserts + _modifies + _cancels;
    }
};

struct Banner {
    hf::price_t _price;
    hf::qty_t _qty;
    uint64_t _expiryTime = 0;
    uint64_t _banPeriod = 0;
    THROTTLING_MODE _mode;

    bool isBanned(uint64_t now) {
        return _expiryTime > now;
    }

    bool isBannedUntilReset() {
        return wasRunning();
    }

    bool wasRunning() {
        return _expiryTime != 0;
    }

    void expire() {
        _expiryTime = 0;
    }
};

struct FillBanner {

    struct Entry {
        hf::price_t _price;
        hf::qty_t _size;
        uint64_t _expiryTime = 0;
        uint32_t _isReject = 0;
    };

    FillBanner() {
        _entries.reserve(65536);
    }

    static bool sortMeForBid(const Entry& lhs, const Entry& rhs) {
        if (lhs._price > rhs._price) {
            return true;
        }
        else if (lhs._price == rhs._price) {
            return lhs._size > rhs._size;
        }
        return false;
    }

    static bool sortMeForAsk(const Entry& lhs, const Entry& rhs) {
        if (lhs._price < rhs._price) {
            return true;
        }
        else if(lhs._price == rhs._price) {
            return lhs._size > rhs._size;
        }
        return false;
    }

    void addEntry(hf::price_t price, hf::qty_t size, uint64_t expiry, uint32_t isReject = 0) {
        _entries.push_back({price, size, expiry, isReject});
        if (_entries.size() > 1) {
            if (_side == BID) {
                std::sort(_entries.begin(), _entries.end(), sortMeForBid);
            } else {
                std::sort(_entries.begin(), _entries.end(), sortMeForAsk);
            }
        }
    }

    bool isBanned() {
        return !_entries.empty();
    }

    void expireEntries(uint64_t now) {
        if (!_entries.empty()) {
            for (auto it = _entries.begin(); it != _entries.end(); ) {
                Entry& entry = *it;
                if (entry._expiryTime < now) {
                    _rejects -= entry._isReject;
                    _hadRejects |= (entry._isReject == 1);
                    it = _entries.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }

    hf::price_t getPrice() {
        return  _entries.front()._price;
    }

    hf::qty_t getSize() {
        return _entries.front()._size;
    }

    bool getRejectsFlag() {
        return (_rejects != 0) | _hadRejects ;
    }

    void resetHadRejects() {
        _hadRejects = false;
    }

    std::vector<Entry> _entries;
    uint64_t _banPeriod = 0;
    uint64_t _rejectBanPeriod = 0;
    uint32_t _side;
    uint32_t _rejects = 0;
    bool _hadRejects = false;
};

typedef std::unordered_map<OrderId_t, Order> OrdersT;
typedef std::multimap<hf::price_t, Order*, std::greater<hf::price_t>> BidOrdersMap;
typedef BidOrdersMap::iterator BidOrdersMapIt;
typedef std::multimap<hf::price_t, Order*> AskOrdersMap;
typedef AskOrdersMap::iterator AskOrdersMapIt;

template <uint32_t Side>
inline hf::price_t adjustPrice(hf::price_t price, hf::price_t tolerance);

template <>
inline hf::price_t adjustPrice<BID>(hf::price_t price, hf::price_t tolerance) {
    return price + tolerance;
}

template <>
inline hf::price_t adjustPrice<ASK>(hf::price_t price, hf::price_t tolerance) {
    return price - tolerance;
}

template <uint32_t Side>
struct Remover {
    template <typename T>
    void remove(T& t, Order& o);

    template <typename T>
    void remove(T& t, StrategyId strategyId);
};

template <>
template <typename T>
void Remover<BID>::remove(T& t, Order& o) {
    t.doRemoveOrder(o, t._bids, t._bidLevels);
}

template <>
template <typename T>
void Remover<BID>::remove(T& t, StrategyId strategyId) {
    t.doRemoveOrder(strategyId, t._bids, t._bidLevels);
}

template <>
template <typename T>
void Remover<ASK>::remove(T& t, Order& o) {
    t.doRemoveOrder(o, t._asks, t._askLevels);
}

template <>
template <typename T>
void Remover<ASK>::remove(T& t, StrategyId strategyId) {
    t.doRemoveOrder(strategyId, t._bids, t._bidLevels);
}

template<uint32_t Side>
struct ContainerSelector {
    template<typename T>
    auto& getOrders(T& t);

    template<typename T>
    auto& getInternalOrderMap(T& t);

    template<typename T>
    auto& getExternalLevels(T& t);
};

template<>
template<typename T>
auto& ContainerSelector<BID>::getOrders(T& t) {
    return t._bidOrders;
}
template<>
template<typename T>
auto& ContainerSelector<ASK>::getOrders(T& t) {
    return t._askOrders;
}

template<>
template<typename T>
auto& ContainerSelector<BID>::getInternalOrderMap(T& t) {
    return t._bids;
}
template<>
template<typename T>
auto& ContainerSelector<ASK>::getInternalOrderMap(T& t) {
    return t._asks;
}

template<>
template<typename T>
auto& ContainerSelector<BID>::getExternalLevels(T& t) {
    return t._bidLevels;
}
template<>
template<typename T>
auto& ContainerSelector<ASK>::getExternalLevels(T& t) {
    return t._askLevels;
}

#define MAX_SELF_MATCH_THRESHOLD 50

template <typename ExecutorT>
class InternalMatchingEngine {
    OrderRequestManager& _orderRequestMgr = OrderRequestManager::get();
    OrderRequestQueue* _orderRequestQueue;
    std::unordered_map<uint32_t, std::vector<StrategyId>> _internalMarketSubscriptions;
    std::unordered_set<uint8_t> _internalMarketNotification;
    uint32_t _selfMatchCounter = 0;

public:

    typedef InternalMatchingEngine<ExecutorT> MeT;
    using ExecutorContractStateT = typename ExecutorT::ContractStateT;

    InternalMatchingEngine()
        : _reconcileGtcOrders(3)
    {
        for (auto& topLevelBySide : _topLevelsByStrategyBySide) {
            topLevelBySide[BID] = hf::NULL_PRICE;
            topLevelBySide[ASK] = hf::NULL_PRICE;
        }
    }

    void initialize(ExecutorT* executor, const ConfigContract& contract, const StackerConfig& stackerConfig,
                    hf::price_t rvHedgeMatchTolerance, uint64_t fillBannerPeriod, uint64_t quoteBannerPeriod,
                    uint64_t hedgePeriod, uint64_t selfMatchBannerPeriod, Book(& marketBooks)[2], bool iceCancelResting) {
        _executor = executor;
        _om[BID].initialize(contract, BID, stackerConfig, this, iceCancelResting);
        _om[ASK].initialize(contract, ASK, stackerConfig, this, iceCancelResting);
        _contract = contract;
        _internalId = contract._internalId;
        _name = contract._name;
        _relativeValueHedgeMatchingTolerance = rvHedgeMatchTolerance;
        _quoteBanner[BID]._banPeriod = _quoteBanner[ASK]._banPeriod = quoteBannerPeriod;
        _fillBanner[BID]._banPeriod = _fillBanner[ASK]._banPeriod = fillBannerPeriod;
        _fillBanner[BID]._rejectBanPeriod = _fillBanner[ASK]._rejectBanPeriod = selfMatchBannerPeriod;
        _fillBanner[BID]._side = BID;
        _fillBanner[ASK]._side = ASK;
        _hedgeBanner[BID]._banPeriod = _hedgeBanner[ASK]._banPeriod = hedgePeriod;
        _marketBooks = marketBooks;
        _maxOffTopLevel = contract._tickSize * stackerConfig._maxTicksOffTopLevel;
        _tickSize = contract._tickSize;
        _orderRequestQueue = &_orderRequestMgr.getRequestQueues(_internalId);
    }

    void registerOrderClient(const RegisterOrderClient& req) {
        createOrder(BID, req._strategy, Order::Type::Quoting, _bidOrders);
        createOrder(BID, req._strategy, Order::Type::Hedging, _bidOrders);
        createOrder(ASK, req._strategy, Order::Type::Quoting, _askOrders);
        createOrder(ASK, req._strategy, Order::Type::Hedging, _askOrders);
        for (uint32_t i = 0; i < req._subscriptionCount; ++i) {
            LOG(0, "IME {C[i1]}. {i2} subscribe {i3} internal market update", getContractName(), strategyName(req._strategy),
                strategyName(req._internalMarketSubscriptions[i]))
            _internalMarketSubscriptions[toUInt(req._strategy)].emplace_back(req._internalMarketSubscriptions[i]);
        }
    }

    void createOrder(uint32_t side,
                     StrategyId strategy,
                     Order::Type type,
                     Order* orders) {
        auto id = orderId(strategy, type);
        uint32_t matchOrder = (id == 1) ? MAX_ORDER_ID : id;
        Order& order = orders[id];
        order._id = id;
        order._side = side;
        order._strategy = strategy;
        order._type = type;
        order._orderType = (id == 0) ? om::ORDER_TYPE::LMT : om::ORDER_TYPE::FAK;
        order._matchOrder = matchOrder;
    }

    template <uint32_t Side>
    void modifyOrder(Order& order,
                     hf::price_t internalPrice,
                     hf::qty_t internalSize,
                     hf::price_t externalPrice,
                     hf::qty_t externalSize,
                     bool persistPrice,
                     bool payUp) {

        removeOrder<Side>(order);

        order._price = internalPrice;
        order._size = internalSize;
        order._externalPrice = externalPrice;
        order._externalSize = externalSize;
        order._inPayUp = payUp;

        if (order._type == Order::Type::Hedging) {
            _pendingHedge[Side] = &order;
            if (internalPrice != hf::NULL_PRICE && internalSize != 0) {
                insertOrder<Side>(order);
                LOG(0, "IME {C[i1]}. {i2} Add order to internal orders. ID={i3}", getContractName(), Side, order._id)
            } else {
                LOG(0, "IME {C[i1]}. {i2} Did not add order to internal orders. ID={i3}", getContractName(), Side, order._id)
            }
        } else {
            if (internalPrice != hf::NULL_PRICE && internalSize != 0) {
                insertOrder<Side>(order);
                LOG(0, "IME {C[i1]}. {i2} Add order to internal orders. ID={i3}", getContractName(), Side, order._id)
            } else {
                LOG(0, "IME {C[i1]}. {i2} Did not add order to internal orders. ID={i3}", getContractName(), Side, order._id)
            }
            const hf::price_t priceToPersist = persistPrice ? order._externalPrice : hf::NULL_PRICE;
            _topLevelsByStrategyBySide[toUInt(order._strategy)][Side] = priceToPersist;

            if (externalPrice != hf::NULL_PRICE && externalSize != 0) {
                PriceLevel* priceLevel = addToPriceLevel<Side>(order);
                LOG(0, "IME {C[i1]}. {i2} Added order to level. Level={d1} LevelSize={i3} ID={i4}", getContractName(),
                    order._side, ToDollars(order._externalPrice), priceLevel->getSize(), order._id)
            } else {
                LOG(0, "IME {C[i1]}. {i2} Did not add order to price level. ID={i3}", getContractName(), Side, order._id)
            }
            PriceLevel* topLevel = getTopLevel(getExternalLevels<Side>());
            if (topLevel != nullptr) {
                LOG(0, "IME {C[i1]}. {i2} TopLevel={i3}@{d1}", getContractName(), Side, topLevel->getSize(), ToDollars(topLevel->_price))
            } else {
                LOG(0, "IME {C[i1]}. {i2} TopLevel=None", getContractName(), Side)
            }
        }
    }

    template <uint32_t Side>
    void insertOrder(Order& order) {
        auto& internalOrderMap = getInternalOrderMap<Side>();
        internalOrderMap.emplace(order._price, &order);
    }

    template <uint32_t Side>
    void removeOrder(Order& order) {
        Remover<Side> remover;
        remover.remove(*this, order);
    }

    template<uint32_t Side>
    auto& getOrders() {
        ContainerSelector<Side> selector;
        return selector.getOrders(*this);
    }

    template<uint32_t Side>
    auto& getExternalLevels() {
        ContainerSelector<Side> selector;
        return selector.getExternalLevels(*this);
    }

    template<uint32_t Side>
    auto& getInternalOrderMap() {
        ContainerSelector<Side> selector;
        return selector.getInternalOrderMap(*this);
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

    template<uint32_t Side>
    void removeOrderByStrategy(StrategyId strategyId) {
        for (auto& order : getOrders<Side>()) {
            if (order._strategy == strategyId) {
                removeOrder<Side>(order);
                order._price = hf::NULL_PRICE;
                order._externalPrice = hf::NULL_PRICE;
                order._size = 0;
                order._externalSize = 0;
            }
        }
    }



    void removeOrderFromLevel(Order* order) {
        if (order->_type == Order::Type::Hedging || order->priceLevel == nullptr) {
            return;
        }

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
            doRemoveOrderFromLevel(&level, *order, levels);
        } else {
            BOOST_ASSERT(false);
        }
    }

    template <typename LevelsT>
    void doRemoveOrderFromLevel(PriceLevel* level, Order& order, LevelsT& levels) {
        level->remove(order);
        LOG(0, "IME {C[i1]}. {i2} Removed order from level. Level={d1} LevelSize={i3} ID={i4}", getContractName(), order._side, ToDollars(order._externalPrice), level->getSize(), order._id)
        if (level->_orders.empty()) {
            removeLevel(level, levels);
            LOG(0, "IME {C[i1]}. {i2} Removed level. Level={d1}", getContractName(), order._side, ToDollars(order._externalPrice))
        }
    }

    template <typename MapT, typename LevelsT>
    void doRemoveOrder(Order& order, MapT& map, LevelsT& levels) {
        auto ret = map.equal_range(order._price);
        for (auto it = ret.first; it != ret.second; ++it) {
            if (it->second->_id == order._id) {
                map.erase(it);
                break;
            }
        }
        PriceLevel* level = order.priceLevel;
        if (level != nullptr) {
            doRemoveOrderFromLevel(level, order, levels);
        }
    }

    template <typename MapT, typename LevelsT>
    void doRemoveOrder(Order& order, MapT& map, LevelsT& levels, StrategyId) {
        auto ret = map.equal_range(order._price);
        for (auto it = ret.first; it != ret.second; ++it) {
            if (it->second->_strategy == order._id) {
                map.erase(it);
                break;
            }
        }
        PriceLevel* level = order.priceLevel;
        if (level != nullptr) {
            doRemoveOrderFromLevel(level, order, levels);
        }
    }

    template <typename LevelsT>
    void removeLevel(PriceLevel* level, LevelsT& levels) {
        BOOST_VERIFY(levels.erase(level->_price) == 1);
    }

    template <typename MapT>
    void removeOrderFromMap(Order* order, MapT& map) {
        auto ret = map.equal_range(order->_price);
        for (auto it = ret.first; it != ret.second; ) {
            if (it->second->_id == order->_id) {
                it = map.erase(it);
                return;
            } else {
                ++it;
            }
        }
    }

    void synchronizeOrders() {
        LOG(0, "IME {C[i1]}. Synchronize orders. Start", getContractName())
        const hf::price_t currentTopBid = getPersistedPrice(BID);
        PriceLevel* bidTopLevel = getTopLevel(_bidLevels);
        const hf::price_t newTopBid = bidTopLevel == nullptr ? hf::NULL_PRICE : bidTopLevel->_price;
        const uint64_t now = clockUtil::GetCycles();
        if ((currentTopBid != hf::NULL_PRICE) & (newTopBid != hf::NULL_PRICE)) {
            if (newTopBid > currentTopBid) {
                synchronizeQuote<ASK>(now);
                synchronizeQuote<BID>(now);
            } else {
                synchronizeQuote<BID>(now);
                synchronizeQuote<ASK>(now);
            }
        } else {
            synchronizeQuote<BID>(now);
            synchronizeQuote<ASK>(now);
        }
    }

    template<uint32_t Side>
    void synchronizeQuote(uint64_t now) {
        hf::price_t price;
        hf::qty_t size;
        getPriceAndSize<Side>(price, size);

        LOG(0, "IME {C[i1]}. {i2} Synchronize orders. Price={d1} Size={i3}", getContractName(), Side, ToDollars(price), size)

        if (price != hf::NULL_PRICE) {
            synchronizeUnderBanners<Side>(price, size, now);
        } else {
            _om[Side].getQuoteOutOfMarket();
        }

        if (_pendingHedge[Side] != nullptr) {
            sendHedge<Side>(*_pendingHedge[Side]);
            _pendingHedge[Side] = nullptr;
        }
    }

    template <uint32_t Side>
    void sendHedge(Order& hedge) {
        Banner& banner = _hedgeBanner[Side];
        if (hedge._inPayUp) {
            LOG(0, "IME {C[i1]}. {i2} Hedger in payup. Bypassing banner.", getContractName(), Side)
            _om[Side].template hedge<Side>(hedge._externalPrice, hedge._size);
            banner._price = hedge._externalPrice;
            banner._qty = hedge._size;
        } else {
            const uint64_t now = clockUtil::GetCycles();
            if (banner.isBanned(now)) {
                const bool isQuoteWorse = IsBetterPrice<Side>(banner._price, hedge._externalPrice);
                if (!isQuoteWorse || (hedge._externalSize == 0 && banner._qty != 0)) {
                    LOG(0, "IME {C[i1]}. {i2} Hedger under ban but quote is not worse. Banner_price={d1} Price={d2}", getContractName(), Side, ToDollars(banner._price), ToDollars(hedge._externalPrice))
                    _om[Side].template hedge<Side>(hedge._externalPrice, hedge._size);
                    banner._price = hedge._externalPrice;
                    banner._qty = hedge._size;
                }
            } else {
                LOG(0, "IME {C[i1]}. {i2} Hedger NOT under ban. ", getContractName(), Side)
                const hf::price_t restingPrice = _om[Side]._hedge ? _om[Side]._hedge->_price : hf::NULL_PRICE;
                _om[Side].template hedge<Side>(hedge._externalPrice, hedge._size);
                if (restingPrice != hf::NULL_PRICE) {
                    const bool isQuoteWorse = IsBetterPrice<Side>(restingPrice, hedge._externalPrice);
                    if (isQuoteWorse) {
                        LOG(0, "IME {C[i1]}. {i2} New price is worse. Resting_price={d1} Price={d2}", getContractName(), Side, ToDollars(restingPrice), ToDollars(hedge._externalPrice))
                        startHedgingBanner(Side, hedge._externalPrice, hedge._size);
                    } else {
                        LOG(0, "IME {C[i1]}. {i2} price not worse than resting. ", getContractName(), Side)
                    }
                } else {
                    LOG(0, "IME {C[i1]}. {i2} no resting price. ", getContractName(), Side)
                    startHedgingBanner(Side, hedge._externalPrice, hedge._size);
                }
            }
        }
    }

    void startHedgingBanner(uint32_t side, hf::price_t price, hf::qty_t size) {
        const uint64_t now = clockUtil::GetCycles();
        Banner& banner = _hedgeBanner[side];
        banner._price = price;
        banner._qty = size;
        banner._expiryTime = now + banner._banPeriod;
        _executor->addQuoteBannerTimer(banner._expiryTime, _internalId, side, TimerType::HedgeBanner);
        LOG(0, "IME {C[i1]}. {i2} Banner. Starting hedge ban period due to quote. Now={i3} Expiry={i4}", getContractName(), side, now, banner._expiryTime)
    }

    template<uint32_t Side>
    bool quoteWithMaxTicksOffTopLevelCheck(hf::price_t price, hf::qty_t size, bool goThroughAllLevels) {
        static_assert((Side == BID) || (Side == ASK), "Invalid side");
        bool outsideMaxOffTopLevel = false;
        if(_maxOffTopLevel >= 0) {
            if(Side == BID && (price + _maxOffTopLevel < _marketBooks[BID]._price) && _marketBooks[BID]._price != hf::NULL_PRICE) {
                size = 0;
                outsideMaxOffTopLevel = true;
                LOG(0, "IME {C[i1]}. BID. Quote outside MaxTicksOffTopLevel. Set size to 0. Price={d1} MaxOffTopLevel={d2} TopBid={d3}", getContractName(), ToDollars(price), ToDollars(_maxOffTopLevel), ToDollars(_marketBooks[BID]._price));
            }
            if(Side == ASK && (price - _maxOffTopLevel > _marketBooks[ASK]._price) && _marketBooks[ASK]._price != hf::NULL_PRICE) {
                size = 0;
                outsideMaxOffTopLevel = true;
                LOG(0, "IME {C[i1]}. ASK. Quote outside MaxTicksOffTopLevel. Set size to 0. Price={d1} MaxOffTopLevel={d2} TopAsk={d3}", getContractName(), ToDollars(price), ToDollars(_maxOffTopLevel), ToDollars(_marketBooks[ASK]._price));
            }
        }
        _om[Side].template quote<Side>(price, size, goThroughAllLevels);

        if (_reconcileGtcOrders.test(Side)) {
            _reconcileGtcOrders.reset(Side);
            if (!_reconcileGtcOrders.any()) {
                cancelRemainingGtc();
            }
        }

        return outsideMaxOffTopLevel;
    }

    template <uint32_t Side>
    void synchronizeUnderBanners(hf::price_t price, hf::qty_t size, uint64_t now) {
        Banner& quoteBanner = _quoteBanner[Side];
        FillBanner& fillBanner = _fillBanner[Side];
        Banner& throttlingBanner = _throttlingBanner[Side];
        fillBanner.expireEntries(now);
        const bool isBanned = quoteBanner.isBanned(now) | fillBanner.isBanned() | throttlingBanner.isBannedUntilReset();
        if (isBanned) {
            if (throttlingBanner._mode != THROTTLING_MODE_AT_CAPACITY) {
                const bool isQuoteBetter = (quoteBanner.isBanned(now) && ((IsBetterPrice<Side>(price, quoteBanner._price)) | ((price == quoteBanner._price) & (size >= quoteBanner._qty)))) |
                                           (fillBanner.isBanned() && ((IsBetterPrice<Side>(price, fillBanner.getPrice())) | ((price == fillBanner.getPrice()) & (size >= fillBanner.getSize())))) |
                                           (throttlingBanner.isBannedUntilReset() && ((throttlingBanner._price == hf::NULL_PRICE) | (IsBetterPrice<Side>(price, throttlingBanner._price)) | ((price == throttlingBanner._price) & (size >= throttlingBanner._qty))));
                if (!isQuoteBetter) {
                    const bool outsetMaxOffTopLevel = quoteWithMaxTicksOffTopLevelCheck<Side>(price, size, fillBanner.getRejectsFlag());
                    fillBanner.resetHadRejects();
                    if (BOOST_LIKELY(!outsetMaxOffTopLevel)) {
                        if(quoteBanner.isBanned(now)) {
                            quoteBanner._price = price;
                            quoteBanner._qty = size;
                        }
                        if (throttlingBanner.isBannedUntilReset()) {
                            throttlingBanner._price = price;
                            throttlingBanner._qty = size;
                        }
                    }
                } else {
                    LOG(0, "IME {C[i1]}. {i2} Banner. Quote under ongoing ban. Not updating. Price={d1} Size={i3} Now={i4}", getContractName(), Side, ToDollars(price), size, now)
                }
            }
        } else {
            const hf::price_t currentPrice = _om[Side].getPrice();
            const hf::qty_t restingSize = _om[Side].getRestingSize();
            const bool outsetMaxOffTopLevel = quoteWithMaxTicksOffTopLevelCheck<Side>(price, size, fillBanner.getRejectsFlag());
            fillBanner.resetHadRejects();
            quoteBanner.expire();
            if (BOOST_LIKELY(!outsetMaxOffTopLevel)) {
                if (BOOST_LIKELY(currentPrice != hf::NULL_PRICE)) {
                    auto newRestingSize = _om[Side].getRestingSize();
                    const bool isQuoteBetter = ((IsBetterPrice<Side>(price, currentPrice)) | ((price == currentPrice) & (newRestingSize > restingSize)));
                    if (isQuoteBetter) {
                        LOG(0, "IME {C[i1]}. {i2} Banner. Not under ban. Has quote in market. Quote better. Current: {i3}@{d1} New={i4}@{d2} Now={i5}", getContractName(), Side, restingSize, ToDollars(currentPrice), newRestingSize, ToDollars(price), now)
                        startQuoteBanner<Side>(price, size);
                    }
                } else {
                    LOG(0, "IME {C[i1]}. {i2} Banner. Putting first quote in market. Now={i3} Price={d1} Qty={i4}", getContractName(), Side, now, ToDollars(price), size)
                    startQuoteBanner<Side>(price, size);
                }
            }
        }
    }

    template <uint32_t Side>
    void startQuoteBanner(hf::price_t price, hf::qty_t size) {
        const uint64_t now = clockUtil::GetCycles();
        Banner& quoteBanner = _quoteBanner[Side];
        quoteBanner._price = price;
        quoteBanner._qty = size;
        quoteBanner._expiryTime = now + quoteBanner._banPeriod;
        _executor->addQuoteBannerTimer(quoteBanner._expiryTime, _internalId, Side, TimerType::QuoteBanner);
        LOG(0, "IME {C[i1]}. {i2} Banner. Starting ban period due to quote. Now={i3} Expiry={i4}", getContractName(), Side, now, quoteBanner._expiryTime)
    }

    void setThrottlingBanner(THROTTLING_MODE mode) {
        if (mode == THROTTLING_MODE_NONE) {
            resetThrottlingBanner();
        } else {
            startThrottlingBanner<BID>(_om[BID].getPrice(), _om[BID].getRestingSize(), mode);
            startThrottlingBanner<ASK>(_om[ASK].getPrice(), _om[ASK].getRestingSize(), mode);
        }
    }

    template <uint32_t Side>
    void startThrottlingBanner(hf::price_t price, hf::qty_t size, THROTTLING_MODE mode) {
        Banner& banner = _throttlingBanner[Side];
        banner._price = price;
        banner._qty = size;
        banner._expiryTime = 1;
        banner._mode = mode;
        LOG(0, "IME {C[i1]}. {i2} Banner. Starting throttling banner.", getContractName(), Side)
    }

    void resetThrottlingBanner() {
        const uint64_t now = clockUtil::GetCycles();
        doResetThrottlingBanner<BID>(now);
        doResetThrottlingBanner<ASK>(now);
    }

    template <uint32_t Side>
    void doResetThrottlingBanner(uint64_t now) {
        Banner& banner = _throttlingBanner[Side];
        banner._expiryTime = 0;
        banner._mode = THROTTLING_MODE_NONE;
        LOG(0, "IME {C[i1]}. {i2} Throttling Banner. Reset. Now={i3}", getContractName(), Side, now)
        putQuoteBackInMarketAfterBan<Side>(now);
    }

    template <uint32_t Side>
    void startFillBanner(hf::price_t price, hf::qty_t size, uint32_t isReject = 0) {
        uint64_t now = clockUtil::GetCycles();
        FillBanner& fillBanner = _fillBanner[Side];
        uint64_t expiry = now + (isReject == 0 ? fillBanner._banPeriod : fillBanner._rejectBanPeriod);
        fillBanner.addEntry(price, size, expiry, isReject);
        _executor->addFillBannerTimer(expiry, _internalId, Side);
        LOG(0, "IME {C[i1]}. {i2} Banner. Starting ban period due to fill. Now={i3} Expiry={i4} Entry={i5}@{d1}", getContractName(), Side, now, expiry, size, ToDollars(price))
    }

    void expireQuoteBanner(uint32_t side) {
        uint64_t now = clockUtil::GetCycles();
        if (side == BID) {
            doExpireQuoteBanner<BID>(now);
        } else {
            doExpireQuoteBanner<ASK>(now);
        }
    }

    template <uint32_t Side>
    void doExpireQuoteBanner(uint64_t now) {
        Banner& quoteBanner = _quoteBanner[Side];
        if (quoteBanner.wasRunning()) {
            LOG(0, "IME {C[i1]}. {i2} Quote Banner. Expired ban. Now={i3}", getContractName(), Side, now)
            _fillBanner[Side].expireEntries(now);
            quoteBanner.expire();
            if (BOOST_LIKELY(_contractCanTrade & _gatewayActive)) {
                putQuoteBackInMarketAfterBan<Side>(now);
            }
        }
    }

    void expireFillBanner(uint32_t side) {
        uint64_t now = clockUtil::GetCycles();
        if (side == BID) {
            doExpireFillBanner<BID>(now);
        } else {
            doExpireFillBanner<ASK>(now);
        }
    }

    template <uint32_t Side>
    void doExpireFillBanner(uint64_t now) {
        FillBanner& fillBanner = _fillBanner[Side];
        if (fillBanner.isBanned()) {
            LOG(0, "IME {C[i1]}. {i2} Fill Banner. Expired ban. Now={i3}", getContractName(), Side, now)
            fillBanner.expireEntries(now);
            if (BOOST_LIKELY(_contractCanTrade & _gatewayActive)) {
                putQuoteBackInMarketAfterBan<Side>(now);
            }
        }
    }

    void expireHedgeQuoteBanner(uint32_t side) {
        uint64_t now = clockUtil::GetCycles();
        if (side == BID) {
            doExpireHedgeQuoteBanner<BID>(now);
        } else {
            doExpireHedgeQuoteBanner<ASK>(now);
        }
    }

    template <uint32_t Side>
    void doExpireHedgeQuoteBanner(uint64_t now) {
        Banner& banner = _hedgeBanner[Side];
        if (banner.wasRunning()) {
            LOG(0, "IME {C[i1]}. {i2} Hedge Quote Banner. Expired ban. Now={i3}", getContractName(), Side, now)
            if (BOOST_LIKELY(_contractCanTrade & _gatewayActive)) {
                Order& hedge = getOrders<Side>()[orderId(StrategyId::MarketMaker, Order::Type::Hedging)];
                _om[Side].template hedge<Side>(hedge._externalPrice, hedge._size);
            }
            banner.expire();
        }
    }

    template <uint32_t Side>
    void putQuoteBackInMarketAfterBan(uint64_t now) {
        hf::price_t price = hf::NULL_PRICE;
        hf::qty_t size = 0;
        getPriceAndSize<Side>(price, size);
        if (price != hf::NULL_PRICE) {
            LOG(0, "IME {C[i1]}. {i2} Banner. Putting quote back in market after ban. Price={d1} Size={i3}", getContractName(), Side, ToDollars(price), size)
            synchronizeUnderBanners<Side>(price, size, now);
        } else {
            LOG(0, "IME {C[i1]}. {i2} Banner. Could not put quote back. Nothing in price level.", getContractName(), Side)
        }
    }

    template <uint32_t Side>
    void getPriceAndSize(hf::price_t& price, hf::qty_t& size) {
        auto& levels = getExternalLevels<Side>();
        PriceLevel* level = getTopLevel(levels);
        auto strategyPricePair = getPersistedStrategyAndPrice<Side>();
        if (level != nullptr) {
            if (strategyPricePair.second != hf::NULL_PRICE && IsBetterPrice<Side>(strategyPricePair.second, level->_price)) {
                price = strategyPricePair.second;
                size = 0;
            } else {
                price = level->_price;
                size = level->getSize();
            }
        } else {
            price = strategyPricePair.second;
            if (price != hf::NULL_PRICE) {
                price = _marketBooks[Side]._price;
            }
            size = 0;
        }
    }

    hf::price_t getPersistedPrice(uint32_t side) {
        hf::price_t toReturn = hf::NULL_PRICE;
        for (uint32_t i = 0; i < toUInt(StrategyId::Execution); ++i) {
            auto price = _topLevelsByStrategyBySide[i][side];
            if (price != hf::NULL_PRICE) {
                if (toReturn == hf::NULL_PRICE) {
                    toReturn = price;
                } else if (side == BID) {
                    toReturn = std::max(toReturn, price);
                } else {
                    toReturn = std::min(toReturn, price);
                }
            }
        }
        return toReturn;
    }

    template<uint32_t Side>
    std::pair<StrategyId, hf::price_t> getPersistedStrategyAndPrice() {
        hf::price_t persistedPrice = hf::NULL_PRICE;
        StrategyId strategy = StrategyId::MAX;
        for (uint32_t i = 0; i < toUInt(StrategyId::Execution); ++i) {
            auto price = _topLevelsByStrategyBySide[i][Side];
            if (price != hf::NULL_PRICE) {
                if (persistedPrice == hf::NULL_PRICE) {
                    persistedPrice = price;
                    strategy = static_cast<StrategyId>(i);
                } else if (Side == BID) {
                    if (price > persistedPrice) {
                        persistedPrice = price;
                        strategy = static_cast<StrategyId>(i);
                    }
                } else {
                    if (price < persistedPrice) {
                        persistedPrice = price;
                        strategy = static_cast<StrategyId>(i);
                    }
                }
            }
        }
        return std::make_pair(strategy, persistedPrice);
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

    template<uint32_t Side>
    hf::qty_t tryMatch(hf::price_t price, hf::qty_t size, hf::price_t tolerance, StrategyId strategy) {
        auto& orderMap = getInternalOrderMap<1 - Side>();
        auto orderIt = orderMap.begin();
        if (orderIt == orderMap.end()) {
            return size;
        }

        price = adjustPrice<Side>(price, tolerance);

        Order* order = orderIt->second;

        while (IsPriceBetterOrEqual<Side>(price, order->_price)) {
            const hf::qty_t fillSize = std::min(order->_size, size);

            if (strategy == order->_strategy) {
                WLOG(0, "IME {C[i1]}. {i2} Self match. Skip order {i3}@{d1} id={i4}", getContractName(),
                     strategyName(strategy), order->_size, ToDollars(order->_price), order->_id)
                _executor->sendErrorMessage(
                        boost::str(boost::format("Skip self match order. Strategy=%1% Contract=%2% OrderId=%3% %4%@%5%") %
                                   toString(strategy) % _name % order->_id % order->_size % ToDollars(order->_price)),
                        ErrorType::Critical);

                if (++orderIt == orderMap.end()) {
                    break;
                }
                order = orderIt->second;
                continue;
            }

            LOG(0, "IME {C[i1]}. Matched with {i2}@{d1} Id={i3} fillSize={fillSize}",
                getContractName(), order->_size, ToDollars(order->_price), order->_id, fillSize)

            _internalFills.push_back({order, order->_price, fillSize});
            order->_size -= fillSize;
            size -= fillSize;
            _internalMarketNotification.emplace(toUInt(order->_strategy));
            doExternalReductionAfterInternalFill(order, fillSize);
            if (order->_size == 0) {
                orderIt = orderMap.erase(orderIt);
            }

            if ((size == 0) | (orderIt == orderMap.end())) {
                break;
            }
            order = orderIt->second;
        }

        return size;
    }

    struct InternalFill {
        Order* _order;
        hf::price_t _price;
        hf::qty_t _size;
    };

    template <typename OrdersT>
    bool removePayUpOrder(OrdersT& orders) {
        for (auto it = orders.begin(); it != orders.end(); ++it) {
            Order* order = it->second;
            if (order->_inPayUp) {
                LOG(0, "IME {C[i1]}. {i2} removePayUpOrder. ID={i3}", getContractName(), order->_side, order->_id)
                orders.erase(it);
                return true;
            }
        }
        return false;
    }

    template <typename PayUpMap, typename OtherMap>
    bool matchPayUpOrder(PayUpMap& payUpMap, OtherMap& otherMap, uint32_t payUpSide) {
        for (auto payUpIt = payUpMap.begin(); payUpIt != payUpMap.end(); ++payUpIt) {
            Order* payUpOrder = payUpIt->second;
            if (payUpOrder->_inPayUp) {
                for (auto otherIt = otherMap.begin(); otherIt != otherMap.end();) {
                    Order* otherOrder = otherIt->second;
                    if (otherOrder->_strategy != payUpOrder->_strategy) {
                        const bool isMatch = IsBetterOrEqual(payUpOrder->_price, otherOrder->_price, payUpSide);
                        if (!isMatch) {
                            break;
                        }

                        const hf::qty_t fillSize = std::min(payUpOrder->_size, otherOrder->_size);
                        LOG(0, "IME {C[i1]}. Side={i2} Matched MM PayUp hedge order with order ID={i3} {i4}@{d1} Fill={i3}",
                            getContractName(), payUpSide, otherOrder->_id, otherOrder->_size,
                            ToDollars(otherOrder->_price), fillSize)
                        payUpOrder->_size -= fillSize;
                        otherOrder->_size -= fillSize;
                        assert(_pendingHedge[payUpSide] == payUpOrder);
                        _internalFills.push_back({payUpOrder, otherOrder->_price, fillSize});
                        _internalFills.push_back({otherOrder, otherOrder->_price, fillSize});
                        if (otherOrder->_size == 0) {
                            otherIt = otherMap.erase(otherIt);
                        } else {
                            ++otherIt;
                        }
                        doExternalReductionAfterInternalFill(otherOrder, fillSize);

                        if (payUpOrder->_size == 0) {
                            break;
                        }
                    } else {
                        ++otherIt;
                    }
                }
                LOG(0, "IME {C[i1]}. Side={i2} remove PayUp order from internal order map", getContractName(), payUpSide)
                payUpMap.erase(payUpIt);
                return true;
            }
        }
        return false;
    }

    void doExternalReductionAfterInternalFill(Order* order, hf::qty_t fillSize) {
        const bool quotingExternally = order->_externalPrice != hf::NULL_PRICE;
        if (quotingExternally) {
            // MM's matching size is sum of internal and external size now, it is possible that
            // the order->_externalPrice < fillSize
            const hf::qty_t newExternalSize = std::max(order->_externalSize - fillSize, static_cast<hf::qty_t>(0));
            if (BOOST_LIKELY(order->_type != Order::Type::Hedging)) {
                if (newExternalSize == 0) {
                    removeOrderFromLevel(order);
                }
            }
            order->_externalSize = newExternalSize;
        }
    }

    template<uint32_t Side>
    PriceLevel* addToPriceLevel(Order& order) {
        auto& priceLevels = getExternalLevels<Side>();
        auto it = priceLevels.find(order._externalPrice);
        if (it != priceLevels.end()) {
            PriceLevel& level = it->second;
            level.addOrder(&order);
            order.priceLevel = &level;
            return &level;
        } else {
            PriceLevel& level = priceLevels[order._externalPrice];
            level._price = order._externalPrice;
            level.addOrder(&order);
            order.priceLevel = &level;
            return &level;
        }
    }

    bool matchOrders(StrategyId aggressingStrategy) {
        LOG(0, "IME {C[i1]}. Internally Match Orders. _bids={i2} _asks={i3}", getContractName(), _bids.size(), _asks.size())

        if (_bids.empty()) {
            removePayUpOrder(_asks);
            return true;
        }

        if (_asks.empty()) {
            removePayUpOrder(_bids);
            return true;
        }

        const bool hadBidPayUp = matchPayUpOrder(_bids, _asks, BID);
        if (!hadBidPayUp) {
            matchPayUpOrder(_asks, _bids, ASK);
        }

        if (_bids.empty() | _asks.empty()) {
            return true;
        }

        auto bidOrdersIt = _bids.begin();
        auto askOrdersIt = _asks.begin();

        Order* bidOrder = bidOrdersIt->second;
        Order* askOrder = askOrdersIt->second;

        hf::price_t bidTop = bidOrdersIt->first;
        hf::price_t askTop = askOrdersIt->first;
        hf::qty_t bidSize = bidOrder->_size;
        hf::qty_t askSize = askOrder->_size;

        while (bidTop >= askTop) {
            bool setlMatch = bidOrder->_strategy == askOrder->_strategy;
            if (setlMatch) {
                std::string message = boost::str(boost::format("%1% self match. Contract=%2% Bid=%3%@%4% Ask=%5%@%6% BidType=%7% AskType=%8%")
                                                 % toString(bidOrder->_strategy) % _name % bidSize % ToDollars(bidTop)
                                                 % askSize % ToDollars(askTop) % bidOrder->_type % askOrder->_type);
                _executor->sendErrorMessage(message, ErrorType::Critical);
                return false;
            }
            const hf::price_t fillPrice = bidOrder->_strategy == aggressingStrategy ? askTop : bidTop;
            LOG(0, "IME {C[i1]}. Internal Cross. Matching. Bid: {i2}@{d1} Id={i3} Ask={i4}@{d2} Id={i5}",
                getContractName(), bidSize, ToDollars(bidTop), bidOrder->_id, askSize, ToDollars(askTop), askOrder->_id)
            if (bidSize == askSize) {
                LOG(0, "IME {C[i1]}. Match. Equal size. Size={i2}", getContractName(), bidSize)
                bidOrder->_size = 0;
                askOrder->_size = 0;
                _internalFills.push_back({bidOrder, fillPrice, bidSize});
                _internalFills.push_back({askOrder, fillPrice, bidSize});
                bidOrdersIt = _bids.erase(bidOrdersIt);
                askOrdersIt = _asks.erase(askOrdersIt);
                _internalMarketNotification.emplace(toUInt(bidOrder->_strategy));
                _internalMarketNotification.emplace(toUInt(askOrder->_strategy));
                doExternalReductionAfterInternalFill(bidOrder, bidSize);
                doExternalReductionAfterInternalFill(askOrder, bidSize);
                const bool hasBidOrder = getNextOrder(bidOrder, bidOrdersIt, _bids, bidTop);
                const bool hasAskOrder = getNextOrder(askOrder, askOrdersIt, _asks, askTop);
                if ((!hasAskOrder) | (!hasBidOrder)) {
                    LOG(0, "IME {C[i1]}. No more orders. Exiting matching.", getContractName())
                    break;
                }
                bidSize = bidOrder->_size;
                askSize = askOrder->_size;
                LOG(0, "IME {C[i1]}. Got new orders. Bid: Id={i2} {i3}@{d1} Ask: Id={i4} {i5}@{d2}", getContractName(),
                    bidOrder->_id, bidOrder->_size, ToDollars(bidOrder->_price), askOrder->_id, askOrder->_size,
                    ToDollars(askOrder->_price))
            }
            else if (bidSize > askSize) {
                LOG(0, "IME {C[i1]}. Match. Bid size > Ask size. MatchSize={i2}", getContractName(), askSize)
                const hf::qty_t fillSize = askSize;
                _internalFills.push_back({bidOrder, fillPrice, fillSize});
                _internalFills.push_back({askOrder, fillPrice, fillSize});
                _internalMarketNotification.emplace(toUInt(bidOrder->_strategy));
                _internalMarketNotification.emplace(toUInt(askOrder->_strategy));
                askOrdersIt = _asks.erase(askOrdersIt);
                doExternalReductionAfterInternalFill(askOrder, fillSize);
                doExternalReductionAfterInternalFill(bidOrder, fillSize);
                bidOrder->_size -= fillSize;
                askOrder->_size = 0;
                const bool hasAskOrder = getNextOrder(askOrder, askOrdersIt, _asks, askTop);
                if (!hasAskOrder) {
                    LOG(0, "IME {C[i1]}. No more ask orders. Exiting matching.", getContractName())
                    break;
                }
                askSize = askOrder->_size;
                bidSize -= fillSize;
                LOG(0, "IME {C[i1]}. Got new ask order. Id={i2} {i3}@{d1}", getContractName(), askOrder->_id,
                    askOrder->_size, ToDollars(askOrder->_price))
            } else {
                LOG(0, "IME {C[i1]}. Match. Bid size < Ask size. MatchSize={i2}", getContractName(), bidSize)
                const hf::qty_t fillSize = bidSize;
                _internalFills.push_back({bidOrder, fillPrice, fillSize});
                _internalFills.push_back({askOrder, fillPrice, fillSize});
                _internalMarketNotification.emplace(toUInt(bidOrder->_strategy));
                _internalMarketNotification.emplace(toUInt(askOrder->_strategy));
                bidOrdersIt = _bids.erase(bidOrdersIt);
                doExternalReductionAfterInternalFill(bidOrder, fillSize);
                doExternalReductionAfterInternalFill(askOrder, fillSize);
                askOrder->_size -= fillSize;
                bidOrder->_size = 0;
                const bool hasBidOrder = getNextOrder(bidOrder, bidOrdersIt, _bids, bidTop);
                if (!hasBidOrder) {
                    LOG(0, "IME {C[i1]}. No more bid orders. Exiting matching.", getContractName())
                    break;
                }
                bidSize = bidOrder->_size;
                askSize -= fillSize;
                LOG(0, "IME {C[i1]}. Got new bid order. Id={i2} {i3}@{d1}", getContractName(), bidOrder->_id,
                    bidOrder->_size, ToDollars(bidOrder->_price))
            }
        }
        return true;
    }

    void sendInternalMarketData() {
        for (auto strategy : _internalMarketNotification) {
            for (const auto& kvp : _internalMarketSubscriptions) {
                if (kvp.first == strategy) {
                    continue;
                }

                if (std::find(kvp.second.begin(), kvp.second.end(), (StrategyId)strategy) == kvp.second.end()) {
                    continue;
                }

                hf::price_t bidPrice = hf::NULL_PRICE;
                hf::qty_t bidSize = 0;
                hf::price_t askPrice = hf::NULL_PRICE;
                hf::qty_t askSize = 0;

                for (auto stg : kvp.second) {
                    const auto& bidQuote = _bidOrders[orderId(stg, Order::Type::Quoting)];
                    const auto& askQuote = _askOrders[orderId(stg, Order::Type::Quoting)];

                    if (bidPrice == hf::NULL_PRICE) {
                        if (bidQuote._price != hf::NULL_PRICE) {
                            bidPrice = bidQuote._price;
                            bidSize = bidQuote._size;
                        }
                    } else if (bidPrice < bidQuote._price) {
                        bidPrice = bidQuote._price;
                        bidSize = bidQuote._size;
                    } else if (bidPrice == bidQuote._price) {
                        bidSize += bidQuote._size;
                    }

                    if (askPrice == hf::NULL_PRICE) {
                        if (askQuote._price != hf::NULL_PRICE) {
                            askPrice = askQuote._price;
                            askSize = askQuote._size;
                        }
                    } else if (askPrice > askQuote._price) {
                        askPrice = askQuote._price;
                        askSize = askQuote._size;
                    } else if (askPrice == askQuote._price) {
                        askSize += askQuote._size;
                    }

                    LOG(0, "IME {C[i1]}. Send {i2} internal md to strategy {i3}. id={i4} bid: {i5}@{d1} ask: {i6}@{d2}",
                         getContractName(), strategyName(stg), strategyName((StrategyId)kvp.first), bidQuote._id, bidSize,
                         ToDollars(bidPrice), askSize, ToDollars(askPrice))

                    _executor->sendMarketUpdateToStrategy((StrategyId)kvp.first, _internalId, bidQuote._id, bidPrice,
                                                          bidSize, askPrice, askSize);

                    if (stg == StrategyId::MarketMaker) {
                        hf::price_t bidPrice = hf::NULL_PRICE;
                        hf::qty_t bidSize = 0;
                        hf::price_t askPrice = hf::NULL_PRICE;
                        hf::qty_t askSize = 0;

                        const auto& bidHedge = _bidOrders[orderId(StrategyId::MarketMaker, Order::Type::Hedging)];
                        const auto& askHedge = _askOrders[orderId(StrategyId::MarketMaker, Order::Type::Hedging)];

                        if (!bidHedge._inPayUp) {
                            bidPrice = bidHedge._price;
                            bidSize = bidHedge._size;
                        }

                        if (!askHedge._inPayUp) {
                            askPrice = askHedge._price;
                            askSize = askHedge._size;
                        }

                        TLOG(0, "IME {C[i1]}. Send {i2} internal md to strategy {i3}. id={i4} bid: {i5}@{d1} ask: {i6}@{d2}",
                             getContractName(), strategyName(stg), strategyName((StrategyId)kvp.first), bidHedge._id, bidSize,
                             ToDollars(bidPrice), askSize, ToDollars(askPrice))
                        _executor->sendMarketUpdateToStrategy((StrategyId) kvp.first, _internalId, bidHedge._id,
                                                              bidPrice, bidSize, askPrice, askSize);
                    }
                }
            }
        }
        _internalMarketNotification.clear();
    }

    void sendInternalFills() {
        if (!_internalFills.empty()) {
            for (auto& fill : _internalFills) {
                _executor->onInternalFill(_internalId, fill._order, fill._price, fill._size);
            }
            _executor->onEndOfFills(_internalId, BID /*not used*/, false);
            _internalFills.clear();
        }
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

    template <typename MapT>
    void doInternalReductionAfterExternalFill(Order* order, hf::qty_t fillSize, MapT& map) {
        const hf::qty_t internalReduction = std::min(order->_size, fillSize);
        order->_size -= internalReduction;
        _internalMarketNotification.emplace(toUInt(order->_strategy));
        if (order->_size == 0) {
            removeOrderFromMap(order, map);
        }
    }

    void allocateExternalFills(const om::OMExecutionEvent &event, const om::order_t &order, uint32_t side) {
        LOG(0, "IME {C[i1]}. Allocate external fill. Side={i2} Price={d1} Size={i3}", getContractName(), ToViperSide(order.side), ToDollars(event.filledPrice), event.filledQty)
        // allocate fills from MM hedge first
        hf::qty_t unmatchedFills = allocateExternalMMHedgerFill(event.filledPrice, event.filledQty, side);
        if (unmatchedFills > 0) {
            // allocate unmatched fills from MM Quote or RV which ever is the best
            if (side == BID) {
                allocateExternalQuoteFill(event.filledPrice, unmatchedFills, order, _bidLevels, _bids);
                const hf::qty_t restingSize = _om[BID].getRestingSize();
                startFillBanner<BID>(event.filledPrice, restingSize);
            } else {
                allocateExternalQuoteFill(event.filledPrice, unmatchedFills, order, _askLevels, _asks);
                const hf::qty_t restingSize = _om[ASK].getRestingSize();
                startFillBanner<ASK>(event.filledPrice, restingSize);
            }
        }
        _executor->onEndOfFills(_internalId, side, false);
    }

    hf::qty_t allocateExternalMMHedgerFill(hf::price_t fillPrice, hf::qty_t fillQty, uint32_t side) {
        auto& orders = side == BID ? _bidOrders : _askOrders;
        auto& mmHedge = orders[orderId(StrategyId::MarketMaker, Order::Type::Hedging)];
        if (mmHedge._externalSize) {
            auto allocatedQty = std::min<hf::qty_t>(mmHedge._externalSize, fillQty);
            mmHedge._externalSize -= allocatedQty;
            fillQty -= allocatedQty;
            _executor->onExternalFill(_internalId, StrategyId::MarketMaker, fillPrice, allocatedQty, side, FillSource::Hedge);
        }
        return fillQty;
    }

    template <typename LevelsT, typename MapT>
    void allocateExternalQuoteFill(hf::price_t filledPrice, hf::qty_t filledQty, const om::order_t &order, LevelsT& levels, MapT& map) {
        LOG(0, "IME {C[i1]}. External fill. Allocating to internal orders. Side={i2} Price={d1} Size={i3}", getContractName(), ToViperSide(order.side), ToDollars(filledPrice), filledQty)
        hf::qty_t filledSize = filledQty;
        for (auto levelIt = levels.begin(); levelIt != levels.end(); ) {
            auto& level = levelIt->second;
            auto& orders = level._orders;
            for (auto orderIt = orders.begin(); orderIt != orders.end(); ) {
                LOG(0, "IME {C[i1]}. Matching on level. Price={d1} NumberOfOrders={i2}", getContractName(), ToDollars(level._price), level._orders.size())
                Order* o = *orderIt;
                auto fillSource = o->_type == Order::Type::Quoting ? FillSource::Quote : FillSource::Hedge;
                if (o->_externalSize == filledSize) {
                    LOG(0, "IME {C[i1]}. Match. Equal size. Order={i2} Price={d1} Size={i3}", getContractName(), o->_id, ToDollars(o->_externalPrice), filledSize)
                    _executor->onExternalFill(_internalId, o->_strategy, filledPrice, filledSize, o->_side, fillSource);
                    orderIt = orders.erase(orderIt);
                    o->priceLevel = nullptr;
                    LOG(0, "IME {C[i1]}. {i2} Removed order from level. Level={d1} LevelSize={i3} ID={i4}", getContractName(), o->_side, ToDollars(o->_externalPrice), level.getSize(), o->_id)
                    if (orderIt == orders.end()) {
                        LOG(0, "IME {C[i1]}. Erasing level. No more orders. Price={d1}", getContractName(), ToDollars(level._price))
                        levels.erase(levelIt);
                    }
                    doInternalReductionAfterExternalFill(o, filledSize, map);
                    return;
                } else if (o->_externalSize < filledSize) {
                    const hf::qty_t oneFill = o->_externalSize;
                    LOG(0, "IME {C[i1]}. Match. Order size < filled size. Order={i2} Price={d1} Size={i3}", getContractName(), o->_id,  ToDollars(o->_externalPrice), oneFill)
                    _executor->onExternalFill(_internalId, o->_strategy, filledPrice, oneFill, o->_side, fillSource);
                    orderIt = orders.erase(orderIt);
                    o->priceLevel = nullptr;
                    LOG(0, "IME {C[i1]}. {i2} Removed order from level. Level={d1} LevelSize={i3} ID={i4}", getContractName(), o->_side, ToDollars(o->_externalPrice), level.getSize(), o->_id)
                    doInternalReductionAfterExternalFill(o, oneFill, map);
                    filledSize -= oneFill;
                    if (orderIt == orders.end()) {
                        LOG(0, "IME {C[i1]}. Erasing level. No more orders. Price={d1}", getContractName(), ToDollars(level._price))
                        levelIt = levels.erase(levelIt);
                        break;
                    }
                } else {
                    LOG(0, "IME {C[i1]}. Match. Order size > filled size. Order={i2} Price={d1} Size={i3}", getContractName(), o->_id, ToDollars(o->_externalPrice), o->_externalSize)
                    _executor->onExternalFill(_internalId, o->_strategy, filledPrice, filledSize, o->_side, fillSource);
                    o->_externalSize -= filledSize;
                    doInternalReductionAfterExternalFill(o, filledSize, map);
                    return;
                }
            }
        }
        if (filledSize > 0) {
            const uint32_t side = ToViperSide(order.side);
            LOG(0, "IME {C[i1]}. Send overfill to MM. Side={i2} Price={d1} Size={i3}", getContractName(), side, ToDollars(filledPrice), filledSize)
            _executor->sendFillToStrategy(_internalId, side, filledPrice, filledSize, StrategyId::MarketMaker, FillType::External, FillSource::Overfill);
        }
    }

    void getAllOutOfMarket(bool cancelGtc = false) {
        LOG(0, "IME {C[i1]}. Removing all orders from market.", getContractName())
        for (uint32_t i = 0; i < toUInt(StrategyId::Execution); ++i) {
            _internalMarketNotification.emplace(i);
            removeOrderByStrategy<BID>((StrategyId)i);
            removeOrderByStrategy<ASK>((StrategyId)i);
        }
        for (auto& topLevelBySide : _topLevelsByStrategyBySide) {
            topLevelBySide[BID] = hf::NULL_PRICE;
            topLevelBySide[ASK] = hf::NULL_PRICE;
        }
        if (_contractCanCancel & _gatewayActive) {
            _om[BID].getAllOutOfMarket(cancelGtc);
            _om[ASK].getAllOutOfMarket(cancelGtc);
        }
        sendInternalMarketData();
    }

    void getStrategyOutOfMarket(StrategyId strategyId, bool disableGTC = false, bool cancelGTC = false) {
        _internalMarketNotification.emplace(toUInt(strategyId));
        if (disableGTC) {
            _om[BID]._orderType = om::ORDER_TYPE::LMT;
            _om[ASK]._orderType = om::ORDER_TYPE::LMT;
            _executor->notifyFrontUseGTCDisabled(_internalId);
        }

        if (_executor->isOtherStrategyEnabled(strategyId)) {
            removeOrderByStrategy<BID>(strategyId);
            removeOrderByStrategy<ASK>(strategyId);
            _topLevelsByStrategyBySide[toUInt(strategyId)][BID] = hf::NULL_PRICE;
            _topLevelsByStrategyBySide[toUInt(strategyId)][ASK] = hf::NULL_PRICE;

            if (_contractCanCancel & _gatewayActive) {
                if (strategyId == StrategyId::MarketMaker) {
                    _om[BID].getHedgeOutOfMarket();
                    _om[ASK].getHedgeOutOfMarket();
                }
                synchronizeOrders();
            }
            sendInternalMarketData();
        } else {
            getAllOutOfMarket(cancelGTC);
        }
    }

    void onOrderExecution(const om::OMExecutionEvent &event, const om::order_t &order) {
        uint32_t side = ToViperSide(order.side);
        _om[side].onOrderExecution(event, order);
        allocateExternalFills(event, order, side);
        sendInternalMarketData();
    }

    void onOrderAccepted(const om::OMOrderAcceptedEvent& event, const om::order_t& order) {
        const uint32_t side = ToViperSide(order.side);
        _om[side].onOrderAccepted(event, order);
    }

    void onOrderRejected(const om::OMOrderRejectedEvent& event, const om::order_t& order) {
        const uint32_t side = ToViperSide(order.side);
        _om[side].onOrderRejected(event, order);
    }

    void onOrderCanceled(const om::OMOrderCanceledEvent& event, const om::order_t& order) {
        int64_t orderOwner = order.debug.i[ORDER_OWNER];
        if (BOOST_LIKELY(orderOwner == OM_OWNER)) {
            const uint32_t side = ToViperSide(order.side);
            _om[side].onOrderCanceled(event, order);
        }
        else if(orderOwner == UNASSIGNED_OWNER) {
            removeOrderFromGTCReload(order);
        }
    }

    void handleSMPUnsolicitedCancel(const om::order_t& order, uint32_t side, hf::price_t restingPrice, uint64_t seqNum, bool isHedge) {
        double price = ToDollars(restingPrice);
        LOG(0, "IME {C[i1]}. SMP Unsolicited Cancel. Side={i2} Price={d1} OID={i3} EventSeqNum={i4} IsHedge={i5}", getContractName(), side, price, order.oid, seqNum, isHedge)
        std::string message =  boost::str(boost::format("SMP Unsolicited Cancel. Contract=%1% Side=%2% Price=%3% OID=%4%") % _name % side % price % order.oid);
        _executor->sendErrorMessage(message, ErrorType::Normal);
        if (!isHedge) {
            if (side == BID) {
                startFillBanner<BID>(restingPrice, 0, 1);
            } else {
                startFillBanner<ASK>(restingPrice, 0, 1);
            }
        }
    }

    void onOrderReplaced(const om::OMOrderModifiedEvent& event, const om::order_t& order) {
        const uint32_t side = ToViperSide(order.side);
        _om[side].onOrderReplaced(event, order);
    }

    void onOrderReplaceRejected(const om::OMOrderModifyRejectedEvent& event, const om::order_t& order) {
        const uint32_t side = ToViperSide(order.side);
        _om[side].onOrderReplaceRejected(event, order);
    }

    void onOrderCancelRejected(const om::OMOrderCancelRejectedEvent &event, const om::order_t &order) {
        const uint32_t side = ToViperSide(order.side);
        int64_t orderOwner = order.debug.i[ORDER_OWNER];
        BOOST_ASSERT((orderOwner == OM_OWNER) || (orderOwner == UNASSIGNED_OWNER) || (orderOwner == GTC_OWNER));

        if(BOOST_LIKELY(orderOwner == OM_OWNER)) {
            _om[side].onOrderCancelRejected(event, order);
        }
        else {
            [this, &event, &order, orderOwner]() NEVER_INLINE COLD {
                if(orderOwner == UNASSIGNED_OWNER) {
                    auto reasonString = std::string(" sym=") + _name + " oid=" + std::to_string(order.oid) + " side=" + std::to_string(static_cast<char>(order.side)) + " px=" + std::to_string(ToDollars(order.pr)) + " reason=" + event.reasonText;;
                    if(BOOST_UNLIKELY(WasRejectedDueToBeingFilled(event.reasonCodeExt, event.reasonText, order.venue))) {
                        _executor->sendErrorMessage("GTC cancel-rejected in response to manual cancel.  Check ace/firmsoft?" + reasonString, ErrorType::Critical);
                    }
                    else {
                        _executor->sendErrorMessage("GTC cancel-rejected in response to manual cancel.  Order filled." + reasonString, ErrorType::Critical);
                    }
                }
                else if(orderOwner == GTC_OWNER) {
                    retryGtcCancel(event, order);
                }
            }();
        }
    }

    void handleStackerParameterUpdate(const proto::GridCell& gridCell) {
        StackerParametersColumns columnType = static_cast<StackerParametersColumns>(gridCell.col());
        switch (columnType) {
            case StackerParametersColumns::QuoteSize: {
                hf::qty_t size = atoi(gridCell.value().c_str());
                    _om[BID].updateQuoteSize(size, _stackerEnabled);
                    _om[ASK].updateQuoteSize(size, _stackerEnabled);
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
                _om[BID].template updateNumberOfLevels<BID>(levels, _stackerEnabled);
                _om[ASK].template updateNumberOfLevels<ASK>(levels, _stackerEnabled);
                break;
            }
            case StackerParametersColumns::StackSlack: {
                const uint32_t stackSlack = atoi(gridCell.value().c_str());
                _om[BID].template updateMaxSlackSize<BID>(stackSlack);
                _om[ASK].template updateMaxSlackSize<ASK>(stackSlack);
                break;
            }
            case StackerParametersColumns::MaxTicksOffTopLevel: {
                const int32_t maxTicksOffTopLevel = atoi(gridCell.value().c_str());
                _maxOffTopLevel = maxTicksOffTopLevel * _tickSize;
                LOG(0, "IME {C[i1]}. Update MaxTicksOffTopLevel. N={i2} Converted={i3}", getContractName(), maxTicksOffTopLevel, _maxOffTopLevel)
                break;
            }
            case StackerParametersColumns::UseGTC: {
                bool useGtc = 1 == atoi(gridCell.value().c_str());
                updateUseGtc(useGtc);
                break;
            }
            case StackerParametersColumns::AllowMultipleNewHedge: {
                bool allowHedgeInFlight = 1 == atoi(gridCell.value().c_str());
                _om[BID].template updateAllowMultipleNewHedge<BID>(allowHedgeInFlight);
                _om[ASK].template updateAllowMultipleNewHedge<ASK>(allowHedgeInFlight);
                break;
            }
        }
    }

    void updateUseGtc(bool useGtc) {
        *_contract._useGTC = useGtc;
        auto marketMaker = _executor->template getStrategy<MarketMaker>(StrategyId::MarketMaker);
        auto& mm = marketMaker->_marketMakers[_internalId];
        bool forceDeleteHedge = !mm.isEnabled() & !useGtc;
        bool forceDeleteQuote = (!mm.isEnabled() | !mm.canQuoteExternally()) & !useGtc;
        _om[BID].template updateUseGtc<BID>(useGtc, !forceDeleteHedge, !forceDeleteQuote);
        _om[ASK].template updateUseGtc<ASK>(useGtc, !forceDeleteHedge, !forceDeleteQuote);
    }

    void handleRejectBannerUpdate(uint64_t period) {
        _fillBanner[BID]._rejectBanPeriod = period;
        _fillBanner[ASK]._rejectBanPeriod = period;
        LOG(0, "IME {C[i1]}. fill banner reject changed. Value={i2}", getContractName(), period)
    }

    void handleQuoteBannerUpdate(uint64_t period) {
        _quoteBanner[BID]._banPeriod = period;
        _quoteBanner[ASK]._banPeriod = period;
        LOG(0, "IME {C[i1]}. quote banner changed. Value={i2}", getContractName(), period)
    }

    void handleFillBannerUpdate(uint64_t period) {
        _fillBanner[BID]._banPeriod = period;
        _fillBanner[ASK]._banPeriod = period;
        LOG(0, "IME {C[i1]}. fill banner changed. Value={i2}", getContractName(), period)
    }

    void handleHedgeQuoteBannerUpdate(uint64_t period) {
        _hedgeBanner[BID]._banPeriod = period;
        _hedgeBanner[ASK]._banPeriod = period;
        LOG(0, "IME {C[i1]}. hedge quote banner period changed. Value={i2}", getContractName(), period)
    }

    void notifyLevelFree(uint32_t side, hf::price_t price) {
        uint32_t otherSide = GetOtherSide(side);
        _om[otherSide].onLevelFree(price);
    }

    void notifyLevelUsed(uint32_t side, hf::price_t price) {
        uint32_t otherSide = GetOtherSide(side);
        _om[otherSide].onLevelUsed(price);
    }

    void handleNewOrderReject(uint32_t side, hf::price_t price, const std::string& reason) {
        std::string message =  boost::str(boost::format("New order reject. Contract=%1% Side=%2% Price=%3% Reason=%4%") % _name % side % ToDollars(price) % reason);
        _executor->sendErrorMessage(message, ErrorType::Critical);
        LOG(0, "IME {C[i1]}. New order reject. Side={i2} Price={d1}", getContractName(), side, ToDollars(price))
        if (side == BID) {
            startFillBanner<BID>(price, 0, 1);
        } else {
            startFillBanner<ASK>(price, 0, 1);
        }
    }

    void handleSMPModifyReject(uint32_t side, hf::price_t price, bool isHedge, const std::string& reason) {
        std::string message =  boost::str(boost::format("(explicit) SMP modify reject. Contract=%1% Side=%2% Price=%3% Reason=%4%") % _name % side % ToDollars(price) % reason);
        _executor->sendErrorMessage(message, ErrorType::Critical);
        LOG(0, "IME {C[i1]}. (explicit) SMP modify reject. Side={i2} Price={d1}", getContractName(), side, ToDollars(price))
        if (!isHedge) {
            if (side == BID) {
                startFillBanner<BID>(price, 0, 1);
            } else {
                startFillBanner<ASK>(price, 0, 1);
            }
        }
    }

    void addGtcOrder(uint32_t side, int64_t route, const om::order_t& order) {
        bool addOrder = order.is_active;
        if (addOrder) {
            auto& orders = _initialGTC[side][order.pr];
            om::order_t& mutableOrder = const_cast<om::order_t&>(order);
            mutableOrder.debug.i[ORDER_OWNER] = UNASSIGNED_OWNER;
            mutableOrder.debug.i[INTERNAL_ID] = _internalId;
            mutableOrder.debug.i[ROUTE] = route;
            orders.push_back(&order);
            LOG(0, "IME {C[i1]}. Side={i2}. Added GTC. OID={i3} Price={d1} Size={i4}", getContractName(), side, order.oid, ToDollars(order.pr), order.qty)
        }
    }

    void tryRemoveGtcOrder(const om::order_t& order) {
        auto side = (order.side == hf::SIDE::BUY) ? BID : ASK;
        auto& ordersByPrice = _initialGTC[side];

        auto vecIter = std::begin(ordersByPrice);
        while(vecIter != std::end(ordersByPrice)) {
            auto& vec = vecIter->second;
            auto orderIter = std::begin(vec);
            while(orderIter != std::end(vec)) {
                if(*orderIter == &order) {
                    vec.erase(orderIter);
                    if(vec.empty()) {
                        ordersByPrice.erase(vecIter);
                    }
                    return;
                }
                ++orderIter;
            }
            ++vecIter;
        }
        LOG(0, "Attempting to remove order from gtcReload collection on full fill.  Contract={C[i1]} Side={i2} Price={d1} oid={i3}", getContractName(), (char)order.side, ToDollars(order.pr), order.oid);
        if(BOOST_LIKELY(_executor != nullptr)) {
            auto formatStr = CREATE_SAFE_FORMATTER("Attempting to remove order from gtcReload collection on full fill.  Contract=%1% Side=%2% Price=%3% oid=%4%");
            _executor->sendErrorMessage(static_cast<std::string>(formatStr % _name % (char)order.side % ToDollars(order.pr) % order.oid), ErrorType::Critical);
        }
    }

    std::vector<const om::order_t*>& getGTCs(hf::price_t price, uint32_t side) {
        auto it = _initialGTC[side].find(price);
        if (it != _initialGTC[side].end()) {
            return it->second;
        } else {
            static std::vector<const om::order_t*> NO_ORDERS;
            return NO_ORDERS;
        }
    }

    template<uint32_t side>
    bool hasGTCs() {
        return !_initialGTC[side].empty();
    }

    COLD
    void cancelRemainingGtc() {
        if(!(_initialGTC[BID].empty() && _initialGTC[ASK].empty())) {
            LOG(0, "IME {C[i1]}.  Unassigned GTCs remain after enable.  Cancelling remainder.  bidCount={i2} offerCount={i3}", getContractName(), _initialGTC[BID].size(), _initialGTC[ASK].size());
        }
        cancelGtc(_initialGTC[BID]);
        _initialGTC[BID].clear();
        cancelGtc(_initialGTC[ASK]);
        _initialGTC[ASK].clear();
    }

    void cancelGtc(std::unordered_map<hf::price_t, std::vector<const om::order_t*>>& map) {
        for (auto priceLevel : map) {
            for (auto order : priceLevel.second) {
                om::order_t& mutableOrder = const_cast<om::order_t&>(*order);
                mutableOrder.debug.i[INTERNAL_ID] = _internalId;
                mutableOrder.debug.i[ORDER_OWNER] = GTC_OWNER;
                _executor->cancelOrder(_internalId, order);
            }
        }
    }

    void removeOrderFromGTCReload(const om::order_t& order) {

        auto& gtcReloadCollection = order.side == hf::SIDE::BUY ? _initialGTC[BID] : _initialGTC[ASK];

        //To be safe we're not going to index by price
        auto kvpIter = gtcReloadCollection.begin();
        while(kvpIter != gtcReloadCollection.end()) {
            auto& vec = kvpIter->second;
            auto vecIter = vec.begin();
            while(vecIter != vec.end()) {
                if(*vecIter == &order) {
                    vec.erase(vecIter);
                    if(vec.empty()) {
                        gtcReloadCollection.erase(kvpIter);
                    }
                    LOG(0, "Removing order from gtcReload collection on cancel.  Contract={C[i1]} Side={i2} Price={d1} size={i5} oid={i4}", getContractName(), (char)order.side, ToDollars(order.pr), (char)order.side, order.oid);
                    std::string message = boost::str(boost::format("Removing order from gtcReload collection.  Contract=%1% Side=%2% Price=%3% size=%4% oid=%5%") % _name % (char)order.side % ToDollars(order.pr) % order.qty %order.oid);
                    if(_executor) {
                        _executor->sendErrorMessage(message, ErrorType::Normal);
                    }
                    return;
                }
                ++vecIter;
            }
            ++kvpIter;
        }
        LOG(0, "Failed to remove order from gtcReload collection.   Contract={C[i1]} Side={i2} Price={d1} size={i5} oid={i4}", getContractName(), (char)order.side, ToDollars(order.pr), (char)order.side, order.oid);
        std::string message = boost::str(boost::format( "Failed to remove order from gtcReload collection.   Contract=%1% Side=%2% Price=%3% size=%4% oid=%5%") % _name % (char)order.side % ToDollars(order.pr) % order.qty % order.oid);
        if(_executor) {
            _executor->sendErrorMessage(message, ErrorType::Critical);
        }
    }

    void retryGtcCancel(const om::OMOrderCancelRejectedEvent &event, const om::order_t& order) {
        constexpr auto retryLimit = 10;
        bool rejectedDueToFill = WasRejectedDueToBeingFilled(event.reasonCodeExt, event.reasonText, order.venue);
        if (!rejectedDueToFill) {
            auto orderInfoString = std::string(" oid=") + std::to_string(order.oid) + " Contract=" + _name + "Side=" + std::to_string(static_cast<char>(order.side)) + "Price= " + std::to_string(ToDollars(order.pr));
            if (order.debug.d[D_GTC_CR_RETRY_LIMIT] <= retryLimit) {
                const_cast<om::order_t &>(order).debug.d[D_GTC_CR_RETRY_LIMIT] += 1;
                _executor->sendErrorMessage("Retrying GTC cancel due to reject." + orderInfoString, ErrorType::Critical);
                _executor->cancelOrder(_internalId, &order);
            }
            else {
                _executor->sendErrorMessage("GTC cancel rejected, retry limit hit.  This order might be unamanged. " + orderInfoString, ErrorType::Critical);
            }
        }
    }

    void disableMarketMakerAndRV(const std::string& message) {
        _executor->disableMarketMakerAndRV(_internalId, message);
    }

    void disableRVDisableMMExternalQuote(const std::string& message) {
        _executor->disableRVDisableMMExternalQuote(_internalId, message);
    }

    void updateHedgeTolerance(hf::price_t hedgeTolerance) {
        _relativeValueHedgeMatchingTolerance = hedgeTolerance;
        LOG(0, "Hedge tolerance change. Name={C[i1]} Value={d1}", getContractName(),ToDollars(hedgeTolerance))
    }

    template<uint32_t Side>
    std::tuple<hf::qty_t, bool> onFAKHedgeOrder(StrategyId strategy, OrderRequest::Hedge& hedge) {
        LOG(0, "IME {C[i1]}. onHedgeOrder from {i2}. Side={i3} InternalPrice={d1} ExternalPrice={d2} HedgeSize={i4} HedgeId={i5} MarketId={i6} IsExternal={i7}",
            getContractName(), strategyName(strategy), Side, ToDollars(hedge._internalPrice),
            ToDollars(hedge._externalPrice), hedge._size, hedge._hedgeId, hedge._marketId, hedge._isExternal)
        const hf::qty_t unmatchedSize = tryMatch<Side>(hedge._externalPrice, hedge._size, _relativeValueHedgeMatchingTolerance, strategy);
        const hf::qty_t fillSize = hedge._size - unmatchedSize;
        if (fillSize > 0) {
            const hf::qty_t sizeToSend = hedge._isExternal ? fillSize : hedge._size;
            _executor->sendFAKHedgeInternalFillToFront(strategy, _internalId, Side, hedge._externalPrice, sizeToSend, hedge._stf);
            synchronizeQuote<1 - Side>(clockUtil::GetCycles());
        }
        if (hedge._isExternal) {
            if (unmatchedSize != 0) {
                hedge._size = unmatchedSize;
                if(BOOST_LIKELY(_executor->sendExternalFAKHedge(strategy, _internalId, Side, hedge))) {
                    LOG(0, "IME {C[i1]}. Sending {i2} hedge to external market. Side={i3} Price={d1} Size={i4}", getContractName(), strategyName(strategy), Side, ToDollars(hedge._externalPrice), hedge._size)
                }
                else {
                    LOG(0, "IME {C[i1]}. Sending {i2} FAK Hedge to external market failed! Side={i3} Price={d1} Size={i4}", getContractName(), strategyName(strategy), Side, ToDollars(hedge._externalPrice), hedge._size);
                    return std::make_tuple(unmatchedSize, false);
                }
            } else {
                _executor->sendHedgeMarketDataAdjustmentAckWithZeroSize(strategy, _internalId, Side, hedge);
            }
        } else {
            if (fillSize == 0) {
                _executor->sendFAKHedgeInternalFillToFront(strategy, _internalId, Side, hedge._externalPrice, hedge._size, hedge._stf);
            }
            _executor->onInternalHedgeDone(strategy, _internalId, hedge._marketId, GetOtherSide<Side>(), hedge._hedgeId);
        }
        return std::make_tuple(unmatchedSize, true);
    }

    void processOrderRequests(StrategyId strategy) {
        _internalMarketNotification.emplace(toUInt(strategy));

        auto startTimeStamp = clockUtil::GetCycles();
        auto lastReadyRequests = _orderRequestMgr.getLastReadyRequests(_internalId, strategy);
        TLOG(0, "IME {C[i1]}. order request. Strategy={i2} lastReadRequests=({i3} {i4} {i5} {i6})", getContractName(),
             strategyName(strategy), lastReadyRequests->_quote[BID], lastReadyRequests->_quote[ASK], lastReadyRequests->_hedge[BID], lastReadyRequests->_hedge[ASK])

        bool execute = false;
        bool hasRequest = processOrderRequests<BID>(orderId(strategy, Order::Type::Quoting), lastReadyRequests->_quote[BID], execute);
        hasRequest |= processOrderRequests<ASK>(orderId(strategy, Order::Type::Quoting), lastReadyRequests->_quote[ASK], execute);
        hasRequest |= processOrderRequests<BID>(orderId(strategy, Order::Type::Hedging), lastReadyRequests->_hedge[BID], execute);
        hasRequest |= processOrderRequests<ASK>(orderId(strategy, Order::Type::Hedging), lastReadyRequests->_hedge[ASK], execute);

//        const uint64_t timestamp1 = clockUtil::GetCycles();
//        DLOG(0, "processOrderRequests OrderModify Latency {i1}", (timestamp1 - startTimeStamp));

        if (execute) {
            const bool matchOk = matchOrders(strategy);
            if (matchOk) {
//                const uint64_t timestamp2 = clockUtil::GetCycles();
//                DLOG(0, "processOrderRequests Matching Latency {i1}", (timestamp2 - timestamp1));
                synchronizeOrders();
//                DLOG(0, "processOrderRequests Execution Latency {i1}", (clockUtil::GetCycles() - timestamp2));
                sendInternalMarketData();
            } else {
                //Currently the only way this can fail
                disableRVDisableMMExternalQuote("Self crossed");
                //sendInternalMarketData() is call inside disableRVDisableMMExternalQuote path
                if (++_selfMatchCounter >= MAX_SELF_MATCH_THRESHOLD) {
                    _executor->onCriticalError("Self match reached 50 times. Shutting Viper down.");
                }
            }
        }
        sendInternalFills();

        if (hasRequest) {
            const uint64_t latency = clockUtil::GetCycles() - startTimeStamp;
            DLOG(0, "processOrderRequests Total Latency {i1}", latency);
        }
    }

    template<uint32_t Side>
    bool processOrderRequests(uint8_t id, uint64_t lastRequest, bool& execute) {
        auto& requestQueue = (*_orderRequestQueue)[id]._queueBySide[Side];
        if (requestQueue._processedCursor == lastRequest) {
            TLOG(0, "No new order request. id={i1} processed={i2} lastRequest={i3}", toUInt(id), requestQueue._processedCursor, lastRequest)
            return false;
        }

        if (id > orderId(StrategyId::MarketMaker, Order::Type::Quoting)) {
            ++requestQueue._processedCursor;
            auto& request = requestQueue._queue[requestQueue._processedCursor];
            execute |= processOrderRequest<Side>(request, requestQueue._processedCursor);
        } else {
            uint32_t drainSize = lastRequest - (requestQueue._processedCursor + 1) + 1;

            // process the last request to avoid order flickering
            auto& request = requestQueue._queue[lastRequest];
            execute |= processOrderRequest<Side>(request, lastRequest);
            requestQueue._processedCursor = lastRequest;

            if (drainSize) {
                DLOG(0, "IME {C[i1]}. Order request. ID={i2} Side={i3} Skip {i4}", getContractName(), toUInt(id), Side,
                     (drainSize - 1));
            }
        }
        return true;
    }

    template<uint32_t Side>
    bool processOrderRequest(OrderRequest& request, uint64_t reqId) {
        Order& order = getOrders<Side>()[request._id];
        if (order._type == Order::Type::Quoting) {
            if (_executor->getStrategy(order._strategy)->isEnabled()) {
                auto& quote = request._quote;
                LOG(0, "IME {C[i1]}. Quote Order request from {i2}. ID={i3} Side={i4} Internal: {i5}@{d1} External: {i6}@{d2} persist={i7} reqId={i8}",
                    getContractName(), strategyName(order._strategy), order._id, Side, quote._internalSize,
                    ToDollars(quote._internalPrice), quote._externalSize, ToDollars(quote._externalPrice),
                    quote._persist, reqId)

                modifyOrder<Side>(order, quote._internalPrice, quote._internalSize, quote._externalPrice,
                                  quote._externalSize, quote._persist, false);
                return true;
            } else {
                LOG(0, "IME {C[i1]}. Strategy disabled drop Quote Order request from {i2}. ID={i3} Side={i4} orderType={i5} reqId={i6}",
                    getContractName(), strategyName(order._strategy), order._id, Side, toUInt(order._type), reqId)
            }
        } else {
            auto& hedge = request._hedge;
            LOG(0, "IME {C[i1]}. Hedge Order request from {i2}. ID={i3} Side={i4} InternalPrice={d1} ExternalPrice={d2} Size={i5} HedgeId={i6} MarketId={i7} isExternal={i8} inPayUp={i9} reqId={i10}",
                getContractName(), strategyName(order._strategy), order._id, Side, ToDollars(hedge._internalPrice),
                ToDollars(hedge._externalPrice), hedge._size, hedge._hedgeId, hedge._marketId, hedge._isExternal,
                hedge._inPayUp, reqId)

            if (order._orderType == om::ORDER_TYPE::LMT) {
                modifyOrder<Side>(order, hedge._internalPrice, hedge._size, hedge._externalPrice, hedge._size,
                                  hedge._isExternal, hedge._inPayUp);
                return true;
            } else {
                handleFAKHedgeOrder<Side>(order._strategy, hedge);
            }
        }
        return false;
    }

    template<uint32_t Side>
    void handleFAKHedgeOrder(StrategyId strategy, OrderRequest::Hedge& hedge) {
        auto result = onFAKHedgeOrder<Side>(strategy, hedge);
        const auto& unmatchedSize = std::get<0>(result);
        //Note when this was written only external FAKs can trigger failure
        if ((unmatchedSize > 0) & !hedge._isExternal) {
            const uint32_t otherSide = GetOtherSide<Side>();
            LOG(0, "IME {C[i1]}. Missed internal only hedge. Back to {i2}. Unfilled={i3}", getContractName(), strategyName(strategy), unmatchedSize)
            _executor->sendFillToStrategy(_internalId, otherSide, hedge._externalPrice, unmatchedSize, strategy, FillType::Internal, FillSource::MissedHedge);
        }
        else if(BOOST_UNLIKELY(!std::get<1>(result))) {
            const auto otherSide = GetOtherSide<Side>();
            LOG(0, "IME {C[i1]}. Attempt to send FAK Hedge Order FAILED!  Back to {i2}.  Unfilled={i3}", getContractName(), strategyName(strategy), unmatchedSize);
            _executor->sendFillToStrategy(_internalId, otherSide, hedge._externalPrice, unmatchedSize, strategy, hedge._isExternal ? FillType::External : FillType::Internal, FillSource::MissedHedge);
        }
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

    // Internal order map
    BidOrdersMap _bids;
    AskOrdersMap _asks;

    // External order map
    std::map<hf::price_t, PriceLevel, std::greater<hf::price_t>> _bidLevels;
    std::map<hf::price_t, PriceLevel> _askLevels;

    hf::price_t _topLevelsByStrategyBySide[toUInt(StrategyId::Execution)][2];

    hf::price_t _relativeValueHedgeMatchingTolerance = 0;
    hf::price_t _tickSize;

    Order _bidOrders[MAX_ORDER_ID];
    Order _askOrders[MAX_ORDER_ID];

    bool _contractCanTrade = false;
    bool _contractCanCancel = false;
    bool _gatewayActive = true;
    bool _stackerEnabled = true;
    OrderManager<MeT> _om[2];
    Order* _pendingHedge[2] = {nullptr, nullptr};

    ExecutorT* _executor;

    ConfigContract _contract;
    uint32_t _internalId;

    Banner _quoteBanner[2];
    FillBanner _fillBanner[2];
    Banner _throttlingBanner[2];
    Banner _hedgeBanner[2];

    hf::price_t _maxOffTopLevel; //Negative value => disabled
    const Book* _marketBooks;

    std::vector<InternalFill> _internalFills;

    std::unordered_map<hf::price_t, std::vector<const om::order_t*>> _initialGTC[2];

    std::string _name;

    Messages _messages;

    std::bitset<2> _reconcileGtcOrders;
};

}

#endif //VIPER_INTERNAL_MATCHING_ENGINE_H
