#ifndef PMGOORDERPLEXER_H
#define PMGORDERPLEXER_H

#include <om/queues/OMQueuesResource.h>
#include <strat/Strategy.hpp>
#include <md/md_event.h>
#include <boost/asio.hpp>
#include <boost/array.hpp>
#include <boost/function.hpp>
#include <boost/bind.hpp>
#include <map>
#include <thread>
#include <om/orders/OMOrdersResource.h>
#include <hf/ResourceManager.h>
#include <queue>
#include <atomic>
#include "ViperConfig.h"
#include "RingBufferQueue.h"

namespace proxySvc {
LOG_DECLARE();
}

using RESOURCE_MANAGER = hf::ResourceManager;
using OM_ORDERS_RESOURCE = om::orders::OMOrdersResource;

#define EUREX_SELL_BITFIELD 1ul << 62

namespace Python {

inline std::string composeMessage(int8_t msgCode, const std::string& symbol, uint64_t oid, hf::VENUE venue, hf::SIDE side) {
    char outBuff[100];
    char lenBuff[104];
    size_t buffLen = 0;
    if (hf::VENUE::EUREX == venue && hf::SIDE::SELL == side) {
        oid |= EUREX_SELL_BITFIELD;
    }
    buffLen = sprintf(outBuff, "*%i*%s*%lu", msgCode, symbol.c_str(), oid);
    buffLen = sprintf(lenBuff, "%04lu%s", buffLen + 4, outBuff);
    return std::string(lenBuff);
}

inline std::string composeMessage(int8_t msgCode, const std::string& symbol) {
    char outBuff[100];
    char lenBuff[104];
    size_t buffLen = 0;
    buffLen = sprintf(outBuff, "*%i*%s", msgCode, symbol.c_str());
    buffLen = sprintf(lenBuff, "%04lu%s", buffLen + 4, outBuff);
    return std::string(lenBuff);
}

class OrderPlexerSvc : public strat::StrategyT<OrderPlexerSvc> {

    boost::asio::io_service m_io_service;
    std::unique_ptr<boost::asio::ip::tcp::socket> m_socket;
    std::unique_ptr<std::thread> m_thread;
    boost::system::error_code m_errorCode;
    std::unordered_map<om::strategy_order_id_t, uint64_t, om::strategy_order_id_hash_t> m_orders;
    std::string m_userName;
    std::string m_ordPlexIP;
    int64_t m_ordPlexPort;
    bool m_logToFile = false;
    bool m_connectionCompleted = false;

    RingBufferQueue<bool> m_msgQueue {32};
    uint64_t m_msgCursor = 0;

    struct om_queue {
        om::queue::OMReplyQueue *ptrOMReplies = nullptr;
        uint_fast64_t  replyPeekIndex = 0;
    };
    std::vector<om_queue> m_omqueues;
    om::strategy_orders_block_t* m_pmgOrdersPtr = nullptr;
    bool m_socketConnected = false;
    ViperConfig* m_viperConfig = nullptr;

public:

    OrderPlexerSvc() { }
    OrderPlexerSvc(int64_t ordPlexPort) : m_ordPlexPort(ordPlexPort) { }

    ~OrderPlexerSvc() {
        if (m_socket.get()) {
            m_socket->close();
            m_socket.reset();
        }
        if (m_thread.get()) {
            m_io_service.stop();
            if (m_thread->joinable()) {
                m_thread->join();
            }
            m_thread.reset();
        }
    }

    void init(std::string userName, ViperConfig* config) {
        m_userName = std::move(userName);
        m_viperConfig = config;
    }

    void notifyOrderRecovered() noexcept {
        auto index = m_msgQueue.reserve();
        m_msgQueue[index] = true;
        m_msgQueue.publish(index);
    }

    bool isConnected() { return m_socketConnected; }

    bool sendMessage(std::string& msg) {
        boost::system::error_code ec;
        boost::asio::write(*(m_socket.get()), boost::asio::buffer(msg), ec);
        if (!ec) {
            return true;
        }
        return false;
    }

