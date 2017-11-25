#ifndef VIPER_SRVRCLIENTMESSENGER_H
#define VIPER_SRVRCLIENTMESSENGER_H

#include <chrono>
#include "messages/omsproto.pb.h"
#include "ClientAcceptor.h"
#include "CommonHelper.h"

namespace oms_ServerClientMessenger_LOGGER {
    LOG_DECLARE();
}

namespace Python {

    class ServerClientMessenger {
    public:
        static ServerClientMessenger *instance();

        ServerClientMessenger();


        template <typename ConnectionT>
        bool doSendClientLoginReject(ConnectionT& traderConnection, proto::OMS_ERROR_CODE errorCode, const std::string &text, message_id_t messageID);

        template <typename ConnectionT>
        bool doSendClientLogin(ConnectionT& traderConnection, bool isFirstForTrader, hf::VENUE venue, bool isGWReady, message_id_t messageID);

        template <typename ConnectionT>
        bool doSendClientLogout(ConnectionT& traderConnection, bool isTraderActive, message_id_t messageID);

        template <typename ConnectionT>
        bool doSendClientRequestRejected(ConnectionT& traderConnection, proto::REQUEST_TYPE requestType, proto::OMS_ERROR_CODE errorCode, const std::string &text, message_id_t messageID);

       template <typename ConnectionT>
        bool sendGatewayStatus(ConnectionT& traderConnection, hf::VENUE venue, om::port_id_t id, bool ready);

        template <typename ConnectionT>
        bool sendMarketDataStatus(ConnectionT& conn, hf::VENUE venue, int32_t securityId, bool status);

        template <typename ConnectionT>
        bool doSendFormatError(ConnectionT& traderConnection, proto::OMS_ERROR_CODE errorCode, const std::string &text, message_id_t messageID);

        template<typename T, typename ConnectionT>
        bool sendMessageToClient(ConnectionT& traderConnection, proto::RESPONSE_TYPE responseType, T &message);

    private:
        static ServerClientMessenger *s_instance;

        template<typename T, typename ConnectionT>
        bool sendMessageToClientCommon(ConnectionT& traderConnection,  std::uint8_t type, T &message, message_id_t messageId);

        void log(const std::string& s);

