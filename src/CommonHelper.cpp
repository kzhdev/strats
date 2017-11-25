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

    std::string toString(proto::REQUEST_TYPE requestType) {
        switch(requestType) {
            case proto::REQUEST_TYPE::REQUEST_TYPE_CLIENT_LOGIN:
                return "TRADER_LOGIN";
            case proto::REQUEST_TYPE::REQUEST_TYPE_CLIENT_LOGOUT:
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
}



