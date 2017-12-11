#ifndef VIPER_CLIENT_ACCEPTOR_H
#define VIPER_CLIENT_ACCEPTOR_H

#include <cstdint>
#include <sys/epoll.h>
#include <strat/Context.hpp>
#include "messages/omsproto.pb.h"
#include "ReadBuffer.h"
#include "Common.h"

namespace Python {

class FrontSide;

class ClientConnection {

public:
    ClientConnection(int fd) : m_fd{fd} { }

    ReadBuffer& getReadBuffer() { return m_readBuffer; }

    virtual bool isDisconnected() const { return m_disconnected; }

    void setClientDisconnected(bool disconnected) { m_disconnected = disconnected; }

    bool isClosing() const { return m_closingMessageID > 0; }

    bool isLoggedIn() const { return _loggedIn; }

    std::string& getUserName() { return _userName; }

    void onLogin(const std::string& userName) {
        _userName = userName;
        _loggedIn = true;
    }

    int32_t getClosingMessageID() const { return m_closingMessageID; }

    int getFileDescriptor() const { return m_fd; }

    void setClosingMessageID(int32_t messageID) { m_closingMessageID = messageID; }

    bool canExecuteRequest() const {
        return isLoggedIn() && !isClosing();
    }

    uint32_t m_messageSequence = 1;
    int m_fd;

private:
    ReadBuffer m_readBuffer;
    bool m_disconnected = false;
    int32_t m_closingMessageID = 0;
    bool _loggedIn = false;
    std::string _userName;
};

class ClientsAcceptor {

public:
    ClientsAcceptor(std::string srvrName, int32_t port)
    :
        m_port(port),
        m_host(srvrName)
    {}

    void createMonitor();
    void runIteration();

private:
    int createMonitorSocket();
    void onDecodeMessageError(ClientConnection& traderConnection, proto::REQUEST_TYPE messageRequestType, message_id_t messageID);
    void removeTraderConnection(ClientConnection& traderConnection);
    void onConnectionError(ClientConnection& traderConnection);
    void sendOtherRequest(ViperMessageType mt, int orderId, uint32_t messageID);

    struct epoll_event events[100]{};

    int32_t m_port;
    std::string m_host;

    struct {
        int port = 0;
        int epollFD = -1;
        int listenFD = -1;
        std::map<int, std::shared_ptr<ClientConnection>> m_traderConnections;
    } m_monitor;

    FrontSide* _frontSide;
};

}

#endif //VIPER_CLIENT_ACCEPTOR_H

