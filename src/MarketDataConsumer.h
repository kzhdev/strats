#pragma once

#include <md/md_event.h>
#include <md/queue/MDQueue.h>
#include <hf/log.h>
#include <atomic>

namespace Python {

LOCAL_EXPLICIT_LOG_DECLARE("MarketDataConsumer.h", MDConsumerSpace)

struct MarketDataConsumerBase {
    static std::vector<md::queue::MDQueue*>& mdQueue() {
        static std::vector<md::queue::MDQueue*> s_mdQueue;
        return s_mdQueue;
    }
};

template<typename T>
class MarketDataConsumer : MarketDataConsumerBase {

    struct QueueState {
        uint_fast64_t _peekIndex = 0;
        bool _indexWrapped = false;
        bool _readingFallBehind = false;
    };

    std::vector<QueueState> _queueState;

    //Temporary to try to debug "Match Orders Failed"
    hf::batch_id_t _lastMMbid = 0;

    T& underlying() { return static_cast<T&>(*this); }

protected:
    std::vector<md::queue::MDQueue*>& _marketDataQueues = mdQueue();

public:
    void initMarkDataQueueState() {
        _queueState.resize(_marketDataQueues.size());
        size_t i = 0;
        for (auto q : _marketDataQueues) {
            _queueState[i++]._peekIndex = q->peekW();
        }
    }

    void drainAllMarketData() {
        for (uint32_t i = 0; i < _marketDataQueues.size(); ++i) {
            auto queue = _marketDataQueues[i];
            auto& state = _queueState[i];
            uint_fast64_t index = queue->peekW();
            if (index > state._peekIndex) {
                state._indexWrapped = true;
            }

            if (BOOST_UNLIKELY(state._indexWrapped & (index < state._peekIndex))) {
                using MDConsumerSpace::s_log;
                LOG_WARN(boost::format("%1% MarketData processing is falling behind.") % queue->getQueueID());
                WLOG(0, "MarketData processing is falling behind. {i1}", i)
                state._indexWrapped = false;
            }

            uint32_t drainSize = 0;
            for (uint_fast64_t j = state._peekIndex; j != index; queue->decrement(j)) {
                auto idx = queue->peek(j);
                if (BOOST_LIKELY(!idx)) continue;

                auto& batch = queue->getPoolP()->getBatch(idx);
                onMDBatch(batch);

                ++drainSize;
            }

            if (drainSize > 0) {
                DLOG(0, "{i1} MD Drain {i2} batch from mdQueue {i3}", resourceName(underlying().id()), drainSize, i)
            }
            state._peekIndex = index;
        }
    }

    void onMDBatch(const md::MDBatch &batch) {
        DLOG(0, "{i1} received MD batch. bid={i2} extSeqNum={i3} venue={i4}", resourceName(underlying().id()), batch.bid,
             batch.extSequenceNumber, toUInt(batch.venue))

        //Attempting to debug duplicate batches
        if (BOOST_UNLIKELY((batch.venue != hf::VENUE::ICE) && (batch.bid <= _lastMMbid) &&
                           (underlying().id() == ResourceId::Execution))) {
            uint64_t eventMask = 0;
            const auto* data = batch.data;
            for (auto i = batch.count; i; --i) {
                const auto& mde = reinterpret_cast<const md::MDEvent&>(*data);
                data += mde.size;
                eventMask |= (1ull << static_cast<uint8_t>(mde.type));
            }
            LOG(0, "EXE DUPLICATE MD BATCH.  bid={i1} count={i2} hwTS={i3} srcTS={i4} srcSentTS={i5} stratTS={i6} venue={i7} extSeqNum={i8} eventMask={i9}",
                batch.bid, batch.count, batch.time.hwTS, batch.time.srcTS, batch.time.srcSentTS, batch.time.stratTS,
                static_cast<int>(batch.venue), batch.extSequenceNumber, eventMask);
        }
        _lastMMbid = batch.bid;

        const auto *data = batch.data;
        for (auto i = batch.count; i; --i) {
            const auto &mde = reinterpret_cast<const md::MDEvent &> (*data);
            data += mde.size;
            switch (mde.type) {
                case md::MDEvent::TYPE::UPDATE: {
                    const auto &event = static_cast<const md::MDUpdateEvent &> (mde);
                    onMDUpdate(event);
                    break;
                }
                case md::MDEvent::TYPE::TRADE: {
                    const auto &event = static_cast<const md::MDTradeEvent &> (mde);
                    underlying().onMDTrade(batch.venue, event);
                    break;
                }
                case md::MDEvent::TYPE::STATUS: {
                    const auto &event = static_cast<const md::MDStatusEvent &> (mde);
                    onMDStatus(batch.venue, batch.extSequenceNumber, event);
                    break;
                }
                default:
                    underlying().handleUncommonMDEvent(mde, batch);
                    break;
            }
        }
        underlying().onEndMDBatch();
    }

    void onMDUpdate(const md::MDUpdateEvent& event) {
        const uint32_t internalId = underlying().sidToInternalId(event.sid);
        if (internalId != INVALID_INTERNAL_ID) {
            underlying().onMDUpdate(event, internalId);
        } else {
            logging::GetLogger().logError(boost::str(boost::format("Received book update for unrecognized SID='%1%'") % event.sid));
        }
    }

    void onMDStatus(hf::VENUE venue, uint64_t extSeqNum, const md::MDStatusEvent &event) {
        const uint32_t internalId = underlying().sidToInternalId(event.sid);
        if (internalId != INVALID_INTERNAL_ID) {
            underlying().onMDStatus(venue, extSeqNum, event, internalId);
        }
    }
};

}