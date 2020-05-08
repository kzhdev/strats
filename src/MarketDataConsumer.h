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
                static_cast<T&>(*this).onMDBatch(batch);

                ++drainSize;
            }

            if (drainSize > 0) {
                DLOG(0, "{i1} MD Drain {i2} batch from mdQueue {i3}", resourceName(static_cast<T&>(*this).id()), drainSize, i)
            }
            state._peekIndex = index;
        }
    }
};

}