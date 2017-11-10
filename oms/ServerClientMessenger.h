//
// Created by azhitlenok on 4/11/16.
//

#ifndef RHONESC_SRVRCLIENTMESSENGER_H
#define RHONESC_SRVRCLIENTMESSENGER_H

#include <chrono>
#include "ServerCommon.h"
#include "messages/omsproto.pb.h"
#include "SpreaderClientAcceptor.h"
#include "CommonHelper.h"
#include "SpreaderQueues.h"

namespace oms_ServerClientMessenger_LOGGER {
    LOG_DECLARE();
}

namespace oms {

    class ServerClientMessenger {
    public:
        static ServerClientMessenger *instance();

        ServerClientMessenger();

        template <typename ConnectionT>
        bool sendFillToRemoteSpreader(ConnectionT& connection, const std::string& spreadId, hf::symbol_gid_t gid, hf::qty_t qty);

        template <typename ConnectionT>
        bool forwardUnsolicitedPauseToSlave(ConnectionT& connection, const std::string& spreadId);

        template <typename ConnectionT>
        bool sendCoverOrder(ConnectionT& connection, const std::string& spreadId, const std::string& coverId, hf::price_t price);

        template <typename ConnectionT>
        bool sendSMATheo(ConnectionT& connection, const std::string& spreadId, hf::price_t price);

        template <typename ConnectionT>
        bool sendSpreadOrderPrice(ConnectionT& connection, const std::string& spreadId, hf::price_t price);

        template <typename ConnectionT>
        bool sendLegsCrossInfo(ConnectionT& connection, spreader::LegsCrossInfo& info, const std::string& spreadId);

        template <typename ConnectionT>
        bool sendSpreadPrice(ConnectionT& connection, std::string& id, hf::price_t bid, hf::price_t ask);

        template <typename ConnectionT>
        bool sendLegOrderStatus(ConnectionT& connection, std::string& spreadId, hf::symbol_gid_t gid, hf::SIDE side, hf::price_t price, hf::qty_t qty,
                                om::order_id_t oid, proto::ORDER_ACTION action, bool rejected);

        template <typename ConnectionT>
        bool sendSpreadStatus(ConnectionT& connection, const std::string& spreadId, proto::SPREAD_ORDER_STATUS status, uint32_t messageId);

        template <typename ConnectionT>
        bool sendSpreadStatus(ConnectionT& connection, const std::string& spreadId, proto::SPREAD_ORDER_STATUS status, uint32_t messageId,  proto::OMS_ERROR_CODE errorCode, const std::string &text);

        template <typename ConnectionT>
        bool doSendFormatError(ConnectionT& traderConnection, oms::proto::OMS_ERROR_CODE errorCode, const std::string &text, message_id_t messageID);

        template <typename ConnectionT>
        bool doSendClientLoginReject(ConnectionT& traderConnection, oms::proto::OMS_ERROR_CODE errorCode, const std::string &text, message_id_t messageID);

        template <typename ConnectionT>
        bool doSendClientLogin(ConnectionT& traderConnection, bool isFirstForTrader, hf::VENUE venue, bool isGWReady, message_id_t messageID);

        template <typename ConnectionT>
        bool doSendClientLogout(ConnectionT& traderConnection, bool isTraderActive, message_id_t messageID);

        template <typename ConnectionT>
        bool doSendClientRequestRejected(ConnectionT& traderConnection, oms::proto::REQUEST_TYPE requestType, oms::proto::OMS_ERROR_CODE errorCode, const std::string &text, message_id_t messageID);

        template <typename ConnectionT>
        bool sendMarketDataStatus(ConnectionT& conn, hf::VENUE venue, int32_t securityId, bool status);

        template <typename ConnectionT>
        bool sendGatewayStatus(ConnectionT& traderConnection, hf::VENUE venue, om::port_id_t id, bool ready);

        template<typename T, typename ConnectionT>
        bool sendMessageToClient(ConnectionT& traderConnection, oms::proto::RESPONSE_TYPE responseType, T &message);

        template<typename T, typename ConnectionT>
        bool sendRequestToClient(ConnectionT& traderConnection, oms::proto::REQUEST_TYPE responseType, T &message);

    private:
        static ServerClientMessenger *s_instance;

        template<typename T, typename ConnectionT>
        bool sendMessageToTeamClient(ConnectionT& traderConnection,  proto::RESPONSE_TYPE responseType, T &message, message_id_t messageId);

        template<typename T, typename ConnectionT>
        bool sendMessageToClientCommon(ConnectionT& traderConnection,  std::uint8_t type, T &message, message_id_t messageId);

        void log(const std::string& s);

