//
// Created by pkusch on 12/21/16.
//

#ifndef RHONEOMS_SPREADER_H
#define RHONEOMS_SPREADER_H

#include <hf/hf.h>
#include <md/md_event.h>
#include <map>
#include <om/om.h>
#include <om/om_event.h>
#include <vector>
#include <cstring>
#include <strat/Strategy.hpp>
#include <hf/log.h>
#include <hf/ResourceManager.h>
#include <md/md.h>
#include <om/om_reject_codes.h>
#include <hf/ConfigManager.h>
#include "SpreaderQueues.h"
#include "SpreaderFrontSide.h"
#include "ClockUtil.h"
#include "EnLegSpread.h"
#include "SpreaderClientAcceptor.h"

namespace oms { namespace spreader  {

template <typename StrategyT, int nLegs, typename SpreaderClientsAcceptorT, typename SymbolsResourceT>
class SyntheticSpreadStrategy : public StrategyT {

public:

    typedef EnLegSpread<SyntheticSpreadStrategy> SyntheticSpreadT;

    enum class State {Running, LoggingOut, OnClientError, NotRunning};

    struct MarketData {
        MarketData() {
            _subscriptions.reserve(10);
        }

        Books _books;
        std::vector<SyntheticSpreadT*> _subscriptions;
    };

    struct Gateway {
        om::port_id_t _id = om::NULL_PORTID;
        bool _portStatus = false;
        bool _portMonitoring = true;
        hf::VENUE _venue;

        bool isGatewayUp() {
            return _portStatus && _portMonitoring;
        }
    };

    SyntheticSpreadStrategy() {
        auto inPool = new SpreaderPool();
        inPool->create();
        _inQueue = new SpreaderQueue();
        _inQueue->create();
        _inQueue->set_pool(inPool);

        auto outPool = new SpreaderPool();
        outPool->create();
        _outQueue = new SpreaderQueue();
        _outQueue->create();
        _outQueue->set_pool(outPool);

        double frequency = clockUtil::GetCpuFrequency();
        _pricePublishPeriodInCycles = static_cast<uint64_t>(frequency * 1.0 * 1000000000.0);
    }

    void onStratBegin() override {
        LOG_INFO(boost::str(boost::format("Starting Spreader Backend. %1%") % this->getName()));
        _inQueue->resetR();
        _outQueue->resetW();

        const auto gatewaysFromProxy = this->context().getGateways();
        LOG_INFO(boost::str(boost::format("Received Gateways %1%.") % gatewaysFromProxy.size()));
        for (auto& proxyGateway : gatewaysFromProxy) {
            Gateway gateway;
            gateway._venue = proxyGateway._venue;
            gateway._id = proxyGateway._id;
            _gateways.push_back(gateway);
        }
    }

    void onStratEnd() override {
        LOG_INFO(boost::str(boost::format("Shutting down Spreader Backend.")));
    }

    void onPostCreate(const libconfig::Setting &config) override {
        const std::string type = config["spreaderType"];
        if (type == "Standalone") {
            _type = SpreaderType::Standalone;
        } else if (type == "Master") {
            _type = SpreaderType::Master;
        } else if (type == "Slave") {
            _type = SpreaderType::Slave;
        } else {
            LOG_FATAL_AND_THROW("Invalid spreader type.");
        }


        std::string netIF;
        std::string slaveHost;
        int slavePort;
        if (_type == SpreaderType::Master) {
            const auto& slave = config["slave"];
            netIF = hf::ConfigManager::getString(slave, "netIF");
            slaveHost = hf::ConfigManager::getString(slave, "host");
            slavePort = hf::ConfigManager::getLong(slave, "port");
        }

        std::string venue = hf::ConfigManager::getString(config, "venue");
        auto venueIt = hf::VENUES.find(venue);
        if (venueIt == hf::VENUES.end()) {
            LOG_FATAL_AND_THROW("Invalid venue type.");
        }
        _venue = venueIt->second;

        const std::string frontName = config["front"];
        auto front = dynamic_cast<SpreaderFrontSide<typename StrategyT::ContextT, SpreaderClientsAcceptorT, SymbolsResourceT> *> (hf::ResourceManager::getResourceP(frontName));
        front->init(_inQueue, _outQueue, &this->context(), _type, netIF, slaveHost, slavePort, _venue);
    }

    void sendLegOrderStatus(int id, hf::symbol_id_t sid, hf::SIDE side, hf::price_t price, hf::qty_t qty, om::order_id_t oid, LegOrderAction action, bool rejected = false) {
        _LOG_INFO(boost::str(boost::format("Spreader Leg status. Sid=%1%  OID=%2% Qty=%3% Price=%4% Action=%5%") % sid % oid % qty % price % (int) action));

        auto &di = createKDBLog();
        di.ch[0] = 'L'; di.ch[1] = 'S';
        di.i[0] = id;
        di.i[1] = sid;
        di.i[2] = oid;
        di.i[3] = qty;
        di.i[4] = price;
        di.i[5] = (int) action;
        di.i[6] = sid;

        auto &outMessage = _outQueue->pool()->getNextMessage();
        outMessage._type = SpreadMessageType::LegOrderStatus;

        IndividualLegOrderStatus& orderStatus = reinterpret_cast<IndividualLegOrderStatus &> (outMessage._data[0]);
        orderStatus._oid = oid;
        orderStatus._sid = sid;
        orderStatus._spreadId = id;
        orderStatus._action = action;
        orderStatus._price = price;
        orderStatus._qty = qty;
        orderStatus._side = side;
        orderStatus._rejected = rejected;

        const auto outIdx = _outQueue->pool()->append(sizeof(IndividualLegOrderStatus) + sizeof(SpreadMessageType));
        mb();
        _outQueue->appendNew(outIdx);
    }

