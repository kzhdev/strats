//
// Created by pkusch on 12/26/16.
//

#ifndef RHONEOMS_SPREADERCLIENTACCEPTOR_H
#define RHONEOMS_SPREADERCLIENTACCEPTOR_H


//
// Created by azhitlenok on 3/29/16.
//

#ifndef RHONESC_CLIENTSACCEPTOR_H
#define RHONESC_CLIENTSACCEPTOR_H

#include <cstdint>
#include <sys/epoll.h>
#include <strat/Context.hpp>
#include "messages/omsproto.pb.h"
#include "ReadBuffer.h"
#include "ServerCommon.h"
#include "SpreaderQueues.h"
#include "AlgoManager.h"

namespace oms { namespace spreader  {

class SpreaderConnection {
public:
    SpreaderConnection(int fd) : m_fd{fd} {}

    ReadBuffer& getReadBuffer() { return m_readBuffer; }
    virtual bool isDisconnected() const { return m_disconnected; }
    void setClientDisconnected(bool disconnected) { m_disconnected = disconnected; }
    bool isClosing() const { return m_closingMessageID > 0; }
    bool isLoggedIn() const { return _loggedIn; }
    std::string& getUserName() { return _userName; }
    void onLogin(const std::string& userName) { _userName = userName; _loggedIn = true; }
    int32_t getClosingMessageID() const { return m_closingMessageID; }
    int getFileDescriptor() const { return m_fd; }
    void setClosingMessageID(int32_t messageID) { m_closingMessageID = messageID; }

    bool canExecuteRequest() const
    {
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

class SpreaderClientsAcceptor {

public:
    SpreaderClientsAcceptor(std:: string srvrName, int32_t port)
            :
            m_port(port),
            m_host(srvrName)
    {}

    void createMonitor();
    void runIteration();
    void initialize(SpreaderQueue* queue, SpreaderType spreaderType, std::string& netIf, std::string& host, int port);
    void setAlgoManager(AlgoManager<strat::Context>* manager) { _algoManager = manager; }
    void handleEndOfTrading(SpreadOrderReply& reply, SpreadMessageType type);
    void sendSpreadPrice(SpreadPrice& price);
    void sendSpreadStatus(uint32_t messageId, int id, SpreadMessageType type, ErrorCode errorCode, const std::string& errorMessage);
    void sendLegOrderStatus(IndividualLegOrderStatus& reply, hf::symbol_gid_t gid);
    SpreaderQueue* getInQueue() { return _inQueue; }
    void sendGatewayStatus(hf::VENUE venue, om::port_id_t id, bool active);
    void sendInstrumentMdStatus(hf::VENUE venue, hf::symbol_gid_t sid, bool status);
    void sendLegsCrossInfo(LegsCrossInfo& info);
    void sendCoverOrder(int original, int cover, hf::price_t price);
    void sendSMATheo(int id, hf::price_t price);
    void sendSpreadOrderPrice(int id, hf::price_t price);
    void sendFillToRemoteSpreader(int id, hf::symbol_gid_t gid, hf::qty_t quantity);
    bool checkForMessagesFromSlave();
    void forwardSpreadToSlave(int id);
    void onSlaveConnectionError();
    void forwardUnsolicitedPauseToSlave(int id);

#ifdef CLIENT_CONNECTION_TEST
    void clientConnectionTest();
        int32_t runEx();
        void stop();
        bool isExit() const;
#endif

private:
    int createMonitorSocket();
    void onDecodeMessageError(SpreaderConnection& traderConnection, oms::proto::REQUEST_TYPE messageRequestType, message_id_t messageID);
    void removeTraderConnection(SpreaderConnection& traderConnection);
    void onConnectionError(SpreaderConnection& traderConnection);
    void processOtherRequests(SpreaderConnection& connection, uint32_t messageID, const std::string& spreadId, SpreadMessageType mt);
    void sendOtherRequest(SpreadMessageType mt, int orderId, uint32_t messageID);
    proto::SPREAD_ORDER_STATUS getSpreadOrderStatus(SpreadMessageType type, ErrorCode errorCode);
    proto::ORDER_ACTION getOrderAction(LegOrderAction action);
    proto::OMS_ERROR_CODE getSpreaderOMSErrorCode(ErrorCode code);
    proto::SPREAD_ORDER_STATUS getReplyStatusFromRequest(SpreadMessageType messageRequestType);

    struct epoll_event events[100]{};

    int32_t m_port;
    std::string m_host;

    struct {
        //
        int port = 0;
        //
        int epollFD = -1;
        int listenFD = -1;

        std::map<int, std::shared_ptr<SpreaderConnection>> m_traderConnections;

    } m_monitor;

    int _slaveFD = -1;
    SpreaderConnection _slaveConnection{1};
    ReadBuffer _slaveReadBuffer;
    std::map<std::string, proto::NewSpreadOrder> _spreadOrders;

    AlgoManager<strat::Context>* _algoManager;
    SpreaderQueue* _inQueue;
    std::map<std::string, int> _orderIdMappingsByClient;
    std::map<int, std::string> _orderIdMappingsByServer;
    int _orderId = 0;
    SpreaderType _spreaderType;

#ifdef CLIENT_CONNECTION_TEST
    bool m_exit = false;
#endif

    SpreadMessageType getSpreadMessageType(proto::REQUEST_TYPE type);
};

#ifdef CLIENT_CONNECTION_TEST
inline void SpreaderClientsAcceptor::clientConnectionTest() {
        createMonitor();
        runEx();
    }

    inline bool SpreaderClientsAcceptor::isExit() const {
        return m_exit;
    }

    inline void SpreaderClientsAcceptor::stop() {
        m_exit = true;
    }
#endif
}}

#endif //RHONESC_CLIENTSACCEPTOR_H



#endif //RHONEOMS_SPREADERCLIENTACCEPTOR_H