        static constexpr int BUFFER_SIZE = 1'048'576;
        char buffer[BUFFER_SIZE];
    };

template<typename T, typename ConnectionT>
bool ServerClientMessenger::sendMessageToClient(ConnectionT& traderConnection,  proto::RESPONSE_TYPE responseType, T &message) {
    if (BOOST_UNLIKELY(traderConnection.isDisconnected())) {
        return false;
    }
    return sendMessageToClientCommon(traderConnection, responseType, message, ++traderConnection.m_messageSequence);
}

template<typename T, typename ConnectionT>
bool ServerClientMessenger::sendMessageToClientCommon(ConnectionT& traderConnection,  std::uint8_t type, T &message, message_id_t messageId) {
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

    bool result;
    try {
        ::write(traderConnection.getFileDescriptor(), buffer, (size_t)length + MessageHeader::MESSAGE_HEADER_SIZE);
        result = true;
    } catch(...) {
        //LOG_ERROR(boost::str(boost::format("Error writing to tcp socket %1%") % errno));
        result = false;
    }

    using namespace oms_ServerClientMessenger_LOGGER;
    if(header.m_messageTypeID != '0') {
        _LOG_TRACE(boost::str(boost::format(" --> Outgoing Message HEADER:{size=%1% messageTypeID=%2% version=%3% messageID=%4% unixTimestamp=%5%} MESSAGE:{type=%6% shortForm=%7%}")
                              % header.m_size % static_cast<int>(header.m_messageTypeID) % static_cast<int>(header.m_version) % header.m_messageID % header.m_unixTimestamp % message.GetTypeName() % message.ShortDebugString()));
    }

    return result;
}

template <typename ConnectionT>
bool ServerClientMessenger::doSendFormatError(ConnectionT& traderConnection, proto::OMS_ERROR_CODE errorCode, const std::string &text, message_id_t messageID) {
    proto::OtherError formatError;
    formatError.set_messageid(messageID);
    formatError.mutable_error()->set_errortext(text);
    formatError.mutable_error()->set_errorcode(errorCode);
    return sendMessageToClient(traderConnection, proto::RESPONSE_TYPE::RESPONSE_TYPE_FORMAT_ERROR, formatError);
}

template <typename ConnectionT>
bool ServerClientMessenger::doSendClientLogout(ConnectionT& traderConnection, bool isTraderActive, message_id_t messageID) {
    proto::ClientClosed response;
    response.set_messageid(messageID);
    response.set_istraderactive(isTraderActive);
    return sendMessageToClient(traderConnection, proto::RESPONSE_TYPE::RESPONSE_TYPE_CLIENT_CLOSED, response);
}

template <typename ConnectionT>
bool ServerClientMessenger::doSendClientLoginReject(ConnectionT& traderConnection, proto::OMS_ERROR_CODE errorCode, const std::string &text, message_id_t messageID) {
    proto::OtherError response;
    response.set_messageid(messageID);
    response.mutable_error()->set_errortext(text);
    response.mutable_error()->set_errorcode(errorCode);
    return sendMessageToClient(traderConnection, proto::RESPONSE_TYPE::RESPONSE_TYPE_CLIENT_REJECTED, response);
}

template <typename ConnectionT>
bool ServerClientMessenger::doSendClientLogin(ConnectionT& traderConnection, bool isFirstForTrader, hf::VENUE venue, bool isGWReady, message_id_t messageID) {
    proto::ClientConnected response;
    response.set_messageid(messageID);
    response.set_isfirstfortrader(isFirstForTrader);
    response.mutable_gwstatus()->set_venue(toOms(venue));
    response.mutable_gwstatus()->set_ready(isGWReady);
    response.set_version("");
    return sendMessageToClient(traderConnection, proto::RESPONSE_TYPE::RESPONSE_TYPE_CLIENT_CONNECTED, response);
}

template <typename ConnectionT>
bool ServerClientMessenger::sendGatewayStatus(ConnectionT& traderConnection, hf::VENUE venue, om::port_id_t id, bool ready) {
    proto::GatewayStatus status;
    status.set_venue(toOms(venue));
    status.set_ready(ready);
    status.set_id(id);
    return sendMessageToClient(traderConnection, proto::RESPONSE_TYPE::RESPONSE_TYPE_GATEWAY_STATUS, status);
}

template <typename ConnectionT>
bool ServerClientMessenger::doSendClientRequestRejected(ConnectionT& traderConnection, proto::REQUEST_TYPE requestType, proto::OMS_ERROR_CODE errorCode, const std::string &text, message_id_t messageID) {
    switch(requestType) {
        case proto::REQUEST_TYPE::REQUEST_TYPE_CLIENT_LOGOUT: {
            proto::OtherError response;
            response.set_messageid(messageID);
            response.mutable_error()->set_errortext(text);
            response.mutable_error()->set_errorcode(errorCode);
            return sendMessageToClient(traderConnection, proto::RESPONSE_TYPE::RESPONSE_TYPE_CLIENT_CLOSE_REJECTED, response);
        }
       default: {
            return false;
        }
    }
}

template <typename ConnectionT>
bool ServerClientMessenger::sendMarketDataStatus(ConnectionT& conn, hf::VENUE venue, int32_t securityId, bool status) {
    proto::MarketDataStatus message;
    message.set_contractid(securityId);
    message.set_venue(toOms(venue));
    message.set_ready(status);
    return sendMessageToClient(conn, proto::RESPONSE_TYPE::RESPONSE_TYPE_MARKET_DATA_STATUS, message);
}

}

#endif //VIPER_SRVRCLIENTMESSENGER_H
