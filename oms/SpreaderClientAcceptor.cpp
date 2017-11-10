//
// Created by pkusch on 12/26/16.
//

#include <sys/socket.h>
#include <cstdlib>
#include <boost/format.hpp>
#include <hf/log.h>
#include <netdb.h>
#include <hf/hf.h>
#include "SpreaderClientAcceptor.h"
#include "ServerCommon.h"
#include "ServerClientMessenger.h"
#include "CommonHelper.h"
#include "messages/omsproto.pb.h"
#include <hf/util/SocketUtils.h>
#include <sys/epoll.h>
#include <netinet/tcp.h>
#include <chrono>
#include <iostream>

LOG_DECLARE();

namespace oms { namespace spreader  {

void SpreaderClientsAcceptor::initialize(SpreaderQueue* queue, SpreaderType spreaderType, std::string& netIf, std::string& host, int port) {
    _inQueue = queue;
    _spreaderType = spreaderType;
    if (spreaderType == SpreaderType::Master) {
        if (BOOST_UNLIKELY((_slaveFD = hf::util::SocketTCPUtils::createSocketREMOTE(netIf, true, hf::util::SocketUtils::Acceleration::ENABLED, 5)) == -1)) {
            LOG_FATAL_AND_THROW("Cannot create socket with slave");
        }
        if (BOOST_LIKELY(-1 == hf::util::SocketTCPUtils::connect(_slaveFD, host, port))) {
            if (errno == EINPROGRESS) {
                LOG_INFO(boost::format("Connecting in progress"));
                while (BOOST_UNLIKELY(-1 == hf::util::SocketTCPUtils::connect(_slaveFD, host, port))) {
                    if ((errno == EALREADY || errno == EINTR))
                        continue;

                    _LOG_TRACE(boost::format("error='%1%' (%2%)") % strerror(errno) % errno);

                    if (BOOST_UNLIKELY(errno != EISCONN && errno != 0)) {
                        LOG_FATAL_AND_THROW("Cannot connect to slave");
                    }

                    break;
                }
            } else {
                LOG_FATAL_AND_THROW("Cannot connect to slave");
            }
        }
        _slaveConnection.m_fd = _slaveFD;
    }
}

bool SpreaderClientsAcceptor::checkForMessagesFromSlave() {
    const ssize_t n = _slaveReadBuffer.readFromStream(_slaveFD);

    if (BOOST_UNLIKELY(n == -1)) {
        if (BOOST_LIKELY(errno == EAGAIN)) {
            return true;
        } else {
            LOG_ERROR(boost::format("Error on read() from slave socket error='%1%' (%2%)") % strerror(errno) % errno);
            ::close(_slaveFD);
            return false;
        }
    }

    if (BOOST_UNLIKELY(n == 0)) {
        LOG_ERROR(boost::format("Diosconnected from slave socket error='%1%' (%2%)") % strerror(errno) % errno);
        ::close(_slaveFD);
        return false;
    }

    while (true) {
        if (!_slaveReadBuffer.prepareToNextMessageProcessingAndCheckReadiness()) {
            _slaveReadBuffer.copyPartialMessageToTheBeginningOfTheBuffer();
            return true;
        }

        const MessageHeader& header = _slaveReadBuffer.getMessageHeader();
        auto messageID = header.m_messageID;

        if (BOOST_UNLIKELY(!checkProtocolVersion(header.m_version))) {
            std::string text = boost::str(boost::format("Message protocol version %1% is incorrect") % (int) header.m_version);
            LOG_ERROR(text);
        }

        auto responseType = (proto::RESPONSE_TYPE) header.m_messageTypeID;

        switch (responseType) {
            case proto::RESPONSE_TYPE_SPREAD_PRICE_UPDATE:
            case proto::RESPONSE_TYPE_LEG_CROSS_INFO:
            case proto::RESPONSE_TYPE_SPREAD_THEO:
            case proto::RESPONSE_TYPE_COVER_SPREAD_CREATED:
            case proto::RESPONSE_TYPE_SPREAD_ORDER_PRICE_UPDATE:
                break;
            case proto::RESPONSE_TYPE_SPREAD_ORDER_STATUS: {
                proto::SpreadOrderStatus message;
                if (BOOST_UNLIKELY(!_slaveReadBuffer.parseMessage(message))) {
                    //TODO finish
                } else {
                    if (message.spreadorderstatus() == proto::SPREAD_ORDER_STATUS::ACKNOWLEDGED) {
                        if (!message.has_error()) {
                            auto spreadIt = _orderIdMappingsByClient.find(message.spreadorderidentifier());
                            if (spreadIt != _orderIdMappingsByClient.end()) {
                                sendOtherRequest(SpreadMessageType::AcceptCrossedOrder, spreadIt->second, messageID);
                            } else {
                                SpreaderConnection* traderConnection = m_monitor.m_traderConnections.begin()->second.get();
                                ServerClientMessenger::instance()->sendMessageToClient(*traderConnection, responseType, message);
                            }
                        }
                    }
                }
                break;
            }
            case proto::RESPONSE_TYPE_CLIENT_CONNECTED: {
                _algoManager->_slaveLoggedIn = true;
                break;
            }
            case proto::RESPONSE_TYPE::RESPONSE_TYPE_CLIENT_REJECTED: {
                _algoManager->_masterLoggedIn = false;
                SpreaderConnection* traderConnection = m_monitor.m_traderConnections.begin()->second.get();
                removeTraderConnection(*traderConnection);
                proto::OtherError message;
                if (BOOST_UNLIKELY(!_slaveReadBuffer.parseMessage(message))) {
                    message.set_messageid(0);
                    message.mutable_error()->set_errortext("Some error during login");
                }
                ServerClientMessenger::instance()->sendMessageToClient(*traderConnection, responseType, message);
                break;
            }
            case proto::RESPONSE_TYPE::RESPONSE_TYPE_GATEWAY_STATUS: {
                SpreaderConnection* connection = m_monitor.m_traderConnections.begin()->second.get();
                proto::GatewayStatus message;
                if (BOOST_UNLIKELY(!_slaveReadBuffer.parseMessage(message))) {
                    proto::OtherError otherError;
                    otherError.set_messageid(0);
                    otherError.mutable_error()->set_errortext("Some error during login");
                    ServerClientMessenger::instance()->sendMessageToClient(*connection, proto::RESPONSE_TYPE::RESPONSE_TYPE_CLIENT_REJECTED, otherError);
                    removeTraderConnection(*connection);
                } else {
                    ServerClientMessenger::instance()->doSendClientLogin(*connection, true, hf::VENUE::CME, true, messageID);
                    for (auto& gs : _algoManager->_connectedGateways) {
                        sendGatewayStatus(gs._venue, gs._id, gs._active);
                    }
                    ServerClientMessenger::instance()->sendMessageToClient(*connection, responseType, message);
                }
                break;
            }
            case proto::RESPONSE_TYPE_SPREAD_LEG_STATUS: {
                proto::SpreadLegStatus message;
                if (BOOST_UNLIKELY(!_slaveReadBuffer.parseMessage(message))) {
                    // TODO do something
                } else {
                    if (message.action() == proto::ORDER_ACTION::FILL) {
                        auto it = _orderIdMappingsByClient.find(message.spreadorderidentifier());
                        _algoManager->processRemoteFill(it->second, message.contractid(), message.quantity());
                    }
                }
                SpreaderConnection* traderConnection = m_monitor.m_traderConnections.begin()->second.get();
                ServerClientMessenger::instance()->sendMessageToClient(*traderConnection, proto::RESPONSE_TYPE_SPREAD_LEG_STATUS, message);
                break;
            }
            default:
                break;
        }

        if (BOOST_LIKELY(!_slaveReadBuffer.moveToNextProtoMessage()))
            break;
    }

    return true;
}

int SpreaderClientsAcceptor::createMonitorSocket() {
    struct hostent *server;
    struct sockaddr_in serv_addr;

    const auto fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        LOG_ERROR(boost::format("Cannot create socket"));
        return -1;
    }