    void sendInstrumentMarketDataStatus(hf::symbol_id_t sid, hf::VENUE venue, bool s) {
        _LOG_INFO(boost::str(boost::format("Instrument md status. Sid=%1% Status=%2%") % sid % s));

        auto &di = createKDBLog();
        di.ch[0] = 'M'; di.ch[1] = 'D'; di.ch[2] = 'S';
        di.i[0] = sid;
        di.i[1] = s;
        di.i[2] = sid;

        auto &outMessage = _outQueue->pool()->getNextMessage();
        outMessage._type = SpreadMessageType::InstrumentStatus ;

        InstrumentMdStatus& status = reinterpret_cast<InstrumentMdStatus &> (outMessage._data[0]);
        status._sid = sid;
        status._venue = venue;
        status._status = s;

        const auto outIdx = _outQueue->pool()->append(sizeof(InstrumentMdStatus) + sizeof(SpreadMessageType));
        mb();
        _outQueue->appendNew(outIdx);
    }

    void sendGatewayStatus(bool active, hf::VENUE venue, om::port_id_t id) {
        auto &outMessage = _outQueue->pool()->getNextMessage();
        outMessage._type = SpreadMessageType::GatewayStatus ;

        OMStatus& status = reinterpret_cast<OMStatus &> (outMessage._data[0]);
        status._active = active;
        status._venue = venue;
        status._id = id;

        const auto outIdx = _outQueue->pool()->append(sizeof(OMStatus) + sizeof(SpreadMessageType));
        mb();
        _outQueue->appendNew(outIdx);

        LOG_INFO(boost::str(boost::format("Gateway status. Active=%1%") % status._active));
    }

    void sendGatewayStatus() {
        for (uint32_t i = 0; i < _gateways.size(); ++i) {
            auto& gw = _gateways[i];
            sendGatewayStatus(gw.isGatewayUp(), gw._venue, i);
        }
    }

    void sendSpreadOrderReply(SpreadMessageType replyType, ErrorCode errorCode, uint32_t messageId, int id, const std::string& errorMessage = "") {
        auto &outMessage = _outQueue->pool()->getNextMessage();
        outMessage._type = replyType;

        SpreadOrderReply& reply = reinterpret_cast<SpreadOrderReply &> (outMessage._data[0]);
        reply._errorCode = errorCode;
        reply._messageId = messageId;
        reply._id = id;

        if (!errorMessage.empty()) {
            strcpy(reply._errorMessage, errorMessage.c_str());
        } else {
            reply._errorMessage[0] = 0;
        }

        const auto outIdx = _outQueue->pool()->append(sizeof(SpreadOrderReply) + sizeof(SpreadMessageType));
        mb();
        _outQueue->appendNew(outIdx);
    }

    void sendClipDone(int id, bool spreadDone, hf::price_t price) {
        _LOG_INFO(boost::str(boost::format("Sent clip done. Id=%1% SpreadDone=%2%") % id % spreadDone));

        auto &di = createKDBLog();
        di.ch[0] = 'C'; di.ch[1] = 'D';
        di.i[0] = id;
        di.i[1] = spreadDone;
        di.i[2] = price;

        auto &outMessage = _outQueue->pool()->getNextMessage();
        outMessage._type = SpreadMessageType::ClipDone;

        ClipDone& reply = reinterpret_cast<ClipDone &> (outMessage._data[0]);
        reply._id = id;
        reply._spreadDone = spreadDone;
        reply._price = price;

        const auto outIdx = _outQueue->pool()->append(sizeof(ClipDone) + sizeof(SpreadMessageType));
        mb();
        _outQueue->appendNew(outIdx);
    }

    void sendFillToRemoteSpreader(int spreadId, Leg& leg) {
        _LOG_INFO(boost::str(boost::format("Sent fill to remote spreader. Id=%1% SID=%2% Filled=%3%") % spreadId % leg._sid % leg._filledQuantity));

        auto &outMessage = _outQueue->pool()->getNextMessage();
        outMessage._type = SpreadMessageType::RemoteFill;

        RemoteFill& remoteFill = reinterpret_cast<RemoteFill &> (outMessage._data[0]);
        remoteFill._spreadId = spreadId;
        remoteFill._gid = leg._definition._gid;
        remoteFill._filledQty = leg._filledQuantity;

        const auto outIdx = _outQueue->pool()->append(sizeof(RemoteFill) + sizeof(SpreadMessageType));
        mb();
        _outQueue->appendNew(outIdx);
    }

    virtual void onStratIdle() override {
        processIncomingMessages();
        sendSpreadPrices();
    }

    void sendSpreadPrices() {
        if (isTimeToSendPrices()) {
            for (auto& pair : _spreadsByOrderId) {
                SyntheticSpreadT* spread = pair.second;
                if (!spread->isDone()) {
                    auto &outMessage = _outQueue->pool()->getNextMessage();
                    outMessage._type = SpreadMessageType::Price;

                    SpreadPrice &reply = reinterpret_cast<SpreadPrice &> (outMessage._data[0]);
                    reply._id = spread->_id;
                    reply._bid = spread->getBidPrice();
                    reply._ask = spread->getAskPrice();

                    const auto outIdx = _outQueue->pool()->append(sizeof(SpreadPrice) + sizeof(SpreadMessageType));
                    mb();
                    _outQueue->appendNew(outIdx);
                }
            }
        }
    }

    bool isTimeToSendPrices() {
        const uint64_t now = clockUtil::GetCycles();
        const bool isTime = now > _lastTimePricePublished;
        if (isTime) {
            _lastTimePricePublished = now + _pricePublishPeriodInCycles;
        }
        return isTime;
    }

