//
// Created by cmilutinovic on 6/23/20.
//

#include <hf/hf.h>
#include <util/log.h>
#include "logging/Coolog.h"
#include "ViperConfig.h"

#ifndef RHONEDS_FILLRATECHECKER_H
#define RHONEDS_FILLRATECHECKER_H

#endif //RHONEDS_FILLRATECHECKER_H

//RWC_LOG message are just for debugging
//#define ROLLING_WINDOW_CLIP_ORDER_COUNTER_LOG
#ifdef ROLLING_WINDOW_CLIP_ORDER_COUNTER_LOG
#define RWC_LOG LOG
#else
#define RWC_LOG(...) do {} while(0)
#endif

namespace Python {

    template<size_t ClipBufferSize = CHECKER_ENTRY_SIZE>
    class RollingWindowClipOrderCounter {
    public:
        using count_t = int32_t;
        static_assert(std::numeric_limits<count_t>::max() > ClipBufferSize, "type of index too small");

        void initialize(int maxClips, uint64_t throttleWindowSize, hf::qty_t sumTotalLimit) {
            RWC_LOG(0, "RollingWindowClipOrderCounter init values maxClips={i1} throttleWindowSize={i2}", maxClips, throttleWindowSize);
            _maxClips = maxClips;
            _throttleWindowSize = throttleWindowSize;
            _sumTotalLimit = sumTotalLimit;
        }

        void changeMaxClips(int maxClips, uint64_t now) {
            _maxClips = maxClips;
            expireEntries(now);
        }

        void changeSumTotal(hf::qty_t sumTotalLimit, uint64_t now) {
            _sumTotalLimit = sumTotalLimit;
            expireEntries(now);
        }

        void changeThrottleWindowSize(uint64_t throttleWindowSize, uint64_t now) {
            _throttleWindowSize = throttleWindowSize;
            expireEntries(now);
        }

        void expireEntries(uint64_t now) {
            //This spams the log hard
            RWC_LOG(0, "RollingWindowClipOrderCounter expireEntries before now={i1} tws={i2} now-tws={i3} count={i4}", now, _throttleWindowSize, (now-_throttleWindowSize), _buffer.count());
            _buffer.pop_older_than(now - _throttleWindowSize);
            RWC_LOG(0, "RollingWindowClipOrderCounter expireEntries after now={i1} tws={i2} now-tws={i3} count={i4}", now, _throttleWindowSize, (now-_throttleWindowSize), _buffer.count());
        }

        bool isOverLimit() const {
            RWC_LOG(0, "RollingWindowClipOrderCounter isOverLimit count={i1} max={i2}", _buffer.count(), _maxClips);
            return (_buffer.count() > _maxClips) | (_buffer.sumTotal() > _sumTotalLimit);
        }

        auto maxClips() const { return _maxClips; }
        auto sumTotalLimit() const { return _sumTotalLimit; }
        auto throttleWindow() const { return _throttleWindowSize; }

        auto clipCount() const { return _buffer.count(); }
        auto sumTotal() const { return _buffer.sumTotal(); }

        class SimpleTimeBuffer {
        public:
            static constexpr size_t InternalBufferSize = ClipBufferSize;

            bool push_back(uint64_t now, hf::qty_t qty, bool allowNegativeCount = false) {
                RWC_LOG(0, "RollingWindowClipOrderCounter push_back pre: _head={i1} _tail={i2} count={i3}", _head, _tail, _count);
                if(BOOST_UNLIKELY(_count == ClipBufferSize - 1)) {
                    return false;
                }
                auto candidateCount = _count + countAdjustment(qty);
                auto candidateSumTotal = _sumTotal + qty;
                //It's possible for count to go negative if we can generate a negative qty outside of the window where we were attempting to adjust
                if(BOOST_LIKELY(allowNegativeCount || (candidateCount >= 0 && candidateSumTotal >= 0))) {
                    _clipTimes[_head++] = {._timeStamp = now, ._qty = qty};
                    _head %= ClipBufferSize;
                    _count = candidateCount;
                    _sumTotal = candidateSumTotal;
                    RWC_LOG(0, "RollingWindowClipOrderCounter push_back pre: _head={i1} _tail={i2} count={i3}", _head, _tail, _count);
                }
                else {
                    RWC_LOG(0, "RollingWindowClipOrderCounter push_back allowNegativeCountViolated: _head={i1} _tail={i2} count={i3} qty={i4}", _head, _tail, _count, qty);
                }
                return true;
            }

