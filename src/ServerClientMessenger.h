#ifndef VIPER_SRVRCLIENTMESSENGER_H
#define VIPER_SRVRCLIENTMESSENGER_H

#include <chrono>
#include "messages/python.pb.h"
#include "ClientAcceptor.h"
#include "CommonHelper.h"
#include <poll.h>

namespace oms_ServerClientMessenger_LOGGER {
    LOG_DECLARE();
}

namespace Python {

    class ServerClientMessenger {
    public:
        static ServerClientMessenger *instance();

        ServerClientMessenger();

        bool doSendClientLoginReject(ClientConnection& traderConnection, proto::ERROR_CODE errorCode, const std::string &text, message_id_t messageID);
        bool doSendClientLogin(ClientConnection& traderConnection, message_id_t messageID, uint32_t heartbeatIntervalInMilliseconds);
        bool doSendClientLogout(ClientConnection& traderConnection, bool isTraderActive, message_id_t messageID);
        bool doSendClientRequestRejected(ClientConnection& traderConnection, proto::GUI_MESSAGE_TYPE requestType, proto::ERROR_CODE errorCode, const std::string &text, message_id_t messageID);
        bool doSendFormatError(ClientConnection& traderConnection, proto::ERROR_CODE errorCode, const std::string &text, message_id_t messageID);
        void sendGUIInitialization(ClientConnection& traderConnection, proto::GUIInitialization& data);
        void sendSwitchChangeReply(ClientConnection& connection, uint32_t strategy, uint32_t action, const std::string& value, uint32_t gridId, bool accepted);

        template<typename T>
        bool sendMessageToClient(ClientConnection& traderConnection, proto::SERVER_MESSAGE_TYPE responseType, T &message);

    private:
        static ServerClientMessenger *s_instance;

        template<typename T>
        bool sendMessageToClientCommon(ClientConnection& traderConnection,  std::uint8_t type, T &message, message_id_t messageId);

        void log(const std::string& s);

        static constexpr int BUFFER_SIZE = 20'971'520; //20 MiB
        char buffer[BUFFER_SIZE];
    };

template<typename T>
bool ServerClientMessenger::sendMessageToClient(ClientConnection& traderConnection,  proto::SERVER_MESSAGE_TYPE responseType, T &message) {
    if (BOOST_UNLIKELY(traderConnection.isDisconnected())) {
        return false;
    }
    return sendMessageToClientCommon(traderConnection, responseType, message, ++traderConnection.m_messageSequence);
}

template<typename T>
bool ServerClientMessenger::sendMessageToClientCommon(ClientConnection& traderConnection,  std::uint8_t type, T &message, message_id_t messageId) {
    MessageHeader header;
    header.m_messageTypeID = type;

    auto now = std::chrono::high_resolution_clock::now();
    auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();

    header.m_version = proto::PROTOCOL_VERSION::VERSION;
    header.m_unixTimestamp = nanos;
    header.m_messageID = messageId;

    std::ostringstream streamMessage;
    message.SerializeToOstream(&streamMessage);

    streamMessage.seekp(0, std::ios::end);
    auto length = streamMessage.tellp();

    header.m_size = length;
    if(BOOST_UNLIKELY(MessageHeader::MESSAGE_HEADER_SIZE + length > BUFFER_SIZE)) {
        using namespace oms_ServerClientMessenger_LOGGER;
        LOG_ERROR("Message too large: type=" + std::to_string(type) + " length=" + std::to_string(length));
        return false;
    }

    header.serializeToArray(buffer);
    memcpy(buffer + MessageHeader::MESSAGE_HEADER_SIZE, streamMessage.str().c_str(), length);

    const char* cursor = buffer;
    ssize_t sent_size;
    size_t size = (size_t)length + MessageHeader::MESSAGE_HEADER_SIZE;
    while (size > 0) {
        sent_size = ::send(traderConnection.getFileDescriptor(), cursor, size, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (sent_size < 0) {
            if (errno == EWOULDBLOCK) {
                struct pollfd poll_data;
                poll_data.fd     = traderConnection.getFileDescriptor();
                poll_data.events = POLLOUT;
                uint32_t retry = 100;

                // wait for socket ready.
                while(retry--) {
                    auto error = poll(&poll_data, 1, 100);  // 100 ms
                    if (error < 0) {
                        // error occurred
                        using namespace oms_ServerClientMessenger_LOGGER;
                        LOG_ERROR(boost::str(boost::format("Error writing to tcp socket %1%") % errno));
                        traderConnection.setClientDisconnected(true);
                        return false;
                    } else if (error >= 0) {
                        // socket is ready, try send again
                        break;
                    } else {
                        // poll timed out
                    }
                }
                if (!retry) {
                    using namespace oms_ServerClientMessenger_LOGGER;
                    LOG_ERROR("Error writing to tcp socket %1%. Max retry reached");
                    traderConnection.setClientDisconnected(true);
                    return false;
                }
                continue;
            }
            using namespace oms_ServerClientMessenger_LOGGER;
            LOG_ERROR(boost::str(boost::format("Error writing to tcp socket %1%") % errno));
            traderConnection.setClientDisconnected(true);
            return false;
        }
        size -= sent_size;
        cursor += sent_size;
    }

    using namespace oms_ServerClientMessenger_LOGGER;
    if(header.m_messageTypeID != '0') {
//        _LOG_TRACE(boost::str(boost::format(" --> Outgoing Message HEADER:{size=%1% messageTypeID=%2% version=%3% messageID=%4% unixTimestamp=%5%} MESSAGE:{type=%6% shortForm=%7%}")
//                              % header.m_size % static_cast<int>(header.m_messageTypeID) % static_cast<int>(header.m_version) % header.m_messageID % header.m_unixTimestamp % message.GetTypeName() % message.ShortDebugString()));
    }

    return true;
}


}

#endif //VIPER_SRVRCLIENTMESSENGER_H