    void sendLegsCrossInfo(LegsCrossed& info, int id) {
        _LOG_INFO(boost::str(boost::format("Sent legs cross info. Id=%1% Crossed=%2%") % id % info._crossedForQuotingLegs));

        auto &outMessage = _outQueue->pool()->getNextMessage();
        outMessage._type = SpreadMessageType::LegsCrossInfo ;

        LegsCrossInfo& message = reinterpret_cast<LegsCrossInfo &> (outMessage._data[0]);
        message._id = id;
        message._nLegs = info._legs.size();
        int offset = 0;
        for (LegCross& cross : info._legs) {
            LegCross& outCross = reinterpret_cast<LegCross &> (message._data[offset]);
            outCross._gid = cross._gid;
            outCross._venue = cross._venue;
            outCross._quotePrice = cross._quotePrice;
            outCross._quoteCross = cross._quoteCross;
            outCross._hedgePrice = cross._hedgePrice;
            offset += sizeof(LegCross);

            _LOG_INFO(boost::str(boost::format("Leg cross. Id=%1% Crossed=%2% Quote=%3% Hedge=%4%") % cross._gid % cross._quoteCross % cross._quotePrice % cross._hedgePrice));

            auto &di = createKDBLog();
            di.ch[0] = 'L'; di.ch[1] = 'C';
            di.i[0] = id;
            di.i[1] = cross._gid;
            di.i[2] = cross._quoteCross;
            di.i[3] = cross._quotePrice;
            di.i[2] = cross._hedgePrice;
        }

        const auto outIdx = _outQueue->pool()->append(sizeof(LegsCrossInfo) + offset + sizeof(SpreadMessageType));
        mb();
        _outQueue->appendNew(outIdx);
    }

    void deleteSpread(SyntheticSpreadT* spread) {
        delete spread;
    }

    void startSpread(SyntheticSpreadT* spread) {
        spread->subscribeAll();
        spread->_state = SyntheticSpreadT::State::Running;
        spread->loadLotSize();
        spread->startQuoting();
    }

    void processIncomingMessages() {
        while(true) {
            uint64_t idx = _inQueue->getNext();

            if (BOOST_UNLIKELY(idx == 0))
                return;

            _inQueue->freeNext();
            auto &message = _inQueue->pool()->getMessage(idx);

            switch (message._type) {
                case SpreadMessageType::NewSpreadOrder: {
                    Spread &order = reinterpret_cast<Spread &> (message._data[0]);
                    SyntheticSpreadT *spread = nullptr;
                    std::string errorMessage;
                    ErrorCode errorCode = createSyntheticSpreadOrder(order, spread, errorMessage);
                    if (errorCode != ErrorCode::Success) {
                        sendSpreadOrderReply(SpreadMessageType::NewSpreadOrderReply, errorCode, order._messageId, order._order._clientOrderId, errorMessage);
                    } else {
                        LegsCrossed info = spread->getLegsCrossInfo();
                        if (order._order._test) {
                            sendLegsCrossInfo(info, order._order._clientOrderId);
                            deleteSpread(spread);
                        } else {
                            if (info._crossedForQuotingLegs && !order._order._skipCrossTest && _type != SpreaderType::Slave) {
                                sendLegsCrossInfo(info, order._order._clientOrderId);
                            } else {
                                if (_type != SpreaderType::Master) {
                                    sendSpreadOrderReply(SpreadMessageType::NewSpreadOrderReply, errorCode, order._messageId, order._order._clientOrderId);
                                    startSpread(spread);
                                } else {
                                    sendSpreadOrderReply(SpreadMessageType::ForwardSpreadToSlave, errorCode, order._messageId, order._order._clientOrderId);
                                }
                            }
                        }
                    }
                    return;
                }
                case SpreadMessageType::ModifySpreadOrder: {
                    Spread& order = reinterpret_cast<Spread &> (message._data[0]);
                    ErrorCode errorCode = updateSpreadOrder(order);
                    sendSpreadOrderReply(SpreadMessageType::ModifySpreadOrderReply, errorCode, 0, order._order._clientOrderId);
                    break;
                }
                case SpreadMessageType::CancelSpreadOrder: {
                    ClientOrderId &orderId = reinterpret_cast<ClientOrderId &> (message._data[0]);
                    bool sendReply = false;
                    ErrorCode errorCode = cancelSpreadOrder(orderId._id, orderId._messageId, sendReply);
                    if (sendReply) {
                        sendSpreadOrderReply(SpreadMessageType::CancelSpreadOrderReply, errorCode, orderId._messageId, orderId._id);
                    }
                    return;
                }
                case SpreadMessageType::UnsolicitedSpreadPause:
                case SpreadMessageType::PauseSpreadOrder: {
                    ClientOrderId &orderId = reinterpret_cast<ClientOrderId &> (message._data[0]);
                    bool sendReply = false;
                    ErrorCode errorCode = pauseSpreadOrder(orderId._id, orderId._messageId, sendReply);
                    if (sendReply) {
                        sendSpreadOrderReply(SpreadMessageType::PauseSpreadOrderReply, errorCode, orderId._messageId, orderId._id);
                    }
                    return;
                }
                case SpreadMessageType::ResumeSpreadOrder: {
                    ClientOrderId &orderId = reinterpret_cast<ClientOrderId &> (message._data[0]);
                    ErrorCode errorCode = resumeSpreadOrder(orderId._id);
                    sendSpreadOrderReply(SpreadMessageType::ResumeSpreadOrderReply, errorCode, orderId._messageId, orderId._id);
                    return;
                }
                case SpreadMessageType::AcceptCrossedOrder: {
                    ClientOrderId &orderId = reinterpret_cast<ClientOrderId &> (message._data[0]);
                    if (_type == SpreaderType::Master) {
                        sendSpreadOrderReply(SpreadMessageType::ForwardSpreadToSlave, ErrorCode::Success, 0, orderId._id);
                    } else {
                        ErrorCode errorCode = acceptCrossedSpreadOrder(orderId._id);
                        sendSpreadOrderReply(SpreadMessageType::AcceptCrossedOrderReply, errorCode, orderId._messageId, orderId._id);
                    }
                    return;
                }
                case SpreadMessageType::DiscardCrossedOrder: {
                    ClientOrderId &orderId = reinterpret_cast<ClientOrderId &> (message._data[0]);
                    ErrorCode errorCode = discardCrossedSpreadOrder(orderId._id);
                    sendSpreadOrderReply(SpreadMessageType::DiscardCrossedOrderReply, errorCode, orderId._messageId, orderId._id);
                    return;
                }
                case SpreadMessageType::Login: {
                    handleLogin();
                    sendSpreadOrderReply(SpreadMessageType::LoginReply, ErrorCode::Success, 0, 0);
                    sendGatewayStatus();
                    return;
                }
                case SpreadMessageType::Logout: {
                    ClientOrderId &orderId = reinterpret_cast<ClientOrderId &> (message._data[0]);
                    bool sendReply = false;
                    handleLogout(orderId._messageId, sendReply);
                    if (sendReply) {
                        clearState();
                        sendSpreadOrderReply(SpreadMessageType::LogoutReply, ErrorCode::Success, orderId._messageId, 0);
                    }
                    return;
                }
                case SpreadMessageType::ClientDown: {
                    ClientOrderId &orderId = reinterpret_cast<ClientOrderId &> (message._data[0]);
                    bool sendReply = false;
                    handleClientDown(orderId._messageId, sendReply);
                    if (sendReply) {
                        clearState();
                        sendSpreadOrderReply(SpreadMessageType::OnClientErrorReply, ErrorCode::Success, orderId._messageId, 0);
                    }
                    return;
                }
                case SpreadMessageType::IncrementAllowedNumberOfClips: {
                    ClientOrderId &spreadId = reinterpret_cast<ClientOrderId &> (message._data[0]);
                    incrementAllowedNumberOfClips(spreadId._id);
                    break;
                }
                case SpreadMessageType::ChangeSpreadPriceBy: {
                    IntUpdate &priceChange = reinterpret_cast<IntUpdate &> (message._data[0]);
                    changeSpreadPriceBy(priceChange._id, priceChange._update);
                    break;
                }
                case SpreadMessageType::SetSpreadPrice: {
                    IntUpdate &priceChange = reinterpret_cast<IntUpdate &> (message._data[0]);
                    setSpreadPrice(priceChange._id, priceChange._update);
                    break;
                }
                case SpreadMessageType::NumberOfClipsToAllowedClips: {
                    IntUpdate &priceChange = reinterpret_cast<IntUpdate &> (message._data[0]);
                    setNumberOfClipsToAllowedClips(priceChange._id);
                    break;
                }
                case SpreadMessageType::RemoteFill: {
                    RemoteFill& fill = reinterpret_cast<RemoteFill &> (message._data[0]);
                    processRemoteFill(fill);
                    //TODO if error then pause spreads and notify client
                   break;
                }
                default:
                    break;
            }
        }
    }