    server = gethostbyname(m_host.c_str());
    if (server == NULL) {
        LOG_ERROR(boost::format("No such host %1%") % m_host);
        return -2;
    }

    bzero((char *) &serv_addr, sizeof(serv_addr));

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(m_port);

    int reuseAddrFlag = 1;
    if(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char*) &reuseAddrFlag, sizeof(int)) < 0){
        LOG_FATAL_AND_THROW("Setting SO_REUSEADDR on SpreaderClientsAcceptor socket failed");
    }

    if (bind(fd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
        LOG_FATAL_AND_THROW(boost::str(boost::format("Cannot bind to %1% %2%: errno%3% strerr:%4%") % m_host % m_port
                                       % errno % strerror(errno)));
        return -2;
    }

    m_monitor.listenFD = fd;

    return 0;
}

void SpreaderClientsAcceptor::createMonitor() {
    LOG_TRACE("CREATE MONITOR");

    if( BOOST_UNLIKELY(createMonitorSocket() < 0 ) )
    {
        LOG_ERROR(boost::format("Cannot create monitor socket to %1% %2%") % m_host % m_port);
        return;
    }

    m_monitor.epollFD = epoll_create1(EPOLL_CLOEXEC);
    if (m_monitor.epollFD == -1) {
        LOG_ERROR_AND_THROW(boost::str(boost::format("onCreateMONITOR()<< ERROR on epoll_create1() '%1%'(%2%)") % strerror(errno) % errno));
    }

    if (::listen(m_monitor.listenFD, 10) == -1) {
        LOG_ERROR_AND_THROW(boost::str(boost::format("onCreateMONITOR()<< ERROR on listen() '%1%'(%2%)") % strerror(errno) % errno));
    }

    {
        struct epoll_event event;
        event.events = EPOLLIN;
        event.data.u64 = 0;
        event.data.fd = m_monitor.listenFD;

        if (::epoll_ctl(m_monitor.epollFD, EPOLL_CTL_ADD, m_monitor.listenFD, &event)) {
            LOG_ERROR_AND_THROW(boost::str(boost::format("onCreateMONITOR()<< ERROR on epoll_ctl(EPOLL_CTL_ADD) '%1%'(%2%)") % strerror(errno) % errno));
        }
    }
}

