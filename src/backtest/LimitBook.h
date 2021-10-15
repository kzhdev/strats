#pragma once

#ifdef VIPER_BACKTEST

#include <map>
#include <deque>
#include <tuple>
#include "src/RingBuffer.h"
#include "om/om_event.h"
#include "md/md.h"

namespace Python {

template<typename Backtester>
class LimitBook {
    friend class ExchangeSimulator;

    struct limit_order_entry {
        hf::price_t pr = hf::NULL_PRICE;
        hf::qty_t qty = 0;
        hf::qty_t leavesQty = 0;
        hf::qty_t executedSize = 0;
        uint64_t priority = -1;
        om::exchange_order_id_t::value_type orderId = 0;
        int64_t enterTime = 0;
        hf::qty_t qtyInFront = 0;
        om::strategy_order_id_t::value_type stratOid = 0;
        uint32_t numOrdersInFront = 0;
        uint32_t side = BID;

        bool operator==(const limit_order_entry& r) const noexcept {
            return orderId == r.orderId;
        }

        void reset() noexcept {
            pr = hf::NULL_PRICE;
            qty = 0;
            leavesQty = 0;
            executedSize = 0;
            priority = -1;
            orderId = 0;
            enterTime = 0;
            stratOid = 0;
            qtyInFront = 0;
            numOrdersInFront = 0;
            side = BID;
        }
    };

    struct compare_limit_order {
        bool operator()(const limit_order_entry* a, const limit_order_entry* b) const noexcept {
            return a->priority < b->priority;
        }
    };

    RingBuffer<limit_order_entry> _limit_order_pool {1 << 20};
    std::unordered_map<om::exchange_order_id_t::value_type, limit_order_entry*> _limit_order_map;
    std::vector<limit_order_entry*> _reusable_limit_order;

    struct PriceLevel {
        hf::qty_t qty = 0;
        uint32_t numOrders = 0;
        std::set<limit_order_entry*, compare_limit_order> orders;
    };
    std::map<hf::price_t, PriceLevel, std::greater<>> _bidBook;
    std::map<hf::price_t, PriceLevel> _askBook;

    md::top_t _lastUpdate[2];

    uint64_t _next_priority = 0;
    om::exchange_order_id_t::value_type _next_exch_oid {0};
    hf::price_t _last_traded_px = 0;

    std::deque<hf::price_t> _traded_prices;

    uint32_t _fill_id = 0;

    std::string _name;
    uint32_t _internalId;
    uint32_t _sid;

public:
    LimitBook() {
        _reusable_limit_order.reserve(1 << 16);
    }

    virtual ~LimitBook() = default;

    LimitBook(const LimitBook&) = delete;
    LimitBook(LimitBook&&) = delete;
    LimitBook& operator=(const LimitBook& book) = delete;
    LimitBook& operator=(LimitBook&&) = delete;

    void init(const std::string& name, uint32_t internalId, uint32_t sid) {
        _name = name;
        _internalId = internalId;
        _sid = sid;
    }

    inline void onMDAddOrder(const md::MDAddOrderEvent& event, hf::VENUE venue);
    inline void onMDDeleteOrder(const md::MDDeleteOrderEvent& event, hf::VENUE venue);
    inline void onMDModifyOrder(const md::MDModifyOrderEvent& event, hf::VENUE venue);
    inline void onMDTradeDetails(const md::MDTradeDetailsEvent& event, hf::VENUE venue);
    inline void onMDUpdate(const md::MDUpdateEvent& event, hf::VENUE);

#ifdef IS_DEV
    template<uint32_t Side, typename Book>
    inline void verifyBook(const md::top_t& eventBook, Book& limitBook);
#endif

    template<uint32_t Side, typename Book>
    inline std::pair<uint32_t, uint32_t> prepareUpdate(md::top_t& eventBook, Book& limitBook);

    inline void submitOrder(const om::pr::SendOmsOrderNewEvent& event);
    inline void modifyOrder(const om::pr::SendOmsOrderModifyEvent& event);
    inline void cancelOrder(const om::pr::SendOmsOrderCancelEvent& event);

private:

#ifdef IS_DEV
    const std::string& logging_name() const noexcept {
        return _name;
    }
#else
    uint32_t logging_name() const noexcept {
        return _internalId;
    }
#endif

    limit_order_entry* allocateLimitOrder() noexcept {
        if (!_reusable_limit_order.empty()) {
            auto order = _reusable_limit_order.back();
            assert(_limit_order_map.find(order->orderId) == _limit_order_map.end());
            _reusable_limit_order.pop_back();
            order->reset();
            return order;
        }
        return &_limit_order_pool.next();
    }

    template<typename Book>
    inline bool addOrderToBook(Book& book, limit_order_entry* order, uint64_t priority);

    template<typename Book>
    inline void removeOrderFromBook(Book& book, limit_order_entry* order);

    template<typename Book, typename Iter>
    inline void removeOrderFromBook(Book& book, Iter iter, limit_order_entry* order);