    ErrorCode processRemoteFill(RemoteFill& fill) {
        auto it = _spreadsByOrderId.find(fill._spreadId);
        if (it != _spreadsByOrderId.end()) {
            bool spreadDone = false;
            SyntheticSpreadT& spread = *it->second;
            spread.onRemoteFill(fill._gid, fill._filledQty, spreadDone);
            if (spreadDone) {
                sendSpreadOrderReply(SpreadMessageType::SpreadFinished, ErrorCode::Success, 0, spread._id);
            }
        }
        return ErrorCode::SpreadNotFound;
    }

    void handleLogin() {
        _state = State::Running;
    }

    void handleLogout(uint32_t messageId, bool& sendReply) {
        LOG_INFO(boost::str(boost::format("Logging out.")));
        _state = State::LoggingOut;
        cancelAllSpreads(messageId, sendReply);
    }

    void handleClientDown(uint32_t messageId, bool& sendReply) {
        LOG_INFO(boost::str(boost::format("Handling client down.")));
        _state = State::OnClientError;
        cancelAllSpreads(messageId, sendReply);
    }

    void cancelAllSpreads(uint32_t messageId, bool& sendReply) {
        bool allSpreadsDone = true;
        for (auto& pair : _spreadsByOrderId) {
            SyntheticSpreadT& spread = *pair.second;
            if (!spread.isDone()) {
                bool oneSpreadDone = false;
                ErrorCode errorCode = spread.cancel(messageId, oneSpreadDone);
                allSpreadsDone &= oneSpreadDone;
                if (errorCode == ErrorCode::Success && oneSpreadDone) {
                    sendCancelPauseReply(&spread);
                }
            }
        }
        if (allSpreadsDone) {
            _state = State::NotRunning;
        }
        sendReply = allSpreadsDone;
    }

    ErrorCode createSyntheticSpreadOrder(Spread& order, SyntheticSpreadT*& spreadOut, std::string &errorMessage) {
        auto it = _spreadsByOrderId.find(order._order._clientOrderId);

        if (it != _spreadsByOrderId.end()) {
            _LOG_INFO(boost::str(boost::format("Rejecting new spread. Duplicate order id. Id=%1%") % order._order._clientOrderId));

            auto &di = createKDBLog();
            di.ch[0] = 'S'; di.ch[1] = 'R';
            di.i[0] = order._order._clientOrderId;

            return ErrorCode::DuplicateOrderId;
        }

        bool isFor2Legs = nLegs == 2;

        SyntheticSpreadT *spread = new SyntheticSpreadT();
        spread->_executor = this;
        ErrorCode errorCode = PopulateSpread(order, this->context(), isFor2Legs, spread, _venue, _type, errorMessage);
        if (errorCode != ErrorCode::Success)
            return errorCode;

        initializeLoggingInfo(spread);

        if (!order._order._test) {
            _spreadsByOrderId.insert(std::pair<int, SyntheticSpreadT *>(spread->_id, spread));
            _LOG_INFO(boost::str(boost::format("Created spread. SpreadId=%1%") % spread->_id));

            auto &di = createKDBLog();
            di.ch[0] = 'S'; di.ch[1] = 'C';
            di.i[0] = spread->_id;
        }

        spreadOut = spread;

        return ErrorCode::Success;
    }

    static const uint32_t N_SUBSCRIBED_LEVELS = 5;

