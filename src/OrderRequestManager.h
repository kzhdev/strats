#pragma once
#include "RingBufferQueue.h"
#include "RingBuffer.h"
#include <limits>
#include <atomic>

namespace Python {

struct OrderRequestQueueBySide {
    struct OrderRequestQueue {
        RingBufferQueue<OrderRequest> _queue;
        uint64_t _processedCursor = std::numeric_limits<uint64_t>::max();
    };
    OrderRequestQueue _queueBySide[2];
};

struct ReadyOrderRequests {
    uint64_t _quote[2] = {std::numeric_limits<uint64_t>::max(), std::numeric_limits<uint64_t>::max()};
    uint64_t _hedge[2] = {std::numeric_limits<uint64_t>::max(), std::numeric_limits<uint64_t>::max()};
};

using OrderRequestQueue = std::array<OrderRequestQueueBySide, MAX_ORDER_ID>;

class OrderRequestManager final {
    std::vector<OrderRequestQueue> _orderRequestQueues;
    std::vector<std::array<RingBuffer<ReadyOrderRequests>, toUInt(StrategyId::Execution)>> _readyRequestsBuffer;
    std::vector<std::atomic<ReadyOrderRequests*>*> _readyRequests;
    RingBufferQueue<>* _notificationQueue;

public:
    static OrderRequestManager& get() {
        static OrderRequestManager instance;
        return instance;
    }

    void init(const ViperConfig& viperConfig, uint32_t queueSize, RingBufferQueue<>* notificationQueue) {
        _orderRequestQueues.resize(viperConfig._allContracts.size());
        _readyRequestsBuffer.resize(viperConfig._allContracts.size());
        _readyRequests.resize(viperConfig._allContracts.size());
        _notificationQueue = notificationQueue;
        for (const auto& contract : viperConfig._allContracts) {
            uint32_t index;
            bool toMarketMake = viperConfig.isContractToMarketMake(contract._name, index);
            if (toMarketMake) {
                auto& queues = _orderRequestQueues[contract._internalId];
                for (auto& queue : queues) {
                    queue._queueBySide[BID]._queue.init(queueSize);
                    queue._queueBySide[ASK]._queue.init(queueSize);
                }

                _readyRequests[contract._internalId] = new std::atomic<ReadyOrderRequests*>[toUInt(StrategyId::Execution)];
                for (uint32_t i = 0; i < toUInt(StrategyId::Execution); ++i) {
                    _readyRequestsBuffer[contract._internalId][i].init(queueSize);
                    _readyRequests[contract._internalId][i] = nullptr;
                }
            }
        }
    }

    OrderRequestQueue& getRequestQueues(uint32_t internalId) noexcept {
        assert((internalId >= 0) & (internalId < _orderRequestQueues.size()));
        return _orderRequestQueues[internalId];
    }

    ReadyOrderRequests* getLastReadyRequests(uint32_t internalId, StrategyId strategy) noexcept {
        return _readyRequests[internalId][toUInt(strategy)].load(std::memory_order_relaxed);
    }

    uint64_t sendQuoteRequest(uint32_t internalId,
                              uint8_t id,
                              uint32_t side,
                              hf::price_t internalPrice,
                              hf::qty_t internalSize,
                              hf::price_t externalPrice,
                              hf::qty_t externalSize,
                              bool persist,
                              bool leanOnRV = false,
                              uint32_t orderExecutionToken = 0) noexcept {
        assert((internalId >= 0) & (internalId < _orderRequestQueues.size()));
        auto& reqQueue = _orderRequestQueues[internalId][id]._queueBySide[side]._queue;
        if (BOOST_UNLIKELY(!reqQueue.size())) {
            BOOST_ASSERT_MSG(false, "Contract is not toMarketMake");
            return -1;
        }

        auto cursor = reqQueue.reserve();
        auto& req = reqQueue[cursor];
        req._id = id;
        req._side = side;
        req._quote._persist = persist;
        req._quote._internalPrice = internalPrice;
        req._quote._internalSize = internalSize;
        req._quote._externalPrice = externalPrice;
        req._quote._externalSize = externalSize;
        req._quote._leanOnRV = leanOnRV;
        req._orderExecutionToken = orderExecutionToken;
        TLOG(0, "{C[i1]}. Send Quote request. id={i2} side={i3} Internal: {i4}@{d1} External: {i5}@{d2} persist={i6} reqId={i7}",
             internalId, toUInt(id), side, internalSize, ToDollars(internalPrice), externalSize,
             ToDollars(externalPrice), persist, cursor)
        reqQueue.publish(cursor);
        return cursor;
    }