void SpreaderClientsAcceptor::runIteration() {
    const auto nready = epoll_wait(m_monitor.epollFD, events, sizeof(events) / sizeof(events[0]), 0);
    if (BOOST_UNLIKELY(nready <= 0)) {
        return;
    }

    for (int i = 0; i < nready; ++i) {
        auto &event = events[i];

        //_LOG_TRACE(boost::format("fd=%1%") % int(event.data.fd));

        if (event.data.fd == m_monitor.listenFD) { //main socket
            struct sockaddr_in addr;
            socklen_t len = sizeof(addr);

            const auto fd = ::accept(m_monitor.listenFD, (struct sockaddr *) &addr, &len);
            if (fd == -1) {
                LOG_ERROR(boost::format("Accept error %1%") % errno);
                continue;
            }

            {
                int value = 1;
                if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value)) < 0) {
                    LOG_ERROR(boost::format("ERROR on setsockopt(SO_REUSEADDR) %1%") % errno);
                    continue;
                };
            }

            // SO_KEEPALIVE
            {
                const int flag = 1;
                if (::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag)) < 0) {
                    LOG_ERROR(boost::format("ERROR on setsockopt(SO_KEEPALIVE) %1%") % errno);
                    continue;
                }
                {
                    int value = 15; // 15 attempts
                    if (::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &value, sizeof(value)) < 0) {
                        LOG_ERROR(boost::format("ERROR on setsockopt(TCP_KEEPCNT) %1%") % errno);
                        continue;
                    };
                }
                {
                    int value = 2; // 2 seconds before first attempt
                    if (::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &value, sizeof(value)) < 0) {
                        LOG_ERROR(boost::format("ERROR on setsockopt(TCP_KEEPIDLE) %1%") % errno);
                        continue;
                    };
                }
                {
                    int value = 2; // 2 seconds between attempts
                    if (::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &value, sizeof(value)) < 0) {
                        LOG_ERROR(boost::format("ERROR on setsockopt(TCP_KEEPINTVL) %1%") % errno);
                        continue;
                    };
                }
            }

            //Disble nagle's
            {
                int value = 1;
                if(::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value)) < 0) {
                    LOG_ERROR(boost::format("ERROR on setsockopt(TCP_NODELAY) %1%") % strerror(errno));
                    continue;
                }
            }

            struct epoll_event event;
            event.events = EPOLLIN | EPOLLRDHUP;
            event.data.u64 = 0;
            event.data.fd = fd;
            if (::epoll_ctl(m_monitor.epollFD, EPOLL_CTL_ADD, fd, &event)) {
                ::close(fd);
                continue;
            }

            SpreaderConnection* connection = new SpreaderConnection(fd);
            (m_monitor.m_traderConnections[fd] = std::shared_ptr<SpreaderConnection>(connection));
            LOG_INFO(boost::format("Socket %1% created") % fd);
            continue;
        }

        const auto fd = event.data.fd;
        BOOST_ASSERT(m_monitor.m_traderConnections.find(fd) != std::end(m_monitor.m_traderConnections));

        //_LOG_DEBUG(boost::format("clientSocketFD=%1%") % fd);

        auto it = m_monitor.m_traderConnections.find(fd);
        if (it == std::end(m_monitor.m_traderConnections)) {
            ::close(fd);
            BOOST_ASSERT(false);
            continue;
        }

        SpreaderConnection&connection = *it->second;

        ReadBuffer& buffer = connection.getReadBuffer();

        const ssize_t n = buffer.readFromStream(fd);
        if (n < 0) {
            const std::string& errorMsg = boost::str(boost::format("Socket Disconnected. Reading from socket %1% errno:%2%. Client disconnected") % fd % errno);
            if(errno == 104) { // "Socket Disconnected"
                LOG_INFO(errorMsg + " (this can result from client closing application gracefully w/o logoff)");
            }
            else {
                LOG_ERROR(errorMsg);
            }
        }

        if (n <= 0) {
            if(!connection.isDisconnected()) {
                connection.setClientDisconnected(true);
                onConnectionError(connection);
            }
            continue;
        }

        bool fatalError = false;
        while (true) {
            if (!buffer.prepareToNextMessageProcessingAndCheckReadiness()){
                buffer.copyPartialMessageToTheBeginningOfTheBuffer();
                break;
            }

            const MessageHeader &header = buffer.getMessageHeader();
            auto messageID = header.m_messageID;

            if (BOOST_UNLIKELY(!checkProtocolVersion(header.m_version))) {
                std::string text = boost::str(boost::format("Message protocol version %1% is incorrect") % (int)header.m_version);
                LOG_ERROR(text);
                ServerClientMessenger::instance()->doSendFormatError(connection, proto::OMS_ERROR_CODE_WRONG_PROTOCOL_VERSION, text, messageID);
                if(BOOST_LIKELY(!buffer.moveToNextProtoMessage())) {
                    break;
                } else {
                    continue;
                }
            }

            auto requestType = (proto::REQUEST_TYPE) header.m_messageTypeID;
            if (requestType != proto::REQUEST_TYPE::REQUEST_TYPE_CLIENT_LOGIN && !connection.canExecuteRequest()) {
                std::string text = "";
                proto::OMS_ERROR_CODE errorCode;
                if (connection.isClosing()) {
                    text = boost::str(boost::format("Cannot process traderConnection request %1% because trader is in closing state") % (int) messageID);
                    errorCode = proto::OMS_ERROR_CODE_TRADER_IS_CLOSED_OR_CLOSING;
                } else {
                    text = boost::str(boost::format("Cannot process traderConnection request %1%. Trader must log in first") % (int) messageID);
                    errorCode = proto::OMS_ERROR_CODE_CLIENT_IS_NOT_LOGGED_IN;
                }
                LOG_INFO(text);
                ServerClientMessenger::instance()->doSendClientRequestRejected(connection, requestType, errorCode, text, messageID);
                if (BOOST_LIKELY(!buffer.moveToNextProtoMessage())) {
                    break;
                } else {
                    continue;
                }
            }

            switch (requestType) {
                case proto::REQUEST_TYPE::REQUEST_TYPE_CLIENT_LOGIN: {
                    proto::ClientLogin message;
                    if (BOOST_UNLIKELY(!buffer.parseMessage(message))) {
                        onDecodeMessageError(connection, requestType, messageID);
                        fatalError = true;
                    } else {
                        if (connection.isLoggedIn()) {
                            auto text = boost::str(boost::format("Client is already logged in as '%1%'") % connection.getUserName());
                            LOG_INFO(text);
                            ServerClientMessenger::instance()->doSendClientLoginReject(connection, proto::OMS_ERROR_CODE_CLIENT_IS_ALREADY_LOGGED_IN, text, messageID);
                        } else {
                            LOG_INFO(boost::str(boost::format("User logged in. User=%1%") % message.username()));
                            connection.onLogin(message.username());
                            sendOtherRequest(SpreadMessageType::Login, 0, 0);
                            if (_spreaderType != SpreaderType::Master) {
                                ServerClientMessenger::instance()->doSendClientLogin(connection, true, hf::VENUE::CME, true, messageID);
                            } else {
                                ServerClientMessenger::instance()->sendRequestToClient(_slaveConnection, requestType, message);
                            }
                        }
                    }
                    break;
                }
                case proto::REQUEST_TYPE::REQUEST_TYPE_NEW_SPREAD_ORDER: {
                    proto::NewSpreadOrder message;
                    if (BOOST_UNLIKELY(!buffer.parseMessage(message))) {
                        onDecodeMessageError(connection, requestType, messageID);
                        fatalError = true;
                    } else {
                        const std::string& clientId(message.spreadorderidentifier());
                        auto mappingIt = _orderIdMappingsByClient.find(clientId);
                        if (mappingIt == _orderIdMappingsByClient.end()) {
                            if (message.legs().size() > 1) {
                                int spreadId = ++_orderId;
                                _orderIdMappingsByClient.insert(std::pair<std::string, int>(clientId, spreadId));
                                _orderIdMappingsByServer.insert(std::pair<int, std::string>(spreadId, clientId));

                                int spreadCoverId = -1;
                                if (message.exitalgo().type() != proto::ExitAlgoType::None) {
                                    spreadCoverId = ++_orderId;
                                    std::string coverClientId = clientId + "_thor_rules";
                                    _orderIdMappingsByClient.insert(std::pair<std::string, int>(coverClientId, spreadCoverId));
                                    _orderIdMappingsByServer.insert(std::pair<int, std::string>(spreadCoverId, coverClientId));
                                }

                                ErrorCode errorCode = _algoManager->processSpreadOrder(message, spreadId, messageID, spreadCoverId);

                                if (errorCode != ErrorCode::Success) {
                                    LOG_INFO(boost::str(boost::format("New spread order - Rejected. User=%1% ClientId=%2% ") % connection.getUserName() % message.spreadorderidentifier()));
                                    ServerClientMessenger::instance()->sendSpreadStatus(connection, clientId, proto::SPREAD_ORDER_STATUS::ACKNOWLEDGED, messageID,
                                                                                        getSpreaderOMSErrorCode(errorCode), "");
                                } else {
                                    if (_spreaderType == SpreaderType::Master && !message.testorder()) {
                                        _spreadOrders.insert(std::pair<std::string, proto::NewSpreadOrder>(clientId, message));
                                    }
                                    LOG_INFO(boost::str(boost::format("New spread order. User=%1% ClientId=%2% SpreadId=%3%") % connection.getUserName() % message.spreadorderidentifier() % _orderId));
                                }

                            } else {
                                LOG_INFO(boost::str(boost::format("New spread order - Rejected. Invalid # of legs. User=%1% ClientId=%2% ") % connection.getUserName() % message.spreadorderidentifier()));
                                ServerClientMessenger::instance()->sendSpreadStatus(connection, clientId, proto::SPREAD_ORDER_STATUS::ACKNOWLEDGED, messageID,
                                                                                    proto::OMS_ERROR_CODE_SPREADER_WRONG_NUMBER_OF_LEGS, "");
                            }
                        } else {
                            LOG_INFO(boost::str(boost::format("New spread order - Rejected. Invalid id. User=%1% ClientId=%2% ") % connection.getUserName() % message.spreadorderidentifier()));
                            ServerClientMessenger::instance()->sendSpreadStatus(connection, clientId, proto::SPREAD_ORDER_STATUS::ACKNOWLEDGED, messageID,
                                                                                proto::OMS_ERROR_CODE_ID_IS_NOT_VALID_ORDER_WITH_SAME_ID_EXISTS, "");
                        }
                    }
                    break;
                }
                case proto::REQUEST_TYPE::REQUEST_TYPE_UPDATE_SPREAD_ORDER: {
                    proto::ModifySpreadOrder message;
                    if (BOOST_UNLIKELY(!buffer.parseMessage(message))) {
                        onDecodeMessageError(connection, requestType, messageID);
                        fatalError = true;
                    } else {
                        const std::string& spreadId(message.spreadorderidentifier());
                        auto mappingIt = _orderIdMappingsByClient.find(spreadId);
                        if (mappingIt != _orderIdMappingsByClient.end()) {
                            ErrorCode errorCode = _algoManager->modifySpreadOrder(message, mappingIt->second);
                            if (errorCode != ErrorCode::Success) {
                                LOG_INFO(boost::str(boost::format("Update spread order - Rejected. User=%1% ClientId=%2% ") % connection.getUserName() % message.spreadorderidentifier()));
                                ServerClientMessenger::instance()->sendSpreadStatus(connection, spreadId, proto::SPREAD_ORDER_STATUS::MODIFIED, messageID,
                                                                                    getSpreaderOMSErrorCode(errorCode), "");
                            } else {
                                if (_spreaderType == SpreaderType::Master) {
                                    ServerClientMessenger::instance()->sendRequestToClient(_slaveConnection, requestType, message);
                                }
                                LOG_INFO(boost::str(boost::format("Update spread order. User=%1% ClientId=%2% SpreadId=%3%") % connection.getUserName() % message.spreadorderidentifier() % _orderId));
                            }
                        } else {
                            ServerClientMessenger::instance()->sendSpreadStatus(connection, spreadId, proto::SPREAD_ORDER_STATUS::MODIFIED, messageID, proto::OMS_ERROR_CODE_ORDER_DOES_NOT_EXIST, "");
                        }
                    }
                    break;
                }
                case proto::REQUEST_TYPE::REQUEST_TYPE_CANCEL_SPREAD_ORDER:
                case proto::REQUEST_TYPE::REQUEST_TYPE_PAUSE_SPREAD_ORDER:
                case proto::REQUEST_TYPE::REQUEST_TYPE_RESUME_SPREAD_ORDER:
                case proto::REQUEST_TYPE::REQUEST_TYPE_CROSSED_ORDER_ACCEPT:
                case proto::REQUEST_TYPE::REQUEST_TYPE_CROSSED_ORDER_DISCARD: {
                    proto::SpreadOrderIdentifier message;
                    if (BOOST_UNLIKELY(!buffer.parseMessage(message))) {
                        onDecodeMessageError(connection, requestType, messageID);
                        fatalError = true;
                    } else {
                        SpreadMessageType mt = getSpreadMessageType(requestType);
                        LOG_INFO(boost::str(boost::format("Action on spread order. User=%1% OrderId=%2% Action=%3%") % connection.getUserName() % message.spreadorderidentifier() % (int) mt));
                        processOtherRequests(connection, messageID, message.spreadorderidentifier(), mt);

                        if (_spreaderType == SpreaderType::Master) {
                            switch (requestType) {
                                case proto::REQUEST_TYPE::REQUEST_TYPE_CANCEL_SPREAD_ORDER:
                                case proto::REQUEST_TYPE::REQUEST_TYPE_PAUSE_SPREAD_ORDER:
                                case proto::REQUEST_TYPE::REQUEST_TYPE_RESUME_SPREAD_ORDER: {
                                    ServerClientMessenger::instance()->sendRequestToClient(_slaveConnection, requestType, message);
                                    break;
                                }
                                default:
                                    break;
                            }
                        }
                    }
                    break;
                }
                case proto::REQUEST_TYPE::REQUEST_TYPE_CLIENT_LOGOUT: {
                    connection.setClosingMessageID(messageID);
                    LOG_INFO(boost::str(boost::format("Logout. User=%1%") % connection.getUserName()));
                    sendOtherRequest(SpreadMessageType::Logout, 0, messageID);
                    break;
                }
                case proto::REQUEST_TYPE::REQUEST_TYPE_REMOTE_FILL: {
                    proto::RemoteFill message;
                    if (BOOST_UNLIKELY(!buffer.parseMessage(message))) {
                        onDecodeMessageError(connection, requestType, messageID);
                        fatalError = true;
                    } else {
                        auto spreadIt = _orderIdMappingsByClient.find(message.spreadorderidentifier());
                        _algoManager->processRemoteFill(spreadIt->second, message.contractid(), message.filledqty());
                    }
                    break;
                }
                case proto::REQUEST_TYPE_REMOTE_PAUSE: {
                    proto::RemotePause message;
                    if (BOOST_UNLIKELY(!buffer.parseMessage(message))) {
                        onDecodeMessageError(connection, requestType, messageID);
                        fatalError = true;
                    } else {
                        auto spreadIt = _orderIdMappingsByClient.find(message.spreadorderidentifier());
                        sendOtherRequest(SpreadMessageType::UnsolicitedSpreadPause, spreadIt->second, messageID);
                    }
                }
                default:
                    break;
            }
            if(fatalError) {
                break;
            }

            if(BOOST_LIKELY(!buffer.moveToNextProtoMessage()))
                break;
        }
    }
}

