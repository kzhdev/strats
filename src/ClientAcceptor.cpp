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
#include "FrontSide.h"

LOG_DECLARE();

namespace Python {

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
                ::close(fd);
                continue;
            }

            ClientConnection* connection = new ClientConnection(fd);
            (m_monitor.m_traderConnections[fd] = std::shared_ptr<ClientConnection>(connection));
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
                proto::ERROR_CODE errorCode;
                if (connection.isClosing()) {
                    text = boost::str(boost::format("Cannot process traderConnection request %1% because trader is in closing state") % (int) messageID);
                    errorCode = proto::ERROR_CODE_TRADER_IS_CLOSED_OR_CLOSING;
                } else {
                    text = boost::str(boost::format("Cannot process traderConnection request %1%. Trader must log in first") % (int) messageID);
                    errorCode = proto::ERROR_CODE_CLIENT_IS_NOT_LOGGED_IN;
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
                case proto::GUI_MESSAGE_TYPE::GUI_MESSAGE_TYPE_CLIENT_LOGIN: {
                    proto::ClientLogin message;
                    if (BOOST_UNLIKELY(!buffer.parseMessage(message))) {
                        onDecodeMessageError(connection, requestType, messageID);
                        fatalError = true;
                    } else {
                        if (connection.isLoggedIn()) {
                            auto text = boost::str(boost::format("Client is already logged in as '%1%'") % connection.getUserName());
                            LOG_INFO(text);
                            ServerClientMessenger::instance()->doSendClientLoginReject(connection, proto::ERROR_CODE_CLIENT_IS_ALREADY_LOGGED_IN, text, messageID);
                        } else {
                            LOG_INFO(boost::str(boost::format("User logged in. User=%1%") % message.username()));
                            connection.onLogin(message.username());
                            ServerClientMessenger::instance()->doSendClientLogin(connection, messageID);
                            sendOtherRequest(ViperMessageType::Login, messageID);
                        }
                    }
                    break;
                }
                case proto::GUI_MESSAGE_TYPE::GUI_MESSAGE_TYPE_CLIENT_LOGOUT: {
                    connection.setClosingMessageID(messageID);
                    LOG_INFO(boost::str(boost::format("Logout. User=%1%") % connection.getUserName()));
                    sendOtherRequest(ViperMessageType::Logout, messageID);
                    break;
                }
                case proto::GUI_MESSAGE_TYPE::GUI_MESSAGE_TYPE_GRID_CHANGE: {
                    proto::GridUpdate message;
                    if (BOOST_UNLIKELY(!buffer.parseMessage(message))) {
                        onDecodeMessageError(connection, requestType, messageID);
                        fatalError = true;
                    } else {
                        _frontSide->onGridUpdate(message);
                    }
                    break;
                }
                case proto::GUI_MESSAGE_TYPE::GUI_MESSAGE_TYPE_GUI_INITIALIZATION_ACCEPTED: {
                    _frontSide->onGuiInitializationAccepted();
                    break;
                }
                case proto::GUI_MESSAGE_TYPE::GUI_MESSAGE_TYPE_GUI_INITIALIZATION_REJECTED: {
                    _frontSide->onGuiInitializationRejected();
                    break;
                }
                case proto::GUI_MESSAGE_TYPE::GUI_MESSAGE_TYPE_SYSTEM_STATE_CHANGE: {
                    proto::SystemStateChange message;
                    if (BOOST_UNLIKELY(!buffer.parseMessage(message))) {
                        onDecodeMessageError(connection, requestType, messageID);
                        fatalError = true;
                    } else {
                        _frontSide->onSystemStateChange(message.newstate());
                    }
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
}

void ClientsAcceptor::sendOtherRequest(ViperMessageType mt, uint32_t messageID) {
    _frontSide->onOtherRequest(mt);
}

void ClientsAcceptor::onConnectionError(ClientConnection& traderConnection) {
    LOG_INFO(boost::str(boost::format("Client connection error.")));
    if(traderConnection.isLoggedIn()) {
        sendOtherRequest(ViperMessageType::ClientDown, 0);
    } else {
        removeTraderConnection(traderConnection);
    }
}

void ClientsAcceptor::removeTraderConnection(ClientConnection& traderConnection) {
    int fd = traderConnection.getFileDescriptor();
    m_monitor.m_traderConnections.erase(fd);
    ::close(fd);
    LOG_INFO(boost::format("Socket %1% erased") % fd);
}

void ClientsAcceptor::onDecodeMessageError(ClientConnection& traderConnection, proto::GUI_MESSAGE_TYPE messageRequestType, message_id_t messageID) {
    std::string text = boost::str(boost::format("Cannot parse proto message from buffer. Type %1%") % toString(messageRequestType));
    LOG_ERROR(text);
    ServerClientMessenger::instance()->doSendFormatError(traderConnection, proto::ERROR_CODE_DECODE_MESSAGE_ERROR, text, messageID);
    traderConnection.getReadBuffer().clear();
    onConnectionError(traderConnection);
}

void ClientsAcceptor::sendGUIInitialization(proto::GUIInitialization& data) {
    ClientConnection* connection = m_monitor.m_traderConnections.begin()->second.get();
    ServerClientMessenger::instance()->sendGUIInitialization(*connection, data);
}

}
