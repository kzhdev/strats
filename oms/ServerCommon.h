//
// Created by azhitlenok on 4/8/16.
//

#ifndef RHONESC_SRVRCOMMON_H
#define RHONESC_SRVRCOMMON_H

#include <boost/format.hpp>
#include <hf/log.h>
#include <hf/hf.h>
#include <om/om.h>
#include "messages/omsproto.pb.h"
#include "ReadBuffer.h"

namespace oms {
    using message_id_t = uint32_t;
    const hf::qty_t NULL_QUANTITY = 0;

    inline bool shouldUseExt(const oms::proto::NewOrder &message) {
        return message.has_minquantity() || message.has_maxshow() || message.ordertype() == ::oms::proto::ORDER_TYPE::ORDER_TYPE_GTC;
    }
}
#endif //RHONESC_SRVRCOMMON_H