void SpreaderClientsAcceptor::processOtherRequests(SpreaderConnection& connection, uint32_t messageID, const std::string& spreadId, SpreadMessageType mt) {
    auto mappingIt = _orderIdMappingsByClient.find(spreadId);
    if (mappingIt != _orderIdMappingsByClient.end()) {
        sendOtherRequest(mt, mappingIt->second, messageID);
    } else {
        ServerClientMessenger::instance()->sendSpreadStatus(connection, spreadId, getReplyStatusFromRequest(mt), messageID, proto::OMS_ERROR_CODE_ORDER_DOES_NOT_EXIST, "");
    }
}

void SpreaderClientsAcceptor::sendOtherRequest(SpreadMessageType mt, int id, uint32_t messageID) {
    _algoManager->processOtherRequest(mt, id, messageID);
}

void SpreaderClientsAcceptor::onSlaveConnectionError() {
    auto it = m_monitor.m_traderConnections.begin();
    if (it != m_monitor.m_traderConnections.end()) {
        SpreaderConnection* traderConnection = it->second.get();
        traderConnection->setClientDisconnected(true);
        onConnectionError(*traderConnection);
    }
}


void SpreaderClientsAcceptor::onConnectionError(SpreaderConnection& traderConnection) {
    LOG_INFO(boost::str(boost::format("Client connection error.")));
    if(traderConnection.isLoggedIn()) {
        sendOtherRequest(SpreadMessageType::ClientDown, 0, 0);
    } else {
        removeTraderConnection(traderConnection);
    }
}