    void createMarketData(hf::symbol_id_t sid, Leg& leg) {
        MarketData* marketData = nullptr;
        auto it = _marketDataSubscriptions.find(sid);
        if (it == _marketDataSubscriptions.end()) {
            marketData = new MarketData();
            marketData->_books._subscribedLevels = N_SUBSCRIBED_LEVELS;
            _marketDataSubscriptions.insert(std::pair<hf::symbol_id_t, MarketData*>(sid, marketData));
            const md::symbol_info_t& topLevel = this->context().getSymbolTop(leg._venue, leg._sid);

            auto& bidBook = marketData->_books._bidBook;
            bidBook.l[0].pr = topLevel.bid.top.pr;
            bidBook.l[0].q = topLevel.bid.top.q;
            bidBook.l[1].pr = hf::NULL_PRICE;
            bidBook.l[2].pr = hf::NULL_PRICE;
            bidBook.l[3].pr = hf::NULL_PRICE;
            bidBook.l[4].pr = hf::NULL_PRICE;

            auto& askBook = marketData->_books._askBook;
            askBook.l[0].pr = topLevel.ask.top.pr;
            askBook.l[0].q = topLevel.ask.top.q;
            askBook.l[1].pr = hf::NULL_PRICE;
            askBook.l[2].pr = hf::NULL_PRICE;
            askBook.l[3].pr = hf::NULL_PRICE;
            askBook.l[4].pr = hf::NULL_PRICE;
        } else {
            marketData = it->second;
        }
        leg.setBooks(&marketData->_books);
    }

    void subscribeAll(SyntheticSpreadT* spread, Leg& leg) {
        this->context().subscribeMD(leg._venue, leg._sid, N_SUBSCRIBED_LEVELS, true, true);
        this->context().subscribeOM(leg._sid, true, true, true, true, true, true, true, true);
        auto it = _marketDataSubscriptions.find(leg._sid);
        MarketData* marketData = it->second;
        marketData->_subscriptions.push_back(spread);
    }

    void unsubscribeMD(hf::symbol_id_t sid, SyntheticSpreadT* spread) {
        auto it = _marketDataSubscriptions.find(sid);
        if (it != _marketDataSubscriptions.end()) {
            MarketData& marketData = *it->second;
            auto& spreads = marketData._subscriptions;
            for (auto sit = spreads.begin(); sit != spreads.end(); ++sit) {
                if (*sit == spread) {
                    spreads.erase(sit);
                    break;
                }
            }
        }
    }

    ErrorCode updateSpreadOrder(Spread& spread) {
        auto it = _spreadsByOrderId.find(spread._order._clientOrderId);
        if (it != _spreadsByOrderId.end()) {
            return it->second->updateSpreadOrder(spread);
        }
        return ErrorCode::SpreadNotFound;
    }

    ErrorCode pauseSpreadOrder(int orderId, uint32_t messageId, bool& sendReply) {
        auto it = _spreadsByOrderId.find(orderId);
        if (it != _spreadsByOrderId.end()) {
            return it->second->pause(messageId, sendReply);
        }
        sendReply = true;
        return ErrorCode::SpreadNotFound;
    }

    ErrorCode cancelSpreadOrder(int orderId, uint32_t messageId, bool& sendReply) {
        auto it = _spreadsByOrderId.find(orderId);
        if (it != _spreadsByOrderId.end()) {
            return it->second->cancel(messageId, sendReply);
        }
        sendReply = true;
        return ErrorCode::SpreadNotFound;
    }

    ErrorCode resumeSpreadOrder(int orderId) {
        auto it = _spreadsByOrderId.find(orderId);
        if (it != _spreadsByOrderId.end()) {
            return it->second->resume();
        }
        return ErrorCode::SpreadNotFound;
    }

    ErrorCode acceptCrossedSpreadOrder(int orderId) {
        auto it = _spreadsByOrderId.find(orderId);
        if (it != _spreadsByOrderId.end()) {
            startSpread(it->second);
            return ErrorCode::Success;
        }
        return ErrorCode::SpreadNotFound;
    }

    ErrorCode discardCrossedSpreadOrder(int orderId) {
        auto it = _spreadsByOrderId.find(orderId);
        if (it != _spreadsByOrderId.end()) {
            auto spread = it->second;
            deleteSpread(spread);
            _spreadsByOrderId.erase(it);
            return ErrorCode::Success;
        }
        return ErrorCode::SpreadNotFound;
    }

    ErrorCode incrementAllowedNumberOfClips(int spreadId) {
        auto it = _spreadsByOrderId.find(spreadId);
        if (it != _spreadsByOrderId.end()) {
            it->second->incrementAllowedNumberOfClips();
            return ErrorCode::Success;
        }
        return ErrorCode::SpreadNotFound;
    }

    ErrorCode changeSpreadPriceBy(int spreadId, hf::price_t change) {
        auto it = _spreadsByOrderId.find(spreadId);
        if (it != _spreadsByOrderId.end()) {
            it->second->changeSpreadPriceBy(change);
            return ErrorCode::Success;
        }
        return ErrorCode::SpreadNotFound;
    }

    ErrorCode setSpreadPrice(int spreadId, hf::price_t price) {
        auto it = _spreadsByOrderId.find(spreadId);
        if (it != _spreadsByOrderId.end()) {
            it->second->setSpreadPrice(price);
            return ErrorCode::Success;
        }
        return ErrorCode::SpreadNotFound;
    }

    ErrorCode setNumberOfClipsToAllowedClips(int spreadId) {
        auto it = _spreadsByOrderId.find(spreadId);
        if (it != _spreadsByOrderId.end()) {
            bool done = it->second->setNumberOfClipsToAllowedClips();
            if (done) {
                sendSpreadOrderReply(SpreadMessageType::SpreadFinished, ErrorCode::Success, 0, spreadId);
            }
            return ErrorCode::Success;
        }
        return ErrorCode::SpreadNotFound;
    }

