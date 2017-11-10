//
// Created by pkusch on 3/7/17.
//

#ifndef RHONEOMSTEST_TESTSPREADERCLIENTSACCEPTOR_H
#define RHONEOMSTEST_TESTSPREADERCLIENTSACCEPTOR_H

#include "../../oms/AlgoManager.h"
#include "TestStrategy.h"

namespace oms { namespace spreader {


struct CoverOrderInfo {
    int _original;
    int _cover;

    void reset() {
        _original = -1;
        _cover = -1;
    }
};

class TestSpreaderClientsAcceptor {
public:
    TestSpreaderClientsAcceptor(std:: string srvrName, int32_t port) {}
    void createMonitor() {}
    void setAlgoManager(AlgoManager<TestStrategy::Context>* manager) {}
    void initialize(SpreaderQueue* queue, SpreaderType spreaderType, std::string& netIf, std::string& host, int port) {}
    void runIteration() {}
    void handleEndOfTrading(SpreadOrderReply& reply, SpreadMessageType type) {}
    void sendSpreadPrice(SpreadPrice& price) {}
    void sendSpreadStatus(uint32_t messageId, int id, SpreadMessageType type, ErrorCode errorCode, const std::string& errorMessage) {}
    void sendLegOrderStatus(IndividualLegOrderStatus& reply, hf::symbol_gid_t gid) {}
    SpreaderQueue* getInQueue() { return nullptr; }
    void sendGatewayStatus(hf::VENUE venue, om::port_id_t id, bool active) {}
    void sendInstrumentMdStatus(hf::VENUE venue, hf::symbol_gid_t sid, bool status) {}
    void sendLegsCrossInfo(LegsCrossInfo& info)  {}
    void sendSpreadOrderPrice(int id, hf::price_t theo) {}

    void sendCoverOrder(int original, int cover, hf::price_t price) {
        _coverOrder._original = original;
        _coverOrder._cover = cover;
    }

    void sendSMATheo(int id, hf::price_t) {}

    bool checkForMessagesFromSlave() { return true; }
    void sendFillToRemoteSpreader(int id, hf::symbol_gid_t gid, hf::qty_t quantity) {}
    void forwardSpreadToSlave(int) {}
    void onSlaveConnectionError() {}
    void forwardUnsolicitedPauseToSlave(int id) {}

    CoverOrderInfo _coverOrder;
};

}}

#endif //RHONEOMSTEST_TESTSPREADERCLIENTSACCEPTOR_H