void SpreaderClientsAcceptor::handleEndOfTrading(SpreadOrderReply& reply, SpreadMessageType type) {
    LOG_INFO(boost::str(boost::format("End of trading.")));
    SpreaderConnection* traderConnection = m_monitor.m_traderConnections.begin()->second.get();
    if (type == SpreadMessageType::LogoutReply) {
        ServerClientMessenger::instance()->doSendClientLogout(*traderConnection, false, traderConnection->getClosingMessageID());
    }
    removeTraderConnection(*traderConnection);
}

void SpreaderClientsAcceptor::removeTraderConnection(SpreaderConnection& traderConnection) {
    _orderIdMappingsByClient.clear();
    _orderIdMappingsByServer.clear();
    _spreadOrders.clear();
    int fd = traderConnection.getFileDescriptor();
    m_monitor.m_traderConnections.erase(fd);
    ::close(fd);
    LOG_INFO(boost::format("Socket %1% erased") % fd);
}

void SpreaderClientsAcceptor::onDecodeMessageError(SpreaderConnection& traderConnection, proto::REQUEST_TYPE messageRequestType, message_id_t messageID) {
    std::string text = boost::str(boost::format("Cannot parse proto message from buffer. Type %1%") % toString(messageRequestType));
    LOG_ERROR(text);
    ServerClientMessenger::instance()->doSendFormatError(traderConnection, proto::OMS_ERROR_CODE_DECODE_MESSAGE_ERROR, text, messageID);
    traderConnection.getReadBuffer().clear();
    onConnectionError(traderConnection);
}