            count_t count() const {
                return _count;
            }

            hf::qty_t sumTotal() const {
                return _sumTotal;
            }

            void pop_older_than(uint64_t clockticks) {
                RWC_LOG(0, "RollingWindowClipOrderCounter pop_until pre: _head={i1} _tail={i2} count={i3}", _head, _tail, _count);
                while((_head != _tail) && (_clipTimes[_tail]._timeStamp <= clockticks)) {
                    _sumTotal -= _clipTimes[_tail]._qty;
                    _count -= countAdjustment(_clipTimes[_tail]._qty);
                    _tail = (_tail + 1) % ClipBufferSize;
                }
                RWC_LOG(0, "RollingWindowClipOrderCounter pop_until post: _head={i1} _tail={i2} count={i3}", _head, _tail, _count);
            }

        private:

            count_t countAdjustment(hf::qty_t qty) {
                return qty > 0 ? 1 : -1;
            }

            struct ClipTimeEntries {
                uint64_t _timeStamp;
                hf::qty_t _qty;
            };

            count_t _head = 0;
            count_t _tail = 0;
            count_t _count = 0;
            hf::qty_t _sumTotal = 0;
            ClipTimeEntries _clipTimes[ClipBufferSize];
        };

    protected:
        bool push_back(uint64_t now, hf::qty_t qty) {
            return _buffer.push_back(now, qty);
        }

        const SimpleTimeBuffer& buffer() { return _buffer; }

    private:
        int _maxClips;
        hf::qty_t _sumTotalLimit;
        uint64_t _throttleWindowSize;
        SimpleTimeBuffer _buffer;
    };

    template<size_t ClipBufferSize = CHECKER_ENTRY_SIZE>
    class FillRateChecker final : public RollingWindowClipOrderCounter<ClipBufferSize>
    {
    public:
        void onFill(uint64_t now, hf::qty_t qty, FillType type, FillSource source) {
            if(type == FillType::External) {
                if(source == FillSource::MissedHedge) {
                    qty = -qty;
                    RWC_LOG(0, "FillRateChecker onFill FillType=External FillSource=MissedHedge flipping qty.  qty={i1}", qty);
                }
                if(BOOST_UNLIKELY(!this->push_back(now, qty))) {
                    LOG_DECLARE();
                    LOG_ERROR("Fill throttle buffer full");
                }
                RWC_LOG(0, "FillRateChecker onFill now={i1} count={i2}", now, this->buffer().count());
            }
        }
    };

    template<size_t ClipBufferSize = CHECKER_ENTRY_SIZE>
    class NewOrderModifyOrderRateChecker final : public RollingWindowClipOrderCounter<ClipBufferSize>
    {
    public:
        void onNewOrModifyOrder(uint64_t now, hf::SIDE side, hf::price_t price, hf::qty_t qty_diff, hf::price_t bestBid, hf::price_t bestOffer) {

            bool shouldInclude = false;
            if(side == hf::SIDE::BUY) {
                if(bestOffer != hf::NULL_PRICE && price >= bestOffer) {
                    shouldInclude = true;
                }
            }
            else {
                if(bestBid != hf::NULL_PRICE && price <= bestBid) {
                    shouldInclude = true;
                }
            }

            RWC_LOG(0, "NewOrderModifyOrderRateChecker onNewOrModifyOrder. now={i1} side={i2}(raw={i3}) price={i3} diff={i4} bestBid={i5} bestOffer={i6} shouldInclude={i7}", now,
                    (side == hf::SIDE::BUY ? BID : side == hf::SIDE::SELL ? ASK : -1), static_cast<int>(side), price, qty_diff, bestBid, bestOffer, shouldInclude);

            if(shouldInclude) {
                if (BOOST_UNLIKELY(!this->push_back(now, qty_diff))) {
                    LOG_DECLARE();
                    LOG_ERROR("Fill throttle buffer full");
                }
            }
        }
    };
}