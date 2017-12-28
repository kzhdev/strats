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

void ServerClientMessenger::sendGUIInitialization(ClientConnection& traderConnection, proto::GUIInitialization& data) {
    data.set_messageid(0);
    sendMessageToClient(traderConnection, proto::SERVER_MESSAGE_TYPE::SERVER_MESSAGE_TYPE_GUI_INITIALIZATION, data);
}

void ServerClientMessenger::sendSwitchChangeReply(ClientConnection& connection, uint32_t strategy, uint32_t action, const std::string& value, uint32_t gridId, bool accepted) {
    proto::SwitchChange message;
    message.set_messageid(0);
    message.set_gridid(gridId);
    proto::GridCell* cell = message.mutable_cell();
    cell->set_col(action);
    cell->set_row(strategy);
    cell->set_value(value);
    proto::SERVER_MESSAGE_TYPE type = accepted ? proto::SERVER_MESSAGE_TYPE::SERVER_MESSAGE_TYPE_SWITCH_CHANGE_ACCEPTED : proto::SERVER_MESSAGE_TYPE::SERVER_MESSAGE_TYPE_SWITCH_CHANGE_REJECTED;
    sendMessageToClient(connection, type, message);
}

bool ServerClientMessenger::doSendFormatError(ClientConnection& traderConnection, proto::ERROR_CODE errorCode, const std::string &text, message_id_t messageID) {
    proto::ErrorMessage formatError;
    formatError.set_messageid(messageID);
    formatError.mutable_error()->set_errortext(text);
    formatError.mutable_error()->set_errorcode(errorCode);
    return sendMessageToClient(traderConnection, proto::SERVER_MESSAGE_TYPE::SERVER_MESSAGE_TYPE_FORMAT_ERROR, formatError);
}

bool ServerClientMessenger::doSendClientLogout(ClientConnection& traderConnection, bool isTraderActive, message_id_t messageID) {
    proto::ClientClosed response;
    response.set_messageid(messageID);
    return sendMessageToClient(traderConnection, proto::SERVER_MESSAGE_TYPE::SERVER_MESSAGE_TYPE_CLIENT_CLOSED, response);
}

bool ServerClientMessenger::doSendClientLoginReject(ClientConnection& traderConnection, proto::ERROR_CODE errorCode, const std::string &text, message_id_t messageID) {
    proto::ErrorMessage response;
    response.set_messageid(messageID);
    response.mutable_error()->set_errortext(text);
    response.mutable_error()->set_errorcode(errorCode);
    return sendMessageToClient(traderConnection, proto::SERVER_MESSAGE_TYPE::SERVER_MESSAGE_TYPE_CLIENT_REJECTED, response);
}

bool ServerClientMessenger::doSendClientLogin(ClientConnection& traderConnection, message_id_t messageID) {
    proto::ClientConnected response;
    response.set_messageid(messageID);
    response.set_version("");
    response.set_heartbeatinterval(10);
    return sendMessageToClient(traderConnection, proto::SERVER_MESSAGE_TYPE::SERVER_MESSAGE_TYPE_CLIENT_CONNECTED, response);
}

bool ServerClientMessenger::doSendClientRequestRejected(ClientConnection& traderConnection, proto::GUI_MESSAGE_TYPE requestType, proto::ERROR_CODE errorCode, const std::string &text, message_id_t messageID) {
    switch(requestType) {
        case proto::GUI_MESSAGE_TYPE::GUI_MESSAGE_TYPE_CLIENT_LOGOUT: {
            proto::ErrorMessage response;
            response.set_messageid(messageID);
            response.mutable_error()->set_errortext(text);
            response.mutable_error()->set_errorcode(errorCode);
            return sendMessageToClient(traderConnection, proto::SERVER_MESSAGE_TYPE::SERVER_MESSAGE_TYPE_CLIENT_CLOSE_REJECTED, response);
        }
        default: {
            return false;
        }
    }
}

}