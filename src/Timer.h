#ifndef VIPER_TIMER_H
#define VIPER_TIMER_H

#include <hf/hf.h>
#include <sys/time.h>
#include <string>
#include <om/om.h>
#include "boost/format.hpp"
#include "Logger.h"
#include "ClockUtil.h"

namespace Python {

    enum class TimerType : uint32_t  {ExpireBanner, CheckHedgeQuote};

    struct TimerEntry {
        uint64_t  _time;
        uint32_t _internalId;
        TimerType  _type;
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

        void addBannerEntry(uint64_t expiryTime, uint32_t internalId) {
            addEntry(expiryTime, internalId, TimerType::ExpireBanner, 0);
        }

        void addCheckQuotesEntry(uint64_t expiryTime, uint32_t internalId, om::order_id_t oid) {
            addEntry(expiryTime, internalId, TimerType::CheckHedgeQuote, oid);
        }

        void addEntry(uint64_t expiryTime, uint32_t internalId, TimerType type, om::order_id_t oid) {
            TimerEntry& entry = _entries[_head];
            entry._time = expiryTime;
            entry._type = type;
            entry._internalId = internalId;
            entry._oid = oid;
            _logger.logInfo(boost::str(boost::format("Timer %1%. Adding entry. Expiry=%2% Id=%3% ") % _name % expiryTime % internalId % oid));
            uint32_t head = getNext(_head);
            if (head != _tail) {
                _head = head;
            } else {
                _logger.logError(boost::str(boost::format("Timer %1%. - head would catch tail.") % _name));
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
                    uint32_t tail = getNext(_tail);
                    if (tail != _head) {
                        _tail = tail;
                    } else {
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
        std::string _name;
        ClientT& _client;
        Logger _logger;
    };
}

#endif	/* VIPER_TIMER_H */