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

struct HeartBeat {
    uint64_t _nextTime = 0;
    uint64_t _interval = 0;
    uint32_t _tolerance = 0;
};

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

    HeartBeat _beatToSend;
    HeartBeat _beatToReceive;

private:
    ReadBuffer m_readBuffer;
    bool m_disconnected = false;
    int32_t m_closingMessageID = 0;
    bool _loggedIn = false;
    std::string _userName;
};

class ClientsAcceptor {

public:
    ClientsAcceptor(std::string srvrName, int32_t port, FrontSide* frontSide, double frequency);

    void createMonitor();
    void runIteration();

    void sendSwitchChangeReply(uint32_t strategy, uint32_t action, const std::string& value, uint32_t gridId, bool accepted);
    void sendGridChangeReply(uint32_t gridId, bool accepted);
    void sendGridUpdate(proto::GridUpdate& data);
    void sendChartDataPoints(proto::DataSeriesPointSet& set);
    void sendPlaySound(uint32_t id);
    void handleLoginAccepted(proto::GUIInitialization& m);
    void handleLoginRejected();
    void handleLoggedOut();
    void doHeartBeats();

private:
    int createMonitorSocket();
    void onDecodeMessageError(ClientConnection& traderConnection, proto::GUI_MESSAGE_TYPE messageRequestType, message_id_t messageID);
    void removeTraderConnection(ClientConnection& traderConnection);
    void onConnectionError(ClientConnection& traderConnection);
    void sendOtherRequest(ViperMessageType mt, uint32_t messageID);
    void sendHeartBeat(ClientConnection& conn);
    bool hasConnection();

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
    double _frequency;
    uint32_t _timeOutSeconds = 15;
};

}

#endif //VIPER_CLIENT_ACCEPTOR_H

