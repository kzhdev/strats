#ifndef VIPER_SCHELPER_H
#define VIPER_SCHELPER_H

#include <om/om.h>
#include "messages/omsproto.pb.h"
#include "Common.h"

namespace Python {

    hf::VENUE toHf(proto::VENUE venue);
    proto::VENUE toOms(hf::VENUE venue);
    SystemState ToSystemStateFromProto(proto::SystemState state);
    std::string toString(hf::VENUE venue);
    std::string toString(proto::GUI_MESSAGE_TYPE requestType);
    bool checkProtocolVersion(int version);
}

#endif //VIPER_SCHELPER_H
