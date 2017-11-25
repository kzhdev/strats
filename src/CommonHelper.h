#ifndef VIPER_SCHELPER_H
#define VIPER_SCHELPER_H

#include <om/om.h>
#include "messages/omsproto.pb.h"
#include <strat/ContextOMS.h>

namespace Python {

    hf::VENUE toHf(proto::VENUE venue);

    proto::VENUE toOms(hf::VENUE venue);

    std::string toString(hf::VENUE venue);
    std::string toString(proto::REQUEST_TYPE requestType);

    bool checkProtocolVersion(int version);
}

#endif //VIPER_SCHELPER_H