    void onRecovered() {
        using proxySvc::s_log;

        for (const auto& o : *m_pmgOrdersPtr) {
            if (!o.action_flags.is_active) {
                continue;
            }

            auto internalId = m_viperConfig->getInternalId(o.sid);
            if (BOOST_UNLIKELY(internalId == INVALID_INTERNAL_ID)) {
                continue;
            }

            uint64_t exchangeOrderId = 0;
            if (o.venue == hf::VENUE::EUREX) {
                exchangeOrderId = o.marketCorrelationID;
                LOG_INFO(boost::format("OP Eurex orderID= %1% marketCorrelationID= %2% exchangeOrderId=%3%") % o.oid % o.marketCorrelationID  % o.exchangeOrderId.value());
            }
            else {
                exchangeOrderId = o.exchangeOrderId.value();
                LOG_INFO(boost::format("OP Other orderID= %1% marketCorrelationID= %2% exchangeOrderId=%3%") % o.oid % o.marketCorrelationID  % o.exchangeOrderId.value());
            }

            if(exchangeOrderId == om::NULL_ORDERID) continue;

            BOOST_ASSERT(exchangeOrderId != om::NULL_ORDERID);
            std::string msg = composeMessage(20, m_viperConfig->_allContracts[internalId]._name, exchangeOrderId, o.venue, o.side);
            boost::asio::write(*(m_socket.get()), boost::asio::buffer(msg), m_errorCode);
            m_orders.emplace(o.oid, exchangeOrderId);
        }
    }

    void connectSocket(std::string ip, int64_t port, std::string userName, boost::function<void (const boost::system::error_code&)> f) {
        boost::system::error_code ec;
        boost::asio::ip::tcp::endpoint e(boost::asio::ip::address::from_string(ip), port);
        m_socket->async_connect(e, f);
    }

    void socketConnectionTimedOut(const boost::system::error_code& ec) {
        using proxySvc::s_log;
        if (!m_connectionCompleted) {
            LOG_ERROR(boost::format("Connection to %1%:%2% timed out") % m_ordPlexIP % m_ordPlexPort);
            m_socket->close();
        }
    }

    void socketConnectionCompleted(const boost::system::error_code& ec) {
        using proxySvc::s_log;
        m_connectionCompleted = true;
        boost::system::error_code lcl_ec;
        if (!ec) {
            std::string msg = composeMessage(10, m_userName);
            bool sent = sendMessage(msg);
            if (sent) {
                LOG_INFO("OrderPlexer Logon Sent");
                boost::array<char, 128> buf;
                m_socket->read_some(boost::asio::buffer(buf), lcl_ec);
                if (!ec) {
                    std::istringstream resp(buf.data());
                    std::string block;
                    int i = 0;
                    while (std::getline(resp, block, '*')) {
                        if (i == 1) {
                            if (block == "11") {
                                LOG_INFO(boost::format("Connected to OrderPlexer.  Return Message: %1%") % buf.data());
                                m_socketConnected = true;
                                break;
                            }
                            else {
                                LOG_ERROR(boost::format("Failed To Connect.  Return Message: %1%") % buf.data());
                                m_socketConnected = false;
                                break;
                            }
                        } else {
                            ++i;
                        }
                    }
                }
            }
        } else {
            LOG_ERROR(ec.message());
        }
    }