    inline void addOrderToPriceLevel(PriceLevel& level, limit_order_entry* order);

    template<typename Iter>
    inline void updateQueuePosition(const PriceLevel& level, Iter iter, hf::qty_t diffQty, uint32_t diffOrders);

    template<typename Book>
    inline bool modifyOrder(Book& book, limit_order_entry* order, hf::price_t pr, hf::qty_t qty, uint64_t priority);

    inline void match(uint32_t sid,
                      hf::price_t price,
                      hf::qty_t qty,
                      om::exchange_order_id_t::value_type exchOid,
                      hf::VENUE venue,
                      limit_order_entry* order);

    template<uint32_t Side, typename Book>
    inline void match(Book& book,
                      hf::VENUE venue,
                      uint32_t sid,
                      hf::qty_t& remQty,
                      hf::price_t px,
                      om::exchange_order_id_t::value_type order_id,
                      om::ORDER_TYPE type,
                      om::strategy_order_id_t::value_type strat_oid);

    inline bool insertLimitOrder(limit_order_entry*& order,
                                 int64_t order_id,
                                 uint32_t side,
                                 hf::price_t px,
                                 hf::qty_t remQty,
                                 int64_t enterTime,
                                 uint64_t priority = -1);

    inline void rejectOrder(om::order_id_t order_id,
                            uint32_t side,
                            hf::price_t px,
                            hf::qty_t qty,
                            om::OrderRejectReason reason,
                            const char* func,
                            const char* reason_str);

