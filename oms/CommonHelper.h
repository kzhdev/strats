//
// Created by azhitlenok on 3/21/16.
//

#ifndef RHONESC_SCHELPER_H
#define RHONESC_SCHELPER_H

#include <om/om.h>
#include "messages/omsproto.pb.h"
#include "ServerCommon.h"
#include <strat/ContextOMS.h>

namespace oms {
    class RiskChecker;

    //conversion from proto to hf
    hf::VENUE toHf(proto::VENUE venue);
    om::ORDER_TYPE toHf(proto::ORDER_TYPE orderType);
    hf::SIDE toHf(proto::SIDE orderSide);
    om::STP_ACTION toHf(proto::STP_ACTION stpAction);

    //conversion from hf to proto
    oms::proto::VENUE toOms(hf::VENUE venue);
    oms::proto::ORDER_TYPE toOms(om::ORDER_TYPE orderType);
    oms::proto::SIDE toOms(hf::SIDE orderSide);
    oms::proto::STP_ACTION toOms(om::STP_ACTION action);

    //to string for logging
    std::string toString(hf::VENUE venue);
    std::string toString(om::RISK_TYPE riskType);
    std::string toString(hf::SIDE side);
    std::string toString(om::STP_ACTION stpAction);
    std::string toString(om::ORDER_TYPE orderType);
    std::string toString(proto::REQUEST_TYPE requestType);
    std::string toString(proto::OMS_ERROR_CODE code);

    ////////////////////////////////////////////////////////////////////////
    // common methods
    ////////////////////////////////////////////////////////////////////////
    bool checkProtocolVersion(int version);
}

#endif //RHONESC_SCHELPER_H