    void onBeforeStart(const libconfig::Setting &config) {
        using proxySvc::s_log;
        {
            //Hook into a Rhone Resource - assumes that Rhone has already brought up the Resource
            const auto name = hf::ConfigManager::getString(config, "pmg-orders");
            auto *const ordersResourcePtr = reinterpret_cast<OM_ORDERS_RESOURCE *> (RESOURCE_MANAGER::getResourceP(name));
            if (!ordersResourcePtr) LOG_FATAL_AND_THROW("Unable to get OMOrders from resource\"pmg-orders\"");
            m_pmgOrdersPtr = &ordersResourcePtr->getStrategyOrders();
        }
        //Get the queues as memory reference
        const auto &queuesConfig = config["om-queues"];
        std::vector<std::string> names;
        for (int i = 0; i < queuesConfig.getLength(); ++i) {
            const libconfig::Setting &queueConfig = queuesConfig[i];

            if (!hf::ConfigManager::getBool(queueConfig, "enabled", true)) {
                continue;
            }

            const auto name = hf::ConfigManager::getString(queueConfig, "name");

            auto *resource = RESOURCE_MANAGER::getResourceP(name);
            auto *const queuesResourcePtr = reinterpret_cast<om::queues::OMQueuesResource*> (resource);
            if (!queuesResourcePtr) LOG_FATAL_AND_THROW("Unable to get OMQueues from resource\"om-queues\"");
            auto *p = &queuesResourcePtr->getReplyQueue();
            om_queue omq;
            omq.ptrOMReplies = p;
            omq.replyPeekIndex = p->peekW();
            m_omqueues.push_back(omq);
        }

        m_ordPlexPort = hf::ConfigManager::getLong(config, "order_plexer_port", -1);
        m_ordPlexIP = hf::ConfigManager::getString(config, "order_plexer_ip", "");

        LOG_INFO(boost::format("Attempting to Log In with %1%@%2%:%3%") % m_userName % m_ordPlexIP % m_ordPlexPort);

        m_socket = std::make_unique<boost::asio::ip::tcp::socket>(m_io_service);
        boost::function<void (const boost::system::error_code& ec)> f = boost::bind(&OrderPlexerSvc::socketConnectionCompleted, this, _1);
        connectSocket(m_ordPlexIP, m_ordPlexPort, m_userName, f);
        m_thread = std::make_unique<std::thread>([this] () {
            m_io_service.run();
            m_io_service.stop();
        });

        m_logToFile = hf::ConfigManager::getBool(config, "logOrdersToFile", false);
        if (m_logToFile){
            LOG_INFO("LOGGING ALL ORDER PLEXER ORDERS TO FILE");
        }
    }

    void onStratEnd() {
    }

    void onStratBegin() {
    }

    void onStratIdle() {
        while (BOOST_UNLIKELY(m_msgQueue.end() != m_msgCursor)) {
            bool& recovered = m_msgQueue[m_msgCursor++];
            if (recovered) {
                onRecovered();
                recovered = false;
            }
        }
        peekOrderEvents();
    }

    void onOMBatchL(const om::OMBatch &batch) {
        using proxySvc::s_log;
        if (!m_socketConnected)
            return;

        const auto *data = batch.data;
        for (auto i = batch.count; i; --i) {
            const auto &ome = *reinterpret_cast<const om::OMEvent *> (data);
            data += ome.size;
            switch (ome.type) {
                case om::OMEvent::TYPE::OA: {
                    const auto &event = static_cast<const om::OMOrderAcceptedEvent &> (ome);
                    const auto &order = m_pmgOrdersPtr->get_order(event.strat_oid);
                    if(BOOST_UNLIKELY(order.ot == om::ORDER_TYPE::FAK)) break;
                    onOrderAccepted(event, order, batch);
                    break;
                }
                case om::OMEvent::TYPE::OM: {
                    const auto &event = static_cast<const om::OMOrderModifiedEvent &> (ome);
                    const auto &order = m_pmgOrdersPtr->get_order(event.strat_oid);
                    if(BOOST_UNLIKELY(order.ot == om::ORDER_TYPE::FAK)) break;
                    onOrderReplaced(event, order, batch);
                    break;
                }
                case om::OMEvent::TYPE::OE: {
                    const auto &event = static_cast<const om::OMExecutionEvent &> (ome);
                    const auto &order = m_pmgOrdersPtr->get_order(event.strat_oid);
                    if(BOOST_UNLIKELY(order.ot == om::ORDER_TYPE::FAK)) break;
                    onOMOrderExecutionBeforeFillArbitration(event, order, batch);
                    break;
                }
                case om::OMEvent::TYPE::OC: {
                    const auto &event = static_cast<const om::OMOrderCanceledEvent &> (ome);
                    const auto &order = m_pmgOrdersPtr->get_order(event.strat_oid);
                    if(BOOST_UNLIKELY(order.ot == om::ORDER_TYPE::FAK)) break;
                    onOrderCanceled(event, order, batch);
                    break;
                }
                default: {
                    // ignore everything else
                    break;
                }
            }
        }
    }