void SpreaderClientsAcceptor::sendSpreadPrice(SpreadPrice& price) {
    auto it = _orderIdMappingsByServer.find(price._id);
    if (it != _orderIdMappingsByServer.end()) {
        std::string& clientId = it->second;
        SpreaderConnection* traderConnection = m_monitor.m_traderConnections.begin()->second.get();
        ServerClientMessenger::instance()->sendSpreadPrice(*traderConnection, clientId, price._bid, price._ask);
    }
}

void SpreaderClientsAcceptor::sendGatewayStatus(hf::VENUE venue, om::port_id_t id, bool active) {
    SpreaderConnection* traderConnection = m_monitor.m_traderConnections.begin()->second.get();
    ServerClientMessenger::instance()->sendGatewayStatus(*traderConnection, venue, id, active);
}

void SpreaderClientsAcceptor::sendInstrumentMdStatus(hf::VENUE venue, hf::symbol_gid_t gid, bool status) {
    SpreaderConnection* traderConnection = m_monitor.m_traderConnections.begin()->second.get();
    ServerClientMessenger::instance()->sendMarketDataStatus(*traderConnection, venue, gid, status);
}

void SpreaderClientsAcceptor::sendLegOrderStatus(IndividualLegOrderStatus& status, hf::symbol_gid_t gid) {
    auto it = _orderIdMappingsByServer.find(status._spreadId);
    if (it != _orderIdMappingsByServer.end()) {
        std::string& clientId = it->second;
        SpreaderConnection* traderConnection = m_monitor.m_traderConnections.begin()->second.get();
        proto::ORDER_ACTION action = getOrderAction(status._action);
        ServerClientMessenger::instance()->sendLegOrderStatus(*traderConnection, clientId, gid, status._side, status._price, status._qty, status._oid, action, status._rejected);
    }
}