    uint64_t sendHedgeRequest(uint32_t internalId,
                              uint8_t id,
                              uint32_t side,
                              hf::price_t internalPrice,
                              hf::price_t externalPrice,
                              hf::qty_t size,
                              bool isExternal,
                              bool inPayUp,
                              uint32_t orderExecutionToken,
                              hf::price_t stf = hf::NULL_PRICE,
                              uint32_t marketId = 0,
                              uint32_t hedgeId = 0) noexcept {
        assert((internalId >= 0) & (internalId < _orderRequestQueues.size()));
        auto& reqQueue = _orderRequestQueues[internalId][id]._queueBySide[side]._queue;
        if (BOOST_UNLIKELY(!reqQueue.size())) {
            BOOST_ASSERT_MSG(false, "Contract is not toMarketMake");
            return -1;
        }

        auto cursor = reqQueue.reserve();
        auto& req = reqQueue[cursor];
        req._id = id;
        req._side = side;
        req._hedge._isExternal = isExternal;
        req._hedge._internalPrice = internalPrice;
        req._hedge._externalPrice = externalPrice;
        req._hedge._size = size;
        req._hedge._inPayUp = inPayUp;
        req._hedge._stf = stf;
        req._hedge._marketId = marketId;
        req._hedge._hedgeId = hedgeId;
        req._orderExecutionToken = orderExecutionToken;
        TLOG(0, "{C[i1]}. Send Hedge request. id={i2} side={i3} Internal: {i4}@{d1} External: {i5}@{d2} inPayUp={i6} isExternal={i7} HedgeId={i8} MarketId={i9} reqId={i10} execToken={i11}",
             internalId, toUInt(id), side, size, ToDollars(internalPrice), size, ToDollars(externalPrice), inPayUp,
             isExternal, hedgeId, marketId, cursor, orderExecutionToken)
        reqQueue.publish(cursor);
        return cursor;
    }

    uint64_t sendCancelRequest(StrategyId strategy, uint32_t internalId = INVALID_INTERNAL_ID, bool disableUseGTC = false) {
        static constexpr uint32_t SZ = CacheLineBuf::calculateReserveSize(sizeof(ViperMessage) + sizeof(CancelStrategyOrders));
        auto cursor = _notificationQueue->reserve(SZ);
        auto& msg = reinterpret_cast<ViperMessage&>((*_notificationQueue)[cursor].data[0]);
        msg._type = ViperMessageType::CancelStrategyOrders;
        msg._size = SZ;
        auto& req = reinterpret_cast<CancelStrategyOrders&>(msg._data[0]);
        req._internalId = internalId;
        req._strategy = strategy;
        req._disableUseGTC = disableUseGTC;
        LOG(0, "Send Cancel request. Strategy={i1} Contract={C[i2]} disableUseGTC={i3} requestId={i4}",
             toUInt(strategy), internalId, disableUseGTC, cursor)
        _notificationQueue->publish(cursor);
        return cursor;
    }

    void notifyOrderRequest(uint32_t internalId, StrategyId strategy, const ReadyOrderRequests& readyRequests) noexcept {
        static constexpr uint32_t SZ = CacheLineBuf::calculateReserveSize(sizeof(ViperMessage) + sizeof(OrderRequestNotification));
        auto cursor = _notificationQueue->reserve(SZ);
        auto& msg = reinterpret_cast<ViperMessage&>((*_notificationQueue)[cursor].data[0]);
        msg._type = ViperMessageType::OrderRequest;
        msg._size = SZ;
        auto& req = reinterpret_cast<OrderRequestNotification&>(msg._data[0]);
        req._strategy = strategy;
        req._internalId = internalId;
        auto& ready = _readyRequestsBuffer[internalId][toUInt(strategy)].next();
        ready = readyRequests;
        _readyRequests[internalId][toUInt(strategy)].store(&ready, std::memory_order_release);
        LOG(0, "{C[i1]}. Notify order request. Strategy={i2} ({i3} {i4} {i5} {i6})", internalId,
            strategyName(strategy), readyRequests._quote[BID], readyRequests._quote[ASK], readyRequests._hedge[BID],
            readyRequests._hedge[ASK])
        _notificationQueue->publish(cursor);
    }

private:
    OrderRequestManager() = default;
    ~OrderRequestManager() = default;
};

}