#include <hf/log.h>
#include "messages/omsproto.pb.h"
#include "CommonHelper.h"

LOG_DECLARE();

namespace Python {

    oms::proto::SIDE toOms(hf::SIDE orderSide) {
        if (orderSide == hf::SIDE::BUY)
            return oms::proto::SIDE::SIDE_BUY;
        return oms::proto::SIDE::SIDE_SELL;
    }

    hf::VENUE toHf(oms::proto::VENUE venue) {
        return (hf::VENUE)(venue);
    }

    oms::proto::VENUE toOms(hf::VENUE venue) {
        return (oms::proto::VENUE)(venue);
    }

    hf::SIDE toHf(oms::proto::SIDE orderSide) {
        switch(orderSide) {
            case oms::proto::SIDE::SIDE_BUY:
                return hf::SIDE::BUY;
            case oms::proto::SIDE::SIDE_SELL:
                return hf::SIDE::SELL;
            default:
                return hf::SIDE::UNKNOWN;
        }
    }

    om::ORDER_TYPE toHf(oms::proto::ORDER_TYPE orderType) {
        return (om::ORDER_TYPE)orderType;
    }

    oms::proto::ORDER_TYPE toOms(om::ORDER_TYPE orderType) {
        return (oms::proto::ORDER_TYPE)orderType;
    }

    om::STP_ACTION toHf(oms::proto::STP_ACTION stpAction) {

        BOOST_ASSERT(static_cast<int>(stpAction) >= 0 && static_cast<int>(stpAction) <= 2);
        BOOST_ASSERT(oms::proto::STP_ACTION_descriptor()->value_count() == 3);
        static_assert(oms::proto::STP_ACTION::CME_CANCEL_AGGRESSOR == 1, "");
        static_assert(oms::proto::STP_ACTION::CME_CANCEL_PASSIVE == 2, "");
        static_assert(oms::proto::STP_ACTION::NONE == 0, "");

        static constexpr const om::STP_ACTION lookup[] = {om::STP_ACTION::DEFAULT, om::STP_ACTION::CANCEL_AGGRESSOR, om::STP_ACTION::CANCEL_PASSIVE };

        return lookup[static_cast<int>(stpAction)];
    }

    oms::proto::STP_ACTION toOms(om::STP_ACTION action) {
        switch (action) {
            case om::STP_ACTION::CANCEL_AGGRESSOR:
                return oms::proto::STP_ACTION::CME_CANCEL_AGGRESSOR;
            case om::STP_ACTION::CANCEL_PASSIVE:
                return oms::proto::STP_ACTION::CME_CANCEL_PASSIVE;
            default:
                return oms::proto::STP_ACTION::NONE;
        }
    }

    std::string toString(om::ORDER_TYPE orderType){
        switch(orderType) {
            case om::ORDER_TYPE::FAK:
                return "FAK";
            case om::ORDER_TYPE::GTC:
                return "GTC";
            case om::ORDER_TYPE::LMT:
                return "LMT";
            case om::ORDER_TYPE::MKT:
                return "MKT";
            case om::ORDER_TYPE::MKT_LMT:
                return "MKT_LMT";
            case om::ORDER_TYPE::STOP:
                return "STOP";
            case om::ORDER_TYPE::STOP_LMT:
                return "STOP_LMT";
            default:
            case om::ORDER_TYPE::UNKNOWN:
                return "UNKNOWN";
        }
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

    std::string toString(om::STP_ACTION value) {
        switch(value){
            case om::STP_ACTION::CANCEL_AGGRESSOR:
                return "CANCEL_AGGRESSOR";
            case om::STP_ACTION::CANCEL_PASSIVE:
                return "CANCEL_PASSIVE";
            default:
                return "DEFAULT";
        }
    }

    std::string toString(oms::proto::REQUEST_TYPE requestType) {
        switch(requestType) {
            case oms::proto::REQUEST_TYPE::REQUEST_TYPE_CLIENT_LOGIN:
                return "TRADER_LOGIN";
            case oms::proto::REQUEST_TYPE::REQUEST_TYPE_CLIENT_LOGOUT:
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
        return oms::proto::PROTOCOL_VERSION::VERSION == version;
    }


}