proto::ORDER_ACTION SpreaderClientsAcceptor::getOrderAction(LegOrderAction action) {
    switch (action) {
        case LegOrderAction::New:
            return proto::ORDER_ACTION::NEW;
        case LegOrderAction::Modify:
            return proto::ORDER_ACTION::MODIFY;
        case LegOrderAction::Cancel:
            return proto::ORDER_ACTION::CANCEL;
        case LegOrderAction::Fill:
            return proto::ORDER_ACTION::FILL;
    }
    throw std::runtime_error("unknown order action");
}

void SpreaderClientsAcceptor::sendSpreadStatus(uint32_t messageId, int id, SpreadMessageType type, ErrorCode errorCode, const std::string& errorMessage) {
    auto it = _orderIdMappingsByServer.find(id);
    if (it != _orderIdMappingsByServer.end()) {
        std::string& clientId = it->second;
        SpreaderConnection* traderConnection = m_monitor.m_traderConnections.begin()->second.get();
        proto::SPREAD_ORDER_STATUS status = getSpreadOrderStatus(type, errorCode);
        if (errorCode == ErrorCode::Success) {
            ServerClientMessenger::instance()->sendSpreadStatus(*traderConnection, clientId, status, messageId);
        } else {
            ServerClientMessenger::instance()->sendSpreadStatus(*traderConnection, clientId, status, messageId, getSpreaderOMSErrorCode(errorCode), errorMessage);
        }
    }
}

void SpreaderClientsAcceptor::sendLegsCrossInfo(LegsCrossInfo& info) {
    auto it = _orderIdMappingsByServer.find(info._id);
    if (it != _orderIdMappingsByServer.end()) {
        std::string &clientId = it->second;
        SpreaderConnection *traderConnection = m_monitor.m_traderConnections.begin()->second.get();
        ServerClientMessenger::instance()->sendLegsCrossInfo(*traderConnection, info, clientId);
    }
}

void SpreaderClientsAcceptor::sendFillToRemoteSpreader(int id, hf::symbol_gid_t gid, hf::qty_t quantity) {
    auto it = _orderIdMappingsByServer.find(id);
    if (it != _orderIdMappingsByServer.end()) {
        std::string &clientId = it->second;
        ServerClientMessenger::instance()->sendFillToRemoteSpreader(_slaveConnection, clientId, gid, quantity);
    }
}

void SpreaderClientsAcceptor::forwardUnsolicitedPauseToSlave(int id) {
    auto it = _orderIdMappingsByServer.find(id);
    if (it != _orderIdMappingsByServer.end()) {
        std::string &clientId = it->second;
        ServerClientMessenger::instance()->forwardUnsolicitedPauseToSlave(_slaveConnection, clientId);
    }
}

void SpreaderClientsAcceptor::forwardSpreadToSlave(int id) {
    auto it = _orderIdMappingsByServer.find(id);
    if (it != _orderIdMappingsByServer.end()) {
        std::string &clientId = it->second;
        auto spreadIt = _spreadOrders.find(clientId);
        if (spreadIt != _spreadOrders.end()) {
            ServerClientMessenger::instance()->sendRequestToClient(_slaveConnection, proto::REQUEST_TYPE::REQUEST_TYPE_NEW_SPREAD_ORDER, spreadIt->second);
        }
    }
}

void SpreaderClientsAcceptor::sendCoverOrder(int original, int cover, hf::price_t price) {
    auto it = _orderIdMappingsByServer.find(original);
    if (it != _orderIdMappingsByServer.end()) {
        std::string& origId = it->second;
        auto cit = _orderIdMappingsByServer.find(cover);
        if (cit != _orderIdMappingsByServer.end()) {
            std::string &coverId = cit->second;
            SpreaderConnection *traderConnection = m_monitor.m_traderConnections.begin()->second.get();
            ServerClientMessenger::instance()->sendCoverOrder(*traderConnection, origId, coverId, price);
        } else {
            LOG_INFO(boost::format("Did not send cover order b/c could not find mapping for cover id %1%") % cover);
        }
    } else {
        LOG_INFO(boost::format("Did not send cover order b/c could not find mapping for orig id %1%") % original);
    }
}

void SpreaderClientsAcceptor::sendSMATheo(int id, hf::price_t theo) {
    auto it = _orderIdMappingsByServer.find(id);
    if (it != _orderIdMappingsByServer.end()) {
        std::string &origId = it->second;
        SpreaderConnection *traderConnection = m_monitor.m_traderConnections.begin()->second.get();
        ServerClientMessenger::instance()->sendSMATheo(*traderConnection, origId, theo);
    }
}

void SpreaderClientsAcceptor::sendSpreadOrderPrice(int id, hf::price_t theo) {
    auto it = _orderIdMappingsByServer.find(id);
    if (it != _orderIdMappingsByServer.end()) {
        std::string &origId = it->second;
        SpreaderConnection *traderConnection = m_monitor.m_traderConnections.begin()->second.get();
        ServerClientMessenger::instance()->sendSpreadOrderPrice(*traderConnection, origId, theo);
    }
}

