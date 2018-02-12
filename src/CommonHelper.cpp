#include <hf/log.h>
#include "messages/omsproto.pb.h"
#include "CommonHelper.h"

LOG_DECLARE();

namespace Python {

    hf::VENUE toHf(proto::VENUE venue) {
        return (hf::VENUE)(venue);
    }

    proto::VENUE toOms(hf::VENUE venue) {
        return (proto::VENUE)(venue);
    }

    std::string toString(hf::VENUE venue) {
        switch(venue) {
            case hf::VENUE::CME:
                return "CME";
            case hf::VENUE::EUREX:
                return "EUREX";
            case hf::VENUE::ICE:
                return "ICE";
            case hf::VENUE::LAST:
                return "LAST";
            case hf::VENUE::TEST:
                return "TEST";
            default:
                return "UNKNOWN";
        }
    }

    std::string toString(proto::GUI_MESSAGE_TYPE requestType) {
        switch(requestType) {
            case proto::GUI_MESSAGE_TYPE::GUI_MESSAGE_TYPE_CLIENT_LOGIN:
                return "TRADER_LOGIN";
            case proto::GUI_MESSAGE_TYPE::GUI_MESSAGE_TYPE_CLIENT_LOGOUT:
                return "TRADER_LOGOUT";
            default:
                return "Unknown";
        }
    }

    std::string toString(hf::SIDE orderSide) {
        switch(orderSide) {
            case hf::SIDE::BUY:
                return "BUY";
            case hf::SIDE::SELL:
                return "SELL";
            default:
                return "UNKNOWN";
        }
    }

    bool checkProtocolVersion(int version) {
        return proto::PROTOCOL_VERSION::VERSION == version;
    }

    SystemState ToSystemStateFromProto(proto::SystemState state) {
        switch (state) {
            case proto::SystemState::SystemStateDisabled:
                return SystemState::Disabled;
            case proto::SystemState::SystemStateEnabled:
                return SystemState::Enabled;
            case proto::SystemState::SystemStateHardShutdown:
                return SystemState::HardShutdown;
        }
        throw std::runtime_error("uknown system state");
    }

}



