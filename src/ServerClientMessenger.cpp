#include <zconf.h>
#include <netinet/in.h>
#include <istream>
#include <hf/hf.h>
#include "ServerClientMessenger.h"

LOG_DECLARE();

namespace Python {

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

    void ServerClientMessenger::log(const std::string& s) {
        LOG_INFO(s);
    }

}