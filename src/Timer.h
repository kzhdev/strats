#ifndef VIPER_TIMER_H
#define VIPER_TIMER_H

#include <hf/hf.h>
#include <sys/time.h>
#include <string>
#include <om/om.h>
#include "boost/format.hpp"
#include "ClockUtil.h"
#include "Common.h"
#include "logging/Coolog.h"

namespace Python {

    struct TimerEntry {
        uint64_t  _time;
        uint32_t _internalId;
        uint32_t _side;
        om::order_id_t  _oid;
        TimerType _type;
    };

    template <typename ClientT>
    class Timer {
    public:
        const static uint32_t SIZE = 10000;

        Timer(ClientT& client)
        :
            _client(client)
        {}

        void addBannerEntry(uint64_t expiryTime, uint32_t internalId, uint32_t side, TimerType type) {
            addEntry(expiryTime, internalId, 0, side, type);
        }

        void addCheckQuotesEntry(uint64_t expiryTime, uint32_t internalId, om::order_id_t oid, TimerType type) {
            addEntry(expiryTime, internalId, oid, 0, type);
        }

        void removeCheckQuotesEntry(uint32_t internalId, om::order_id_t oid, TimerType type) {
            removeAllMatching(internalId, oid, type);
        }

        void addEntry(uint64_t expiryTime, uint32_t internalId, om::order_id_t oid, uint32_t side, TimerType type) {
            TimerEntry& entry = _entries[_head];
            entry._time = expiryTime;
            entry._internalId = internalId;
            entry._oid = oid;
            entry._side = side;
            entry._type = type;
            LOG(0, "Timer {i1}. Adding entry. Expiry={i2} InternalId={i3} OrderId={i4} ", toUInt(type), expiryTime, internalId, oid)
            uint32_t head = getNext(_head);
            if (head != _tail) {
                _head = head;
            } else {
                logging::GetLogger().logError(boost::str(boost::format("Timer %1%. - head would catch tail.") % toUInt(type)));
            }
        }

        void removeAllMatching(uint32_t internalId, om::order_id_t oid, TimerType type) {
            for(auto i = _tail; i != _head; i = getNext(i)) {
                TimerEntry& entry = _entries[i];
                if(entry._internalId == internalId && entry._oid == oid && entry._type == type) {
                    entry._type = TimerType::DoNothing;
                }
            }
        }

        void expire(uint64_t now) {
            if (_tail == _head) {
                return;
            }
            while (true) {
                TimerEntry& entry = _entries[_tail];
                if (entry._time < now) {
                    _client.expireTimer(entry);
                     LOG(0, "Timer {i1}. Expired InternalId={i2} OrderId={i3} Now={i4}", toUInt(entry._type), entry._internalId, entry._oid, now)
                    _tail = getNext(_tail);
                    if (_tail == _head) {
                        break;
                    }
                } else {
                    break;
                }
            }
        }

    private:
        uint32_t getNext(uint32_t current) {
            return current == _size - 1 ? 0 : current + 1;
        }

        uint32_t _head = 0;
        uint32_t _tail = 0;
        uint32_t _size = SIZE;
        TimerEntry _entries[SIZE];
        ClientT& _client;
    };
}

#endif	/* VIPER_TIMER_H */