    void peekOrderEvents() {
        for (auto &omq : m_omqueues) {
            auto *q = omq.ptrOMReplies;
            const uint_fast64_t w = q->peekW();
            for (auto i = omq.replyPeekIndex; i != w; q->decrement(i)) {
                auto idx = q->peek(i);
                if (idx == 0) {
                    continue;
                }
                auto *const batchPtr = static_cast<om::OMBatch *> (&q->getPool().getBatch(idx));
                if (batchPtr == nullptr || batchPtr->count == 0) {
                    asm volatile("mfence":::"memory");
                    q->incrementReadCount(idx);
                    continue;
                }
                onOMBatchL(*batchPtr);
                asm volatile("mfence":::"memory");
                q->incrementReadCount(idx);
            }
            omq.replyPeekIndex = w;
        }
    }

    void onPreMDBatch(const md::MDBatch& batch) {}
    void onPostMDBatch(const md::MDBatch& batch) {}
    void onPreOMBatch(const om::OMBatch& batch) {}
    void onPostOMBatch(const om::OMBatch& batch) {}

    void onOrderRecoveryStarted(om::port_gid_t /*port_gid*/, om::instance_id_t /*strategy_instance_id*/) {}
    void onOrderRecoveryFinished(om::port_gid_t /*port_gid*/, om::instance_id_t /*strategy_instance_id*/) {}
    bool willManageRecoveredOrder(const om::strategy_order_t& /*order*/) { return true; }

    void onOMGWStatusUpdate(const om::GWStatusUpdate &event) {};

