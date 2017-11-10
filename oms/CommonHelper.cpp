//
// Created by azhitlenok on 4/20/16.
//

#include <hf/log.h>
#include "messages/omsproto.pb.h"
#include "CommonHelper.h"

LOG_DECLARE();

namespace oms {

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
            case oms::proto::REQUEST_TYPE::REQUEST_TYPE_GSTOP_CREATE:
                return "GSTOP_CREATE";
            case oms::proto::REQUEST_TYPE::REQUEST_TYPE_GSTOP_MODIFY:
                return "GSTOP_MODIFY";
            case oms::proto::REQUEST_TYPE::REQUEST_TYPE_GSTOP_CANCEL:
                return "GSTOP_CLOSE";
            case oms::proto::REQUEST_TYPE::REQUEST_TYPE_ORDER_CREATE:
                return "ORDER_CREATE";
            case oms::proto::REQUEST_TYPE::REQUEST_TYPE_ORDER_MODIFY:
                return "ORDER_REPLACE";
            case oms::proto::REQUEST_TYPE::REQUEST_TYPE_ORDER_CANCEL:
                return "ORDER_CANCEL";
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

    std::string toString(proto::OMS_ERROR_CODE code) {
        switch (code) {
            case proto::OMS_ERROR_CODE_DECODE_MESSAGE_ERROR: return "OMS_ERROR_CODE_DECODE_MESSAGE_ERROR";
            case proto::OMS_ERROR_CODE_WRONG_PROTOCOL_VERSION: return "OMS_ERROR_CODE_WRONG_PROTOCOL_VERSION";
            case proto::OMS_ERROR_CODE_CLIENT_IS_ALREADY_LOGGED_IN: return "OMS_ERROR_CODE_CLIENT_IS_ALREADY_LOGGED_IN";
            case proto::OMS_ERROR_CODE_CONNECTION_TO_DB_CANNOT_BE_SET: return "OMS_ERROR_CODE_CONNECTION_TO_DB_CANNOT_BE_SET";
            case proto::OMS_ERROR_CODE_RISK_PARAMETERS_CANNOT_BE_LOADED_FROM_DB: return "OMS_ERROR_CODE_RISK_PARAMETERS_CANNOT_BE_LOADED_FROM_DB";
            case proto::OMS_ERROR_CODE_OBTAINING_OMS_PARAMETERS_ERROR: return "OMS_ERROR_CODE_OBTAINING_OMS_PARAMETERS_ERROR";
            case proto::OMS_ERROR_CODE_INCORRECT_USERNAME: return "OMS_ERROR_CODE_INCORRECT_USERNAME";
            case proto::OMS_ERROR_CODE_CLIENT_IS_NOT_LOGGED_IN: return "OMS_ERROR_CODE_CLIENT_IS_NOT_LOGGED_IN";
            case proto::OMS_ERROR_CODE_ID_IS_NOT_VALID_WRONG_SUFFIX: return "OMS_ERROR_CODE_ID_IS_NOT_VALID_WRONG_SUFFIX";
            case proto::OMS_ERROR_CODE_ID_IS_NOT_VALID_ORDER_WITH_SAME_ID_EXISTS: return "OMS_ERROR_CODE_ID_IS_NOT_VALID_ORDER_WITH_SAME_ID_EXISTS";
            case proto::OMS_ERROR_CODE_ID_IS_NOT_VALID_GSTOP_WITH_SAME_ID_EXISTS: return "OMS_ERROR_CODE_ID_IS_NOT_VALID_GSTOP_WITH_SAME_ID_EXISTS";
            case proto::OMS_ERROR_CODE_TRADER_IS_CLOSED_OR_CLOSING: return "OMS_ERROR_CODE_TRADER_IS_CLOSED_OR_CLOSING";
            case proto::OMS_ERROR_CODE_INSTRUMENT_CANNOT_BE_CREATED: return "OMS_ERROR_CODE_INSTRUMENT_CANNOT_BE_CREATED";
            case proto::OMS_ERROR_CODE_THERE_IS_NO_RISK_DATA_FOR_INSTRUMENT: return "OMS_ERROR_CODE_THERE_IS_NO_RISK_DATA_FOR_INSTRUMENT";
            case proto::OMS_ERROR_CODE_UNRECOGNIZED_SPECIAL_GSTOP_MODE: return "OMS_ERROR_CODE_UNRECOGNIZED_SPECIAL_GSTOP_MODE";
            case proto::OMS_ERROR_CODE_GSTOP_DOES_NOT_EXIST: return "OMS_ERROR_CODE_GSTOP_DOES_NOT_EXIST";
            case proto::OMS_ERROR_CODE_RISK_LIMIS_VIOLATION_LOT_SIZE: return "OMS_ERROR_CODE_RISK_LIMIS_VIOLATION_LOT_SIZE";
            case proto::OMS_ERROR_CODE_RISK_LIMITS_VIOLATION_POSITION: return "OMS_ERROR_CODE_RISK_LIMITS_VIOLATION_POSITION";
            case proto::OMS_ERROR_CODE_RISK_LIMIT_VIOLATION_MAX_LOSS: return "OMS_ERROR_CODE_RISK_LIMIT_VIOLATION_MAX_LOSS";
            case proto::OMS_ERROR_CODE_CONTEXT_CANNOT_CREATE_ORDER: return "OMS_ERROR_CODE_CONTEXT_CANNOT_CREATE_ORDER";
            case proto::OMS_ERROR_CODE_ORDER_DOES_NOT_EXIST: return "OMS_ERROR_CODE_ORDER_DOES_NOT_EXIST";
            case proto::OMS_ERROR_CODE_CANCEL_ORDER_CONTEXT_REJECTED: return "OMS_ERROR_CODE_CANCEL_ORDER_CONTEXT_REJECTED";
            case proto::OMS_ERROR_CODE_MODIFY_ORDER_CONTEXT_REJECTED: return "OMS_ERROR_CODE_MODIFY_ORDER_CONTEXT_REJECTED";
            case proto::OMS_ERROR_CODE_ORDER_REJECTED_BY_EXCHANGE: return "OMS_ERROR_CODE_ORDER_REJECTED_BY_EXCHANGE";
            case proto::OMS_ERROR_CODE_ORDER_REPLACE_REJECTED_BY_EXCHANGE: return "OMS_ERROR_CODE_ORDER_REPLACE_REJECTED_BY_EXCHANGE";
            case proto::OMS_ERROR_CODE_ORDER_CANCEL_REJECTED_BY_EXCHANGE: return "OMS_ERROR_CODE_ORDER_CANCEL_REJECTED_BY_EXCHANGE";
            case proto::OMS_ERROR_CODE_GATEWAY_DOWN: return "OMS_ERROR_CODE_GATEWAY_DOWN";
            case proto::OMS_ERROR_CODE_ORDER_REPLACE_IN_FLIGHT: return "OMS_ERROR_CODE_ORDER_REPLACE_IN_FLIGHT";
            case proto::OMS_ERROR_CODE_INCORRECT_STP_ACTION: return "OMS_ERROR_CODE_INCORRECT_STP_ACTION";
            case proto::OMS_ERROR_CODE_TRADER_NOT_ACTIVE: return "OMS_ERROR_CODE_TRADER_NOT_ACTIVE";
            case proto::OMS_ERROR_CODE_TRADING_DISABLED_FOR_TRADER: return "OMS_ERROR_CODE_TRADING_DISABLED_FOR_TRADER";
            case proto::OMS_ERROR_CODE_CANNOT_OBTAIN_INITIAL_POSITION_FOR_TRADER: return "OMS_ERROR_CODE_CANNOT_OBTAIN_INITIAL_POSITION_FOR_TRADER";
            case proto::OMS_ERROR_CODE_INCORRECT_TRIGGER_QUANTITY: return "OMS_ERROR_CODE_INCORRECT_TRIGGER_QUANTITY";
        }
        return "";
    }
}



