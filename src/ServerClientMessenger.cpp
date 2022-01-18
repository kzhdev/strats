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
    sendMessageToClient(&traderConnection, proto::SERVER_MESSAGE_TYPE::SERVER_MESSAGE_TYPE_GUI_INITIALIZATION, data);
}

void ServerClientMessenger::sendSwitchChangeReply(uint32_t strategy, uint32_t action, const std::string& value, uint32_t gridId, bool accepted) {
    proto::SwitchChange message;
    message.set_messageid(0);
    message.set_gridid(gridId);
    proto::GridCell* cell = message.mutable_cell();
    cell->set_col(action);
    cell->set_row(strategy);
    cell->set_value(value);
    cell->set_colorid(value == "1" ? toUInt(Color::TRANSPARENT_SWITCH_ENABLED) : toUInt(Color::TRANSPARENT_SWITCH_DISABLED));
    proto::SERVER_MESSAGE_TYPE type = accepted ? proto::SERVER_MESSAGE_TYPE::SERVER_MESSAGE_TYPE_SWITCH_CHANGE_ACCEPTED : proto::SERVER_MESSAGE_TYPE::SERVER_MESSAGE_TYPE_SWITCH_CHANGE_REJECTED;
    sendMessageToClient(nullptr, type, message);
}

bool ServerClientMessenger::doSendFormatError(ClientConnection& traderConnection, proto::ERROR_CODE errorCode, const std::string &text, message_id_t messageID) {
    proto::ErrorMessage formatError;
    formatError.set_messageid(messageID);
    formatError.mutable_error()->set_errortext(text);
    formatError.mutable_error()->set_errorcode(errorCode);
    return sendMessageToClient(&traderConnection, proto::SERVER_MESSAGE_TYPE::SERVER_MESSAGE_TYPE_FORMAT_ERROR, formatError);
}

bool ServerClientMessenger::doSendClientLogout(ClientConnection& traderConnection, bool isTraderActive, message_id_t messageID) {
    proto::ClientClosed response;
    response.set_messageid(messageID);
    return sendMessageToClient(&traderConnection, proto::SERVER_MESSAGE_TYPE::SERVER_MESSAGE_TYPE_CLIENT_CLOSED, response);
}

bool ServerClientMessenger::doSendClientLoginReject(ClientConnection& traderConnection, proto::ERROR_CODE errorCode, const std::string &text, message_id_t messageID) {
    proto::ErrorMessage response;
    response.set_messageid(messageID);
    response.mutable_error()->set_errortext(text);
    response.mutable_error()->set_errorcode(errorCode);
    return sendMessageToClient(&traderConnection, proto::SERVER_MESSAGE_TYPE::SERVER_MESSAGE_TYPE_CLIENT_REJECTED, response);
}

bool ServerClientMessenger::doSendClientLogin(ClientConnection& traderConnection, message_id_t messageID, uint32_t heartbeatIntervalInMilliseconds, const char* trader) {
    proto::ClientConnected response;
    response.set_messageid(messageID);
    response.set_version(boost::str(boost::format("Viper - %1%") % Python::ViperVersion));
    response.set_trader(trader);
    response.set_heartbeatinterval(heartbeatIntervalInMilliseconds);
    return sendMessageToClient(&traderConnection, proto::SERVER_MESSAGE_TYPE::SERVER_MESSAGE_TYPE_CLIENT_CONNECTED, response);
}

bool ServerClientMessenger::doSendClientRequestRejected(ClientConnection& traderConnection, proto::GUI_MESSAGE_TYPE requestType, proto::ERROR_CODE errorCode, const std::string &text, message_id_t messageID) {
    switch(requestType) {
        case proto::GUI_MESSAGE_TYPE::GUI_MESSAGE_TYPE_CLIENT_LOGOUT: {
            proto::ErrorMessage response;
            response.set_messageid(messageID);
            response.mutable_error()->set_errortext(text);
            response.mutable_error()->set_errorcode(errorCode);
            return sendMessageToClient(&traderConnection, proto::SERVER_MESSAGE_TYPE::SERVER_MESSAGE_TYPE_CLIENT_CLOSE_REJECTED, response);
        }
        default: {
            return false;
        }
    }
}

bool ServerClientMessenger::sendMessageToClientCommon(ClientConnection* traderConnection,  std::uint8_t type, std::ostringstream& streamMessage, uint32_t length) {
    if (traderConnection) {
        if (BOOST_UNLIKELY(traderConnection->isDisconnected())) {
            return false;
        }

        MessageHeader header;
        header.m_messageTypeID = type;

        auto now = std::chrono::high_resolution_clock::now();
        auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();

        header.m_version = proto::PROTOCOL_VERSION::VERSION;
        header.m_unixTimestamp = nanos;
        header.m_messageID = ++traderConnection->m_messageSequence;
        header.m_size = length;
        if(BOOST_UNLIKELY(MessageHeader::MESSAGE_HEADER_SIZE + length > BUFFER_SIZE)) {
            LOG_ERROR("Message too large: type=" + std::to_string(type) + " length=" + std::to_string(length));
            return false;
        }

        header.serializeToArray(buffer);
        memcpy(buffer + MessageHeader::MESSAGE_HEADER_SIZE, streamMessage.str().c_str(), length);

        const char* cursor = buffer;
        ssize_t sent_size;
        size_t size = (size_t)length + MessageHeader::MESSAGE_HEADER_SIZE;
        while (size > 0) {
            sent_size = ::send(traderConnection->getFileDescriptor(), cursor, size, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (sent_size < 0) {
                if (errno == EWOULDBLOCK) {
                    struct pollfd poll_data;
                    poll_data.fd     = traderConnection->getFileDescriptor();
                    poll_data.events = POLLOUT;
                    uint32_t retry = 100;

                    // wait for socket ready.
                    while(retry--) {
                        auto error = poll(&poll_data, 1, 100);  // 100 ms
                        if (error < 0) {
                            // error occurred
                            LOG_ERROR(boost::str(boost::format("Error writing to tcp socket %1%") % errno));
                            traderConnection->setClientDisconnected(true);
                            return false;
                        } else if (error >= 0) {
                            // socket is ready, try send again
                            break;
                        } else {
                            // poll timed out
                        }
                    }
                    if (!retry) {
                        LOG_ERROR("Error writing to tcp socket %1%. Max retry reached");
                        traderConnection->setClientDisconnected(true);
                        return false;
                    }
                    continue;
                }
                LOG_ERROR(boost::str(boost::format("Error writing to tcp socket %1%") % errno));
                traderConnection->setClientDisconnected(true);
                return false;
            }
            size -= sent_size;
            cursor += sent_size;
        }

//        using namespace oms_ServerClientMessenger_LOGGER;
//        if(header.m_messageTypeID != '0') {
//        _LOG_TRACE(boost::str(boost::format(" --> Outgoing Message HEADER:{size=%1% messageTypeID=%2% version=%3% messageID=%4% unixTimestamp=%5%} MESSAGE:{type=%6% shortForm=%7%}")
//                              % header.m_size % static_cast<int>(header.m_messageTypeID) % static_cast<int>(header.m_version) % header.m_messageID % header.m_unixTimestamp % message.GetTypeName() % message.ShortDebugString()));
//        }

        return true;
    }

    bool rt = false;
    for (auto& kvp : m_traderConnections) {
        rt |= sendMessageToClientCommon(kvp.second.get(), type, streamMessage, length);
    }
    return rt;
}

}