    virtual void onMDBatch(const md::MDBatch &batch) override {
        const auto *data = batch.data;
        for (auto i = batch.count; i; --i) {
            const auto &mde = reinterpret_cast<const md::MDEvent &> (*data);
            data += mde.size;
            switch (mde.type) {
                case md::MDEvent::TYPE::UPDATE: {
                    const auto &event = static_cast<const md::MDUpdateEvent &> (mde);
                    onMDUpdate(batch.venue, event);
                    break;
                }
                case md::MDEvent::TYPE::TRADE: {
                    const auto &event = static_cast<const md::MDTradeEvent &> (mde);
                    onMDTrade(batch.venue, event);
                    break;
                }
                case md::MDEvent::TYPE::GAP: {
                    const auto &event = static_cast<const md::MDGapEvent &> (mde);
                    onMDGap(batch.venue, event);
                    break;
                }
                case md::MDEvent::TYPE::STATUS: {
                    const auto &event = static_cast<const md::MDStatusEvent &> (mde);
                    onMDStatus(batch.venue, event);
                    break;
                }
                default:
                    break;
            }
        }
    }

    void onMDUpdate(hf::VENUE venue, const md::MDUpdateEvent &event) {
        auto it = _marketDataSubscriptions.find(event.sid);
        if (it != _marketDataSubscriptions.end()) {
            MarketData& marketData = *it->second;
            updateBooks(event, marketData);
            for (auto spread : marketData._subscriptions) {
                spread->onMDUpdate(venue, event);
            }
        }
    }

    void updateBooks(const md::MDUpdateEvent &event, MarketData& marketData) {
        updateBook(marketData._books._bidBook, event.buy.top, event.buy.updatedQtyNum, marketData._books._subscribedLevels);
        updateBook(marketData._books._askBook, event.sell.top, event.sell.updatedQtyNum, marketData._books._subscribedLevels);
    }

    void updateBook(md::top_t& book, const md::top_t& inBook, md::level_index_t maxIndex, md::level_index_t subscribedLevels) {
        if (maxIndex == 0)
            return;

        for (md::level_index_t index = 0; index < subscribedLevels; ++index) {
            copyLevel(book, inBook, index);
        }
    }

    void copyLevel(md::top_t& book, const md::top_t& inBook, md::level_index_t index) {
        auto& level = book.l[index];
        auto& inLevel = inBook.l[index];
        level.pr = inLevel.pr;
        level.q = inLevel.q;
    }

    void onMDTrade(hf::VENUE venue, const md::MDTradeEvent &event) {
    }

    void onMDStatus(hf::VENUE venue, const md::MDStatusEvent &event) {
        auto it = _marketDataSubscriptions.find(event.sid);
        if (it != _marketDataSubscriptions.end()) {
            bool good = event.statusType != md::MDStatusEvent::TYPE::RECOVERING;
            sendInstrumentMarketDataStatus(event.sid, venue, good);
        }
    }

    void onMDGap(hf::VENUE venue, const md::MDGapEvent &event) {
        LOG_ERROR(boost::str(boost::format("MDGapEvent")));
    }

    void onOMGWStatusUpdate(const om::GWStatusUpdate &event) override {
        LOG_ERROR(boost::str(boost::format("GWStatusUpdate - OM down. ")));
        Gateway* gw = getGateway(event.pid);
        if (gw != nullptr) {
            bool oldStatus = gw->isGatewayUp();
            gw->_portMonitoring = event.active;
            handleGatewayStatusChange(oldStatus, *gw);
        }
    }

    Gateway* getGateway(int pid) {
        for (Gateway& g : _gateways) {
            if ((int)g._id == pid) { //AZ700
                return &g;
            }
        }
        return nullptr;
    }

    void onOMPortStatusUpdate(const om::PortStatusUpdate &event) override {
        LOG_INFO(boost::str(boost::format("PortStatusUpdate Pid=%1%") % event.pid));

        auto *pGateway = getGateway(event.pid);
        if (pGateway == nullptr) return; //AZ700
        Gateway &gw = *getGateway(event.pid);
        bool oldStatus = gw.isGatewayUp();
        gw._portStatus = event.active;
        handleGatewayStatusChange(oldStatus, gw);
    }

    void handleGatewayStatusChange(bool oldStatus, Gateway& info) {
        bool newStatus = info.isGatewayUp();
        if (_state != State::NotRunning) {
            if (oldStatus != newStatus) {
                sendGatewayStatus(newStatus, info._venue, info._id);
                if (!newStatus) {
                    clearState();
                    sendSpreadOrderReply(SpreadMessageType::LogoutReply, ErrorCode::Success, 0, 0);
                }
            }
        }
    }

    void onOrderAccepted(const om::OMOrderAcceptedEvent &event, const om::order_t &order) override {
        auto spreadIt = _ordersToSpreads.find(order.oid);
        if (spreadIt != _ordersToSpreads.end()) {
            spreadIt->second->onOrderAccepted(event, order);
            sendLegOrderStatus(spreadIt->second->_id, order.sid, order.side, order.pr, order.qty, order.oid, LegOrderAction::New);
        }
    }

    void onOrderRejected(const om::OMOrderRejectedEvent &event, const om::order_t &order) override {
        auto spreadIt = _ordersToSpreads.find(order.oid);
        if (spreadIt != _ordersToSpreads.end()) {
            spreadIt->second->onOrderRejected(event, order);
            sendLegOrderStatus(spreadIt->second->_id, order.sid, order.side, order.pr, order.qty, order.oid, LegOrderAction::New, true);
            sendCancelPauseReply(spreadIt->second);
        }
    }

    void onOrderReplaced(const om::OMOrderModifiedEvent &event, const om::order_t &order) override {
        auto spreadIt = _ordersToSpreads.find(order.oid);
        if (spreadIt != _ordersToSpreads.end()) {
            spreadIt->second->onOrderReplaced(event, order);
            sendLegOrderStatus(spreadIt->second->_id, order.sid, order.side, event.acceptedPrice, event.acceptedQty, order.oid, LegOrderAction::Modify);
        }
    }

