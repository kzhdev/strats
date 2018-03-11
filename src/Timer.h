#ifndef VIPER_TIMER_H
#define VIPER_TIMER_H

#include <hf/hf.h>
#include <sys/time.h>
#include <string>

namespace Python {

    class Timer {

        enum class ActionType : uint32_t  {ExpireBanner, ReturnHedgeQuote};

        struct Action {
            uint64_t  _time;
            uint32_t _internalId;
            ActionType  _actionType;
        };

    public:
        void addTimer(uint64_t time, uint32_t internalId, ActionType type) {
        }

    };

}

#endif	/* VIPER_TIMER_H */