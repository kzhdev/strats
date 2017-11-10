//
// Created by azhitlenok on 4/11/16.
//

#include <zconf.h>
#include <netinet/in.h>
#include <istream>
#include <hf/hf.h>
#include "ServerClientMessenger.h"
#include "CommonHelper.h"
#include "SpreaderClientAcceptor.h"

LOG_DECLARE();

namespace oms {

    ServerClientMessenger * ServerClientMessenger::s_instance = nullptr;

    ServerClientMessenger::ServerClientMessenger() {
        GOOGLE_PROTOBUF_VERIFY_VERSION;
    }

    ServerClientMessenger *ServerClientMessenger::instance() {
        if ( BOOST_UNLIKELY(s_instance == nullptr) ) {
            s_instance = new ServerClientMessenger();
        }
        return s_instance;
    }



    namespace { static const oms::proto::Heartbeat HEARTBEAT = []() { oms::proto::Heartbeat hb; hb.set_dummyvalue(true); return hb; }(); }
    void ServerClientMessenger::log(const std::string& s) {
        LOG_INFO(s);
    }

}