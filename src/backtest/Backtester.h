#pragma once

#ifdef VIPER_BACKTEST

#include "src/Executor.h"
#include "src/RingBufferQueue.h"
#include "LimitBook.h"

#define MAX_BOOK 512
#define MAX_ORDERS 2 < 20

namespace Python {

class BackTester final : public Executor {
    uint64_t _begin;
    uint64_t _orderLatency;
    struct comparator {
        bool operator() (const ViperMessage* lhs, const ViperMessage* rhs) const {
            return (lhs->_timestamp > rhs->_timestamp);
        }
    };

    RingBufferQueue<> _event_pool {1 << 26};
    std::priority_queue<ViperMessage*, std::vector<ViperMessage*>, comparator> _event_queue;
    RingBuffer<om::strategy_order_t> _order_pool {MAX_ORDERS};
    std::vector<om::strategy_order_t*> _reusable_orders;
    std::unordered_map<om::strategy_order_id_t, om::strategy_order_t*, om::strategy_order_id_hash_t> _orders;

    LimitBook<BackTester> _limitBooks[MAX_BOOK];

    om::strategy_order_id_t::value_type _nextStratOid {0};
    bool _ready {false};

public:
    BackTester() {
        _reusable_orders.reserve(1 << 16);
    }

    static BackTester* get() noexcept;

    void onMDTradeDetails(const md::MDTradeDetailsEvent &/*event*/, const md::MDBatch &/*batch*/) override;
    void onEndTradeSummary(const md::MDEndTradeSummary &/*event*/, const md::MDBatch &/*batch*/) {}
    void onMDAddOrder(const md::MDAddOrderEvent &/*event*/, const md::MDBatch &/*batch*/) override;
    void onMDDeleteOrder(const md::MDDeleteOrderEvent &/*event*/, const md::MDBatch &/*batch*/) override;
    void onMDModifyOrder(const md::MDModifyOrderEvent &/*event*/, const md::MDBatch &/*batch*/) override;
    void onMDStatus(const md::MDStatusEvent &event, const md::MDBatch &batch) override;

    void onStratBegin() override;
    
    void onStratEnd() override {
        LOG(0, "Backtester: Ended. Duration: {i1}(s)", ((clockUtil::GetCycles() - _begin) / _frequency * 1000000000))
        rhone::util::Service::exitAll();
    }

    // This is the main event loop
    // onStratLoop is not called in RhoneTest
    void onStratIdle() override;

    om::order_t* createOrder(om::instance_id_t iid,
                             om::RISK_TYPE risk,
                             hf::VENUE venue,
                             uint32_t sid,
                             om::ORDER_TYPE type,
                             hf::SIDE side,
                             hf::price_t price,
                             hf::qty_t qty,
                             om::STP_ACTION stp = om::STP_ACTION::CANCEL_AGGRESSOR,
                             const char (* tag)[om::TAG_SIZE] = nullptr) override;



private:
    template<typename Backtester> friend class LimitBook;

    template<typename MsgT, typename MessagePopulateFunc>
    inline void pushEvent(ViperMessageType type, MessagePopulateFunc&& msgPopulateFunc, uint32_t msgDataSize = 0);

    ViperMessage* popEvent();

    void handleMDAddOrder(const md::MDAddOrderEvent& event, hf::VENUE venue);
    void handleMDDeleteOrder(const md::MDDeleteOrderEvent& event, hf::VENUE venue);
    void handleMDModifyOrder(const md::MDModifyOrderEvent& event, hf::VENUE venue);
    void handleMDTradeDetails(const md::MDTradeDetailsEvent& event, hf::VENUE venue);

    void submitOrder(const om::pr::SendOmsOrderNewEvent& event);
    void modifyOrder(const om::pr::SendOmsOrderModifyEvent& event);
    void cancelOrder(const om::pr::SendOmsOrderCancelEvent& event);
};

template<typename MsgT, typename MessagePopulateFunc>
inline void BackTester::pushEvent(ViperMessageType type, MessagePopulateFunc&& msgPopulateFunc, uint32_t msgDataSize) {
    uint32_t sz = CacheLineBuf::calculateReserveSize(sizeof(ViperMessage) + sizeof(MsgT) + msgDataSize);
    auto cursor = _event_pool.reserve(sz);
    auto& outMessage = reinterpret_cast<ViperMessage&>(_event_pool[cursor].data[0]);
    outMessage._size = sz;
    outMessage._type = type;
    outMessage._timestamp = rhone::util::getTimeStamp().time_since_epoch().count();
    auto& message = reinterpret_cast<MsgT&>(outMessage._data[0]);
    msgPopulateFunc(message);
    _event_pool.publish(cursor, sz);
    _event_queue.emplace(&outMessage);
}

}
#endif