    template<uint32_t Side, typename Book>
    inline bool handleTradeDetail(Book& book, const md::MDTradeDetailsEvent& event, hf::VENUE venue);
};

template<typename Backtester>
inline void LimitBook<Backtester>::onMDAddOrder(const md::MDAddOrderEvent& event, hf::VENUE venue)  {
    auto qty = event.q;
    limit_order_entry* order;
    if (insertLimitOrder(order, event.oid, ToViperSide(event.side), event.pr, qty, event.extTimeMatch, event.extTimeSent /* this is priority */)) {
        // this if the first order of the TOB
        match(event.sid, event.pr, event.q, static_cast<om::exchange_order_id_t::value_type>(event.oid), venue, order);
    }
}

template<typename Backtester>
inline void LimitBook<Backtester>::match(uint32_t sid, hf::price_t price, hf::qty_t qty, om::exchange_order_id_t::value_type exchOid, hf::VENUE venue, limit_order_entry* order) {
    if (order->side == BID) {
        match<ASK>(_askBook, venue, sid, qty, price, exchOid, om::ORDER_TYPE::LMT, order->stratOid);
        order->executedSize = order->leavesQty - qty;
        order->leavesQty = qty;
        if (!qty) {
            removeOrderFromBook(_bidBook, order);
        }
    } else {
        match<BID>(_bidBook, venue, sid, qty, price, exchOid, om::ORDER_TYPE::LMT, order->stratOid);
        order->executedSize = order->leavesQty - qty;
        order->leavesQty = qty;
        if (!qty) {
            removeOrderFromBook(_askBook, order);
        }
    }
}

template<typename Backtester>
inline void LimitBook<Backtester>::onMDDeleteOrder(const md::MDDeleteOrderEvent& event, hf::VENUE) {
    auto iter = _limit_order_map.find(event.oid);
    if (iter == _limit_order_map.end()) {
        return;
    }

    auto* order = iter->second;
    if (order->side == BID) {
        removeOrderFromBook(_bidBook, order);
    } else {
        removeOrderFromBook(_askBook, order);
    }
    _reusable_limit_order.emplace_back(order);
    _limit_order_map.erase(iter);
}

template<typename Backtester>
inline void LimitBook<Backtester>::onMDModifyOrder(const md::MDModifyOrderEvent& event, hf::VENUE venue) {
    auto iter = _limit_order_map.find(event.oid);
    if (iter == _limit_order_map.end()) {
        auto side = ToViperSide(event.side);
        limit_order_entry* order;
        if (insertLimitOrder(order, event.oid, side, event.pr, event.q, event.extTimeMatch, event.extTimeSent)) {
            // this if the first order of the TOB
            match(event.sid, event.pr, event.q, static_cast<om::exchange_order_id_t::value_type>(event.oid), venue, order);
        }
        return;
    }

    auto order = iter->second;
    assert(order->side == ToViperSide(event.side));

    bool needToMatch = false;
    if (order->side == BID) {
        needToMatch = modifyOrder(_bidBook, order, event.pr, event.q, event.extTimeSent);
    } else {
        needToMatch = modifyOrder(_askBook, order, event.pr, event.q, event.extTimeSent);
    }

    if (needToMatch) {
        match(event.sid, event.pr, event.q, static_cast<om::exchange_order_id_t::value_type>(event.oid), venue, order);
    }
}

template<typename Backtester>
template<typename Book>
inline bool LimitBook<Backtester>::modifyOrder(Book& book, limit_order_entry* order, hf::price_t pr, hf::qty_t qty, uint64_t priority) {
    TLOG(0, " - BACKTEST {C[i1]}. Side={i2} Modify order {i3} {i4}({i5} {i6})@{d1} -> {i7}({i8})@{d2} ours={i9}", logging_name(),
         order->side, order->orderId, order->qty, order->leavesQty, order->priority, ToDollars(order->pr), qty, priority, ToDollars(pr),
         (uint)(order->stratOid != 0))
    auto iter = book.find(order->pr);
    if (pr != order->pr) {
        if (BOOST_LIKELY(iter != book.end())) {
            removeOrderFromBook(book, iter, order);
        } else if (!order->stratOid && !order->leavesQty && order->priority >= priority) {
            // If the order's leavesQty == 0 and priority > order->priority, it might be an
            // iceberg order re-entry. Otherwise, this order is filled by our order. Ignore the modify
            return false;
        }

        order->pr = pr;
        order->leavesQty -= order->qty - qty;
        order->qty = qty;

        return addOrderToBook(book, order, priority);
    }

    if (BOOST_LIKELY(iter != book.end())) {
        auto& level = iter->second;
        assert(!level.empty());
        auto ordIter = level.orders.find(order);
        if (ordIter == level.orders.end()) {
            if (!order->stratOid && !order->leavesQty && order->priority >= priority) {
                // If the order's leavesQty == 0 and priority > order->priority, it might be an
                // iceberg order re-entry. Otherwise, this order is filled by our order. Ignore the modify
                TLOG(0, " - BACKTEST {C[i1]}. Ignore order modify. order filled by our order", logging_name())
                return false;
            }
            order->priority = (priority == static_cast<uint64_t>(-1)) ? (*level.orders.rbegin())->priority : priority;
            order->qty = qty;
            order->leavesQty = qty;
            addOrderToPriceLevel(level, order);
        } else {
            if (order->qty >= qty) {
                assert(order->priority == priority || priority == -1);
                auto diff = order->qty - qty;
                order->leavesQty -= diff;
                assert(order->leavesQty > 0);
                order->qty = qty;
                level.qty -= diff;
                updateQueuePosition(level, ordIter, -diff, 0);
                TLOG(0, " - BACKTEST {C[i1]}. Order size decreased. Level: {i2}({i3})", logging_name(), level.qty,
                     level.numOrders)
            } else {
                removeOrderFromBook(book, iter, order);
                order->qty = qty;
                addOrderToBook(book, order, priority);
            }
        }
    } else if (order->stratOid || order->leavesQty || order->priority < priority) {
            // If the order's leavesQty == 0 and priority > order->priority, it is an
            // iceberg order re-entry. Otherwise, this order is fully filled by our order. Ignore the modify
            TLOG(0, " - BACKTEST {C[i1]}. {i2} Modify order {i3}. Price level {d1} not found. Add order to book", logging_name(), order->side,
                 order->orderId, ToDollars(pr))
            return addOrderToBook(book, order, priority);
    }
    return false;
}

template<typename Backtester>
inline void LimitBook<Backtester>::onMDTradeDetails(const md::MDTradeDetailsEvent& event, hf::VENUE venue) {
    switch(event.aggressorSide) {
        case hf::SIDE::BUY:
            handleTradeDetail<ASK>(_askBook, event, venue);
            break;
        case hf::SIDE::SELL:
            handleTradeDetail<BID>(_bidBook, event, venue);
            break;
        case hf::SIDE::UNKNOWN: {
            if (!handleTradeDetail<ASK>(_askBook, event, venue)) {
                handleTradeDetail<BID>(_bidBook, event, venue);
            }
            break;
        }
    }
}

template<typename Backtester>
template<typename Book>
inline void LimitBook<Backtester>::removeOrderFromBook(Book& book, limit_order_entry* order) {
    auto iter = book.find(order->pr);
    if (iter == book.end()) {
        return;
    }

    removeOrderFromBook(book, iter, order);
}

template<typename Backtester>
template<typename Book, typename Iter>
inline void LimitBook<Backtester>::removeOrderFromBook(Book& book, Iter iter, limit_order_entry* order) {
    auto& level = iter->second;
    auto ordIter = level.orders.find(order);
    if (ordIter == level.orders.end()) {
        return;
    }

    ordIter = level.orders.erase(ordIter);
    if (level.orders.empty()) {
        TLOG(0, " - BACKTEST {C[i1]}. Side={i2} Last order removed at {d1}. OID={i3}", logging_name(), order->side,
             ToDollars(iter->first), order->orderId)
        book.erase(iter);
    } else {
        TLOG(0, " - BACKTEST {C[i1]}. Side={i2} remove order {i3} {i4}({i5})@{d1} ours={i6}. Level: {i7}({i8})", logging_name(),
             order->side, order->orderId, order->qty, order->leavesQty, ToDollars(order->pr),
             (uint32_t)(order->stratOid != 0), level.qty, level.numOrders)
        if (!order->stratOid) {
            level.qty -= order->leavesQty;
            --level.numOrders;
            updateQueuePosition(level, ordIter, -order->leavesQty, -1);
        }
        TLOG(0, " - BACKTEST {C[i1]}. Side={i2} after remove order {i3} Level: {i4}({i5})", logging_name(), order->side,
            order->orderId, level.qty, level.numOrders)
    }
}

template<typename Backtester>
template<typename Iter>
inline void LimitBook<Backtester>::updateQueuePosition(const PriceLevel& level, Iter iter, hf::qty_t diffQty, uint32_t diffOrders) {
    for (; iter != level.orders.end(); ++iter) {
        auto order = *iter;
        if (order->stratOid) {
            order->numOrdersInFront += diffOrders;
            order->qtyInFront += diffQty;
        }
    }
}

template<typename Backtester>
template<uint32_t Side, typename Book>
inline void LimitBook<Backtester>::match(Book& book,
                                         hf::VENUE venue,
                                         uint32_t sid,
                                         hf::qty_t& remQty,
                                         hf::price_t px,
                                         om::exchange_order_id_t::value_type order_id,
                                         om::ORDER_TYPE type,
                                         om::strategy_order_id_t::value_type strat_oid) {
    if (type == om::ORDER_TYPE::FAK) {
        assert(strat_order);
        auto iter = book.begin();
        hf::qty_t qty = 0;
        while (iter != book.end() && IsPriceBetterOrEqual<Side>(iter->first, px) && qty < remQty) {
            auto& level = iter->second;
            qty += level.qty;
        }

        if (qty < remQty) {
            // Can't fill the FAK order, send cancel
            Backtester::get()->template pushEvent<om::OMOrderCanceledEvent>(
                    ViperMessageType::OM_OrderCanceled,
                    [sid, strat_oid, px, remQty, venue, order_id](ViperMessage& viperMsg, om::OMOrderCanceledEvent& event) {
                        //TODO:
                        viperMsg._timestamp = rhone::util::getTimeStamp().time_since_epoch().count();
                        viperMsg._venue = venue;
                        event.strat_oid = strat_oid;
                        event.type = om::OMEvent::TYPE::OC;
                        if (venue == hf::VENUE::CME) {
                            event.reasonCodeExt = 67;
                        }
                        event.orderSnapshot.oid_ = strat_oid;
                        event.orderSnapshot.pr_ = px;
                        event.orderSnapshot.qty_ = remQty;
                        event.orderSnapshot.side_ = ToRhoneSide(1 - Side);
                        event.orderSnapshot.sid_ = sid;
                        event.orderSnapshot.exchange_order_id_ = order_id;
                    });
        }
    }

    auto iter = book.begin();
    while (remQty && iter != book.end() && IsPriceBetterOrEqual<Side>(iter->first, px)) {
        auto& level = iter->second;
        auto& orders = level.orders;
        auto ordIter = orders.begin();
        while (ordIter != orders.end()) {
            auto* order = *ordIter;
            // Self Match Prevention - Cancel Resting Order
            if (order->stratOid && strat_oid) {
                Backtester::get()->template pushEvent<om::OMOrderCanceledEvent>(
                        ViperMessageType::OM_OrderCanceled,
                        [order, sid, venue](ViperMessage& viperMsg, om::OMOrderCanceledEvent& event) {
                            //TODO:
                            viperMsg._timestamp = rhone::util::getTimeStamp().time_since_epoch().count();
                            viperMsg._venue = venue;
                            event.strat_oid = order->stratOid;
                            event.type = om::OMEvent::TYPE::OC;
                            if (venue == hf::VENUE::CME || venue == hf::VENUE::B3) {
                                event.reasonCodeExt = 103; // cancel resting on CME
                            } else if (venue == hf::VENUE::ICE) {
                                strcpy(event.reasonText, "noSelfTrading");
                            }
                            event.orderSnapshot.oid_ = order->stratOid;
                            event.orderSnapshot.pr_ = order->pr;
                            event.orderSnapshot.qty_ = order->leavesQty;
                            event.orderSnapshot.side_ = ToRhoneSide(order->side);
                            event.orderSnapshot.sid_ = sid;
                            event.orderSnapshot.exchange_order_id_ = order->orderId;
                        });
                ordIter = orders.erase(ordIter);
                if (_limit_order_map.erase(order->orderId)) {
                    _reusable_limit_order.emplace_back(order);
                } else {
                    assert(false);
                }
                continue;
            }

            if (order->stratOid) {
                Backtester::get()->template pushEvent<om::OMExecutionEvent>(
                        ViperMessageType::OM_OrderExecution,
                        [order, sid, venue](ViperMessage& viperMsg, om::OMExecutionEvent& event) {
                            //TODO:
                            viperMsg._timestamp = rhone::util::getTimeStamp().time_since_epoch().count();
                            viperMsg._venue = venue;
                            event.strat_oid = order->stratOid;
                            event.type = om::OMEvent::TYPE::OE;
                            event.exchangeOrderId = order->orderId;
                            event.orderSnapshot.oid_ = order->stratOid;
                            event.orderSnapshot.pr_ = order->pr;
                            event.orderSnapshot.qty_ = 0;
                            event.orderSnapshot.side_ = ToRhoneSide(order->side);
                            event.orderSnapshot.sid_ = sid;
                            event.orderSnapshot.venue_ = venue;
                            event.orderSnapshot.exchange_order_id_ = order->orderId;
                        });
                ordIter = orders.erase(ordIter);
                if (_limit_order_map.erase(order->orderId)) {
                    _reusable_limit_order.emplace_back(order);
                } else {
                    assert(false);
                }
                continue;
            }

            if (remQty <= order->leavesQty) {
                if (strat_oid) {
                    Backtester::get()->template pushEvent<om::OMExecutionEvent>(
                            ViperMessageType::OM_OrderExecution,
                            [order, strat_oid, sid, venue, order_id](ViperMessage& viperMsg, om::OMExecutionEvent& event) {
                                //TODO:
                                viperMsg._timestamp = rhone::util::getTimeStamp().time_since_epoch().count();
                                viperMsg._venue = venue;
                                event.strat_oid = strat_oid;
                                event.type = om::OMEvent::TYPE::OE;
                                event.exchangeOrderId = order_id;
                                event.orderSnapshot.oid_ = strat_oid;
                                event.orderSnapshot.pr_ = order->pr;
                                event.orderSnapshot.qty_ = 0;
                                event.orderSnapshot.side_ = ToRhoneSide(1 - Side);
                                event.orderSnapshot.sid_ = sid;
                                event.orderSnapshot.venue_ = venue;
                                event.orderSnapshot.exchange_order_id_ = order_id;
                            });
                }
                level.qty -= remQty;
                order->executedSize += remQty;
                order->leavesQty -= remQty;
                remQty = 0;
                break;
            } else {
                remQty -= order->leavesQty;
                order->executedSize += order->leavesQty;
                level.qty -= order->leavesQty;
                order->leavesQty = 0;
                ordIter = orders.erase(ordIter);
                // MD order will be removed from the order map when MD order delete received
                if (strat_oid) {
                    Backtester::get()->template pushEvent<om::OMExecutionEvent>(
                            ViperMessageType::OM_OrderExecution,
                            [order, strat_oid, sid, venue, remQty, order_id](ViperMessage& viperMsg, om::OMExecutionEvent& event) {
                                //TODO:
                                viperMsg._timestamp = rhone::util::getTimeStamp().time_since_epoch().count();
                                viperMsg._venue = venue;
                                event.strat_oid = strat_oid;
                                event.type = om::OMEvent::TYPE::OE;
                                event.exchangeOrderId = order_id;
                                event.orderSnapshot.oid_ = strat_oid;
                                event.orderSnapshot.pr_ = order->pr;
                                event.orderSnapshot.qty_ = remQty;
                                event.orderSnapshot.side_ = ToRhoneSide(1 - Side);
                                event.orderSnapshot.sid_ = sid;
                                event.orderSnapshot.venue_ = venue;
                                event.orderSnapshot.exchange_order_id_ = order_id;
                            });
                }
            }
        }

        if (orders.empty()) {
            iter = book.erase(iter);
        }
    }
}

template<typename Backtester>
inline bool LimitBook<Backtester>::insertLimitOrder(limit_order_entry*& order,
                                        int64_t order_id,
                                        uint32_t side,
                                        hf::price_t px,
                                        hf::qty_t remQty,
                                        int64_t enterTime,
                                        uint64_t priority) {

    order = allocateLimitOrder();

    order->orderId = order_id;
    order->side = side;
    order->pr = px;
    order->qty = remQty;
    order->leavesQty = remQty;
    order->enterTime = enterTime;

    _limit_order_map.emplace(order_id, order);
    if (side == BID) {
        return addOrderToBook(_bidBook, order, priority);
    }
    return addOrderToBook(_askBook, order, priority);
}

template<typename Backtester>
template<typename Book>
inline bool LimitBook<Backtester>::addOrderToBook(Book& book, limit_order_entry* order, uint64_t priority) {
    auto iter = book.find(order->pr);
    PriceLevel* level;
    if (iter == book.end()) {
        order->priority = (priority == static_cast<uint64_t>(-1)) ? 0 : priority;
        level = &book[order->pr];
    } else {
        level = &iter->second;
        order->priority = (priority == static_cast<uint64_t>(-1)) ? (*level->orders.rbegin())->priority : priority;
    }
    addOrderToPriceLevel(*level, order);
    return book.begin()->first == order->pr && book[order->pr].orders.size() == 1;
}

template<typename Backtester>
inline void LimitBook<Backtester>::addOrderToPriceLevel(PriceLevel& level, limit_order_entry* order) {
    TLOG(0, " - BACKTEST {C[i1]}. Side={i2} Add order {i3} {i4}({i5})@{d1} ours={i6}. Level: {i7}({i8})", logging_name(),
         order->side, order->orderId, order->qty, order->leavesQty, ToDollars(order->pr), (uint)(order->stratOid != 0),
         level.qty, level.numOrders)

    auto iter = std::next(level.orders.emplace(order).first);
    if (BOOST_LIKELY(iter == level.orders.end())) {
        // order added at the back
        if (!order->stratOid) {
            level.qty += order->leavesQty;
            ++level.numOrders;
        } else {
            order->qtyInFront = level.qty;
            order->numOrdersInFront = level.numOrders;
        }
    } else {
        TLOG(0, "Added order in middle");
        if (!order->stratOid) {
            level.qty += order->leavesQty;
            ++level.numOrders;
            updateQueuePosition(level, iter, order->leavesQty, 1);
        } else {
            hf::qty_t qtyInFront = 0;
            uint32_t numOrderInFront = 0;
            for (auto it = level.orders.begin(); it != iter; ++it) {
                if (!(*it)->stratOid) {
                    qtyInFront += (*it)->qty;
                    ++numOrderInFront;
                }
            }
            order->qtyInFront = qtyInFront;
            order->numOrdersInFront = numOrderInFront;
        }
    }

    TLOG(0, " - BACKTEST {C[i1]}. Side={i2} After add order {i3} Level: {i4}({i5})", logging_name(), order->side,
         order->orderId, level.qty, level.numOrders)
}

template<typename Backtester>
inline void LimitBook<Backtester>::submitOrder(const om::pr::SendOmsOrderNewEvent& event) {
    auto exchOid = ++_next_exch_oid;
    Backtester::get()->template pushEvent<om::OMOrderAcceptedEvent>(
            ViperMessageType::OM_OrderAccepted,
            [this, &event, exchOid](ViperMessage& viperMsg, om::OMOrderAcceptedEvent& actEvent) {
                // TODO:
                viperMsg._timestamp = rhone::util::getTimeStamp().time_since_epoch().count();
                viperMsg._venue = event.venue;
                actEvent.strat_oid = event.strat_oid;
                actEvent.acceptedPrice = event.price;
                actEvent.acceptedQty = event.qty;
                actEvent.type = om::OMEvent::TYPE::OA;
                actEvent.orderSnapshot.exchange_order_id_ = exchOid;
                actEvent.orderSnapshot.side_ = event.side;
                actEvent.orderSnapshot.sid_ = event.sid;
                actEvent.orderSnapshot.qty_ = event.qty;
                actEvent.orderSnapshot.oid_ = event.strat_oid;
                actEvent.orderSnapshot.pr_ = event.price;
            });

    hf::qty_t remQty = event.qty;
    auto side = ToViperSide(event.side);

    // check in instant fills and handle SMP cancel first
    if (side == BID) {
        match<ASK>(_askBook, event.venue, event.sid, remQty, event.price, exchOid, event.ot, event.strat_oid.value());
    } else {
        match<BID>(_bidBook, event.venue, event.sid, remQty, event.price, exchOid, event.ot, event.strat_oid.value());
    }

    // check in leftover to my limit orders
    if (remQty > 0 && event.ot != om::ORDER_TYPE::FAK) {
        // TODO: Fix timestamp
        limit_order_entry* limitOrder = nullptr;
        insertLimitOrder(limitOrder, exchOid, side, event.price, event.qty, rhone::util::getTimeStamp().time_since_epoch().count(), -1);
        limitOrder->executedSize = event.qty - remQty;
        limitOrder->leavesQty = remQty;
    }
}

template<typename Backtester>
inline void LimitBook<Backtester>::modifyOrder(const om::pr::SendOmsOrderModifyEvent& event) {
    auto iter = _limit_order_map.find(event.orderSnapshot.exchangeOrderId.value());
    if (iter == _limit_order_map.end()) {
        Backtester::get()->template pushEvent<om::OMOrderModifyRejectedEvent>(
                ViperMessageType::OM_OrderModifyRejected,
                [&event](ViperMessage& viperMsg, om::OMOrderModifyRejectedEvent& rejectEvent) {
                    //TODO:
                    viperMsg._timestamp = rhone::util::getTimeStamp().time_since_epoch().count();
                    viperMsg._venue = event.orderSnapshot.venue;
                    switch(event.orderSnapshot.venue) {
                        case hf::VENUE::ICE:
                            rejectEvent.reasonCodeExt = ice_rejects::TOO_LATE_TO_CANCEL;
                            break;
                        case hf::VENUE::B3:
                        case hf::VENUE::CME:
                            rejectEvent.reasonCodeExt = CME_ORDER_IS_NOT_IN_BOOK;
                            break;
                        case hf::VENUE::EUREX:
                            rejectEvent.reasonCodeExt = EUREX_ORDER_NOT_FOUND;
                            break;
                        default:
                            break;
                    }
                    rejectEvent.strat_oid = event.strat_oid;
                    memcpy(&rejectEvent.orderSnapshot, &event.orderSnapshot, sizeof(event.orderSnapshot));
                });
        return;
    }

    Backtester::get()->template pushEvent<om::OMOrderModifiedEvent>(
            ViperMessageType::OM_OrderModified,
            [&event](ViperMessage& viperMsg, om::OMOrderModifiedEvent& modifyEvent) {
                // TODO:
                viperMsg._timestamp = rhone::util::getTimeStamp().time_since_epoch().count();
                viperMsg._venue = event.orderSnapshot.venue;
                modifyEvent.strat_oid = event.strat_oid;
                memcpy(&modifyEvent.orderSnapshot, &event.orderSnapshot, sizeof(event.orderSnapshot));
            });

    auto order = iter->second;
    assert(order->side == ToViperSide(event.side));
    bool needToMatch = false;
    if (order->side == BID) {
        needToMatch = modifyOrder(_bidBook, order, event.price, event.qty, -1);
    } else {
        needToMatch = modifyOrder(_askBook, order, event.price, event.qty, -1);
    }

    if (needToMatch) {
        match(event.orderSnapshot.sid_, event.orderSnapshot.pr_, event.orderSnapshot.qty_,
              event.orderSnapshot.exchange_order_id_.value(), event.orderSnapshot.venue_, order);
    }
}

template<typename Backtester>
inline void LimitBook<Backtester>::cancelOrder(const om::pr::SendOmsOrderCancelEvent& event) {
    auto iter = _limit_order_map.find(event.orderSnapshot.exchangeOrderId.value());
    if (iter == _limit_order_map.end()) {
        Backtester::get()->template pushEvent<om::OMOrderModifyRejectedEvent>(
                ViperMessageType::OM_OrderModifyRejected,
                [&event](ViperMessage& viperMsg, om::OMOrderModifyRejectedEvent& rejectEvent) {
                    // TODO:
                    viperMsg._timestamp = rhone::util::getTimeStamp().time_since_epoch().count();
                    viperMsg._venue = event.orderSnapshot.venue;
                    switch(event.orderSnapshot.venue) {
                        case hf::VENUE::ICE:
                            rejectEvent.reasonCodeExt = ice_rejects::TOO_LATE_TO_CANCEL;
                            break;
                        case hf::VENUE::B3:
                        case hf::VENUE::CME:
                            rejectEvent.reasonCodeExt = CME_ORDER_IS_NOT_IN_BOOK;
                            break;
                        case hf::VENUE::EUREX:
                            rejectEvent.reasonCodeExt = EUREX_ORDER_NOT_FOUND;
                            break;
                        default:
                            break;
                    }
                    rejectEvent.strat_oid = event.strat_oid;
                    memcpy(&rejectEvent.orderSnapshot, &event.orderSnapshot, sizeof(event.orderSnapshot));
                });
        return;
    }

    auto order = iter->second;
    if (order->side == BID) {
        removeOrderFromBook(_bidBook, order);
    } else {
        removeOrderFromBook(_askBook, order);
    }

    Backtester::get()->template pushEvent<om::OMOrderCanceledEvent>(
            ViperMessageType::OM_OrderCanceled,
            [&event](ViperMessage& viperMsg, om::OMOrderCanceledEvent& cancelEvent) {
                //TODO:
                viperMsg._timestamp = rhone::util::getTimeStamp().time_since_epoch().count();
                viperMsg._venue = event.orderSnapshot.venue;
                memcpy(&cancelEvent.orderSnapshot, &event.orderSnapshot, sizeof(event.orderSnapshot));
            });
}

template<typename Backtester>
inline void LimitBook<Backtester>::rejectOrder(om::order_id_t order_id,
                                   uint32_t side,
                                   hf::price_t px,
                                   hf::qty_t qty,
                                   om::OrderRejectReason reason,
                                   const char* func,
                                   const char* reason_str) {
}

template<typename Backtester>
template<uint32_t Side, typename Book>
inline bool LimitBook<Backtester>::handleTradeDetail(Book& book, const md::MDTradeDetailsEvent& event, hf::VENUE venue) {
    bool orderFound = false;
    auto* trade_orders = event.orders;
    for (uint32_t i = 0; i < event.numberOfOrders; ++i) {
        auto& trade_order = trade_orders[i];
        auto orderIter = _limit_order_map.find(trade_order.oid);
        if (orderIter == _limit_order_map.end()) {
            continue;
        }

        orderFound = true;
        auto order = orderIter->second;
        assert(order->orderId == trade_order.oid);
        if (order->side != Side) {
            assert(false);
            continue;
        }

        auto newOrderQty = std::max<hf::qty_t>(0, order->qty - trade_order.qty);
        if (newOrderQty) {
            modifyOrder(book, order, order->pr, newOrderQty, order->priority);
        } else {
            removeOrderFromBook(book, order);
        }
    }
    return orderFound;
}

template<typename Backtester>
inline void LimitBook<Backtester>::onMDUpdate(const md::MDUpdateEvent& event, hf::VENUE venue) {
#ifdef IS_DEV
    if (event.sell.updatedQtyNum) {
        verifyBook<ASK>(event.sell.top, _askBook);
    }

    if (event.buy.updatedQtyNum) {
        verifyBook<BID>(event.buy.top, _bidBook);
    }
#endif

    auto askUpdateIndices = prepareUpdate<ASK>(_lastUpdate[ASK], _askBook);
    auto bidUpdateIndices = prepareUpdate<ASK>(_lastUpdate[ASK], _askBook);

    if (askUpdateIndices.second || bidUpdateIndices.second) {
        Backtester::get()->template pushEvent<md::MDUpdateEvent>(
                ViperMessageType::MD_UpdateToStrategy,
                [this, venue, &askUpdateIndices, &bidUpdateIndices](ViperMessage& viperMsg, md::MDUpdateEvent& event) {
                    //TODO:
                    viperMsg._timestamp = hf::util::TimeUtils::clock_gettime();
                    viperMsg._venue = venue;
                    if (askUpdateIndices.second) {
                        memcpy(&event.sell.top, &_lastUpdate[ASK], sizeof(md::top_t));
                    }
                    if (bidUpdateIndices.second) {
                        memcpy(&event.buy.top, &_lastUpdate[BID], sizeof(md::top_t));
                    }
                    event.sell.updatedPriceNum = askUpdateIndices.first;
                    event.sell.updatedQtyNum = askUpdateIndices.second;
                    event.buy.updatedPriceNum = bidUpdateIndices.first;
                    event.buy.updatedQtyNum = bidUpdateIndices.second;

                    event.size = toUInt(venue);
                });
    }
}

template<typename Backtester>
template<uint32_t Side, typename Book>
inline std::pair<uint32_t, uint32_t> LimitBook<Backtester>::prepareUpdate(md::top_t& eventBook, Book& limitBook) {
    uint32_t priceUpdateIndex = 0;
    uint32_t qtyUpdateIndex = 0;
    auto iter = limitBook.begin();
    uint32_t i = 0;
    for (; i < md::top_t::LEVEL_NUM && iter != limitBook.end(); ++i) {
        auto& l = eventBook.l[i];
        if (l.pr == hf::NULL_PRICE || l.q == 0) {
            break;
        }

        if (iter->first != l.pr) {
            if (!priceUpdateIndex) {
                priceUpdateIndex = i;
            }

            if (!qtyUpdateIndex) {
                qtyUpdateIndex = i;
            }

            l.pr = iter->first;
            l.q = iter->second.qty;
            l.o = iter->second.numOrders;
        } else if (iter->second.qty != l.q) {
            if (!qtyUpdateIndex) {
                qtyUpdateIndex = i;
            }
            l.q = iter->second.qty;
            l.o = iter->second.numOrders;
        }
    }

    while (i < md::top_t::LEVEL_NUM) {
        auto& l = eventBook.l[i];
        l.pr = hf::NULL_PRICE;
        l.q = 0;
        l.o = 0;
        if (!priceUpdateIndex) {
            priceUpdateIndex = i;
        }
        if (!qtyUpdateIndex) {
            qtyUpdateIndex = i;
        }
        ++i;
    }
    return std::make_pair(priceUpdateIndex, qtyUpdateIndex);
}

#ifdef IS_DEV
template<typename Backtester>
template<uint32_t Side, typename Book>
inline void LimitBook<Backtester>::verifyBook(const md::top_t& eventBook, Book& limitBook) {
    uint32_t i = 0;
    auto iter = limitBook.begin();
    for (uint32_t ii = 0; ii < md::top_t::LEVEL_NUM; ++ii) {
        auto& l = eventBook.l[ii];
        if (l.pr == hf::NULL_PRICE || l.q == 0) {
            break;
        }

        while (iter != limitBook.end() && IsBetterPrice<Side>(iter->first, l.pr)) {
            LOG(0, " - BACKTEST {C[i1]}. {i2} 0(0)@{d1} - {i3}({i4})", logging_name(), Side, ToDollars(iter->first), iter->second.qty, iter->second.numOrders)
            ++iter;
        }
        if (iter == limitBook.end()) {
            assert(iter2 == limitBook.end());
            LOG(0, " - BACKTEST {C[i1]}. {i2} {i3}({i4})@{d1} - not found in limit book", logging_name(), Side, l.q, l.o, ToDollars(l.pr))
        } else if (iter->first == l.pr) {
            if (l.q != iter->second.qty) {
                LOG(0, " - BACKTEST {C[i1]}. {i2} {i3}({i4})@{d1} - {i5}({i6})", logging_name(), Side, l.q, l.o, ToDollars(l.pr), iter->second.qty, iter->second.numOrders)
            } else {
                LOG(0, " - BACKTEST {C[i1]}. {i2} {i3}({i4})@{d1}", logging_name(), Side, l.q, l.o, ToDollars(l.pr))
            }
            ++iter;
        } else {
            LOG(0, " - BACKTEST {C[i1]}. {i2} {i3}({i4})@{d1} - not found in limit book", logging_name(), Side, l.q, l.o, ToDollars(l.pr))
        }
    }
}
#endif

}

#endif  //VIPER_BACKTEST