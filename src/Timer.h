#ifndef VIPER_TIMER_H
#define VIPER_TIMER_H

#include <hf/hf.h>
#include <sys/time.h>
#include <string>
#include <om/om.h>

namespace Python {

    enum class TimerActionType : uint32_t  {ExpireBanner, CheckHedgeQuote};

    struct TimerAction {
        uint64_t  _time;
        uint32_t _internalId;
        TimerActionType  _actionType;
        om::order_id_t  _oid;
    };

    template <typename ClientT>
    class Timer {
    public:

        Timer(ClientT& client)
        :
            _client(client)
        {}

        void addTimer(uint64_t time, uint32_t internalId, TimerActionType type) {
        }

        void expire(uint64_t time) {
            TimerAction action;
            _client.expireTimer(action);
        }

    private:

        ClientT& _client;
    };
}

#endif	/* VIPER_TIMER_H */