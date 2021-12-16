#include <sys/socket.h>
#include <boost/format.hpp>
#include <util/log.h>
#include <netdb.h>
#include <hf/hf.h>
#include "ClientAcceptor.h"
#include "ServerClientMessenger.h"
#include "messages/python.pb.h"
#include <hf/util/SocketUtils.h>
#include <sys/epoll.h>
#include <netinet/tcp.h>
#include <chrono>
#include <iostream>
#include "Common.h"
#include "FrontSide.h"

LOG_DECLARE();

namespace Python {

ClientsAcceptor::ClientsAcceptor(std::string srvrName, int32_t port, FrontSide* frontSide, double frequency)
:
    m_port(port),
    m_host(srvrName),
    _frontSide(frontSide),
    _frequency(frequency)
{}

int ClientsAcceptor::createMonitorSocket() {
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

void ClientsAcceptor::createMonitor() {
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

void ClientsAcceptor::runIteration() {
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
                LOG_ERROR(boost::format("ERROR on epoll_ctl(EPOLL_CTL_ADD) %1%") % strerror(errno));
                ::close(fd);
                continue;
            }

            (m_monitor.m_traderConnections[fd] = std::make_shared<ClientConnection>(fd));
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

        ClientConnection& connection = *it->second;

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
                onConnectionError(it);
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
                ServerClientMessenger::instance()->doSendFormatError(connection, proto::ERROR_CODE_WRONG_PROTOCOL_VERSION, text, messageID);
                if(BOOST_LIKELY(!buffer.moveToNextProtoMessage())) {
                    break;
                } else {
                    continue;
                }
            }

            auto requestType = (proto::GUI_MESSAGE_TYPE) header.m_messageTypeID;
            if (requestType != proto::GUI_MESSAGE_TYPE::GUI_MESSAGE_TYPE_CLIENT_LOGIN && !connection.canExecuteRequest()) {
                std::string text = "";
                if (connection.isClosing()) {
                    text = boost::str(boost::format("Cannot process traderConnection request %1% because trader is in closing state") % (int) messageID);
                } else {
                    text = boost::str(boost::format("Cannot process traderConnection request %1%. Trader must log in first") % (int) messageID);
                    ServerClientMessenger::instance()->doSendClientRequestRejected(connection, requestType, proto::ERROR_CODE_CLIENT_IS_NOT_LOGGED_IN, text, messageID);
                }
                LOG_INFO(text);
                if (BOOST_LIKELY(!buffer.moveToNextProtoMessage())) {
                    break;
                } else {
                    continue;
                }
            }

            switch (requestType) {
                case proto::GUI_MESSAGE_TYPE::GUI_MESSAGE_TYPE_CLIENT_LOGIN: {
                    proto::ClientLogin message;
                    if (BOOST_UNLIKELY(!buffer.parseMessage(message))) {
                        onDecodeMessageError(it, requestType, messageID);
                        fatalError = true;
                    } else {
                        if (connection.isLoggedIn()) {
                            auto text = boost::str(boost::format("Client is already logged in as '%1%'") % connection.getUserName());
                            LOG_INFO(text);
                            ServerClientMessenger::instance()->doSendClientLoginReject(connection, proto::ERROR_CODE_CLIENT_IS_ALREADY_LOGGED_IN, text, messageID);
                        } else {
                            LOG_INFO(boost::str(boost::format("Logging in. User=%1%") % message.username()));
                            connection.onLogin(message.username());
                            connection._beatToSend._interval = GetCyclesFromSeconds(message.heartbeatinterval(), _frequency);
                            connection._beatToSend._nextTime = connection._beatToSend._interval + clockUtil::GetCycles();
                            connection._beatToReceive._interval = GetCyclesFromSeconds(_timeOutSeconds, _frequency);
                            connection._beatToReceive._nextTime = connection._beatToReceive._interval + clockUtil::GetCycles();
                            sendOtherRequest(ViperMessageType::Login, messageID, connection.m_fd);
                            connection.loginComplete();
                            sendClientCount();
                        }
                    }
                    break;
                }
                case proto::GUI_MESSAGE_TYPE::GUI_MESSAGE_TYPE_CLIENT_LOGOUT: {
                    connection.setClosingMessageID(messageID);
                    LOG_INFO(boost::str(boost::format("Logging out. User=%1%") % connection.getUserName()));
                    sendOtherRequest(ViperMessageType::Logout, messageID, connection.m_fd);
                    break;
                }
                case proto::GUI_MESSAGE_TYPE::GUI_MESSAGE_TYPE_GRID_CHANGE: {
                    proto::GridUpdate message;
                    if (BOOST_UNLIKELY(!buffer.parseMessage(message))) {
                        onDecodeMessageError(it, requestType, messageID);
                        fatalError = true;
                    } else {
                        _frontSide->onGridUpdate(message, connection.m_fd);
                    }
                    break;
                }
                case proto::GUI_MESSAGE_TYPE::GUI_MESSAGE_TYPE_GUI_INITIALIZATION_ACCEPTED: {
                    LOG_INFO("GUI Initialization accepted. connectionId=" + std::to_string(connection.m_fd));
                    _frontSide->onGuiInitializationAccepted();
                    break;
                }
                case proto::GUI_MESSAGE_TYPE::GUI_MESSAGE_TYPE_GUI_INITIALIZATION_REJECTED: {
                    LOG_INFO("GUI Initialization rejected. connectionId=" + std::to_string(connection.m_fd));
                    _frontSide->onGuiInitializationRejected();
                    break;
                }
                case proto::GUI_MESSAGE_TYPE::GUI_MESSAGE_TYPE_SYSTEM_STATE_CHANGE: {
                    proto::GridUpdate message;
                    if (BOOST_UNLIKELY(!buffer.parseMessage(message))) {
                        onDecodeMessageError(it, requestType, messageID);
                        fatalError = true;
                    } else {
                        BOOST_ASSERT(message.updatetype() == proto::GridUpdateType::GridButtonType);
                        SystemState state = ToSystemStateFromButtonId(message.gridbutton());
                        _frontSide->onSystemStateChange(state, "GUI requested. user=" + connection.getUserName());
                    }
                    break;
                }
                case proto::GUI_MESSAGE_TYPE::GUI_MESSAGE_TYPE_SWITCH_CHANGE: {
                    proto::SwitchChange message;
                    if (BOOST_UNLIKELY(!buffer.parseMessage(message))) {
                        onDecodeMessageError(it, requestType, messageID);
                        fatalError = true;
                    } else {
                        _frontSide->onSwitchChange(message, connection.getUserName());
                    }
                    break;
                }
                case proto::GUI_MESSAGE_TYPE::GUI_MESSAGE_TYPE_HEARTBEAT: {
                    connection._beatToReceive._nextTime = clockUtil::GetCycles() + connection._beatToReceive._interval;
                    break;
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
    //doHeartBeats();
}


void ClientsAcceptor::doHeartBeats() {
    uint64_t now = clockUtil::GetCycles();
    for (auto iter = m_monitor.m_traderConnections.begin(); iter != m_monitor.m_traderConnections.end();) {
        ClientConnection& conn = *iter->second;
        if (conn._beatToSend._nextTime < now) {
            sendHeartBeat(conn);
            conn._beatToSend._nextTime = now + conn._beatToSend._interval;
        }
        if (conn._beatToReceive._nextTime + conn._beatToReceive._tolerance < now) {
            LOG_INFO(boost::str(boost::format("GUI timed out. connectionId=%1%") % conn.m_fd));
            onConnectionError(iter);
        } else {
            ++iter;
        }
    }
}

void ClientsAcceptor::sendHeartBeat(ClientConnection& conn) {
    proto::EmptyMessage message;
    message.set_messageid(0);
    ServerClientMessenger::instance()->sendMessageToClient(conn, proto::SERVER_MESSAGE_TYPE_HEARTBEAT, message);
}

void ClientsAcceptor::sendOtherRequest(ViperMessageType mt, uint32_t messageID, int32_t connectionId) {
    _frontSide->onOtherRequest(mt, connectionId);
}

void ClientsAcceptor::onConnectionError(std::map<int, std::shared_ptr<ClientConnection>>::iterator& iter) {
    auto& traderConnection = *iter->second;
    LOG_INFO(boost::str(boost::format("Client connection error. connectionId=%1%") % traderConnection.m_fd));
    if(traderConnection.isLoggedIn()) {
        auto id = traderConnection.m_fd;
        removeTraderConnection(iter);
        if (!hasConnection()) {
            sendOtherRequest(ViperMessageType::ClientDown, 0, 0);
        }
    } else {
        removeTraderConnection(iter);
    }
}

void ClientsAcceptor::removeTraderConnection(std::map<int, std::shared_ptr<ClientConnection>>::iterator& iter) {
    int fd = iter->second->getFileDescriptor();
    iter = m_monitor.m_traderConnections.erase(iter);
    ::close(fd);
    LOG_INFO(boost::format("Socket %1% erased") % fd);
    sendClientCount();
}

void ClientsAcceptor::onDecodeMessageError(std::map<int, std::shared_ptr<ClientConnection>>::iterator& it, proto::GUI_MESSAGE_TYPE messageRequestType, message_id_t messageID) {
    auto& traderConnection = *it->second;
    std::string text = boost::str(boost::format("Cannot parse proto message from buffer. Type %1%. connectionId=%2%") % toString(messageRequestType) % it->first);
    LOG_ERROR(text);
    ServerClientMessenger::instance()->doSendFormatError(traderConnection, proto::ERROR_CODE_DECODE_MESSAGE_ERROR, text, messageID);
    traderConnection.getReadBuffer().clear();
    onConnectionError(it);
}

bool ClientsAcceptor::hasConnection() const noexcept {
    return m_monitor.m_traderConnections.begin() !=  m_monitor.m_traderConnections.end();
}

void ClientsAcceptor::sendGridUpdate(proto::GridUpdate& data, int32_t connectionId) {
    if (hasConnection()) {
        data.set_messageid(0);
        if (connectionId < 0) {
            sendMessageToAllConnection([&data](ClientConnection* connection) {
                ServerClientMessenger::instance()->sendMessageToClient(*connection, proto::SERVER_MESSAGE_TYPE_GRID_STATE_CHANGE, data);
            });
        } else {
            auto iter = m_monitor.m_traderConnections.find(connectionId);
            if (iter != m_monitor.m_traderConnections.end()) {
                auto& connection = *iter->second;
                ServerClientMessenger::instance()->sendMessageToClient(connection, proto::SERVER_MESSAGE_TYPE_GRID_STATE_CHANGE, data);
            }
        }
    } else {
        LOG_WARN(boost::str(boost::format("Wanted to send GridUpdate but do not have connection. %1%") %  data.ShortDebugString()));
    }
}

void ClientsAcceptor::sendChartDataPoints(proto::DataSeriesPointSet& data, std::ostringstream& streamMessage) {
    if (hasConnection()) {
        sendMessageToAllConnection([&data, &streamMessage](ClientConnection* connection) {
            ServerClientMessenger::instance()->sendMessageToClient(*connection,
                                                                   proto::SERVER_MESSAGE_TYPE_DATA_POINT_SET,
                                                                   data, streamMessage);
        });
    } else {
        LOG_WARN(boost::str(boost::format("Wanted to send ChartDataPoint but do not have connection. %1%") % data.ShortDebugString()));
    }
}

void ClientsAcceptor::sendChartHistories(proto::DataSeriesReplay& data, int32_t connectionId) {
    auto iter = m_monitor.m_traderConnections.find(connectionId);
    if (BOOST_LIKELY(iter != m_monitor.m_traderConnections.end())) {
        ClientConnection* connection = iter->second.get();
        ServerClientMessenger::instance()->sendMessageToClient(*connection, proto::SERVER_MESSAGE_TYPE_DATA_POINT_REPLAY, data);
    }
}

void ClientsAcceptor::sendSwitchChangeReply(uint32_t strategy, uint32_t action, const std::string& value, uint32_t gridId, bool accepted) {
    if (hasConnection()) {
        sendMessageToAllConnection([strategy, action, &value, gridId, accepted](ClientConnection* connection) {
            ServerClientMessenger::instance()->sendSwitchChangeReply(*connection, strategy, action, value, gridId,
                                                                     accepted);
        });
    } else {
        LOG_WARN(boost::str(boost::format("Wanted to send SwitchReply but do not have connection. ")));
    }
}

void ClientsAcceptor::handleLoginAccepted(proto::GUIInitialization& m, uint64_t heartbeatIntervalInMilliseconds, int32_t connectionId, const char* trader) {
    if (hasConnection()) {
        auto iter = m_monitor.m_traderConnections.find(connectionId);
        if (BOOST_LIKELY(iter != m_monitor.m_traderConnections.end())) {
            ClientConnection* connection = iter->second.get();
            ServerClientMessenger::instance()->doSendClientLogin(*connection, 0, heartbeatIntervalInMilliseconds, trader);
            ServerClientMessenger::instance()->sendGUIInitialization(*connection, m);
        } else {
            LOG_WARN(boost::str(boost::format("Wanted to send LoginAccepted but do not have connection. connectionId=%1%") % connectionId));
        }
    } else {
        LOG_WARN(boost::str(boost::format("Wanted to send LoginAccepted but do not have connection. ")));
    }
}

void ClientsAcceptor::handleLoginRejected(int32_t connectionId) {
    if (hasConnection()) {
        auto iter = m_monitor.m_traderConnections.find(connectionId);
        if (BOOST_LIKELY(iter != m_monitor.m_traderConnections.end())) {
            ClientConnection* connection = iter->second.get();
            ServerClientMessenger::instance()->doSendClientLoginReject(*connection, proto::ERROR_CODE_CLIENT_IS_ALREADY_LOGGED_IN, "strategies reject", 0);
        } else {
            LOG_WARN(boost::str(boost::format("Wanted to send LoginRejected but do not have connection. connectionId=%1%") % connectionId));
        }
    } else {
        LOG_WARN(boost::str(boost::format("Wanted to send LoginRejected but do not have connection. ")));
    }
}

void ClientsAcceptor::handleLoggedOut(int32_t connectionId) {
    if (hasConnection()) {
        auto iter = m_monitor.m_traderConnections.find(connectionId);
        if (BOOST_LIKELY(iter != m_monitor.m_traderConnections.end())) {
            ClientConnection* connection = iter->second.get();
            ServerClientMessenger::instance()->doSendClientLogout(*connection, false, 0);
            removeTraderConnection(iter);
        } else {
            LOG_WARN(boost::str(boost::format("Wanted to send LoggedOut but do not have connection. connectionId=%1%") % connectionId));
        }
    } else {
        LOG_WARN(boost::str(boost::format("Wanted to send LoggedOut but do not have connection. ")));
    }
}

void ClientsAcceptor::sendGridChangeReply(proto::GridUpdate& message, int32_t connectionId) {
    if (hasConnection()) {
        if (connectionId < 0) {
            sendMessageToAllConnection([&message](ClientConnection* connection) {
                ServerClientMessenger::instance()->sendMessageToClient(*connection,
                                                                       proto::SERVER_MESSAGE_TYPE_GRID_CHANGE_ACKNOWLEDGED,
                                                                       message);
            });
        } else {
            auto iter = m_monitor.m_traderConnections.find(connectionId);
            if (iter != m_monitor.m_traderConnections.end()) {
                ServerClientMessenger::instance()->sendMessageToClient(*iter->second,
                                                                       proto::SERVER_MESSAGE_TYPE_GRID_CHANGE_ACKNOWLEDGED,
                                                                       message);
            }
        }
    } else {
        LOG_WARN(boost::str(boost::format("Wanted to send GridChangeReply but do not have connection. ")));
    }
}

void ClientsAcceptor::sendPlaySound(uint32_t id) {
    if (hasConnection()) {
        proto::PlaySound message;
        message.set_id(id);
        sendMessageToAllConnection([&message](ClientConnection* connection) {
            ServerClientMessenger::instance()->sendMessageToClient(*connection, proto::SERVER_MESSAGE_TYPE_PLAY_SOUND,
                                                                   message);
        });
    } else {
        LOG_WARN(boost::str(boost::format("Wanted to send PlaySound but do not have connection. ")));
    }
}

void ClientsAcceptor::sendTraderUpdate(const char* trader) {
    if (hasConnection()) {
        proto::TraderChanged message;
        message.set_trader(trader);
        sendMessageToAllConnection([&message](ClientConnection* connection) {
            ServerClientMessenger::instance()->sendMessageToClient(*connection, proto::SERVER_MESSAGE_TYPE_TRADER_CHANGED,
                                                                   message);
        });
    } else {
        LOG_WARN(boost::str(boost::format("Wanted to send PlaySound but do not have connection. ")));
    }
}

void ClientsAcceptor::sendClientCount() {
    if (hasConnection()) {
        proto::ClientCount message;
        message.set_count(m_monitor.m_traderConnections.size());
        sendMessageToAllConnection([&message](ClientConnection* connection) {
            ServerClientMessenger::instance()->sendMessageToClient(*connection, proto::SERVER_MESSAGE_TYPE_CLIENT_COUNT,
                                                                   message);
        });
    }
}

void ClientsAcceptor::sendGuiHeartbeat(uint64_t utc_ts_in_nanos) {
    if(hasConnection()) {
        proto::HeartBeatMessage heartBeatMessage;
        heartBeatMessage.set_timestamp(utc_ts_in_nanos);
        sendMessageToAllConnection([&heartBeatMessage](ClientConnection* connection) {
            ServerClientMessenger::instance()->sendMessageToClient(*connection, proto::SERVER_MESSAGE_TYPE_HEARTBEAT,
                                                                   heartBeatMessage);
        });
    }
}

void ClientsAcceptor::checkConnectionHealth() {
    for (auto iter = m_monitor.m_traderConnections.begin(); iter != m_monitor.m_traderConnections.end();) {
        auto connection = iter->second.get();
        if (connection->isLoggedIn() && connection->isDisconnected()) {
            onConnectionError(iter);
        } else {
            ++iter;
        }
    }
}

const char* const ClientsAcceptor::getConnectionUserName(int32_t connectionId) const noexcept {
    auto iter = m_monitor.m_traderConnections.find(connectionId);
    if (iter == m_monitor.m_traderConnections.end()) {
        return nullptr;
    }
    return iter->second->getUserName().c_str();
}

uint32_t ClientsAcceptor::getNumberConnection() const noexcept {
    return m_monitor.m_traderConnections.size();
}

}
