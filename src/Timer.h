#ifndef VIPER_TIMER_H
#define VIPER_TIMER_H

#include <hf/hf.h>
#include <sys/time.h>
#include <string>
#include <om/om.h>
#include "boost/format.hpp"
#include "Logger.h"

namespace Python {

    enum class TimerActionType : uint32_t  {ExpireBanner, CheckHedgeQuote};

    struct TimerEntry {
        uint64_t  _time;
        uint32_t _internalId;
        TimerActionType  _actionType;
        om::order_id_t  _oid;
    };

    template <typename ClientT>
    class Timer {
    public:
        const static uint32_t SIZE = 10000;

        Timer(ClientT& client)
        :
            _client(client)
        {}

        void addEntry(uint64_t now, uint32_t internalId, TimerActionType type, om::order_id_t oid) {
            TimerEntry& entry = _entries[_head];
            entry._time = now;
            entry._actionType = type;
            entry._internalId = internalId;
            entry._oid = oid;
            _logger.logInfo(boost::str(boost::format("Timer %1%. Adding entry. Expiry=%2% Id=%3% ") % _name % now % internalId % oid));
            _head = getNext(_head);
        }

        void expire(uint64_t now) {
            while (true) {
                TimerEntry& entry = _entries[_tail];
                if (entry._time < now) {
                    _client.expireTimer(entry);
                    _tail = getNext(_tail);
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
        std::string _name;
        ClientT& _client;
        Logger _logger;
    };
}

#endif	/* VIPER_TIMER_H */