    void onOrderReplaceRejected(const om::OMOrderModifyRejectedEvent &event, const om::order_t &order) override {
        auto spreadIt = _ordersToSpreads.find(order.oid);
        if (spreadIt != _ordersToSpreads.end()) {
            spreadIt->second->onOrderReplaceRejected(event, order);
            sendLegOrderStatus(spreadIt->second->_id, order.sid, order.side, 0, 0, order.oid, LegOrderAction::Modify, true);
            sendCancelPauseReply(spreadIt->second);
        }
    }

    void onOrderCanceled(const om::OMOrderCanceledEvent &event, const om::order_t &order) override {
        auto spreadIt = _ordersToSpreads.find(order.oid);
        if (spreadIt != _ordersToSpreads.end()) {
            SyntheticSpreadT* spread = spreadIt->second;
            sendLegOrderStatus(spread->_id, order.sid, order.side, 0, 0, order.oid, LegOrderAction::Cancel);
            if (event.reasonCodeExt != 0) {
                _LOG_INFO(boost::str(boost::format("Received unsolicited cancel. Will pause the spread if running. Code=%1% Oid=%2%") % event.reasonCodeExt % order.oid));
                spread->onUnsolicitedOrderCancelled(order, event);
                sendCancelPauseReply(spread);
                return;
            }
            spread->onOrderCanceled(event, order);
            sendCancelPauseReply(spread);
            checkForEndOfTrading();
        }
    }

    void onOrderCancelRejected(const om::OMOrderCancelRejectedEvent &event, const om::order_t &order) override {
        auto spreadIt = _ordersToSpreads.find(order.oid);
        if (spreadIt != _ordersToSpreads.end()) {
            SyntheticSpreadT* spread = spreadIt->second;
            sendLegOrderStatus(spread->_id, order.sid, order.side, 0, 0, order.oid, LegOrderAction::Cancel, true);
            const bool isOk = spread->onOrderCancelRejected(event, order);
            sendCancelPauseReply(spread);
            if (!isOk) {
                checkForEndOfTrading();
            }
        }
    }

    om::order_t* newOrder(Leg& leg, hf::price_t price, hf::qty_t qty, SyntheticSpreadT* spread) {
        _LOG_INFO(boost::str(boost::format("New order. Sid=%1% Side=%2% Price=%3% Quantity=%4%") % leg._sid % (int)leg._side % price % qty));
        om::order_t* order = this->context().createOrder(0, om::RISK_TYPE::ENTERING, leg._routingId, leg._sid, om::ORDER_TYPE::LMT, leg._side, price, qty);
        if (order != nullptr) {
            _ordersToSpreads.insert(std::pair<om::order_id_t, SyntheticSpreadT *>(order->oid, spread));
            return order;
        } else {
            _LOG_INFO(boost::str(boost::format("Could not create order. Will stop trading.")));

            auto &di = createKDBLog();
            di.ch[0] = 'O'; di.ch[1] = 'C';
            di.i[0] = spread->_id;
            di.i[1] = leg._sid;

            stopTrading();
            return nullptr;
        }
    }

    void modifyOrder(const om::order_t* order, hf::price_t price, hf::qty_t quantity, SyntheticSpreadT* spread) {
        this->context().modifyOrder(order->oid, price, quantity);
    }

    void cancelOrder(const om::order_t* order, SyntheticSpreadT* spread) {
        this->context().cancelOrder(order->oid);
    }

    void sendCancelPauseReply(SyntheticSpreadT* spread) {
        switch (spread->_state) {
            case SyntheticSpreadT::State::Cancelled:
                sendSpreadOrderReply(SpreadMessageType::CancelSpreadOrderReply, ErrorCode::Success, spread->_messageId, spread->_id);
                spread->_messageId = 0;
                break;
            case SyntheticSpreadT::State::Paused: {
                PauseType &pauseType = spread->getPauseType();
                SpreadMessageType mt = pauseType._isUnsolicited ? SpreadMessageType::UnsolicitedSpreadPause : SpreadMessageType::PauseSpreadOrderReply;
                ErrorCode errorCode = pauseType._isUnsolicited ? ErrorCode::Unsolicited : ErrorCode::Success;
                pauseType._isUnsolicited = false;
                std::string errorMessage = std::to_string(pauseType._code);
                sendSpreadOrderReply(mt, errorCode, spread->_messageId, spread->_id, errorMessage);
                spread->_messageId = 0;
                break;
            }
            default:
                break;
        }
    }

    void onOrderExecution(const om::OMExecutionEvent &event, const om::order_t &order) override {
        auto spreadIt = _ordersToSpreads.find(order.oid);
        if (spreadIt != _ordersToSpreads.end()) {
            SyntheticSpreadT* spread = spreadIt->second;
            const bool done = spread->onOrderExecution(event, order);
            sendLegOrderStatus(spread->_id, order.sid, order.side, event.filledPrice, event.filledQty, order.oid, LegOrderAction::Fill);
            if (done) {
                sendSpreadOrderReply(SpreadMessageType::SpreadFinished, ErrorCode::Success, 0, spread->_id);
            }
        }
    }

