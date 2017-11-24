
#include <sys/socket.h>
#include <cstdlib>
#include <boost/format.hpp>
#include <hf/log.h>
#include <netdb.h>
#include <hf/hf.h>
#include "ClientAcceptor.h"
#include "ServerClientMessenger.h"
#include "messages/omsproto.pb.h"
#include <hf/util/SocketUtils.h>
#include <sys/epoll.h>
#include <netinet/tcp.h>
#include <chrono>
#include <iostream>
#include "Common.h"

LOG_DECLARE();

namespace Python {

void SpreaderClientsAcceptor::initialize(ViperQueue* queue) {
    _inQueue = queue;
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

void SpreaderClientsAcceptor::sendGatewayStatus(hf::VENUE venue, om::port_id_t id, bool active) {
    SpreaderConnection* traderConnection = m_monitor.m_traderConnections.begin()->second.get();
    ServerClientMessenger::instance()->sendGatewayStatus(*traderConnection, venue, id, active);
}

void SpreaderClientsAcceptor::sendInstrumentMdStatus(hf::VENUE venue, hf::symbol_gid_t gid, bool status) {
    SpreaderConnection* traderConnection = m_monitor.m_traderConnections.begin()->second.get();
    ServerClientMessenger::instance()->sendMarketDataStatus(*traderConnection, venue, gid, status);
}


}}
