//
// Created by cmilutinovic on 6/23/20.
//

#include <hf/hf.h>
#include <hf/log.h>
#include "logging/Coolog.h"
#include "ViperConfig.h"

#ifndef RHONEDS_FILLRATECHECKER_H
#define RHONEDS_FILLRATECHECKER_H

#endif //RHONEDS_FILLRATECHECKER_H

//FC_LOG message are just for debugging
#ifdef FILL_RATE_CHECKER_DEBUG
#define FC_LOG LOG
#else
#define FC_LOG(...) do {} while(0)
#endif

namespace Python {

    template<size_t ClipBufferSize = CHECKER_ENTRY_SIZE>
    class FillRateChecker {
    public:
        using count_t = uint16_t;
        static_assert(std::numeric_limits<count_t>::max() > ClipBufferSize, "type of index too small");

        void initialize(int maxClips, uint64_t throttleWindowSize, hf::qty_t sumTotalLimit) {
            LOG(0, "FillRateChecker init values maxClips={i1} throttleWindowSize={i2}", maxClips, throttleWindowSize);
            _maxClips = maxClips;
            _throttleWindowSize = throttleWindowSize;
            _sumTotalLimit = sumTotalLimit;
        }

        void changeMaxClips(int maxClips, uint64_t now) {
            _maxClips = maxClips;
            expireEntries(now);
        }

        void changeSumTotal(hf::qty_t sumTotalLimit) {
            _sumTotalLimit = sumTotalLimit;
        }

        void changeThrottleWindowSize(uint64_t throttleWindowSize, uint64_t now) {
            _throttleWindowSize = throttleWindowSize;
            expireEntries(now);
        }

        void expireEntries(uint64_t now) {
            FC_LOG(0, "FillRateChecker expireEntries before now={i1} tws={i2} now-tws={i3} count={i4}", now, _throttleWindowSize, (now-_throttleWindowSize), _buffer.count());
            _buffer.pop_older_than(now - _throttleWindowSize);
            FC_LOG(0, "FillRateChecker expireEntries after now={i1} tws={i2} now-tws={i3} count={i4}", now, _throttleWindowSize, (now-_throttleWindowSize), _buffer.count());
        }

        void onFill(uint64_t now, hf::qty_t qty) {
            if(BOOST_UNLIKELY(!_buffer.push_back(now, qty))) {
                LOG_DECLARE();
                LOG_ERROR("Fill throttle buffer full");
            }
            FC_LOG(0, "FillRateChecker onFill now={i1} count={i2}", now, _buffer.count());
        }

        bool isOverLimit() const {
            FC_LOG(0, "FillRateChecker isOverLimit count={i1} max={i2}", _buffer.count(), _maxClips);
            return (_buffer.count() > _maxClips) | (_buffer.sumTotal() > _sumTotalLimit);
        }

        auto maxClips() const { return _maxClips; }
        auto throttleWindow() const { return _throttleWindowSize; }

        class SimpleTimeBuffer {
        public:
            static constexpr size_t InternalBufferSize = ClipBufferSize;

            bool push_back(uint64_t now, hf::qty_t qty) {

                if(BOOST_UNLIKELY(_count == ClipBufferSize - 1)) {
                    return false;
                }
                _clipTimes[_head++] = {._timeStamp = now, ._qty = qty };
                _head %= ClipBufferSize;
                ++_count;
                _sumTotal += qty;
                return true;
            }

            count_t count() const {
                return _count;
            }

            hf::qty_t sumTotal() const {
                return _sumTotal;
            }

            void pop_older_than(uint64_t clockticks) {
                FC_LOG(0, "FillRateChecker pop_until pre: _head={i1} _tail={i2} count={i3}", _head, _tail, _count);
                while((_head != _tail) && (_clipTimes[_tail]._timeStamp <= clockticks)) {
                    _sumTotal -= _clipTimes[_tail]._qty;
                    --_count;
                    _tail = (_tail + 1) % ClipBufferSize;
                    BOOST_ASSERT(_sumTotal >= 0);
                }
                FC_LOG(0, "FillRateChecker pop_until post: _head={i1} _tail={i2} count={i3}", _head, _tail, _count);
            }

        private:
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

    private:
        int _maxClips;
        hf::qty_t _sumTotalLimit;
        uint64_t _throttleWindowSize;
        SimpleTimeBuffer _buffer;
    };
}