    void onOrdersTransferredToRisk(const om::OMOrdersTransferredToRiskEvent& transferAck, const om::OMBatch& batch) {};
    void onRecoveredOrders(const om::OMRecoveredOrdersEvent& event, const om::OMBatch& batch) {};
    void onRecoveredFills(const om::OMRecoveredFillsEvent& event, const om::OMBatch& batch) {};
    void onStrategyLoginResponse(const om::StrategyLoginResponseEvent &event, const om::OMBatch& batch) {};
    void onTraderLogInOutResponse(const om::TraderLogInOutResponseEvent &event, const om::OMBatch& batch) {};
    void onOMPortStatusUpdate(const om::PortStatusUpdate &event, const om::OMBatch& batch) {};
    void onOrderAccepted(const om::OMOrderAcceptedEvent &event, const om::strategy_order_t &order, const om::OMBatch& batch) {
        using proxySvc::s_log;
        if (BOOST_UNLIKELY(!m_socketConnected)) {
            return;
        }
        auto internalId = m_viperConfig->getInternalId(event.orderSnapshot.sid);
        if (BOOST_UNLIKELY(internalId == INVALID_INTERNAL_ID)) {
            return;
        }
        uint64_t exchangeOrderId = 0;
        if (event.orderSnapshot.venue == hf::VENUE::EUREX) {
            exchangeOrderId = event.orderSnapshot.marketDataCorrelationID;
        } else {
            exchangeOrderId = event.orderSnapshot.exchangeOrderId.value();
        }
        std::string msg = composeMessage(20, m_viperConfig->_allContracts[internalId]._name, exchangeOrderId, event.orderSnapshot.venue, event.orderSnapshot.side);
        boost::asio::write(*(m_socket.get()), boost::asio::buffer(msg), m_errorCode);
        if (m_logToFile) {
            LOG_INFO(boost::format("OPX::onOA, symbol = %1%, exch_ord_id = %2%, msg = %3%, price = %4%") % m_viperConfig->_allContracts[internalId]._name % exchangeOrderId % msg % event.orderSnapshot.pr);
        }
        auto it = m_orders.find(event.orderSnapshot.oid);
        if (it == m_orders.end()) {
            m_orders.emplace(event.orderSnapshot.oid, exchangeOrderId);
        } else if (m_logToFile) {
            LOG_INFO(boost::format("OPX::onOA, Got an unexpected duplicate. OID: %1%, Exchange ID: %2%") % event.orderSnapshot.oid % exchangeOrderId );
        }
    }
    void onOrderRejected(const om::OMOrderRejectedEvent &event, const om::strategy_order_t &order, const om::OMBatch& batch) {}
    void onOrderReplaced(const om::OMOrderModifiedEvent &event, const om::strategy_order_t &order, const om::OMBatch& batch) {
        using proxySvc::s_log;
        if (BOOST_UNLIKELY(!m_socketConnected)) {
            return;
        }

        auto internalId = m_viperConfig->getInternalId(event.orderSnapshot.sid);
        if (BOOST_UNLIKELY(internalId == INVALID_INTERNAL_ID)) {
            return;
        }
        uint64_t exchangeOrderId = 0;
        if (event.orderSnapshot.venue == hf::VENUE::EUREX) {
            exchangeOrderId = event.orderSnapshot.marketDataCorrelationID;
        } else if (event.orderSnapshot.venue == hf::VENUE::ICE) {
            exchangeOrderId = event.orderSnapshot.exchangeOrderId.value();
        }
        if (exchangeOrderId > 0) {
            auto it = m_orders.find(event.orderSnapshot.oid);
            if (it != m_orders.end()) {
                if (it->second != exchangeOrderId) {
                    if (m_logToFile) {
                        LOG_INFO(boost::format("OPX::onOM, symbol = %1%, exch_ord_id = %2%") % m_viperConfig->_allContracts[internalId]._name % exchangeOrderId);
                    }
                    std::string msg = composeMessage(20, m_viperConfig->_allContracts[internalId]._name, exchangeOrderId, event.orderSnapshot.venue, event.orderSnapshot.side);
                    boost::asio::write(*(m_socket.get()), boost::asio::buffer(msg), m_errorCode);
                    msg = composeMessage(21, m_viperConfig->_allContracts[internalId]._name, it->second, event.orderSnapshot.venue, event.orderSnapshot.side);
                    boost::asio::write(*(m_socket.get()), boost::asio::buffer(msg), m_errorCode);
                    it->second = exchangeOrderId;
                }
            } else {
                m_orders.emplace(event.orderSnapshot.oid, exchangeOrderId);
                std::string msg = composeMessage(20, m_viperConfig->_allContracts[internalId]._name, exchangeOrderId, event.orderSnapshot.venue, event.orderSnapshot.side);
                boost::asio::write(*(m_socket.get()), boost::asio::buffer(msg), m_errorCode);
            }
        }
    };
    void onOrderReplaceRejected(const om::OMOrderModifyRejectedEvent &event, const om::strategy_order_t &order, const om::OMBatch& batch) {};
    void onOrderCanceled(const om::OMOrderCanceledEvent &event, const om::strategy_order_t &order, const om::OMBatch& batch) {
        using proxySvc::s_log;
        if (BOOST_UNLIKELY(!m_socketConnected)) {
            return;
        }

        auto internalId = m_viperConfig->getInternalId(event.orderSnapshot.sid);
        if (BOOST_UNLIKELY(internalId == INVALID_INTERNAL_ID)) {
            return;
        }

        if (m_logToFile) {
            LOG_INFO(boost::format("OPX::onOC, Cancel, event active = %1%, venue = %2%, order exec qty=%3%") % event.orderSnapshot.action_flags.is_active % static_cast<uint16_t >(event.orderSnapshot.venue) %  event.orderSnapshot.omTotalExecutedQty);
        }
        auto it = m_orders.find(event.orderSnapshot.oid);
        if (it != m_orders.end()) {
            std::string msg = composeMessage(21, m_viperConfig->_allContracts[internalId]._name, it->second, event.orderSnapshot.venue, event.orderSnapshot.side);
            boost::asio::write(*(m_socket.get()), boost::asio::buffer(msg), m_errorCode);
            m_orders.erase(it);
        }
    };
    void onMassCancelAccepted(const om::OMMassCancelAcceptedEvent &event, const om::OMBatch& batch) {};
    void onMassCancelRejected(const om::OMMassCancelRejectedEvent &event, const om::OMBatch& batch) {};
    void onOrderCancelRejected(const om::OMOrderCancelRejectedEvent &event, const om::strategy_order_t &order, const om::OMBatch& batch) {};
    FillProcessingStatus onOMOrderExecutionBeforeFillArbitration(const om::OMExecutionEvent &event, const om::strategy_order_t &order, const om::OMBatch& batch) {
        using proxySvc::s_log;
        if (BOOST_UNLIKELY(!m_socketConnected)) {
            return FillProcessingStatus::FillNotAccepted;
        }

        auto internalId = m_viperConfig->getInternalId(event.orderSnapshot.sid);
        if (BOOST_UNLIKELY(internalId == INVALID_INTERNAL_ID)) {
            return FillProcessingStatus::FillNotAccepted;
        }

        if (m_logToFile) {
            LOG_INFO(boost::format("OPX::onOE, event active = %1%, venue = %2%, event exec qty = %3%, order exec qty=%4%") % event.is_active % static_cast<uint16_t >(event.orderSnapshot.venue) % event.totalExecutedQty % event.orderSnapshot.omTotalExecutedQty);
        }
        uint64_t exchangeOrderId = 0;
        if (event.orderSnapshot.venue == hf::VENUE::EUREX) {
            exchangeOrderId = event.orderSnapshot.marketDataCorrelationID;
        }
        else if (event.orderSnapshot.venue == hf::VENUE::ICE) {
            exchangeOrderId = event.orderSnapshot.exchangeOrderId.value();
        }
        auto it = m_orders.find(event.orderSnapshot.oid);
        if (!exchangeOrderId && it != m_orders.end()){
            exchangeOrderId = it->second;
        }
        if (exchangeOrderId > 0) {
            // if full fill, remove
            if (!event.is_active) {
                if (m_logToFile) {
                    LOG_INFO(boost::format("OPX::onOE, FullFill, symbol = %1%, exch_ord_id = %2%, event exec qty = %3%, order qty=%4%") % m_viperConfig->_allContracts[internalId]._name % exchangeOrderId % event.totalExecutedQty % event.orderSnapshot.omTotalExecutedQty);
                }
                std::string msg = composeMessage(21, m_viperConfig->_allContracts[internalId]._name, it->second, event.orderSnapshot.venue, event.orderSnapshot.side);
                boost::asio::write(*(m_socket.get()), boost::asio::buffer(msg), m_errorCode);
                m_orders.erase(it);
            } else {
                if (it != m_orders.end()) {
                    if (it->second != exchangeOrderId) {
                        if (m_logToFile) {
                            LOG_INFO(boost::format("OPX::onOE, symbol = %1%, exch_ord_id = %2%") % m_viperConfig->_allContracts[internalId]._name % exchangeOrderId);
                        }
                        std::string msg = composeMessage(20, m_viperConfig->_allContracts[internalId]._name, exchangeOrderId, event.orderSnapshot.venue, event.orderSnapshot.side);
                        boost::asio::write(*(m_socket.get()), boost::asio::buffer(msg), m_errorCode);
                        msg = composeMessage(21, m_viperConfig->_allContracts[internalId]._name, it->second, event.orderSnapshot.venue, event.orderSnapshot.side);
                        boost::asio::write(*(m_socket.get()), boost::asio::buffer(msg), m_errorCode);
                        it->second = exchangeOrderId;
                    }
                } else {
                    m_orders.emplace(event.orderSnapshot.oid, exchangeOrderId);
                    std::string msg = composeMessage(20, m_viperConfig->_allContracts[internalId]._name, exchangeOrderId, event.orderSnapshot.venue, event.orderSnapshot.side);
                    boost::asio::write(*(m_socket.get()), boost::asio::buffer(msg), m_errorCode);
                }
            }
        }
        return FillProcessingStatus::FillNotAccepted;
    }
    void onOMOrderExecutionWonFillArbitration(const om::OMExecutionEvent &event, const om::strategy_order_t &order, const om::OMBatch& batch) {}
    void onOMOrderExecutionLostFillArbitration(const om::OMExecutionEvent &event, const om::strategy_order_t &order, const om::OMBatch& batch) {}
    FillProcessingStatus onOrderSpreadLegExecution(const om::OMSpreadLegExecutionEvent &event, const om::strategy_order_t &order, const om::OMBatch& batch) { return FillProcessingStatus::FillNotAccepted; }
    void onRecoveredOrders(const om::port_id_t pid, const om::OMBatch& batch) {}
    void onRecoveredOrder(const om::strategy_order_t& order, const om::OMBatch& batch) {}
    void onRecoveredExecution(const om::OMRecoveredExecution& event, const om::OMBatch& batch) {}
    void onRequestQueueUsageExceeded(const om::OMRequestQueuePoolUsageExceedRejectEvent &event, const om::OMBatch& batch) {}
    void onRequestPoolUsageExceeded(const om::OMRequestQueuePoolUsageExceedRejectEvent &event, const om::OMBatch& batch) {}
    void onUnhandledOMEvent(const om::OMEvent &event, const om::OMBatch& batch) {}
    void onOMQuote(const om::OMQuoteEvent& /*event*/, const om::OMBatch& /*batch*/) {}
    void onOMQuoteCancel(const om::OMQuoteCancelEvent& /*event*/, const om::OMBatch& /*batch*/) {}
    FillProcessingStatus onOMOrderMultiLegExecution(const om::OMMultiLegExecutionEvent & /*event*/, const om::strategy_multi_leg_order_t& /*multi_leg_order*/, const om::OMBatch& /*batch*/) { return FillProcessingStatus::FillNotAccepted; }
    void onOrderMultiLegAccepted(const om::OMOrderMultiLegAcceptedEvent & /*event*/, const om::strategy_multi_leg_order_t& /*multi_leg_order*/, const om::OMBatch& /*batch*/) {}
    void onOrderMultiLegRejected(const om::OMOrderMultiLegRejectedEvent & /*event*/, const om::strategy_multi_leg_order_t& /*multi_leg_order*/, const om::OMBatch& /*batch*/) {}
    void onOMQuoteRequestCancel(const om::OMQuoteRequestCancelEvent &event, const om::OMBatch &batch) {}
    void onOrderRequestForQuoteReject(const om::OMRequestForQuoteReject& /*event*/, const om::OMBatch& /*batch*/) {}
    void onOrderRequestForQuoteCancelReject(const om::OMRequestForQuoteCancelReject& /*event*/, const om::OMBatch& /*batch*/) {}
    void onOrderTimedOut(const om::OMOrderTimedOutEvent &/*event*/, const om::strategy_order_t &/*order*/, const om::OMBatch& /*batch*/) {}
    void onOrderMultiLegTimedOut(const om::OMOrderMultiLegTimedOutEvent & /*event*/, const om::strategy_multi_leg_order_t& /*multi_leg_order*/, const om::OMBatch& /*batch*/) {}
    void onSessionReject(const om::OMSessionRejectEvent& /*event*/, const om::OMBatch& /*batch*/) {}
    void onExecutionMissingInfo(const om::OMExecutionMissingInfoEvent& event, const om::OMBatch& batch) {}
    void onSpreadLegExecutionMissingInfo(const om::OMSpreadLegExecutionMissingInfoEvent& event, const om::OMBatch& batch) {}
    void onRecoveredFillsMissingInfo(const om::OMRecoveredFillsEvent& event, const om::OMBatch& batch) {}
    void onThrottleThresholdWarning(const om::ThrottleThresholdWarning &event, const om::OMBatch& batch) {}

