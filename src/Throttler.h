#ifndef VIPER_THROTTLER_H
#define VIPER_THROTTLER_H

#include <stdint.h>

namespace Python {

class Throttler {
public:
    bool addMessage(uint32_t route) {
        return true;
    }

    bool wasInDangerZone() {
        return true;
    }

    bool canGoFullSpeed(uint64_t now) {
        return true;
    }
};

}

#endif //VIPER_THROTTLER_H