    virtual void onOMBatch(const om::OMBatch &batch) override {
        const auto *data = batch.data;
        for (auto i = batch.count; i; --i) {
            const auto &ome = reinterpret_cast<const om::OMEvent &> (*data);
            data += ome.size;
            switch (ome.type) {
                case om::OMEvent::TYPE::OE: {
                    const auto &event = static_cast<const om::OMExecutionEvent &> (ome);
                    const auto &order = this->context().getOrder(event.oid);
                    const_cast<om::order_t &> (order).is_active = event.is_active;
                    const_cast<om::order_t &> (order).executedQty = event.totalExecutedQty;
                    onOrderExecution(event, order);
                    break;
                }
                case om::OMEvent::TYPE::SPREAD_LEG_EXECUTION: {
                    const auto &event = static_cast<const om::OMSpreadLegExecutionEvent &> (ome);
                    const auto &order = this->context().getOrder(event.oid);
                    const_cast<om::order_t &> (order).is_active = event.is_active;
                    onOrderSpreadLegExecution(event, order);
                    break;
                }
                case om::OMEvent::TYPE::OA: {
                    const auto &event = static_cast<const om::OMOrderAcceptedEvent &> (ome);
                    const auto &order = this->context().getOrder(event.oid);
                    onOrderAccepted(event, order);
                    break;
                }
                case om::OMEvent::TYPE::OR: {
                    const auto &event = static_cast<const om::OMOrderRejectedEvent &> (ome);
                    const auto &order = this->context().getOrder(event.oid);
                    //
                    const_cast<om::order_t &> (order).is_active = false;
                    onOrderRejected(event, order);
                    break;
                }
                case om::OMEvent::TYPE::OC: {
                    const auto &event = static_cast<const om::OMOrderCanceledEvent &> (ome);
                    const auto &order = this->context().getOrder(event.oid);
                    //
                    const_cast<om::order_t &> (order).is_active = false;
                    onOrderCanceled(event, order);
                    break;
                }
                case om::OMEvent::TYPE::CR: {
                    const auto &event = static_cast<const om::OMOrderCancelRejectedEvent &> (ome);
                    const auto &order = this->context().getOrder(event.oid);
                    onOrderCancelRejected(event, order);
                    break;
                }
                case om::OMEvent::TYPE::MASS_CANCEL_ACCEPTED: {
                    const auto &event = static_cast<const om::OMMassCancelAcceptedEvent &> (ome);
                    onMassCancelAccepted(event);
                    break;
                }
                case om::OMEvent::TYPE::MASS_CANCEL_REJECTED: {
                    const auto &event = static_cast<const om::OMMassCancelRejectedEvent &> (ome);
                    onMassCancelRejected(event);
                    break;
                }
                case om::OMEvent::TYPE::OM: {
                    const auto &event = static_cast<const om::OMOrderModifiedEvent &> (ome);
                    const auto &order = this->context().getOrder(event.oid);
                    const_cast<om::order_t &>(order).is_modifying = false;
                    onOrderReplaced(event, order);
                    break;
                }
                case om::OMEvent::TYPE::MR: {
                    const auto &event = static_cast<const om::OMOrderModifyRejectedEvent &> (ome);
                    const auto &order = this->context().getOrder(event.oid);
                    const_cast<om::order_t &>(order).is_modifying = false;
                    onOrderReplaceRejected(event, order);
                    break;
                }
                case om::OMEvent::TYPE::PORT_STATUS: {
                    const auto &event = static_cast<const om::PortStatusUpdate &> (ome);
                    onOMPortStatusUpdate(event);
                    break;
                }
                case om::OMEvent::TYPE::GW_STATUS: {
                    const auto &event = static_cast<const om::GWStatusUpdate &> (ome);
                    onOMGWStatusUpdate(event);
                    break;
                }
                default:
                    break;
            }
        }
    }

    virtual void onMassCancelAccepted(const om::OMMassCancelAcceptedEvent &event) {
    }

    virtual void onMassCancelRejected(const om::OMMassCancelRejectedEvent &event) {
    }

    virtual void onOrderSpreadLegExecution(const om::OMSpreadLegExecutionEvent &event, const om::order_t &order) {
    }

    bool areAllSpreadsDone() {
        for (auto& pair : _spreadsByOrderId)
            if (!pair.second->isDone())
                return false;
        return true;
    }

    void clearState() {
        _LOG_INFO(boost::str(boost::format("Clearing state.")));

        for (auto& pair : _spreadsByOrderId)
            delete pair.second;

        for (auto& pair : _marketDataSubscriptions)
            delete pair.second;

        _spreadsByOrderId.clear();
        _ordersToSpreads.clear();
        _marketDataSubscriptions.clear();

        _state = State::NotRunning;
    }

    void checkForEndOfTrading() {
        if (_state == State::LoggingOut || _state == State::OnClientError) {

            if (!areAllSpreadsDone()) {
                LOG_INFO(boost::str(boost::format("All spreads are not done yet.")));
                return;
            }

            SpreadMessageType mt = _state == State::LoggingOut ? SpreadMessageType::LogoutReply : SpreadMessageType::OnClientErrorReply;
            sendSpreadOrderReply(mt, ErrorCode::Success, 0, 0);

            clearState();
        }
    }

    void stopTrading() {
        sendSpreadOrderReply(SpreadMessageType::LogoutReply, ErrorCode::Success, 0, 0);
        clearState();
    }

    om::debug_t& createKDBLog() {
        return this->context().createDebug(_loggingRoutingId, _loggingSID);
    }

    void initializeLoggingInfo(SyntheticSpreadT* spread) {
        if (_loggingRoutingId == om::NULL_ROUTING_ID) {
            for (uint32_t i = 0; i < spread->_nLegs; ++i) {
                Leg& leg = spread->getLegByIndex(i);
                if (!leg._isRemote) {
                    _loggingRoutingId = leg._routingId;
                    _loggingSID = leg._sid;
                    break;
                }
            }
        }
    }

    std::unordered_map<int, SyntheticSpreadT*> _spreadsByOrderId;
    std::unordered_map<hf::symbol_id_t, MarketData*> _marketDataSubscriptions;
    std::unordered_map<om::order_id_t , SyntheticSpreadT*> _ordersToSpreads;
    uint64_t _pricePublishPeriodInCycles;
    uint64_t _lastTimePricePublished = 0;
    State _state = State::NotRunning;
    SpreaderType _type;

    SpreaderQueue* _inQueue;
    SpreaderQueue* _outQueue;

    std::vector<Gateway> _gateways;

    om::routing_id_t _loggingRoutingId = om::NULL_ROUTING_ID;
    om::routing_id_t _loggingSID;

    hf::VENUE _venue;
};

typedef SyntheticSpreadStrategy<::strat::Strategy, 10, SpreaderClientsAcceptor, md::sym::SymbolsResource> EnLegSpreaderStrategy;

}}

#endif //RHONEOMS_SPREADER_H
