#ifndef VIPER_CLIENT_ACCEPTOR_H
#define VIPER_CLIENT_ACCEPTOR_H

#include <cstdint>
#include <sys/epoll.h>
#include <strat/Context.hpp>
#include "messages/python.pb.h"
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
    }

    void loginComplete() {
        BOOST_ASSERT(!_loggedIn);
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

using ClientConnectionIterT = std::unordered_map<int, std::shared_ptr<ClientConnection>>::iterator;

class ClientsAcceptor {
public:
    ClientsAcceptor(std::string srvrName, int32_t port, FrontSide* frontSide, double frequency);

    void createMonitor();
    void runIteration();

    void sendSwitchChangeReply(uint32_t strategy, uint32_t action, const std::string& value, uint32_t gridId, bool accepted);
    void sendGridChangeReply(proto::GridUpdate& message, ClientConnection* connection = nullptr);
    void sendGridUpdate(proto::GridUpdate& data, ClientConnection* connection);
    void sendGridUpdate(proto::GridUpdate& data, int32_t connectionId = -1);
    void sendChartDataPoints(proto::DataSeriesPointSets& data, std::ostringstream& streamMessage);
    void sendChartHistories(proto::DataSeriesReplay& data, ClientConnection* connection);
    void sendChartHistoriesEnd(ClientConnection* connection);
    void sendGuiHeartbeat(uint64_t utc_ts_in_nanos);
    void sendPlaySound(uint32_t id);
    void sendTraderUpdate(const char* trader);
    void sendClients();
    void handleLoginAccepted(proto::GUIInitialization& m, uint64_t heartbeatIntervalInMilliseconds, ClientConnection& connection, const char* trader);
    void handleLoginRejected(int32_t connectionId);
    void handleLoggedOut(ClientConnectionIterT& connIter);
    void checkConnectionHealth();
    const char* const getConnectionUserName(int32_t connectionId) const noexcept;
    uint32_t getNumberConnection() const noexcept;

private:
    int createMonitorSocket();
    void onDecodeMessageError(ClientConnectionIterT& traderConnection, proto::GUI_MESSAGE_TYPE messageRequestType, message_id_t messageID);
    void removeTraderConnection(ClientConnectionIterT& traderConnection);
    void onConnectionError(ClientConnectionIterT& traderConnection);
    void sendOtherRequest(ViperMessageType mt, uint32_t messageID, ClientConnectionIterT& connection);
    bool hasConnection() const noexcept;

    struct epoll_event events[100]{};

    int32_t m_port;
    std::string m_host;

    struct {
        int port = 0;
        int epollFD = -1;
        int listenFD = -1;
    } m_monitor;

    FrontSide* _frontSide;
    double _frequency;
    uint32_t _timeOutSeconds = 15;
};

}

#endif //VIPER_CLIENT_ACCEPTOR_H