proto::SPREAD_ORDER_STATUS SpreaderClientsAcceptor::getReplyStatusFromRequest(SpreadMessageType messageRequestType) {
    switch (messageRequestType) {
        case SpreadMessageType::AcceptCrossedOrder:
            return proto::SPREAD_ORDER_STATUS::ACKNOWLEDGED;
        case SpreadMessageType::DiscardCrossedOrder:
        case SpreadMessageType::CancelSpreadOrder:
            return proto::SPREAD_ORDER_STATUS::CANCELLED;
        case SpreadMessageType::PauseSpreadOrder:
            return proto::SPREAD_ORDER_STATUS::PAUSED;
        case SpreadMessageType::ResumeSpreadOrder:
            return proto::SPREAD_ORDER_STATUS::RESUMED;
        default:
            LOG_ERROR(boost::str(boost::format("Not handled type of request %1%") % (int) messageRequestType));
            return proto::SPREAD_ORDER_STATUS::STATUS_UNKNOWN;
    }
}

proto::SPREAD_ORDER_STATUS SpreaderClientsAcceptor::getSpreadOrderStatus(SpreadMessageType type, ErrorCode errorCode) {
    switch (type) {
        case SpreadMessageType::UnsolicitedCancel:
            return proto::SPREAD_ORDER_STATUS::UNSOLICITED_CANCEL;
        case SpreadMessageType::SpreadFinished:
            return proto::SPREAD_ORDER_STATUS::FILLED;
        case SpreadMessageType::CancelSpreadOrderReply:
        case SpreadMessageType::DiscardCrossedOrderReply:
            return proto::SPREAD_ORDER_STATUS::CANCELLED;
        case SpreadMessageType::PauseSpreadOrderReply:
            return proto::SPREAD_ORDER_STATUS::PAUSED;
        case SpreadMessageType::ResumeSpreadOrderReply:
            return proto::SPREAD_ORDER_STATUS::RESUMED;
        case SpreadMessageType::NewSpreadOrderReply:
        case SpreadMessageType::AcceptCrossedOrderReply:
            return proto::SPREAD_ORDER_STATUS::ACKNOWLEDGED;
        case SpreadMessageType::UnsolicitedSpreadPause:
            return proto::SPREAD_ORDER_STATUS::UNSOLICITED_PAUSE;
        case SpreadMessageType::ModifySpreadOrderReply:
            return proto::SPREAD_ORDER_STATUS::MODIFIED;
        default:
            LOG_ERROR(boost::str(boost::format("Not handled type of SpreadMessageType %1%") % (int) type));
            return proto::SPREAD_ORDER_STATUS::STATUS_UNKNOWN;
    }
}

proto::OMS_ERROR_CODE SpreaderClientsAcceptor::getSpreaderOMSErrorCode(ErrorCode code) {
    switch (code) {
        case ErrorCode::CannotCancelAlreadyFinished:
            return proto::OMS_ERROR_CODE_SPREADER_CANNOT_CANCEL_ALREADY_FINISHED;
        case ErrorCode::CannotPauseSpreadNotRunning:
            return proto::OMS_ERROR_CODE_SPREADER_CANNOT_PAUSE_SPREAD_NOT_RUNNING;
        case ErrorCode::CannotResumeNotPaused:
            return proto::OMS_ERROR_CODE_SPREADER_CANNOT_RESUME_NOT_PAUSED;
        case ErrorCode::DuplicateOrderId:
            return proto::OMS_ERROR_CODE_SPREADER_DUPLICATE_ORDER_ID;
        case ErrorCode::SpreadNotFound:
            return proto::OMS_ERROR_CODE_SPREADER_SPREAD_NOT_FOUND;
        case ErrorCode::Unsolicited:
            return proto::OMS_ERROR_CODE_SPREADER_UNSOLICITED;
        case ErrorCode::WrongNumberOfLegs:
            return proto::OMS_ERROR_CODE_SPREADER_WRONG_NUMBER_OF_LEGS;
        case ErrorCode::WorkBothSidesNotSupported:
            return proto::OMS_ERROR_CODE_SPREADER_WORK_BOTH_SIDES_NOT_SUPPORTED;
        case ErrorCode::UnsupportedProduct:
            return proto::OMS_ERROR_CODE_SPREADER_UNSUPPORTED_PRODUCT;
        default:
            return proto::OMS_ERROR_CODE_SPREADER_GENERIC_SPREADER_ERROR;
    }
}

SpreadMessageType SpreaderClientsAcceptor::getSpreadMessageType(proto::REQUEST_TYPE type) {
    switch (type) {
        case proto::REQUEST_TYPE::REQUEST_TYPE_CROSSED_ORDER_DISCARD:
            return SpreadMessageType::DiscardCrossedOrder;
        case proto::REQUEST_TYPE::REQUEST_TYPE_CROSSED_ORDER_ACCEPT:
            return SpreadMessageType::AcceptCrossedOrder;
        case proto::REQUEST_TYPE::REQUEST_TYPE_CANCEL_SPREAD_ORDER:
            return SpreadMessageType::CancelSpreadOrder;
        case proto::REQUEST_TYPE::REQUEST_TYPE_RESUME_SPREAD_ORDER:
            return SpreadMessageType::ResumeSpreadOrder;
        case proto::REQUEST_TYPE::REQUEST_TYPE_PAUSE_SPREAD_ORDER:
            return SpreadMessageType::PauseSpreadOrder;
        default:
            throw std::runtime_error("uknown request type");
    }
}
}}