    void onMDFirmOrderExecutionEventBeforeFillArbitration(const md::MDFirmOrderExecutionEvent& event, const om::strategy_order_t& order, const md::MDBatch& batch) {};
    void onMDFirmOrderExecutionEventWonFillArbitration(const md::MDFirmOrderExecutionEvent& event, const om::strategy_order_t& order, const md::MDBatch& batch) {};
    void onMDFirmOrderExecutionEventLostFillArbitration(const md::MDFirmOrderExecutionEvent & /*event*/, const om::strategy_order_t &/*order*/, const md::MDBatch& /*batch*/) {}
    void onMDQueuePositionUpdate(const md::MDQueuePositionUpdateEvent& event, const md::MDBatch& batch) {};
    void onMDUpdate(const md::MDUpdateEvent &event, const md::MDBatch& batch) {};
    void onTradeCleanedMDUpdate(const md::MDUpdateEvent &event, const md::MDBatch& batch) {};
    void onMDTrade(const md::MDTradeEvent &event, const md::MDBatch& batch) {};
    void onMDTradeDetails(const md::MDTradeDetailsEvent& event, const md::MDBatch& batch) {};
    void onEndTradeSummary(const md::MDEndTradeSummary& event, const md::MDBatch& batch) {};
    void onMDAddOrder(const md::MDAddOrderEvent& event, const md::MDBatch& batch) {};
    void onMDSnapshotOrders(const md::MDSnapshotOrdersEvent& event, const md::MDBatch& batch) {};
    void onMDDeleteOrder(const md::MDDeleteOrderEvent& event, const md::MDBatch& batch) {};
    void onMDModifyOrder(const md::MDModifyOrderEvent& event, const md::MDBatch& batch) {};
    void onMDModifyOrderWithNewID(const md::MDModifyOrderWithNewIDEvent& event, const md::MDBatch& batch) {};
    void onMDLevelChange(const md::MDLevelChangeEvent& event, const md::MDBatch& batch) {};
    void onMDLevelClear(const md::MDLevelClearEvent& event, const md::MDBatch& batch) {};
    void onMDLevelDelete(const md::MDLevelDeleteEvent& event, const md::MDBatch& batch) {};
    void onMDLevelDeleteFrom(const md::MDLevelDeleteFromEvent& event, const md::MDBatch& batch) {};
    void onMDLevelNew(const md::MDLevelNewEvent& event, const md::MDBatch& batch) {};
    void onMDLevelOverlay(const md::MDLevelOverlayEvent& event, const md::MDBatch& batch) {};
    void onMDLimitPrice(const md::MDLimitPriceEvent& event, const md::MDBatch& batch) {};
    void onMDPreOpenPrice(const md::MDPreOpenPriceEvent& event, const md::MDBatch& batch) {};
    void onMDStatistics(const md::MDStatisticsEvent& event, const md::MDBatch& batch) {};
    void onMDVolumes(const md::MDVolumesEvent& event, const md::MDBatch& batch) {};
    void onMDSettlementPublished(const md::MDSettlementPublishedEvent& event, const md::MDBatch& batch) {};
    void onMDChannelStateChange(const md::MDChannelStateChangeEvent& event, const md::MDBatch& batch) {};
    void onMDTransactionEvent(const md::MDTransactionEvent& event, const md::MDBatch& batch) {};
    void onMDStatus(const md::MDStatusEvent &event, const md::MDBatch& batch) {};
    void onMDGap(const md::MDGapEvent &event, const md::MDBatch& batch) {};
    void onMDUnknown(const md::MDEvent &event, const md::MDBatch& batch) {};
    void onMDIntraEventSweep(const md::MDIntraEventSweepEvent &event, const md::MDBatch &batch) {}
    void onMDCurrencyEvent(const md::MDCurrencyEvent& event, const md::MDBatch& batch) {}
};
}

#endif