        static constexpr int BUFFER_SIZE = 1'048'576;
        char buffer[BUFFER_SIZE];
    };

template<typename T, typename ConnectionT>
bool ServerClientMessenger::sendRequestToClient(ConnectionT& traderConnection, oms::proto::REQUEST_TYPE type, T &message) {
    if (BOOST_UNLIKELY(traderConnection.isDisconnected())) {
        return false;
    }
    return sendMessageToClientCommon(traderConnection, type, message, ++traderConnection.m_messageSequence);
};

template<typename T, typename ConnectionT>
bool ServerClientMessenger::sendMessageToClient(ConnectionT& traderConnection,  proto::RESPONSE_TYPE responseType, T &message) {
    if (BOOST_UNLIKELY(traderConnection.isDisconnected())) {
        return false;
    }
    return sendMessageToClientCommon(traderConnection, responseType, message, ++traderConnection.m_messageSequence);
}

template<typename T, typename ConnectionT>
bool ServerClientMessenger::sendMessageToTeamClient(ConnectionT& traderConnection,  proto::RESPONSE_TYPE responseType, T &message, message_id_t messageId) {
    if (BOOST_UNLIKELY(traderConnection.isDisconnected())) {
        return false;
    }
    return sendMessageToClientCommon(traderConnection, responseType, message, messageId);
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
bool ServerClientMessenger::sendMarketDataStatus(ConnectionT& conn, hf::VENUE venue, int32_t securityId, bool status) {
    proto::MarketDataStatus message;
    message.set_contractid(securityId);
    message.set_venue(toOms(venue));
    message.set_ready(status);
    return sendMessageToClient(conn, proto::RESPONSE_TYPE::RESPONSE_TYPE_MARKET_DATA_STATUS, message);
}

template <typename ConnectionT>
bool ServerClientMessenger::sendSpreadPrice(ConnectionT& connection, std::string& id, hf::price_t bid, hf::price_t ask) {
    proto::SpreadPriceUpdate response;
    response.set_spreadorderidentifier(id.c_str());
    response.set_bidprice(bid);
    response.set_askprice(ask);
    return sendMessageToClient(connection, proto::RESPONSE_TYPE::RESPONSE_TYPE_SPREAD_PRICE_UPDATE, response);
}

template <typename ConnectionT>
bool ServerClientMessenger::sendLegOrderStatus(ConnectionT& connection, std::string& spreadId, hf::symbol_gid_t gid, hf::SIDE side, hf::price_t price, hf::qty_t qty,
                                               om::order_id_t oid, proto::ORDER_ACTION action, bool rejected) {
    proto::SpreadLegStatus message;
    message.set_spreadorderidentifier(spreadId);
    message.set_contractid(gid);
    message.set_oid(oid);
    message.set_side(side == hf::SIDE::BUY ? proto::SIDE_BUY : proto::SIDE_SELL);
    message.set_price(price);
    message.set_quantity(qty);
    message.set_action(action);
    message.set_rejected(rejected);
    return sendMessageToClient(connection, proto::RESPONSE_TYPE::RESPONSE_TYPE_SPREAD_LEG_STATUS, message);
}

template <typename ConnectionT>
bool ServerClientMessenger::sendCoverOrder(ConnectionT& connection, const std::string& spreadId, const std::string& coverId, hf::price_t price) {
    proto::CoverSpreadCreated message;
    message.set_originalspreadorderidentifier(spreadId);
    message.set_coverspreadorderidentifier(coverId);
    message.set_price(price);
    return sendMessageToClient(connection, proto::RESPONSE_TYPE::RESPONSE_TYPE_COVER_SPREAD_CREATED, message);
}

template <typename ConnectionT>
bool ServerClientMessenger::forwardUnsolicitedPauseToSlave(ConnectionT& connection, const std::string& spreadId) {
    proto::RemotePause message;
    message.set_spreadorderidentifier(spreadId);
    return sendRequestToClient(connection, proto::REQUEST_TYPE::REQUEST_TYPE_REMOTE_PAUSE, message);
}

template <typename ConnectionT>
bool ServerClientMessenger::sendFillToRemoteSpreader(ConnectionT& connection, const std::string& spreadId, hf::symbol_gid_t gid, hf::qty_t qty) {
    proto::RemoteFill message;
    message.set_spreadorderidentifier(spreadId);
    message.set_contractid(gid);
    message.set_filledqty(qty);
    return sendRequestToClient(connection, proto::REQUEST_TYPE::REQUEST_TYPE_REMOTE_FILL, message);
}

template <typename ConnectionT>
bool ServerClientMessenger::sendSMATheo(ConnectionT& connection, const std::string& spreadId, hf::price_t theo) {
    proto::SpreadTheo message;
    message.set_spreadorderidentifier(spreadId);
    message.set_theo(theo);
    return sendMessageToClient(connection, proto::RESPONSE_TYPE::RESPONSE_TYPE_SPREAD_THEO, message);
}

template <typename ConnectionT>
bool ServerClientMessenger::sendSpreadOrderPrice(ConnectionT& connection, const std::string& spreadId, hf::price_t theo) {
    proto::SpreadOrderPriceUpdate message;
    message.set_spreadorderidentifier(spreadId);
    message.set_price(theo);
    return sendMessageToClient(connection, proto::RESPONSE_TYPE::RESPONSE_TYPE_SPREAD_ORDER_PRICE_UPDATE, message);
}

template <typename ConnectionT>
bool ServerClientMessenger::sendSpreadStatus(ConnectionT& connection, const std::string& spreadId, proto::SPREAD_ORDER_STATUS status, uint32_t messageId) {
    proto::SpreadOrderStatus message;
    message.set_spreadorderidentifier(spreadId.c_str());
    message.set_spreadorderstatus(status);
    return sendMessageToClient(connection, proto::RESPONSE_TYPE::RESPONSE_TYPE_SPREAD_ORDER_STATUS, message);
}

template <typename ConnectionT>
bool ServerClientMessenger::sendSpreadStatus(ConnectionT& connection, const std::string& spreadId, proto::SPREAD_ORDER_STATUS status, uint32_t messageId,  proto::OMS_ERROR_CODE errorCode, const std::string &text) {
    proto::SpreadOrderStatus message;
    message.set_spreadorderidentifier(spreadId.c_str());
    message.set_spreadorderstatus(status);
    message.mutable_error()->set_errortext(text);
    message.mutable_error()->set_errorcode(errorCode);

    log(message.ShortDebugString());

    return sendMessageToClient(connection, proto::RESPONSE_TYPE::RESPONSE_TYPE_SPREAD_ORDER_STATUS, message);
}

template <typename ConnectionT>
bool ServerClientMessenger::sendLegsCrossInfo(ConnectionT& connection, spreader::LegsCrossInfo& info, const std::string& spreadId) {
    proto::LegsCrossInfo response;
    response.set_spreadorderidentifier(spreadId.c_str());
    int offset = 0;
    for (int i = 0; i < info._nLegs; ++i) {
        spreader::LegCross& inCross = reinterpret_cast<spreader::LegCross &> (info._data[offset]);
        proto::LegCross* cross = response.add_legs();
        cross->set_contractid(inCross._gid);
        cross->set_venue(toOms(inCross._venue));
        cross->set_hedgeprice(inCross._hedgePrice);
        cross->set_quoteprice(inCross._quotePrice);
        cross->set_quotepricecrossed(inCross._quoteCross);
        offset += sizeof(spreader::LegCross);
    }
    return sendMessageToClient(connection, proto::RESPONSE_TYPE::RESPONSE_TYPE_LEG_CROSS_INFO, response);
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
        case proto::REQUEST_TYPE ::REQUEST_TYPE_GSTOP_CANCEL: {
            proto::OrderError response;
            response.set_messageid(messageID);
            response.set_gscalperorderid("");
            response.mutable_error()->set_errortext(text);
            response.mutable_error()->set_errorcode(errorCode);
            return sendMessageToClient(traderConnection, proto::RESPONSE_TYPE::RESPONSE_TYPE_GSTOP_REJECTED, response);
        }
        case proto::REQUEST_TYPE::REQUEST_TYPE_GSTOP_CREATE: {
            proto::OrderError response;
            response.set_messageid(messageID);
            response.set_gscalperorderid("");
            response.mutable_error()->set_errorcode(errorCode);
            return sendMessageToClient(traderConnection, proto::RESPONSE_TYPE::RESPONSE_TYPE_GSTOP_REJECTED, response);
        }
        case proto::REQUEST_TYPE::REQUEST_TYPE_GSTOP_MODIFY: {
            proto::OrderError response;
            response.set_messageid(messageID);
            response.set_gscalperorderid("");
            response.mutable_error()->set_errortext(text);
            response.mutable_error()->set_errorcode(errorCode);
            return sendMessageToClient(traderConnection, proto::RESPONSE_TYPE::RESPONSE_TYPE_GSTOP_MODIFY_REJECTED, response);
        }
        case proto::REQUEST_TYPE::REQUEST_TYPE_ORDER_CREATE: {
            proto::OrderError response;
            response.set_messageid(messageID);
            response.set_gscalperorderid("");
            response.mutable_error()->set_errortext(text);
            response.mutable_error()->set_errorcode(errorCode);
            return sendMessageToClient(traderConnection, proto::RESPONSE_TYPE::RESPONSE_TYPE_ORDER_REJECTED, response);
        }
        case proto::REQUEST_TYPE::REQUEST_TYPE_ORDER_MODIFY: {
            proto::OrderError response;
            response.set_messageid(messageID);
            response.set_gscalperorderid("");
            response.mutable_error()->set_errortext(text);
            response.mutable_error()->set_errorcode(errorCode);
            return sendMessageToClient(traderConnection, proto::RESPONSE_TYPE::RESPONSE_TYPE_ORDER_MODIFY_REJECTED, response);
        }
        case proto::REQUEST_TYPE::REQUEST_TYPE_ORDER_CANCEL: {
            proto::OrderError response;
            response.set_messageid(messageID);
            response.set_gscalperorderid("");
            response.mutable_error()->set_errortext(text);
            response.mutable_error()->set_errorcode(errorCode);
            return sendMessageToClient(traderConnection, proto::RESPONSE_TYPE::RESPONSE_TYPE_ORDER_CANCEL_REJECTED, response);
        }
        default: {
            return false;
        }
    }
}

}

#endif //RHONESC_SRVRCLIENTMESSENGER_H
