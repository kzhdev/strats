#ifndef VIPER_EXECUTOR_H
#define VIPER_EXECUTOR_H

#include <hf/ConfigManager.h>
#include <stdint.h>
#include <hf/hf.h>
#include <hf/log.h>
#include <md/md_event.h>
#include <om/om_event.h>
#include <strat/Strategy.hpp>
#include "Common.h"
#include "ViperConfig.h"
#include "InternalMatchingEngine.h"
#include <md/queue/MDQueue.h>
#include <md/queue/MDQueueResource.h>
#include <md/subscription/RequestMessage.h>
#include "Timer.h"
#include <hf/queue/DebugQueueLogger.h>
#include "Throttler.h"
#include "IOPTracker.h"
#include <hf/util/SocketUtils.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <signal.h>
#include <unordered_set>
#include <ven/VenueUtils.h>
#include "OrderRequestManager.h"
#include "ViperStrategyBase.h"
#include "ViperResource.h"
#include "MarketMaker.h"
#include "MarketDataConsumer.h"
#include <boost/dynamic_bitset.hpp>
#include "FillRateChecker.h"

namespace Python {

LOCAL_EXPLICIT_LOG_DECLARE("Executor.h", ExecutorSpace)

class Executor;

struct ContractState {
    struct SimpleSweepDefinition {
        hf::price_t _sweepTicksAtPrice = hf::NULL_PRICE;
        uint32_t _tradedQty = 0;
        bool sweepDetected(const md::MDIntraEventSweepEvent& event) const {
            return (std::abs(event.diff) >= _sweepTicksAtPrice) && (event.qty >= _tradedQty);
        }
    };

    InternalMatchingEngine<Executor> _ime;
    Book _books[2];
    ConfigContract _contract;
    SimpleSweepDefinition _sweepDefinition;
    orderrouter::RoutingPolicy _routingPolicy;
};

struct GatewayStatus {
    std::string _message;
    bool _active = true;
};

class Executor : public strat::StrategyT<Executor>, public ViperResource {
    OrderRequestManager& _orderRequestManager = OrderRequestManager::get();
    bool _recoveryNotGood = false;
    int _numSuccessfulRecoveredOrders = 0;
    hf::qty_t _totalSuccessfulRecoveredOrderQty = 0;

public:
    Executor()
        : _fillBannerTimer(*this)
        , _quoteBannerTimer(*this)
        , _checkQuotesTimer(*this)
    {
        _manualFills.reserve(5);
    }

    ResourceId id() const noexcept override { return ResourceId::Execution; }

    void onStratBegin() override {
        initialize();
        _id = this->context().getContainerId();
        if (!this->context().strategyLogin(_id, _strategyName, _strategyVersion, true)) {
            logging::GetLogger().logErrorAndThrow(boost::str(boost::format("Strategy login did not work. Name=%1% Version=%2%") % _strategyName % _strategyVersion));
        }
        LOG(0, "Executor: Started. sc_id={i1}", _id)
    }

    void onStratEnd() override {
        LOG(0, "Executor: Ended. {i1}", 0)
    }

    void onCreate(const libconfig::Setting& config) override {
        ViperResource::_onCreate(config);
    }

    void onPostCreate(const libconfig::Setting& config) override {
        ViperResource::_onPostCreate(config);
        _strategyName = hf::ConfigManager::getString(config, "strategy_name");
        _strategyVersion = hf::ConfigManager::getString(config, "strategy_version");
        _orderRequestQueueSize = hf::ConfigManager::getULong(config, "order_queue_size", 2048);
        _slow._allowStartupWithUncontrolledGTCs = hf::ConfigManager::getBool(config, "allow_start_with_uncontrolled_gtcs", false);
        logging::GetLogger().logInfo(boost::str(boost::format("Executor: order_queue_size=%1%") % _orderRequestQueueSize));

        if(_slow._allowStartupWithUncontrolledGTCs) {
            logging::GetLogger().logInfo("Allowing startup with uncontrolled GTCs");
        }

        _nextInternalHeartbeat = 0;

        if (config.exists("md-queues")) {
            const auto& queuesConfig = config["md-queues"];

            for (int i = 0; i < queuesConfig.getLength(); ++i) {
                const libconfig::Setting& queueConfig = queuesConfig[i];

                if (!hf::ConfigManager::getBool(queueConfig, "enabled", true)) continue;

                const auto name = hf::ConfigManager::getString(queueConfig, "name");

                auto* const ptr = dynamic_cast<md::queue::MDQueueResource*> (hf::ResourceManager::getResourceP(name));
                if (!ptr) {
                    logging::GetLogger().logErrorAndThrow(
                            boost::str(boost::format("Cannot Find MDQueue by name '%1%'") % name));
                }

                if (!ptr->getPoolP()) {
                    logging::GetLogger().logErrorAndThrow(
                            boost::str(boost::format("MDPool is not set for MDQueue with name '%1%'") % name));
                }

                MarketDataConsumerBase::mdQueue().emplace_back(ptr);
            }
        }
    }

    virtual void onStratLoop() override {
        if (BOOST_UNLIKELY(_needToSetLogger)) {
            _needToSetLogger = false;
            _kdbLogger->resetW();
            logging::setKdbLogger(_kdbLogger);
            logging::GetLogger().logInfo(boost::str(boost::format("EX logger %1%") % _kdbLogger));
            for (const auto& contract : _viperConfig._allContracts) {
                this->context().subscribeMD(contract._venue, contract._sid, 1,true, true);
            }
        }
        processMessages();
        checkSystemState();
        uint64_t now = clockUtil::GetCycles();
        checkTimers(now);
        sendInternalHeartbeat(now);
        _newOrderModifyOrderRateChecker.expireEntries(now);

        for (uint32_t id : _updatedContracts) {
            onMarketDataChange(id);
        }
        _updatedContracts.clear();
    }

    void sendInternalHeartbeat(uint64_t now) {
        if (BOOST_UNLIKELY(now > _nextInternalHeartbeat)) {
            _nextInternalHeartbeat = now + _internalHeartbeatPeriod;
            sendGenericMessageToFront(ViperMessageType::InternalHeartbeat);
        }
    }

    void processMessages() {
        uint32_t drainSize = 0;
        while (_msgCursor != _msgQueue.end()) {
            auto& message = reinterpret_cast<ViperMessage&>(_msgQueue[_msgCursor].data[0]);
            _msgCursor += message._size;
            ++drainSize;
            switch (message._type) {
                case ViperMessageType::RegisterOrderClient: {
                    auto request = reinterpret_cast<RegisterOrderClient&>(message._data[0]);
                    registerOrderClient(request);
                    break;
                }
                case ViperMessageType::OrderRequest: {
                    auto orderRequest = reinterpret_cast<OrderRequestNotification&>(message._data[0]);
                    processOrderRequest(orderRequest);
                    break;
                }
                case ViperMessageType::CancelStrategyOrders: {
                    auto request = reinterpret_cast<CancelStrategyOrders&>(message._data[0]);
                    getStrategyOutOfMarket(request._strategy, request._internalId, request._disableUseGTC);
                    break;
                }
                // Fill returned from other strategy
                case ViperMessageType::Fill: {
                    const auto& fill = reinterpret_cast<const FillMessage&> (message._data[0]);
                    handleReturnedFillFromStrategy(fill);
                    break;
                }

                // From MM
                case ViperMessageType::DisableUseGTC: {
                    auto& msg = reinterpret_cast<GenericMessage &> (message._data[0]);
                    uint32_t internalId = msg._number_1;
                    auto& state = _contractStates[internalId];
                    state._ime.updateUseGtc(false);
                    break;
                }

                // From RV
                case ViperMessageType::EndOfQuotes: {
                    auto& reply = reinterpret_cast<GenericMessage &> (message._data[0]);
                    sendGenericMessageToRV(ViperMessageType::EndOfQuotes, reply._number_1);
                    break;
                }

                // From Front
                case ViperMessageType::GridUpdate: {
                    auto& gridChange = reinterpret_cast<GridUpdateMessage &>(message._data[0]);
                    processGridChange(*gridChange._gridUpdate);
                    sendGenericMessageToFront(ViperMessageType::GridUpdateAccepted, gridChange._gridUpdate->_update.gridid());
                    break;
                }
                case ViperMessageType::GuiInitialized: {
                    handleGuiInitialized();
                    break;
                }
                case ViperMessageType::DebugStatuses : {
                    const DebugStatusEvents& dses = reinterpret_cast<DebugStatusEvents &>(message._data[0]);
                    handleDebugStatusEvents(dses);
                    break;
                }

                default:
//                    DLOG(0, "EXE. unhandled ViperMessage {i1}", toUInt(message._type))
                    break;
            }
        }

        if (drainSize) {
            DLOG(0, "EXE msgQueue drainSize. {i1}", drainSize);
        }
    }

    void registerOrderClient(const RegisterOrderClient& request) {
        auto& state = _contractStates[request._internalId];
        state._ime.registerOrderClient(request);
    }

    void processOrderRequest(const OrderRequestNotification& request) {
        auto& state = _contractStates[request._internalId];
        state._ime.processOrderRequests(request._strategy);
    }

    void handleReturnedFillFromStrategy(const FillMessage& fill) {
        LOG(0, "EXE {C[i1]}. Handling fill returned from {i2}. Side={i3} Price={d1} Size={i4}",
            getContract(fill._contractId), strategyName(fill._strategy), fill._side, ToDollars(fill._price), fill._qty)

        auto marketMaker = getStrategy<MarketMaker>(StrategyId::MarketMaker);
        auto id = marketMaker->beginFillProcess(fill._contractId);
        FillSource source;
        switch (fill._strategy) {
            case StrategyId::RelativeValue:
                source = FillSource::RvDisable;
                break;
            default:
                assert(false);  // should not come here
                break;
        }
        bool withinPositionLimit = marketMaker->forceTakeOverFill(fill._contractId, fill._side, fill._price, fill._qty, source);
        onEndOfFills(fill._contractId, fill._side, id, true);
        if (!withinPositionLimit) {
            auto formatStr = CREATE_SAFE_FORMATTER("Handling fill returned from RV.  Contract=%1% Side=%2% Price=%3% Size=%4%");
            disableMMExternalQuoteWithMessage(fill._contractId, static_cast<std::string>(formatStr % getContract(fill._contractId) % fill._side % ToDollars(fill._price) % fill._qty));
        }
    }

    void sendFAKHedgeInternalFillToFront(StrategyId strategy,
                                         uint32_t internalId,
                                         uint32_t side,
                                         hf::price_t price,
                                         hf::qty_t size,
                                         hf::price_t stf) {
        sendFillToFront(internalId, side, price, size, strategy, FillType::Internal, FillSource::Hedge,
                        0, 0, stf);
    }

    void onInternalFill(uint32_t internalId, Order* order, hf::price_t price, hf::qty_t size, bool payUp, uint32_t &mmFillId) {
        if (order->_strategy == StrategyId::MarketMaker) {
            auto marketMaker = getStrategy<MarketMaker>(StrategyId::MarketMaker);
            if (!mmFillId) {
                mmFillId = marketMaker->beginFillProcess(internalId);
            }
            marketMaker->onFill(internalId, order, price, size, payUp, FillType::Internal);
        } else {
            LOG(0, "EXE {C[i1]}. Internal fill. Sending to {i2}. Side={i3} Price={d1} Size={i4}",
                getContract(internalId), strategyName(order->_strategy), order->_side, ToDollars(price), size)
            sendFillToStrategy(internalId, order->_side, price, size, order->_strategy, FillType::Internal, FillSource::Quote);
        }
    }

    void onExternalFill(uint32_t internalId, Order* order, hf::price_t price, hf::qty_t size, bool payUp = false) {
        if (order->_strategy == StrategyId::MarketMaker) {
            auto marketMaker = getStrategy<MarketMaker>(StrategyId::MarketMaker);
            marketMaker->onFill(internalId, order, price, size, payUp, FillType::External);
        } else {
            LOG(0, "EXE {C[i1]}. External fill. Sending to {i2}. Side={i3} Price={d1} Size={i4}",
                getContract(internalId), toUInt(order->_strategy), order->_side, ToDollars(price), size)
            sendFillToStrategy(internalId, order->_side, price, size, order->_strategy, FillType::External, FillSource::Quote);
        }
    }

    void onExternalOverFill(uint32_t internalId, uint32_t side, hf::price_t price, hf::qty_t size) {
       LOG(0, "EXE {C[i1]}. External overfill. Side={i2} Price={d1} Size={i3}", getContract(internalId), side, ToDollars(price), size)
        hf::qty_t remainingSize = size;
        auto marketMaker = reinterpret_cast<MarketMaker*>(_strategies[toUInt(StrategyId::MarketMaker)]);
        remainingSize -= marketMaker->takeOverFill(internalId, side, price, remainingSize);
        if (remainingSize > 0) {
            bool overFillSendToStrategy = false;
            for (uint32_t i = toUInt(StrategyId::RelativeValue); i < toUInt(StrategyId::Execution); ++i) {
                if (_strategies[i]->isEnabled()) {
                    _strategies[i]->sendMessage<FillMessage>(
                            ViperMessageType::Fill,
                            [=](FillMessage& message) {
                                message._contractId = internalId;
                                message._side = side;
                                message._qty = remainingSize;
                                message._strategy = (StrategyId)i;
                                message._type = FillType::External;
                                message._price = price;
                                message._source = FillSource::Overfill;
                                message._internalAllocation = 0;
                                message._externalAllocation = 0;
                            });
                    overFillSendToStrategy = true;
                    break;
                }
            }

            if (!overFillSendToStrategy) {
                marketMaker->forceTakeOverFill(internalId, side, price, remainingSize, FillSource::Overfill);
                disableMMExternalQuoteWithMessage(internalId, static_cast<std::string>(CREATE_SAFE_FORMATTER("Exectuor: External overfill.  Contract=%1% remainingSize after MM takes=%2% Price=%3%")
                                                                                       % getContract(internalId) % remainingSize % ToDollars(price)));
            }
        }
    }

    void sendFillToFront(uint32_t contractIndex, uint32_t side, hf::price_t price, hf::qty_t size, StrategyId strategyId, FillType type, FillSource source,
                         hf::qty_t internalAllocation, hf::qty_t externalAllocation, hf::price_t stf) {
        sendMessageToFront<FillMessage>(ViperMessageType::Fill, [=](FillMessage& message) {
            message._contractId = contractIndex;
            message._side = side;
            message._qty = size;
            message._strategy = strategyId;
            message._type = type;
            message._price = price;
            message._source = source;
            message._internalAllocation = internalAllocation;
            message._externalAllocation = externalAllocation;
            message._rvStf = stf;
        });
    }

    void onEndOfFills(uint32_t internalId, uint32_t side, uint32_t fillId, bool isInternal, bool isManualFill = false) {
        _strategies[toUInt(StrategyId::MarketMaker)]->sendMessage<EndOfFill>(
                ViperMessageType::EndOfFill,
                [=](EndOfFill& message) {
                    message._internalId = internalId;
                    message._side = side;
                    message._id = fillId;
                    message._isInternal = isInternal;
                    message._isManualFill = isManualFill;
                });
    }

    void sendExternalFAKHedge(StrategyId strategy, uint32_t internalId, uint32_t side, OrderRequest::Hedge& hedge) {
        om::order_t* rhoneOrder = newFAKOrder(internalId, side, hedge._externalPrice, hedge._size, strategy);
        sendHedgeMarketDataAdjustmentAck(strategy, hedge._hedgeId, internalId, GetOtherSide(side), rhoneOrder->oid.value(), hedge._size);
        sendFillToFront(internalId, side, hedge._externalPrice, hedge._size, strategy, FillType::External, FillSource::Hedge, 0,
                        0, hedge._stf);
    }

    void sendHedgeMarketDataAdjustmentAckWithZeroSize(StrategyId strategy,
                                                      uint32_t internalId,
                                                      uint32_t side,
                                                      OrderRequest::Hedge& order) {
        sendHedgeMarketDataAdjustmentAck(strategy, order._hedgeId, internalId, GetOtherSide(side), 0, 0);
    }

    void onInternalHedgeDone(StrategyId strategy,
                             uint32_t instrumentId,
                             uint32_t marketId,
                             uint32_t side,
                             uint32_t hedgeId) {
        getStrategy(strategy)->template sendMessage<InternalHedgeDone>(
                ViperMessageType::InternalHedgeDone,
                [=](InternalHedgeDone& message) {
                    message._hedgeId = hedgeId;
                    message._instrumentIndex = instrumentId;
                    message._side = side;
                    message._marketIndex = marketId;
                });
    }

    void sendHedgeMarketDataAdjustmentAck(StrategyId strategy,
                                          uint32_t hedgeId,
                                          uint32_t instrumentIndex,
                                          uint32_t side,
                                          om::order_id_t orderId,
                                          hf::qty_t size) {
        _strategies[toUInt(strategy)]->sendMessage<HedgeMarketDataAdjustmentAck>(
                ViperMessageType::HedgeMDAdjsutAck,
                [=](HedgeMarketDataAdjustmentAck& message) {
                    message._hedgeId = hedgeId;
                    message._instrumentIndex = instrumentIndex;
                    message._side = side;
                    message._size = size;
                    message._orderId = orderId;
                });
    }

    void sendFillToStrategy(uint32_t contractIndex,
                            uint32_t side,
                            hf::price_t price,
                            hf::qty_t size,
                            StrategyId strategyId,
                            FillType type,
                            FillSource source) {
        _strategies[toUInt(strategyId)]->sendMessage<FillMessage>(
                ViperMessageType::Fill,
                [=](FillMessage& message) {
                    message._contractId = contractIndex;
                    message._side = side;
                    message._qty = size;
                    message._strategy = strategyId;
                    message._type = type;
                    message._price = price;
                    message._source = source;
                });
    }

    void sendGenericMessageToRV(ViperMessageType type, int64_t value1 = 0, int64_t value2 = 0, int64_t value3 = 0) {
        _strategies[toUInt(StrategyId::RelativeValue)]->sendGenericMessage(type, value1, value2, value3);
    }

    InternalMatchingEngine<Executor>& getInternalMatchingEngine(const om::order_t& order) {
        ContractState& state = _contractStates[getInternalId(order)];
        return state._ime;
    }

    void sendReverseFillToStrategy(StrategyId strategy, uint32_t internalId, const om::order_t& order, FillSource source) {
        const hf::qty_t unfilledSize = order.qty - order.executedQty;
        const uint32_t reverseSide = order.side == hf::SIDE::BUY ? 1 : 0;
        LOG(0, "EXE {C[i1]}. Sending size {i2} as reverse to strategy {i3}. OID={i4} Type={i5}",
            getContract(internalId), unfilledSize, toUInt(strategy), order.oid.value(), toUInt(source))
        sendFillToStrategy(internalId, reverseSide, order.pr, unfilledSize, strategy, FillType::External, source);
    }

    void disableExternalQuotingOnAllMMInstruments() {
        if (disableExternalQuotingOnAllMMInstrumentsKernel()) {
            LOG(0, "Disabling external quoting", 0);
            sendErrorMessage("Disabling ExternalQuoting", ErrorType::Critical);
        }
    }

    bool disableExternalQuotingOnAllMMInstrumentsKernel() {
        return DisableExternalQuotingOnAllMMInstrumentsKernel(s_systemState);
    }

    COLD
    void onOrderRecoveryStarted(om::port_gid_t port_gid, om::instance_id_t strategy_instance_id) {
        LOG(0, "Start order recovery. port_gid_t={i1}", port_gid.value())

        _recoveryNotGood = false;
        _numSuccessfulRecoveredOrders = 0;
        _totalSuccessfulRecoveredOrderQty = 0;
    }

    COLD
    bool willManageRecoveredOrder(const om::strategy_order_t& order) {
        const uint32_t internalId = _viperConfig.getInternalId(order.sid);
        if (internalId == INVALID_INTERNAL_ID) {
            using ExecutorSpace::s_log;
            LOG(0, "Order. OID={i1} SID={i3}", order.oid.value(), order.sid)
            LOG_ERROR(boost::str(boost::format("Recovered GTC Could not be matched to existing sym.  sid=%1% Side=%2% Price=%3% Qty(before unprocessed fills)=%4% oid=%5%") %
                                 static_cast<int>(order.sid) % static_cast<int>(order.side) % order.pr % order.qty % order.oid.value()));
            std::string message = boost::str(boost::format("GTC Order for not traded contract - need to cancel. OID=%1% SID=%2%") % order.oid.value() % order.sid);
            sendErrorMessage(message, ErrorType::Critical);

            //Until the Rhone OM can handle cancelling a GTC that is not in the secdb, we're just going to force exit
            if (!_slow._allowStartupWithUncontrolledGTCs) {
                _recoveryNotGood = true;
            }

            // TODO: KUN
//        auto& badGTCCollection = _slow._badReloadedGTCsToCancel[order.pid][order.msgw_idx];
//        const_cast<om::order_t &>(order).debug.i[ORDER_OWNER] = UNPROCESSABLE_RELOADED_ORDER;
//        BOOST_VERIFY(badGTCCollection.insert(order.oid.value()).second);
            return false;
        }

        return order.action_flags.is_active;
    }

    COLD
    void onOrderRecoveryFinished(om::port_gid_t port_gid, om::instance_id_t strategy_instance_id) {
        if(_recoveryNotGood) {
            hf::Service::exitAll();
        }

        if (_numSuccessfulRecoveredOrders) {
            auto message = boost::str(boost::format("GTC Orders recovered.  OrderCount=%1% Qty(before unprocessed filles)=%2%")
                    % _numSuccessfulRecoveredOrders % _totalSuccessfulRecoveredOrderQty);
            sendErrorMessage(message, ErrorType::Normal);
        }

        LOG(0, "Order recovery completed. port_gid_t={i1}", port_gid.value())
        _slow._iopTracking.forceReconcileGTCs(true);
    }

    COLD
    void onRecoveredOrder(om::strategy_order_t& order, const om::OMBatch& batch) {
        using ExecutorSpace::s_log;
        const uint32_t internalId = _viperConfig.getInternalId(order.sid);
        uint32_t side = order.side == hf::SIDE::BUY ? BID : ASK;
        LOG_INFO(boost::format("Recovered GTC: sym=%1% side=%2% px=%3% qty=%4% oid=%5% rhone_oid=%6% exch_oid=%7%")
                % _viperConfig._allContracts[internalId]._name % hf::SIDE_NAMES.at(static_cast<unsigned char>(order.side))
                % order.pr % order.qty % order.oid.value() % order.rhone_oid.value() % order.exchangeOrderId.value());
        ContractState& state = _contractStates[internalId];
        state._ime.addGtcOrder(side,  order);
        auto message = boost::str(boost::format("GTC Order recovered.  Contract=%1% Side=%2% Price=%3% Qty(before unprocessed fills)=%4% oid=%5%") %
                                   getContractName(internalId) % hf::SIDE_NAMES.at(static_cast<unsigned char>(order.side)) % ToDollars(order.pr) % order.leavesQty % order.oid.value());
        sendErrorMessage(message, ErrorType::Normal);

        ++_numSuccessfulRecoveredOrders;
        _totalSuccessfulRecoveredOrderQty += order.leavesQty;
    }

    COLD
    void onRecoveredFills(const om::OMRecoveredFillsEvent& recoveredFills, const om::OMBatch& batch) {
        using ExecutorSpace::s_log;
        LOG(0, "Recovered GTC fills. {i1}", recoveredFills.num_fills);
        for (uint16_t i(0); i < recoveredFills.num_fills; ++i) {
//            const auto& fill = recoveredFills.executions[i];
            // TODO: Kun
//            const om::order_t& order = this->context().getOrder(fill.unique_order_id.decode(fill.venue));
//            om::order_t& mutableOrder = const_cast<om::order_t &>(order);
//            mutableOrder.executedQty += fill.filledQty;
//            mutableOrder.leavesQty -= fill.filledQty;
//            auto internalId = _viperConfig.getInternalId(order.sid);
//            if (internalId != INVALID_INTERNAL_ID) {
//                handleFillForUnassignedGtcOrder(fill, internalId, order);
//                auto message = boost::str(boost::format("GTC Order filled.  Contract=%1% Side=%2% FillQty=%3% FillPrice=%4% OrderPrice=%5% LeavesQty=%6%")
//                        % getContractName(internalId) % hf::SIDE_NAMES.at(static_cast<unsigned char>(order.side)) % fill.filledQty % ToDollars(fill.filledPrice) % ToDollars(order.pr) % order.leavesQty);
//                LOG_INFO(message);
//                sendErrorMessage(message, ErrorType::Critical);
//            } else {
//                std::string message = boost::str(boost::format("Fill for not traded contract. Contract=%1% Price=%2% Size%3%") % order.sid % ToRhonePrice(fill.filledPrice) % fill.filledQty);
//                sendErrorMessage(message, ErrorType::Critical);
//                LOG_ERROR(message);
//            }
        }

        //Cancel bad GTCs if necessary
//        auto badGTCPidIter = _slow._badReloadedGTCsToCancel.find(recoveredFills.pid);
//        if(badGTCPidIter != std::end(_slow._badReloadedGTCsToCancel)) {
//            auto badMsgwIndexIter = badGTCPidIter->second.find(recoveredFills.msgw_idx);
//            if(badMsgwIndexIter != std::end(badGTCPidIter->second)) {
//                for(const auto& oid : badMsgwIndexIter->second) {
//                    //Unfortunately this version of Rhone cannot handle cancelling GTCs if oid is unknown.  The below codes does however work
//                    //this->context().cancelOrder(oid);
//                    //sendErrorMessage("Cancelling GTC due to unknown symbol.  oid=" + std::to_string(oid), ErrorType::Critical);
//                    LOG(0, "BadGTC [need to be] cancelled.  oid={i1}", oid)
//                    sendErrorMessage("Need to cancel GTCs due to unknown symbol.  However this version of Rhone does not support that functionality.  oid=" + std::to_string(oid), ErrorType::Critical);
//                }
//                badGTCPidIter->second.erase(badMsgwIndexIter);
//            }
//        }
    }

    COLD NEVER_INLINE
    void handleDebugStatusEvents(const DebugStatusEvents& dses) {
        HandleDebugStatusEvents(dses, this);
    }

    bool handleLogout() {
       LOG(0, "EXE. logging out. {i1}", 0);
        handleSystemDisabled();
        return true;
    }

    void sendMarketUpdateToStrategy(StrategyId strategy,
                                    uint32_t internalId,
                                    uint8_t id,
                                    hf::price_t bidPrice,
                                    hf::qty_t bidSize,
                                    hf::price_t askPrice,
                                    hf::qty_t askSize) {
        getStrategy(strategy)->template sendMessage<MarketUpdate>(
                ViperMessageType::MarketUpdate,
                [=](MarketUpdate& message) {
                    message._instrumentIndex = internalId;
                    message._marketId = id;
                    message._bidPrice = bidPrice;
                    message._bidSize = bidSize;
                    message._askPrice = askPrice;
                    message._askSize = askSize;
                });
    }

    void sendIOPUpdateToFront(uint32_t internalId, hf::price_t price, int numBidGTCs, int numOfferGTCs, int ticksThroughBid, int ticksThroughAsk, bool forceCrossed) {
        sendMessageToFront<IOPUpdate>(ViperMessageType::IOPUpdate, [=](IOPUpdate& iopUpdate) {
            iopUpdate._internalId = internalId;
            iopUpdate.price = price;
            iopUpdate.numBidGTCs = numBidGTCs;
            iopUpdate.numOfferGTCs = numOfferGTCs;
            iopUpdate.ticksThroughBid = ticksThroughBid;
            iopUpdate.ticksThroughAsk = ticksThroughAsk;
            iopUpdate.forceCrossed = forceCrossed;
        });
    }

    void sendAuctionMDUpdateToFront(uint32_t internalId, hf::price_t bid, hf::price_t offer) {
        sendMessageToFront<AuctionMDUpdate>(ViperMessageType::AuctionMDUpdate, [=](AuctionMDUpdate& update) {
            update._internalId = internalId;
            update._bid = bid;
            update._offer = offer;
        });
    }

    void sendNumGTCOrdersToFront(uint32_t internalId, hf::qty_t numBid, hf::qty_t numOffer) {
        sendGenericMessageToFront(ViperMessageType::NumGTCUpdate, internalId, numBid, numOffer);
    }

    void sendEndIOPTrackingToFront(uint32_t instrumentId) {
        sendGenericMessageToFront(ViperMessageType::EndIOPTracking, instrumentId);
    }

    bool processGridChange(GridUpdate& update) {
        if (update._update.gridid() == toUInt(WindowIds::StackerParameters)) {
            for (proto::GridCell& cell : *update._update.mutable_cells()) {
                const uint32_t internalId = cell.row();
                ContractState& state = _contractStates[internalId];
                if(!_viperConfig._allowGTCs) {
                    StackerParametersColumns columnType = static_cast<StackerParametersColumns>(cell.col());
                    if(columnType == StackerParametersColumns::UseGTC && (1 == atoi(cell.value().c_str()))) {
                        sendErrorMessage("EnableGTCs changed to true for contract" + _viperConfig._allContracts[internalId]._name + ". This shouldn't be possible!  overriding", ErrorType::Critical);
                        LOG(0, "EXE: Contract {C[i1]} EnableGTCs changed to true for contract, this shouldn't be possible.  Overriding", getContract(internalId));
                        cell.set_value("0");
                        continue;
                    }
                }
                state._ime.handleStackerParameterUpdate(cell);
            }
        }
        else if (update._update.gridid() == toUInt(WindowIds::GlobalConfigs)) {
            for (const proto::GridCell& cell : update._update.cells()) {
                GlobalConfig& config = _viperConfig._globalConfigs[cell.row()];
                if (config._key == QUOTE_BANNER_PERIOD) {
                    uint64_t period = GetCyclesFromMicroSeconds(boost::lexical_cast<uint32_t>(cell.value()), _frequency);
                    for (auto& state : _contractStates) {
                        state._ime.handleQuoteBannerUpdate(period);
                    }
                } else if (config._key == FILL_BANNER_PERIOD) {
                    uint64_t period = GetCyclesFromMicroSeconds(boost::lexical_cast<uint32_t>(cell.value()), _frequency);
                    for (auto& state : _contractStates) {
                        state._ime.handleFillBannerUpdate(period);
                    }
                } else if (config._key == SELF_MATCH_BANNER_MICS) {
                    uint64_t period = GetCyclesFromMicroSeconds(boost::lexical_cast<uint32_t>(cell.value()), _frequency);
                    for (auto& state : _contractStates) {
                        state._ime.handleRejectBannerUpdate(period);
                    }
                } else if (config._key == MSG_REDUCE_QUOTE_MICS) {
                    uint64_t inDangerZonePeriod = GetCyclesFromMicroSeconds(boost::lexical_cast<uint32_t>(cell.value()), _frequency);
                    for (auto& kvp : _throttlers) {
                        kvp.second.updateInDangerZonePeriod(inDangerZonePeriod);
                    }
                }
                else if (config._key == MSG_HEDGES_INTERVAL_MICS) {
                    uint64_t atCapacityPeriod = GetCyclesFromMicroSeconds(boost::lexical_cast<uint32_t>(cell.value()), _frequency);
                    for (auto& kvp : _throttlers) {
                        kvp.second.updateAtCapacityPeriod(atCapacityPeriod);
                    }
                }
                else if (config._key == PERCENT_MSG_HEDGES) {
                    double percent = boost::lexical_cast<double>(cell.value());
                    for (auto& kvp : _throttlers) {
                        kvp.second.updateHedgePercentage(percent);
                    }
                }
                else if (config._key == PERCENT_MSG_REDUCE_QUOTE) {
                    double percent = boost::lexical_cast<double>(cell.value());
                    for (auto& kvp : _throttlers) {
                        kvp.second.updateDangerZonePercentage(percent);
                    }
                }
                else if (config._key == HEDGE_BANNER_MICS) {
                    uint64_t period = GetCyclesFromMicroSeconds(boost::lexical_cast<uint32_t>(cell.value()), _frequency);
                    for (auto& state : _contractStates) {
                        state._ime.handleHedgeQuoteBannerUpdate(period);
                    }
                }
                else if(config._key == ORDER_RATE_CLIPS) {
                    uint32_t clips = boost::lexical_cast<uint32_t>(cell.value());
                    _newOrderModifyOrderRateChecker.changeMaxClips(clips, clockUtil::GetCycles());
                    LOG(0, "Order Rate Checker.  Update max clips={i1}", clips);
                }
                else if(config._key == ORDER_RATE_SUM_TOTAL) {
                    hf::qty_t sumTotal = boost::lexical_cast<hf::qty_t>(cell.value());
                    _newOrderModifyOrderRateChecker.changeSumTotal(sumTotal, clockUtil::GetCycles());
                    LOG(0, "Order Rate Checker.  Update sum total={i1}", sumTotal);
                }
                else if(config._key == ORDER_RATE_PERIOD) {
                    uint32_t period = boost::lexical_cast<uint32_t>(cell.value());
                    uint64_t periodInCycles = GetCyclesFromMilliSeconds(period, _frequency);
                    _newOrderModifyOrderRateChecker.changeThrottleWindowSize(periodInCycles, clockUtil::GetCycles());
                    LOG(0, "Order Rate Checker. Update period={i1} periodInCycles={i2}", period, periodInCycles);
                }
            }
        }
        else if (update._update.gridid() == toUInt(WindowIds::PricingConfig)) {
            bool valid = validateRvPricingConfigUpdate(update);

            if (!valid) {
                return false;
            }

            for (const proto::GridCell& cell : update._update.cells()) {
                PricingConfigsColumns columnType = static_cast<PricingConfigsColumns>(cell.col());
                switch (columnType) {
                    case PricingConfigsColumns::SweepTicks: {
                        auto& state = _contractStates[cell.row()];
                        uint32_t gridValue = boost::lexical_cast<uint32_t>(cell.value());
                        state._sweepDefinition._sweepTicksAtPrice = static_cast<hf::price_t>(gridValue) * state._contract._tickSize;
                        LOG(0, "EXE sweepDefinition changed.  Contract={C[i1]} ExternalValue={i2} InTicks={i3}",
                            getContract(state._contract._internalId), gridValue, state._sweepDefinition._sweepTicksAtPrice)
                        break;
                    }
                    case PricingConfigsColumns::SweepQty: {
                        auto& state = _contractStates[cell.row()];
                        state._sweepDefinition._tradedQty = boost::lexical_cast<int32_t>(cell.value());
                        LOG(0, "EXE sweepQty changed.  Contract={C[i1]} Value={i2}",
                            getContract(state._contract._internalId), state._sweepDefinition._tradedQty);
                        break;
                    }
                    default:
                        break;
                }
            }
        }
        else if (update._update.gridid() == toUInt(WindowIds::ContractParametersRv)) {
            for (const proto::GridCell& cell : update._update.cells()) {
                ContractParameterRVColumns columnType = static_cast<ContractParameterRVColumns>(cell.col());
                if (columnType == ContractParameterRVColumns::HedgeTolerance) {
                    const uint32_t internalId = cell.row();
                    ContractState& state = _contractStates[internalId];
                    double value = boost::lexical_cast<double>(cell.value());
                    state._ime.updateHedgeTolerance(ToRhonePrice(value));
                }
            }
        }
        else if (update._update.gridid() == toUInt(WindowIds::ManualFillsEntry)) {
            onManualFills(update);
        }
        else if (update._update.gridid() == toUInt(WindowIds::TraderSwitch)) {
            switchTrader(update);
        }
        else if(update._update.gridid() == toUInt(WindowIds::GTCManagement)) {
            _slow._iopTracking.handleGTCManagementGridUpdate(update);
        }
        return true;
    }

    bool validateRvPricingConfigUpdate(GridUpdate& update, bool verbose = true) {
        bool errorsOccured = false;
        for(int i = 0; i < update._update.cells_size(); ++i) {
            proto::GridCell& gridCell = *update._update.mutable_cells(i);
            PricingConfigsColumns columnType = static_cast<PricingConfigsColumns>(gridCell.col());
            switch (columnType) {
                case PricingConfigsColumns::SweepTicks: {
                    bool errorOccuredInThisCheck = false;
                    auto &state = _contractStates[gridCell.row()];
                    try {
                        int32_t value = boost::lexical_cast<int32_t>(gridCell.value());
                        if (value <= 0) {
                            if (verbose) {
                                sendErrorMessage("PricingConfigsColumn::SweepTicks <= 0", ErrorType::Critical);
                            }
                            errorsOccured = errorOccuredInThisCheck = true;
                        }
                        uint64_t maxValue = static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) / state._contract._tickSize;
                        if (static_cast<uint64_t>(value) > maxValue) {
                            if (verbose) {
                                sendErrorMessage("PricingConfigsColumn::SweepTicks too big.  value=" + std::to_string(value), ErrorType::Critical);
                            }
                            errorsOccured = errorOccuredInThisCheck = true;
                        }
                    }
                    catch(const boost::bad_lexical_cast& blc) {
                        if(verbose) {
                            sendErrorMessage("Error parsing SweepTicks value", ErrorType::Critical);
                        }
                        errorsOccured = errorOccuredInThisCheck = true;
                    }

                    if(errorOccuredInThisCheck && verbose) {
                        gridCell.set_value(std::to_string(state._sweepDefinition._sweepTicksAtPrice / state._contract._tickSize));
                    }
                    break;
                }
                case PricingConfigsColumns::SweepQty: {
                    bool errorOccuredInThisCheck = false;
                    auto &state = _contractStates[gridCell.row()];
                    try {
                        if (boost::lexical_cast<int32_t>(gridCell.value()) < 0) {
                            if (verbose) {
                                sendErrorMessage("PricingConfigsColumn::SweepQty < 0", ErrorType::Critical);
                            }
                            errorsOccured = errorOccuredInThisCheck = true;
                        }
                    }
                    catch(const boost::bad_lexical_cast& blc) {
                        if(verbose) {
                            sendErrorMessage("Error parsing SweepQty value", ErrorType::Critical);
                        }
                        errorsOccured = errorOccuredInThisCheck = true;
                    }

                    if(errorOccuredInThisCheck && verbose) {
                        gridCell.set_value(std::to_string(state._sweepDefinition._tradedQty));
                    }
                    break;
                }
                default:
                    break;
            }
        }

        return !errorsOccured;
    }

    void init(double frequency,
              uint64_t internalHeartbeatPeriod,
              std::map<hf::VENUE, uint32_t>& transactionCapacity,
              om::instance_id_t iid) {
        _internalHeartbeatPeriod = internalHeartbeatPeriod;
        _transactionCapacity = transactionCapacity;
        _iid = iid;
        logging::GetLogger().logInfo(boost::str(boost::format("Executor initialized. Frequency=%1%") % _frequency));
    }

    void initialize() {
        try {
            _orderRequestManager.init(_viperConfig, _orderRequestQueueSize, &_msgQueue);
            _contractStates.resize(_viperConfig._allContracts.size());
            _slow._potentiallyGapped.resize(_viperConfig._allContracts.size());
            uint32_t fillBannerMicros = boost::lexical_cast<uint32_t>(_viperConfig.getGlobal(FILL_BANNER_PERIOD));
            uint64_t fillBannerPeriod = GetCyclesFromMicroSeconds(fillBannerMicros, _frequency);
            uint32_t quoteBannerMicros = boost::lexical_cast<uint32_t>(_viperConfig.getGlobal(QUOTE_BANNER_PERIOD));
            uint64_t quoteBannerPeriod = GetCyclesFromMicroSeconds(quoteBannerMicros, _frequency);
            uint32_t hedgeBannerMicos = boost::lexical_cast<uint32_t>(_viperConfig.getGlobal(HEDGE_BANNER_MICS));
            uint64_t hedgeBannerPeriod = GetCyclesFromMicroSeconds(hedgeBannerMicos, _frequency);
            uint32_t selfMatchBannerMics = boost::lexical_cast<uint32_t>(_viperConfig.getGlobal(SELF_MATCH_BANNER_MICS));
            uint64_t selfMatchBannerPeriod = GetCyclesFromMicroSeconds(selfMatchBannerMics, _frequency);
            for (uint32_t i = 0; i < _viperConfig._allContracts.size(); ++i) {
                const ConfigContract &contract = _viperConfig._allContracts[i];
                auto& state = _contractStates[i];
                if (contract._toMarketMake) {
                    const StackerConfig &stackerConfig = _viperConfig._stackerConfigs[contract._marketMakeIndex];
                    const RelativeValueConfig &rvConfig = _viperConfig._relativeValueConfigs[contract._marketMakeIndex];
                    hf::price_t rvHedgeMatchTolerance = ToRhonePrice(rvConfig._hedgeTolerance);
                    state._routingPolicy = getRoutingPolicy(contract);
                    if (!state._routingPolicy.hasConnections()) {
                        for (const auto& conn : context().getAllExchangeConnections()) {
                            logging::GetLogger().logInfo(boost::str(boost::format("Conn=%1% venue=%2% port_gid=%3% is_convenient=%4%")
                                % conn.getName() % to_string(strat::venue(conn)) % strat::port_gid(conn).value() % strat::is_convenience(conn)));
                        }
                        logging::GetLogger().logErrorAndThrow(boost::str(boost::format("No viable connection for %1% %2%")
                            % to_string(contract._venue) % contract._name));
                    }
                    state._ime.initialize(this, contract, stackerConfig, rvHedgeMatchTolerance,
                                          fillBannerPeriod, quoteBannerPeriod, hedgeBannerPeriod, selfMatchBannerPeriod,
                                          state._books, _viperConfig._iceCancelResting);
                    state._contract = contract;
                }
                const auto& pc = _viperConfig._pricingConfigs[i];
                state._sweepDefinition._sweepTicksAtPrice = pc._sweepTicks * contract._tickSize;
                state._sweepDefinition._tradedQty = pc._sweepQuantity;

                _usedVenues.insert(contract._venue);
            }

            for (hf::VENUE venue : _usedVenues) {
                const uint32_t nRoutes = this->context().getNumberOfOMPorts(venue);
                logging::GetLogger().logInfo("Will round-robin between " + std::to_string(nRoutes) + " routes.");
                LOG(0, "Will round-robin between {i1} routes.", nRoutes);
                auto it = _transactionCapacity.find(venue);
                if (it == _transactionCapacity.end()) {
                    logging::GetLogger().logErrorAndThrow(boost::str(boost::format("does not have transaction capacity for %1%") % (int)venue));
                }
            }

            validateContracts(_viperConfig._allContracts);

            for (const auto& om_queue_info : this->context().getAllExchangeConnections()) {
                auto port_gid = strat::port_gid(om_queue_info);
                auto venue = strat::venue(om_queue_info);
                logging::GetLogger().logInfo(boost::str(
                    boost::format("Gateway PortGid=%1% Venue=%2%") % port_gid.value() % venue));
                _pendingGatewayLogin.emplace(port_gid.value());
                _mapGatewayVenue.emplace(port_gid.value(), venue);
            }

            _slow._iopTracking.init(this, _viperConfig._allContracts.size());

            uint64_t ocheckPeriod = GetCyclesFromMilliSeconds(
                    boost::lexical_cast<uint32_t>(_viperConfig.getGlobal(ORDER_RATE_PERIOD)), _frequency);
            uint32_t ocheckClips = boost::lexical_cast<uint32_t>(_viperConfig.getGlobal(ORDER_RATE_CLIPS));
            uint64_t ocheckSumTotalLimit = boost::lexical_cast<uint64_t>(_viperConfig.getGlobal(ORDER_RATE_SUM_TOTAL));
            _newOrderModifyOrderRateChecker.initialize(ocheckClips, ocheckPeriod, ocheckSumTotalLimit);
        }
        catch(std::exception& ex) {
            //Without an initial heartbeat we will never detect a timeout
            sendInternalHeartbeat(clockUtil::GetCycles());
            throw ex;
        }
    }

    void validateContracts(const std::vector<ConfigContract>& contracts) {
        auto& logger = logging::GetLogger();
        const std::regex eurodollarSpreadRegex{"^GE[FGHJKMNQUVXZ][0-9]-GE[FGHJKMNQUVXZ][0-9]$"};
        const std::regex eurodollarOutrighRegex{"^GE[FGHJKMNQUVXZ][0-9]$"};
        const hf::price_t half_tick = hf::RhoneAdjustedPrice(5, 3); //5/10^3 = 0.005
        const hf::price_t quarter_tick = hf::RhoneAdjustedPrice(25, 4); //25/10^4 = .0025
        double expected_display_factor = 0.01;

        for(const auto& contract : contracts) {
            const hf::symbol_info_t& cmeSI = this->context().getSymbolInfo(contract._sid);
            if(std::regex_match(cmeSI.sym, eurodollarSpreadRegex) || std::regex_match(cmeSI.sym, eurodollarOutrighRegex)) {
                if(cmeSI.tick_size != half_tick && cmeSI.tick_size != quarter_tick) {
                    logger.logErrorAndThrow(boost::str(boost::format("Eurodollar has invalid tick size.  sym=%1% expected=(%2% or %3%) actual=%4%")
                        % cmeSI.sym % half_tick % quarter_tick % cmeSI.tick_size));
                }

                if(std::fabs(cmeSI.cme.display_factor - expected_display_factor) > 1e-20) {
                       logger.logErrorAndThrow(boost::str(boost::format("Eurodollar has invalid display factor.  sym=%1% expected=%2% actual=%3%")
                       % cmeSI.sym % expected_display_factor % cmeSI.cme.display_factor));
                }
            }
        }
    }

    orderrouter::RoutingPolicy getRoutingPolicy(const ConfigContract& contract) {
        // TODO: doesn to work in SIM env. CME is convenience connection
//        orderrouter::RoutingSpec spec(contract._venue, contract._sid, (contract._venue == hf::VENUE::CME
//                                                                       ? orderrouter::ROUTE_OPTIONS::EXCLUDE_CONVENIENCE_CONNECTIONS
//                                                                       : orderrouter::ROUTE_OPTIONS::NO_OPTIONS_SPECIFIED));
        orderrouter::RoutingSpec spec(contract._venue, contract._sid, orderrouter::ROUTE_OPTIONS::NO_OPTIONS_SPECIFIED);
        return this->context().getRoutingPolicy(spec);
    }

    bool gatherRhoneLimitsAndSendToRV(om::port_gid_t port_gid) {
        std::unordered_map<uint32_t, int64_t> internalIdToLimit;

        for(const auto& kvp : _viperConfig._sidToInternalId) {
            const auto* limit = this->context().getRiskLimits(port_gid, kvp.first);
            if(limit) {
                auto maxOrderQty = limit->params.max_order_qty;
                if (maxOrderQty == 0) {
                    continue;
                }
                auto iter = internalIdToLimit.find(kvp.second);
                if(iter == std::end(internalIdToLimit)) {
                    if(maxOrderQty > std::numeric_limits<int>::max()) {
                        throw std::runtime_error("Discovered limit too large for RV to handle. sid=" + std::to_string(kvp.first) + " limit=" + std::to_string(maxOrderQty));
                    }
                    LOG(0, "EXE {C[i1]}. port_gid={i2} SID={i3} limit={i4}", getContract(kvp.second), port_gid.value(), kvp.first, maxOrderQty)
                    internalIdToLimit[kvp.second] = maxOrderQty;
                }
                else {
                    if(iter->second != maxOrderQty) {
                        throw std::runtime_error("Mismatch in OM limits between enabled ports.  sid=" + std::to_string(kvp.first));
                    }
                }
            }
            else
            {
                logging::GetLogger().logError(boost::str(boost::format("No limit found. pid=%1% sid=%2%") % port_gid.value() % kvp.first));
            }
        }

        //Send the limits
        if(!internalIdToLimit.empty()) {
            _strategies[toUInt(StrategyId::RelativeValue)]->sendMessage<SetRhoneLimits>(
                    ViperMessageType::SetRhoneLimits,
                    [&internalIdToLimit](SetRhoneLimits& limits) {
                        limits._numLimits = internalIdToLimit.size();
                        RhoneLimit* rhoneLimit = limits._limits;
                        for (const auto& limit : internalIdToLimit) {
                            rhoneLimit->_internalId = limit.first;
                            rhoneLimit->_max_order_qty = limit.second;
                            ++rhoneLimit;
                        }
                    });
            return true;
        }
        return false;
    }

    void checkSystemState() {
        static auto marketMaker = getStrategy(StrategyId::MarketMaker);
        SystemState state = marketMaker->state();
        if (state != _oldState) {
            switch (state) {
                case SystemState::HardShutdown: {
                    handleShutdown();
                    break;
                }
                case SystemState::LoggedOut: {
                    handleLogout();
                    break;
                }
                case SystemState::CancelGtcAndDisable: {
                    cancelGtcAndDisable();
                    break;
                }
                default:
                    break;
            }
            _oldState = state;
        }

        auto quoteExt = s_systemState._exchQuoteExternally.load(std::memory_order_relaxed);
        if (quoteExt != _oldQuoteExt)
        {
            onExchangeQuoteExtChange(quoteExt);
            _oldQuoteExt = quoteExt;
        }
    }

    bool isOtherStrategyEnabled(StrategyId excludeStrategy) {
        auto otherStrategyEnabled = false;
        for (auto strategy : _strategies) {
            if (strategy->strategyId() == excludeStrategy) {
                continue;
            }
            otherStrategyEnabled |= strategy->isEnabled();
        }
        return otherStrategyEnabled;
    }

    void onExchangeQuoteExtChange(const SharedSystemState::QuoteExtBitset& quoteExt) {
        LOG(0, "EXE. ExchangeQuoteExternal Switch Changed. {i1} -> {i2}", _oldQuoteExt.to_ulong(), quoteExt.to_ulong())
        for (uint8_t i = 1; i < static_cast<uint8_t>(hf::VENUE::TEST); ++i) {
            if (_oldQuoteExt[i] != quoteExt[i]) {
                sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Enabled),
                                          (quoteExt.test(i) ? proto::SystemState::SystemStateEnabled : proto::SystemState::SystemStateDisabled),
                                          i + toUInt(StrategyId::Execution) * 2);
            }
        }
    }

    void handleShutdown() {
        LOG(0, "Executor - shutting down {i1}", 0)
        hf::Service::exitAll();
    }

    void disableSystem(const std::string& reason) {
        getAllOutOfMarket();
        LOG(0, "EXE. Disable System. {i1}", 0)
        sendErrorMessage(boost::str(boost::format("Disable System from Executor. Reason=%1%") % reason), ErrorType::Critical);
        for (auto strategy : _strategies) {
            strategy->disable();
        }
    }

    void disableSystemViaShutdown(const std::string& reason) {
        LOG(0, "Shutdown System from Executor. {i1}", 0)
        sendErrorMessage(boost::str(boost::format("Shutdown System from Executor. Reason=%1%") % reason), ErrorType::Critical);
        handleShutdown();
    }

    void cancelGtcAndDisable() {
        LOG(0, "Executor. Market Makers disable and cancel GTC. Reason=Requested {i1}", 0)
        getAllOutOfMarket(true);
        sendErrorMessage("Executor. Market Makers disabled. Removing als GTC orders. Reason=Requested", ErrorType::Critical);
    }

    void handleStrategyDisabled(StrategyId strategy, bool skipSync = false) {
        for (auto& state : _contractStates) {
            if (state._contract._toMarketMake) {
                state._ime.getStrategyOutOfMarket(strategy, false, false, skipSync);
            }
        }
    }

    void handleSystemDisabled() {
        disableSystem("Logout");
    }

    void getAllOutOfMarket(bool cancelGtc = false) {
        LOG(0, "Executor. Removing all orders from market. {i1}", 0)
        for (auto& state : _contractStates) {
            if (state._contract._toMarketMake) {
                state._ime.getAllOutOfMarket(cancelGtc);
            }
        }

        if(cancelGtc) {
            _slow._iopTracking.cancelAllGTCs();
        }
    }

    void getStrategyOutOfMarket(StrategyId strategyId, uint32_t internalId = INVALID_INTERNAL_ID, bool disableUseGTC = false) {
        if (internalId == INVALID_INTERNAL_ID) {
            LOG(0, "EXE. Removing all strategies' orders from market. strategyId={i1}", toUInt(strategyId))
            for (auto& state : _contractStates) {
                if (state._contract._toMarketMake) {
                    state._ime.getStrategyOutOfMarket(strategyId, disableUseGTC);
                }
            }
        } else {
            LOG(0, "EXE {C[i1]}. Removing strategies' orders from market. strategyId={i2}", getContract(internalId), toUInt(strategyId))
            auto& state = _contractStates[internalId];
            if (state._contract._toMarketMake) {
                state._ime.getStrategyOutOfMarket(strategyId, disableUseGTC);
            }
        }
    }

    void subscribeToMarketData(md::queue::MDQueue* queue) {
        bool proxyIdSet = queue->trySetProxyIdentifier(_id);
        if (!proxyIdSet) {
            throw std::runtime_error("Could not set proxy id on MD.");
        }

        int port;
        bool portOk = queue->tryGetMonitorPort(port);
        if (!portOk) {
            throw std::runtime_error("Could not get port from MD.");
        }

        int fd = hf::util::SocketTCPUtils::createSocketREMOTE("127.0.0.1", false, hf::util::SocketUtils::Acceleration::DISABLED);

        if (fd == -1) {
            throw std::runtime_error("MM could not create socket to MD");
        }

        if (-1 == hf::util::SocketTCPUtils::connect(fd, "127.0.0.1", port)) {
            throw std::runtime_error("MM could not connect to MD socket");
        }

        sendMessageToMDSocket(fd, _id);

        for (const auto& contract : _viperConfig._allContracts) {
            this->context().subscribeMD(contract._venue, contract._sid, 1,true, true);
//            struct md::request::MDBookRequestMsg bookRequestMsg (_id, contract._venue, contract._sid, 1, true, true);
//            sendMessageToMDSocket(fd, bookRequestMsg);
        }
    }

    template <typename MessageT>
    void sendMessageToMDSocket(int fd, MessageT& message) {
        const auto len = sizeof(MessageT);
        const auto bytesSent = send(fd, &message, len, MSG_NOSIGNAL);
        if (BOOST_UNLIKELY(bytesSent != len)){
            throw std::runtime_error("Could not send message to MD from RV");
        }
    }

    // Strategy MD event callbacks which we are not used directly
    void onTradeCleanedMDUpdate(const md::MDUpdateEvent &/*event*/, const md::MDBatch &/*batch*/) {}
    void onMDTradeDetails(const md::MDTradeDetailsEvent &/*event*/, const md::MDBatch &/*batch*/) {}
    void onEndTradeSummary(const md::MDEndTradeSummary &/*event*/, const md::MDBatch &/*batch*/) {}
    void onMDAddOrder(const md::MDAddOrderEvent &/*event*/, const md::MDBatch &/*batch*/) {}
    void onMDSnapshotOrders(const md::MDSnapshotOrdersEvent &/*event*/, const md::MDBatch &/*batch*/) {}
    void onMDDeleteOrder(const md::MDDeleteOrderEvent &/*event*/, const md::MDBatch &/*batch*/) {}
    void onMDModifyOrder(const md::MDModifyOrderEvent &/*event*/, const md::MDBatch &/*batch*/) {}
    void onMDModifyOrderWithNewID(const md::MDModifyOrderWithNewIDEvent &/*event*/, const md::MDBatch &/*batch*/) {}
    void onMDLevelChange(const md::MDLevelChangeEvent &/*event*/, const md::MDBatch &/*batch*/) {}
    void onMDLevelClear(const md::MDLevelClearEvent &/*event*/, const md::MDBatch &/*batch*/) {}
    void onMDLevelDelete(const md::MDLevelDeleteEvent &/*event*/, const md::MDBatch &/*batch*/) {}
    void onMDLevelDeleteFrom(const md::MDLevelDeleteFromEvent &/*event*/, const md::MDBatch &/*batch*/) {}
    void onMDLevelNew(const md::MDLevelNewEvent &/*event*/, const md::MDBatch &/*batch*/) {}
    void onMDLevelOverlay(const md::MDLevelOverlayEvent &/*event*/, const md::MDBatch &/*batch*/) {}
    void onMDLimitPrice(const md::MDLimitPriceEvent &/*event*/, const md::MDBatch &/*batch*/) {}
    void onMDVolumes(const md::MDVolumesEvent &/*event*/, const md::MDBatch &/*batch*/) {}
    void onMDSettlementPublished(const md::MDSettlementPublishedEvent &/*event*/, const md::MDBatch &/*batch*/) {}
    void onMDChannelStateChange(const md::MDChannelStateChangeEvent &/*event*/, const md::MDBatch &/*batch*/) {}
    void onMDTransactionEvent(const md::MDTransactionEvent &/*event*/, const md::MDBatch &/*batch*/) {}
    void onMDQueuePositionUpdate(const md::MDQueuePositionUpdateEvent &/*event*/, const md::MDBatch &/*batch*/) {}
    void onMDFirmOrderExecutionEventBeforeFillArbitration(const md::MDFirmOrderExecutionEvent &/*event*/, const om::strategy_order_t &/*order*/, const md::MDBatch &/*batch*/) {}
    void onMDFirmOrderExecutionEventWonFillArbitration(const md::MDFirmOrderExecutionEvent &/*event*/, const om::strategy_order_t &/*order*/, const md::MDBatch &/*batch*/) {}
    void onMDFirmOrderExecutionEventLostFillArbitration(const md::MDFirmOrderExecutionEvent &/*event*/, const om::strategy_order_t &/*order*/, const md::MDBatch &/*batch*/) {}

#ifdef IS_DEV
    void onPreMDBatch(const md::MDBatch &batch) override {
        DLOG(0, "EXE received MD batch. bid={i1} extSeqNum={i2} venue={i3}", batch.bid, batch.extSequenceNumber,
             toUInt(batch.venue))
    }
#endif

    void onMDUpdate(const md::MDUpdateEvent& event, const md::MDBatch &batch) {
        const uint32_t internalId = sidToInternalId(event.sid);
        if (internalId == INVALID_INTERNAL_ID) {
            return;
        }

        ContractState& state = _contractStates[internalId];
        if (!state._contract._toMarketMake) {
            return;
        }

        const bool bidChanged = event.buy.updatedQtyNum == 1;
        const bool askChanged = event.sell.updatedQtyNum == 1;
        auto& bid = event.buy.top.l[0];
        auto& ask = event.sell.top.l[0];
        if(event.cleanedLevelRemoved != hf::NULL_PRICE) {
            DLOG(0, "EXE: Contract {C[i1]} Level cleaned from book.  Bid book adjustment due to trade. px={i2} oldBid:{i3}@{i4} newBid:{i5}@{i6} oldOffer:{i7}@{i8} offer:{i9}@{i10} seq={i11}",
                 getContract(internalId), event.cleanedLevelRemoved, state._books[BID]._otherQuantity, state._books[BID]._price, bid.q, bid.pr, state._books[ASK]._otherQuantity, state._books[ASK]._price,
                 ask.q, ask.pr, batch.extSequenceNumber);
        }
        copyBooks(state, bid.pr, bid.q, bid.o, ask.pr, ask.q, ask.o, bidChanged, askChanged);
        if (bidChanged | askChanged) {
            _updatedContracts.insert(internalId);
        }
    }

    void onMDTrade(const md::MDTradeEvent &event, const md::MDBatch &batch) {
        if (batch.venue == hf::VENUE::CME) {
            for (uint32_t i = 0; i < event.num; ++i) {
                auto& trade = event.trades[i];
                const uint32_t internalId = _viperConfig.getInternalId(event.sid);
                if (internalId != INVALID_INTERNAL_ID) {
                    ContractState& state = _contractStates[internalId];
                    if (!state._contract._toMarketMake) {
                        continue;
                    }
                    if (trade.side == hf::SIDE::BUY) {
                        //LOG(0, "EXE: Contract {C[i1]} Bid book adjustment due to trade. {i2}@{d1}", getContract(internalId), trade.q, ToDollars(trade.pr))
                        copyBook(state._books[BID], trade.pr, trade.q, trade.ext2.cme_python_book_cleaning.orderCount);
                        _updatedContracts.insert(internalId);
                    } else if (trade.side == hf::SIDE::SELL) {
                        //LOG(0, "EXE: Contract {C[i1]} Ask book adjustment due to trade. {i2}@{d1}", getContract(internalId), trade.q, ToDollars(trade.pr))
                        copyBook(state._books[ASK], trade.pr, trade.q, trade.ext2.cme_python_book_cleaning.orderCount);
                        _updatedContracts.insert(internalId);
                    }
                }
            }
        }
    }

    void onMarketDataChange(uint32_t internalId) {
        ContractState& state = _contractStates[internalId];
        if (state._contract._toMarketMake) {
            if(BOOST_UNLIKELY(!state._ime._contractCanTrade)) {
                _slow._iopTracking.onMDUpdateDuringAuction(internalId, state._books[BID]._price, state._books[ASK]._price);
            }
        }
    }

    void copyBooks(ContractState& state, hf::price_t bidPrice, hf::qty_t bidSize, md::order_count_t bidOrderCount,
                   hf::price_t askPrice, hf::qty_t askSize, md::order_count_t askOrderCount, bool bidChanged, bool askChanged) {
        if (bidChanged) {
            copyBook(state._books[BID], bidPrice, bidSize, bidOrderCount);
        }
        if (askChanged) {
            copyBook(state._books[ASK], askPrice, askSize, askOrderCount);
        }
    }

    void copyBook(Book& book, hf::price_t price, hf::qty_t size, md::order_count_t orderCount) {
        book._price = price;
        book._otherQuantity = size;
        book._otherOrderCount = orderCount;
    }

    void onMDGap(const md::MDGapEvent &event, const md::MDBatch &batch) {
        disableSystem(boost::str(boost::format("MD feed gapped. venue=%1%") % batch.venue));

        for(unsigned i = 0; i < _viperConfig._allContracts.size(); ++i) {
            const auto& sym = _viperConfig._allContracts[i];
            std::string channelName = GetMDChannel(sym._venue, this->context().getSymbolInfo(sym._sid));
            //event.channelName is not necessarily null terminated
            auto bufferSize = sizeof(event.channelName)/sizeof(event.channelName[0]);
            auto eventChannelNameLength = ::strnlen(event.channelName, bufferSize);
            std::string eventChannelName = std::string{event.channelName, event.channelName + eventChannelNameLength};

            bool isMatch = [&]() {
                if(channelName == eventChannelName) { return true; }
                if(bufferSize == eventChannelNameLength || bufferSize == eventChannelNameLength - 1) {
                    return channelName.find(eventChannelName) != std::string::npos;
                }
                return false;
            }();

            if(isMatch)
            {
                if(!std::strncmp(channelName.c_str(), event.channelName, eventChannelNameLength)) {
                    _slow._potentiallyGapped.set(i);
                    s_systemState._inMDGap.set(sym._venue);
                }
            }
        }

        //Explicitly disabling external quoting for *all* exchanges, not just the gapped ones
        s_systemState._exchQuoteExternally.store(0, std::memory_order_release);
        for(auto* strategy : _strategies) {
            strategy->disableQuoteExternal();
        }
    }

    void onMDStatus(const md::MDStatusEvent &event, const md::MDBatch &batch) {
        const uint32_t internalId = sidToInternalId(event.sid);
        if (internalId == INVALID_INTERNAL_ID) {
            return;
        }

        onMDStatus(batch.venue, batch.extSequenceNumber, event, internalId);
    }

    void handleMDRecoveryDuringGapIfNeeded(hf::VENUE venue, hf::symbol_id_t sid) {
        if(_slow._potentiallyGapped.any()) {
            auto internalIdIter = _viperConfig._sidToInternalId.find(sid);
            if(internalIdIter != _viperConfig._sidToInternalId.end() && _slow._potentiallyGapped.test(internalIdIter->second)) {
                _slow._potentiallyGapped.reset(internalIdIter->second);
                for(unsigned i = 0; i < _slow._potentiallyGapped.size(); ++i) {
                    if(_slow._potentiallyGapped.test(i) && _viperConfig._allContracts[i]._venue == venue) {
                        return;
                    }
                }
                sendErrorMessage("All gapped contracts recovered for venue=" + hf::VENUE_NAMES.at(static_cast<unsigned>(venue)), ErrorType::Critical);
                s_systemState._inMDGap.reset(venue);
            }
        }
    }

    void onMDStatus(hf::VENUE venue, uint64_t extSeqNum, const md::MDStatusEvent& event, uint32_t internalId) {
        ContractState& state = _contractStates[internalId];
        if (!state._contract._toMarketMake) {
            if(event.statusType == md::MDStatusEvent::TYPE::RECOVERED) {
                handleMDRecoveryDuringGapIfNeeded(venue, event.sid);
            }
            return;
        }

        LOG(0, "EXE {C[i1]}. onMDStatus: event.venue={i2} contract.venue={i3} sid={i4} internalId={i5}/{i6} statusType={i7} extStatus={i8} cme.securityTradingEvent={i9}",
            getContract(internalId), toUInt(venue), toUInt(state._contract._venue), event.sid,
            internalId, state._ime._internalId, toUInt(event.statusType), static_cast<int>(event.extStatus),
            static_cast<int>(event.ext2.cme.securityTradingEvent))

        bool needToNotify = _oldMDStatus[static_cast<uint8_t>(venue)] != event.extStatus;
        _oldMDStatus[static_cast<uint8_t>(venue)] = event.extStatus;
        switch (event.statusType) {
            case md::MDStatusEvent::TYPE::RECOVERING:
            case md::MDStatusEvent::TYPE::TIMEOUT:
            case md::MDStatusEvent::TYPE::NOT_CONFIGURED: {
                LOG(0, "Contract {C[i1]} market data not working. Reason={i2}", getContract(internalId), toUInt(event.statusType))
                state._ime._contractCanTrade = false;
                state._ime._contractCanCancel = true;
                if (needToNotify) {
                    sendErrorMessage(boost::str(boost::format("Contract %1% market data not working. Reason=%2%") % getContractName(internalId) % toUInt(event.statusType)), ErrorType::Critical);
                }
                break;
            }
            case md::MDStatusEvent::TYPE::RECOVERED: {
                if (canExchangeQuoteExternally(venue)) {
                    changeExchangeExternalQuoting(venue, SystemState::Disabled);
                    sendErrorMessage(boost::str(boost::format("Disable %1% external quoting. MDStatus=RECOVERED Reason=%2% changed from NotTradable to Tradable") % ven::VenueUtils::getName(venue) % state._ime._name), ErrorType::Critical);
                }
                handleMDRecoveryDuringGapIfNeeded(venue, event.sid);
                state._ime._contractCanTrade = true;
                state._ime._contractCanCancel = true;
                LOG(0, "Contract {C[i1]} market data recovered.", getContract(internalId))
                break;
            }
            case md::MDStatusEvent::TYPE::TRADING_STATUS: {
                switch (venue) {
                    case hf::VENUE::CME:
                        handleCMEMDStatus(internalId, extSeqNum, event, state);
                        break;
                    case hf::VENUE::ICE:
                        handleICEMDStatus(internalId, event, state);
                        break;
                    default:
                        break;
                }
                break;
            }
            case md::MDStatusEvent::TYPE::UNKNOWN: {
                LOG(0, "Contract {C[i1]} market data - received unknown md status.", getContract(internalId))
                sendErrorMessage(boost::str(boost::format("Contract %1% market data - received unknown md status.") % state._ime._name), ErrorType::Critical);
                break;
            }

                // remove compile warning
            case md::MDStatusEvent::TYPE::UNRECOVERABLE:
                break;
        }
    }

    void handleICEMDStatus(uint32_t internalId, const md::MDStatusEvent& event, ContractState& state) {
        switch (event.extStatus) {
            case 'O': {
                state._ime._contractCanTrade = true;
                state._ime._contractCanCancel = true;
                sendEndIOPTrackingToFront(internalId);
                LOG(0, "EXE Contract {C[i1]} market data - opened.", getContract(internalId));
                sendErrorMessage(boost::str(boost::format("Contract %1% market data - opened. ICE external quoting save to enable.") % getContractName(internalId)), ErrorType::Normal);
                break;
            }
            case 'C': {
                state._ime._contractCanTrade = false;
                state._ime._contractCanCancel = false;
                LOG(0, "EXE Contract {C[i1]} market data - closed.", getContract(internalId))
                if (canExchangeQuoteExternally(hf::VENUE::ICE)) {
                    changeExchangeExternalQuoting(hf::VENUE::ICE, SystemState::Disabled);
                    sendErrorMessage(boost::str(
                            boost::format("Contract %1% market data - closed. Disable ICE external quoting.") %
                            getContractName(internalId)), ErrorType::Critical);
                } else {
                    sendErrorMessage(boost::str(boost::format("Contract %1% market data - closed.") % getContractName(internalId)), ErrorType::Normal);
                }
                break;
            }
            case 'E': {
                LOG(0, "EXE Contract {C[i1]} market data - expired.", getContract(internalId))
                state._ime._contractCanTrade = false;
                state._ime._contractCanCancel = false;
                if (canExchangeQuoteExternally(hf::VENUE::ICE)) {
                    changeExchangeExternalQuoting(hf::VENUE::ICE, SystemState::Disabled);
                    sendErrorMessage(boost::str(
                            boost::format("Contract %1% market data - expired. Disable ICE external quoting.") %
                            getContractName(internalId)), ErrorType::Critical);
                } else {
                    sendErrorMessage(boost::str(boost::format("Contract %1% market data - expired.") % getContractName(internalId)), ErrorType::Normal);
                }
                break;
            }
            case '1': {
                state._ime._contractCanTrade = false;
                state._ime._contractCanCancel = true;
                LOG(0, "EXE Contract {C[i1]} market data - entered pre-open.", getContract(internalId))
                if (canExchangeQuoteExternally(hf::VENUE::ICE)) {
                    changeExchangeExternalQuoting(hf::VENUE::ICE, SystemState::Disabled);
                    sendErrorMessage(boost::str(
                            boost::format("Contract %1% market data - pre-open. Disable ICE external quoting.") %
                            getContractName(internalId)), ErrorType::Critical);
                } else {
                    sendErrorMessage(boost::str(boost::format("Contract %1% market data - pre-open.") % getContractName(internalId)), ErrorType::Normal);
                }
                break;
            }
            default: {
                break;
            }
        }
    }

    void handleCMEMDStatus(uint32_t internalId, uint64_t extSeqNum, const md::MDStatusEvent& event, ContractState& state) {
        switch (event.extStatus) {
            case 17: {
                state._ime._contractCanTrade = true;
                state._ime._contractCanCancel = true;
                sendEndIOPTrackingToFront(internalId);
                LOG(0, "EXE {C[i1]}. market data - opened.", getContract(internalId))
                sendErrorMessage(boost::str(boost::format("Contract %1% market data - opened. CME ExternalQuoting safe to enabled") % getContractName(internalId)), ErrorType::Normal);
                break;
            }
            case 2: {
                state._ime._contractCanTrade = false;
                state._ime._contractCanCancel = true;
                LOG(0, "EXE {C[i1]}. market data - halted.", getContract(internalId))
                if (canExchangeQuoteExternally(hf::VENUE::CME)) {
                    changeExchangeExternalQuoting(hf::VENUE::CME, SystemState::Disabled);
                    sendErrorMessage(boost::str(
                            boost::format("Contract %1% market data - halted. Disable CME external quoting.") %
                            getContractName(internalId)), ErrorType::Critical);
                } else {
                    sendErrorMessage(boost::str(boost::format("Contract %1% market data - halted.") % getContractName(internalId)), ErrorType::Normal);
                }
                break;
            }
            case 18:
            case 4: {
                state._ime._contractCanTrade = false;
                state._ime._contractCanCancel = false;
                LOG(0, "EXE {C[i1]}. market data - closed({i2}).", getContract(internalId), (uint32_t)(event.extStatus))
                if (canExchangeQuoteExternally(hf::VENUE::CME)) {
                    changeExchangeExternalQuoting(hf::VENUE::CME, SystemState::Disabled);
                    sendErrorMessage(boost::str(
                            boost::format("Contract %1% market data - closed(%2%). Disable CME external quoting.") %
                            getContractName(internalId) % (uint32_t) event.extStatus),
                            ErrorType::Critical);
                } else {
                    sendErrorMessage(boost::str(boost::format("Contract %1% market data - closed(%2%).") % getContractName(internalId) % (uint32_t)event.extStatus), ErrorType::Normal);
                }
                break;
            }
            case 21: {
                state._ime._contractCanTrade = false;
                state._ime._contractCanCancel = event.ext2.cme.securityTradingEvent != 1;
                LOG(0, "EXE {C[i1]}. market data - entered pre-open. canCancel {i2}", getContract(internalId), state._ime._contractCanCancel)
                if (canExchangeQuoteExternally(hf::VENUE::CME)) {
                    changeExchangeExternalQuoting(hf::VENUE::CME, SystemState::Disabled);
                    sendErrorMessage(boost::str(
                            boost::format("Contract %1% market data - pre-open. Disable CME external quoting.") %
                            getContractName(internalId)), ErrorType::Critical);
                } else {
                    sendErrorMessage(boost::str(boost::format("Contract %1% market data - pre-open.") % getContractName(internalId)), ErrorType::Normal);
                }

                //0=Velocity Halt, 4=StartOfSession
                if(event.ext2.cme.securityTradingEvent == 0 || event.ext2.cme.securityTradingEvent == 4) {
                    if(_slow.lastForceReconcileSeqNum != extSeqNum) {
                        _slow._iopTracking.forceReconcileGTCs();
                        _slow.lastForceReconcileSeqNum = extSeqNum;  //All the MD status for a product group should be triggered by the group event in the same packet
                    }
                }
                break;
            }
            default: {
                break;
            }
        }
    }

    void onMDIntraEventSweep(const md::MDIntraEventSweepEvent& event) {
        const uint32_t internalId = _viperConfig.getInternalId(event.sid);
        if (BOOST_LIKELY(internalId != INVALID_INTERNAL_ID)) {
            const auto& state = _contractStates[internalId];
            const auto& sweepLimits = state._sweepDefinition;
            if(sweepLimits.sweepDetected(event)) {
                LOG(0, "RV {C[i1]}. Sweep detected, disabling RV, disabling ExternalQuote. PriceLimit={i2} QtyLimit={i3} event.diff={i4} event.qty={i5}",
                    getContract(internalId), sweepLimits._sweepTicksAtPrice, sweepLimits._tradedQty, event.diff, event.qty)

                LOG(0, "EXE {C[i1]}. Sweep detected, disabling all strategies except MM, disabling ExternalQuote.", getContract(state._contract._internalId))
                if(BOOST_LIKELY( disableAllStrategyExceptMarketMakerDisableExternalQuotingIfEnabled(state._contract._venue))) {
                    sendErrorMessage("Sweep detected, disabling all strategies except MM, disabling ExternalQuote.  Contract=" + getContractName(state._contract._internalId) + " PriceLimit=" +
                                     std::to_string(sweepLimits._sweepTicksAtPrice) + " QtyLimit=" + std::to_string(sweepLimits._tradedQty) +
                                     " event.diff=" + std::to_string(event.diff) + " event.qty=" + std::to_string(event.qty), ErrorType::Critical);
                }
            }
        }
    }

    void onMDPreOpenPrice(const md::MDPreOpenPriceEvent &event, const md::MDBatch &/*batch*/) {
        _slow._iopTracking.onIOPUpdate(event);
    }

    void onMDUnknown(const md::MDEvent &mde, const md::MDBatch &batch) {
        if (mde.type == md::MDEvent::TYPE::INTRA_EVENT_SWEEP) {
            const auto& event = static_cast<const md::MDIntraEventSweepEvent&>(mde);
            onMDIntraEventSweep(event);
        }
    }

    void onMDStatistics(const md::MDStatisticsEvent &event, const md::MDBatch &/*batch*/) {
        for(int i = 0; i < static_cast<int>(event.num); ++i) {
            const auto& stat = event.statistics[i];
            if((stat.type == '4') & (stat.action == 0) & (stat.openCloseSettlFlag == 5)) {
                _slow._iopTracking.onIOPUpdate(event.sid, stat.price, stat.price);
            }
        }
    }

    // Order Events
    FillProcessingStatus onOMOrderExecutionBeforeFillArbitration(const om::OMExecutionEvent& event, const om::strategy_order_t& order, const om::OMBatch& batch) {
        if (!event.owned_by_strategy) {
            return FillProcessingStatus::FillNotAccepted;
        }

        return FillProcessingStatus::FillAccepted;
    }
    void onOMOrderExecutionWonFillArbitration(const om::OMExecutionEvent& event, const om::strategy_order_t& order, const om::OMBatch& batch) {
        const_cast<om::order_t&> (order).action_flags.is_active = event.is_active; //Is this set on full fill?
        const_cast<om::order_t&> (order).executedQty = event.totalExecutedQty;
        //Note that leavesqty is set in various places around the code.  One day we should clean it up and just set it here.
        onOrderExecution(event, order);
    }

    void onOMOrderExecutionLostFillArbitration(const om::OMExecutionEvent& /*event*/, const om::strategy_order_t& /*order*/, const om::OMBatch& /*batch*/) {}
    FillProcessingStatus onOrderSpreadLegExecution(const om::OMSpreadLegExecutionEvent &/*event*/, const om::strategy_order_t &/*order*/, const om::OMBatch& /*batch*/) { return FillNotAccepted; }
    void onMassCancelAccepted(const om::OMMassCancelAcceptedEvent &/*event*/, const om::OMBatch& /*batch*/) {}
    void onMassCancelRejected(const om::OMMassCancelRejectedEvent &/*event*/, const om::OMBatch& /*batch*/) {}
    void onTraderLogInOutResponse(const om::TraderLogInOutResponseEvent &/*event*/, const om::OMBatch& /*batch*/) {
        printf("****\n");
    }

    // TODO: Kun
    void onRecoveredExecution(const om::OMRecoveredExecution& event, const om::OMBatch& /*batch*/) {
        LOG(0, "onRecoveredExecution venue={i1}", toUInt(event.venue))
    }

    void onOrdersTransferredToRisk(const om::OMOrdersTransferredToRiskEvent& /*transferAck*/, const om::OMBatch& /*batch*/) {}
    void onRequestQueueUsageExceeded(const om::OMRequestQueuePoolUsageExceedRejectEvent &event, const om::OMBatch& /*batch*/) {
        sendErrorMessage(
                boost::str(boost::format("OM request queue %1% usage exceeded. usage=%2%") % event.name % event.usage),
                ErrorType::Critical);
    }
    void onRequestPoolUsageExceeded(const om::OMRequestQueuePoolUsageExceedRejectEvent &event, const om::OMBatch& /*batch*/) {
        sendErrorMessage(
                boost::str(boost::format("OM request pool %1% usage exceeded. usage=%2%") % event.name % event.usage),
                ErrorType::Critical);
    }
    void onUnhandledOMEvent(const om::OMEvent &/*event*/, const om::OMBatch& /*batch*/) {}

    void onMDMonitorStatusUpdate(const om::MDMonitorStatusUpdate &event) override {
        if (!event.active) {
            sendErrorMessage("MD/OM died - killing viper.", ErrorType::Critical);
            kill(getpid(), SIGKILL);
        }
    }

    virtual void onStrategyLoginResponse(const om::StrategyLoginResponseEvent &event, const om::OMBatch& batch) {
        logging::GetLogger().logInfo(boost::str(
                boost::format("Received Strategy login response. Success=%1% port_gid=%2% AlgoId=%3%") % event.success %
                event.port_gid.value() % event.algo_id.value()));

        //If the gateway was already connected prior to launching Viper, we will not get a port update status so we will explicitly update the Rhone limits here.  In case we are not connected yet,
        //this becomes a no-op since all get(...) calls will return nullPtr and structure will be properly populated on the {GW,Port}StatusUpdate messages.
        _rhoneLimitsReady |= gatherRhoneLimitsAndSendToRV(event.port_gid);

        if (!_pendingGatewayLogin.empty()) {
            auto it = _pendingGatewayLogin.find(event.port_gid.value());
            if (it != _pendingGatewayLogin.end()) {
                _pendingGatewayLogin.erase(it);
            }

            if (_pendingGatewayLogin.empty()) {
                static bool logged = false;
                if (!logged) {
                    logged = true;
                    using ExecutorSpace::s_log;
                    LOG_INFO("All Gateway Logged in");

                    // On Rhone 3.55 rhone limits are not read yet
                    // Start a time to gather rhone limits later
                    if (_rhoneLimitsReady) {
                        // Hack: startViper.sh looking for "Wanted to send GridUpdate but do not have connection" in rhone.log
                        // The problem is that the string is not always logged.
                        // Log a hack string here so that the startViper.sh script can reliably pass
                        LOG_INFO("Wanted to send GridUpdate but do not have connection. Hack for startViper.sh backward compatibility");
                    }
                }
            }
        }
    }

    void onOMGWStatusUpdate(const om::GWStatusUpdate& event) override {
        logging::GetLogger().logInfo(boost::str(boost::format("onOMGWStatusUpdate. port_gid=%1% active=%2%") % event.port_gid.value() % event.active));

        handleGatewayStatusChange(event);

        if (!event.active && _strategies[toUInt(StrategyId::MarketMaker)]->isEnabled()) {
            sendErrorMessage("OMS not running - killing viper.", ErrorType::Critical);
            kill(getpid(), SIGKILL);
        }
    }

    void onOMPortStatusUpdate(const om::PortStatusUpdate& event, const om::OMBatch& /*batch*/) {
        handleGatewayStatusChange(event);
    }

    template <typename T>
    void handleGatewayStatusChange(T& event) {
        _gatewayStatus._active = event.active;
        _gatewayStatus._message = event.reasonText;
        hf::VENUE venue = getGatewayVenue(event.port_gid.value());

        if (getStrategy(StrategyId::MarketMaker)->state() != SystemState::LoggedOut) {
            sendGatewayStatusToFront();
            std::string message = boost::str(
                    boost::format("%1% Gateway status change. port_gid=%2% Running=%3%") % ven::VenueUtils::getName(venue) %
                    event.port_gid.value() % (event.active ? "YES" : "NO"));
            sendErrorMessage(message, ErrorType::Critical);
        }

        LOG(0, "EXE: {i1} gateway status changed. port_gid={i2} Active={i2}", static_cast<uint32_t>(venue), event.port_gid.value(), _gatewayStatus._active)
        for (auto& state : _contractStates) {
            if (state._contract._venue == venue) {
                state._ime._gatewayActive = _gatewayStatus._active;
            }
        }

        if (!_gatewayStatus._active) {
            _strategies[toUInt(StrategyId::RelativeValue)]->disable();
            changeExchangeExternalQuoting(venue, SystemState::Disabled);
            std::string message = boost::str(boost::format("%1% OMS not running. Disable RV and %1% ExternalQuoting.") % ven::VenueUtils::getName(venue));
            sendErrorMessage(message, ErrorType::Critical);
        }
        else {
            bool notifyViperReady = !_rhoneLimitsReady;
            _rhoneLimitsReady |= gatherRhoneLimitsAndSendToRV(event.port_gid);

            if (notifyViperReady) {
                using ExecutorSpace::s_log;
                // Hack: startViper.sh looking for "Wanted to send GridUpdate but do not have connection" in rhone.log
                // The problem is that the string is not always logged.
                // Log a hack string here so that the startViper.sh script can reliably pass
                LOG_INFO("Wanted to send GridUpdate but do not have connection. Hack for startViper.sh backward compatibility");
            }
        }
    }

    hf::VENUE getGatewayVenue(om::port_id_t pid) {
        auto iter = _mapGatewayVenue.find(pid);
        if (BOOST_LIKELY(iter != _mapGatewayVenue.end())) {
            return iter->second;
        }
        return hf::VENUE::UNKNOWN;
    }

    void sendGatewayStatusToFront() {
        sendMessageToFront<GatewayStatusMessage>(
                ViperMessageType::GatewayStatus,
                [this](GatewayStatusMessage& message) {
                    message._active = _gatewayStatus._active;
                    if (!_gatewayStatus._message.empty()) {
                        strcpy(message._message, _gatewayStatus._message.c_str());
                    }
                });
    }

    void handleGuiInitialized() {
        sendGatewayStatusToFront();
        this->context().traderConnect("", _iid);
    }

    bool isFAKHedge(const om::order_t& order) {
        return order.ot == om::ORDER_TYPE::FAK;
    }

    uint32_t getInternalId(const om::order_t& order) {
        return order.debug.i[INTERNAL_ID];
    }

    StrategyId getStrategyId(const om::order_t& order) {
        assert(order.debug.i[ORDER_OWNER] > UNASSIGNED_OWNER);
        return (StrategyId)(order.debug.i[ORDER_OWNER] - UNASSIGNED_OWNER);
    }

    bool isLoggedIn() {
        return _strategies[toUInt(StrategyId::MarketMaker)]->state() != SystemState::LoggedOut;
    }

    void onOrderAccepted(const om::OMOrderAcceptedEvent& event, const om::strategy_order_t &order, const om::OMBatch& /*batch*/) {
        if (isLoggedIn()) {
            const_cast<om::strategy_order_t&> (order).leavesQty = event.acceptedQty;
            if (!isFAKHedge(order)) {
                auto& ime = getInternalMatchingEngine(order);
                TLOG(0, "OrderAccepted: oid={i1} rhone_oid={i2} exch_oid={i3}", order.oid.value(),
                     order.rhone_oid.value(), order.exchangeOrderId.value())
                ime.onOrderAccepted(event, order);
            }
        } else {
            LOG(0, "Received Order Accepted when logged out. {i1}", event.strat_oid.value())
        }
    }

    void onOrderRejected(const om::OMOrderRejectedEvent &event, const om::strategy_order_t &order, const om::OMBatch& /*batch*/) {
        if (isLoggedIn()) {
            if (isFAKHedge(order)) {
                LOG(0, "Hedge order rejected. Contract={C[i1]} OID={i2} Reason={i3}", getContract(getInternalId(order)), order.oid.value(), event.reasonCodeExt)
                if (IsSelfMatchReject(order.venue, event.reasonCodeExt)) {
                    ++_rvHedgeRejectCount;
                    if (_rvHedgeRejectCount == RV_HEDGE_REJECT_COUNT_LIMIT) {
                        handleFAKHedgeRejectLimitBreach(order);
                    } else {
                        sendReverseFillToStrategy(getStrategyId(order), getInternalId(order), order, FillSource::SelfMatch);
                    }
                } else {
                    reverseFAKHedgeFillToMarketMaker(order);
                    if (BOOST_LIKELY(!IsRejectDueToMarketClosed(order.venue, event.reasonCodeExt))) {
                        disableContractExternalQuoteOnAllStrategy(getInternalId(order), boost::str(boost::format("RV hedge reject. Code=%1% Text=%2%") % event.reasonCodeExt % event.reasonText));
                    }
                }
            } else {
                auto& ime = getInternalMatchingEngine(order);
                ime.onOrderRejected(event, order);
            }
        } else {
            LOG(0, "Received Order Rejected when logged out. {i1}", event.strat_oid.value())
        }
    }

    void handleFAKHedgeRejectLimitBreach(const om::order_t& order) {
        _rvHedgeRejectCount = 0;
        _strategies[toUInt(getStrategyId(order))]->disable();
        moveSelfMatchRejectToMarketMaker(order);
    }

    bool reverseFAKHedgeFillToMarketMaker(const om::order_t& order) {
        const uint32_t internalId = getInternalId(order);
        const hf::qty_t unfilledSize = order.qty - order.executedQty;
        const uint32_t reverseSide = order.side == hf::SIDE::BUY ? 1 : 0;
        auto marketMaker = getStrategy<MarketMaker>(StrategyId::MarketMaker);
        auto fillId = marketMaker->beginFillProcess(internalId);
        bool withinPositionLimits = marketMaker->forceTakeOverFill(internalId, reverseSide, order.pr, unfilledSize, FillSource::RvHedgeReject);
        onEndOfFills(internalId, reverseSide, fillId, false);
        return withinPositionLimits;
    }

    void moveSelfMatchRejectToMarketMaker(const om::order_t& order) {
        if (!reverseFAKHedgeFillToMarketMaker(order)) {
            auto internalId = getInternalId(order);
            disableMMExternalQuoting(internalId);
        }
    }

    void onOrderReplaced(const om::OMOrderModifiedEvent &event, const om::strategy_order_t &order, const om::OMBatch& /*batch*/) {
        if (isLoggedIn()) {
            auto& ime = getInternalMatchingEngine(order);
            TLOG(0, "OrderReplaced: oid={i1} rhone_oid={i2} exch_oid={i3}({i4})", order.oid.value(),
                 order.rhone_oid.value(), order.exchangeOrderId.value(), __bswap_64(order.exchangeOrderId.value()))
            ime.onOrderReplaced(event, order);
        } else {
            LOG(0, "Received Order Replaced when logged out. {i1}", event.strat_oid.value())
        }
    }

    void onOrderReplaceRejected(const om::OMOrderModifyRejectedEvent &event, const om::strategy_order_t &order, const om::OMBatch& /*batch*/) {
        if (isLoggedIn()) {
            auto& ime = getInternalMatchingEngine(order);
            ime.onOrderReplaceRejected(event, order);
            ++ime._messages._modifyRejects;
        } else {
            LOG(0, "Received Order Replace Rejected when logged out. {i1}", event.strat_oid.value())
        }
    }

    void onOrderExecution(const om::OMExecutionEvent &event, const om::order_t &order) {
        if(BOOST_UNLIKELY(order.debug.i[ORDER_OWNER] == UNPROCESSABLE_RELOADED_ORDER)) {
            const_cast<om::order_t &>(order).leavesQty -= event.filledQty;
            LOG(0, "EXE Fill for unprocessable GTC.  sid={i1} oid={i2} px={i3} fill_qty={i4} cumul_fill_qty={i5] total_qty={i6} leaves_qty={i7}", order.sid, order.oid.value(),
                    event.filledPrice, event.filledQty, event.totalExecutedQty, order.qty, order.leavesQty);
            sendErrorMessage(boost::str(boost::format("Fill for unprocessable GTC.  Cancel rejected for unprocessable GTC!  sid=%1% oid=%2% px=%3% fill_qty=%4% cumul_fill_qty=%5% total_qty=%6% leaves_qty=%7%")
                % order.sid % order.oid % event.filledPrice % event.filledQty % event.totalExecutedQty % order.qty % order.leavesQty), ErrorType::Critical);
            return;
        }

        if (BOOST_LIKELY(isLoggedIn())) {
            auto& ime = getInternalMatchingEngine(order);
            if (!isFAKHedge(order)) {
                if (BOOST_UNLIKELY(order.debug.i[ORDER_OWNER] == UNASSIGNED_OWNER)) {
                    const_cast<om::order_t &>(order).leavesQty -= event.filledQty;
                    handleFillForUnassignedGtcOrder(event, ime._internalId, order);
                } else {
                    ime.onOrderExecution(event, order);
                }
            } else {
                const uint32_t internalId = ime._internalId;
                LOG(0, "Hedge order filled. Contract={C[i1]} OID={i2}", getContract(internalId), order.oid.value())
            }
            ime._messages._fills += event.filledQty;
        } else {
            LOG(0, "Received Order Replace Executed when logged out. {i1}", event.strat_oid.value())
        }
    }

    template <typename FillT>
    void handleFillForUnassignedGtcOrder(const FillT& event, uint32_t internalId, const om::order_t &order) {
        BOOST_ASSERT(order.leavesQty >= 0);
        auto marketMaker = getStrategy<MarketMaker>(StrategyId::MarketMaker);
        uint32_t side = order.side == hf::SIDE::BUY ? BID : ASK;
        auto fillId = marketMaker->beginFillProcess(internalId);
        marketMaker->forceTakeOverFill(internalId, side, event.filledPrice, event.filledQty, FillSource::ViperOff);
        onEndOfFills(internalId, side, fillId, false);
        if((!order.action_flags.is_active) || (!order.leavesQty)) {
            getInternalMatchingEngine(order).tryRemoveGtcOrder(order);
            _slow._iopTracking.stopTrackingOrder(internalId, order.oid);
        }
    }

    void onOrderCanceled(const om::OMOrderCanceledEvent& event, const om::order_t& order, const om::OMBatch &/*batch*/) {
        if (BOOST_LIKELY(isLoggedIn())) {
            if (isFAKHedge(order)) {
                auto internalId = getInternalId(order);
                StrategyId strategy = getStrategyId(order);
                LOG(0, "EXE {C[i1]}. Hedge order cancelled. OID={i2}", getContract(internalId), order.oid.value())
                sendReverseFillToStrategy(strategy, internalId, order, FillSource::MissedHedge);
            } else {
                if(BOOST_UNLIKELY(order.debug.i[ORDER_OWNER] == UNPROCESSABLE_RELOADED_ORDER)) {
                    LOG(0, "EXE Received cancel of unprocessable GTC reload.  sid={i1} oid={i2}", order.sid, order.oid.value());
                    sendErrorMessage("Received cancel of unprocessable GTC.  sid=" + std::to_string(order.sid) + " oid=" + std::to_string(order.oid.value()), ErrorType::Normal);
                    return;
                }
                auto& ime = getInternalMatchingEngine(order);
                ime.onOrderCanceled(event, order);
                if (BOOST_UNLIKELY(order.debug.i[ORDER_OWNER] == UNASSIGNED_OWNER)) {
                    _slow._iopTracking.onOrderCancelled(getInternalId(order), order.oid);
                }
                else if (BOOST_UNLIKELY(IsExternalUnsolicitedCancel(order.venue, event.reasonCodeExt, event.reasonText))) {
                    auto internalId = getInternalId(order);
                    LOG(0, "EXE {C[i1]}. Received external cancel. OID={i2} px={i3} leavesQty={i4}", getContract(internalId), order.oid.value(), order.pr, order.leavesQty);
                    sendErrorMessage("Received external cancel.  Contract=" + getContractName(internalId) + " OID=" + std::to_string(order.oid.value()) + " px=" + std::to_string(order.pr) +
                       " leavesQty=" + std::to_string(order.leavesQty), ErrorType::Critical);
                }
            }
        } else {
            LOG(0, "Received Order Cancelled when logged out. {i1}", event.strat_oid.value())
        }
    }

    void onOrderCancelRejected(const om::OMOrderCancelRejectedEvent &event, const om::order_t &order, const om::OMBatch &/*batch*/) {

        if(BOOST_UNLIKELY(order.debug.i[ORDER_OWNER] == UNPROCESSABLE_RELOADED_ORDER)) {
            LOG(0, "EXE Cancel rejected for unprocessable GTC.  sid={i1} oid={i2}", order.sid, order.oid.value());
            sendErrorMessage("Cancel rejected for unprocessable GTC!  sid=" + std::to_string(order.sid) + " oid=" + std::to_string(order.oid.value()), ErrorType::Normal);
            return;
        }

        if (isLoggedIn()) {
            auto& ime = getInternalMatchingEngine(order);
            ime.onOrderCancelRejected(event, order);
            ++ime._messages._cancelRejects;
        } else {
            LOG(0, "Received Order Cancel Rejected when logged out. {i1}", event.strat_oid.value())
        }
    }

    void cancelOrder(uint32_t internalId, const om::order_t* order) {
        DLOG(0, "Cancel Order {C[i1]} OID={i2}", internalId, order->oid.value())
        this->context().cancelOrder(order->oid);
        ContractState& state = _contractStates[internalId];
        ++state._ime._messages._cancels;
    }

    om::order_t* newLimitOrder(uint32_t internalId, uint32_t side, hf::price_t price, hf::qty_t size, om::ORDER_TYPE orderType, bool isHedge = false) {
        ContractState& state = _contractStates[internalId];
        auto connection = state._routingPolicy.getAllConnections()[state._routingPolicy.getNextConnectionIndex()];
        om::order_t* order = this->context().createOrder(_iid, om::RISK_TYPE::ENTERING, state._routingPolicy, orderType,
                                                         ToRhoneSide(side), price, size,
                                                         om::STP_ACTION::CANCEL_PASSIVE);
        const uint64_t now = clockUtil::GetCycles();
        if (BOOST_UNLIKELY(order == nullptr)) {
            LOG(0, "Could not send limit order. Contract={C[i1]} Side={i2} Price={d1} Size={i3}", getContract(internalId), side, ToDollars(price), size)
        } else {
            LOG(0, "Sent limit order. Contract={C[i1]} Side={i2} Price={d1} Size={i3} OID={i4}", getContract(internalId), side, ToDollars(price), size, order->oid.value())
            addRoutingStateToNewLimitOrder(*order, internalId, connection);
            ++state._ime._messages._inserts;
            if (!isHedge) {
                addMessageToThrottler(now, connection);
            }
        }
        applyQtyDiffToOrderThrottleAndCheck(now, internalId, ToRhoneSide(side), price, size);
        return order;
    }

    void addRoutingStateToNewLimitOrder(om::order_t& order, uint32_t internalId) {
        ContractState& state = _contractStates[internalId];
        auto connection = state._routingPolicy.getAllConnections()[state._routingPolicy.getNextConnectionIndex()];
        addRoutingStateToNewLimitOrder(order, internalId, connection);
    }

    void addRoutingStateToNewLimitOrder(om::order_t& order, uint32_t internalId, const strat::om_queue_info_t* connection) {
        order.debug.i[INTERNAL_ID] = internalId;
        order.debug.i[ROUTE] = strat::port_gid(*connection).value();
    }

    void disableMarketMakerDisableQuoteExternalOnOtherStrategies(uint32_t internalId, const std::string& reason) {
        std::string message = boost::str(boost::format("Disabled MM *COMPLETELY*.  Disable RV quote external. Contract=%1% Reason=%2%") % _viperConfig._allContracts[internalId]._name % reason);
        sendErrorMessage(message, ErrorType::Critical);
        LOG(0, "Disabling MM and RV due to rejects. Contract={C[i1]}", getContract(internalId))
        ContractState& state = _contractStates[internalId];
        for (auto strategy : _strategies) {
            if (strategy->strategyId() == StrategyId::MarketMaker) {
                strategy->sendGenericMessage(ViperMessageType::DisableMarketMakerInstrument, internalId);
            } else {
                strategy->sendGenericMessage(ViperMessageType::QuoteExternallyOff, internalId);
            }
        }
        state._ime.getAllOutOfMarket();
    }

    void disableContractExternalQuoteOnAllStrategy(uint32_t internalId, const std::string& reason) {
        std::string message = boost::str(boost::format("Disabled all strategy quote external. Contract=%1% Reason=%2%") % _viperConfig._allContracts[internalId]._name % reason);
        sendErrorMessage(message, ErrorType::Critical);
        LOG(0, "Disabling external quoting on all strategy due to rejects. Contract={C[i1]}", getContract(internalId))

        for (auto strategy : _strategies) {
            strategy->sendGenericMessage(ViperMessageType::QuoteExternallyOff, internalId);
        }

        ContractState& state = _contractStates[internalId];
        state._ime.getAllOutOfMarket();
    }

    void disableMMExternalQuoteWithMessage(uint32_t internalId, const std::string& reason) {
        std::string message = boost::str(boost::format("Disabled MM quote external. Contract=%1% Reason=%2%") %
                                         _viperConfig._allContracts[internalId]._name % reason);
        sendErrorMessage(message, ErrorType::Critical);
        LOG(0, "Disabling external quoting on MM due to rejects. Contract={C[i1]}", getContract(internalId))
        disableMMExternalQuoting(internalId);
    }

    //No message, this is critical enough that we're going to forgo building the string and log manually
    //returns if we took action
    bool disableAllStrategyExceptMarketMakerDisableExternalQuotingIfEnabled(hf::VENUE venue) {
        bool needToNotifyUser = false;
        for (auto strategy : _strategies) {
            if(strategy->strategyId() == StrategyId::MarketMaker) {
                continue;
            } else if (BOOST_LIKELY(strategy->isEnabled())) {
                needToNotifyUser = true;
                strategy->disable();
                handleStrategyDisabled(strategy->strategyId(), true);
            }
        }

        if (BOOST_LIKELY((_strategies[toUInt(StrategyId::MarketMaker)]->isEnabled()) &&
                         s_systemState._exchQuoteExternally.load(std::memory_order_relaxed).any())) {
            needToNotifyUser |= true;
        }

        s_systemState._exchQuoteExternally.store(0, std::memory_order_release);

        if (needToNotifyUser) {
            LOG(0, "EXE. Relative Value Disabled Quote External Disabled. Removing orders. Reason=disableRVDisableMMExternalQuoteForAllContractsIfEnabled {i1}", 0)
            sendErrorMessage("EXE. Relative Value Disabled Quote External Disabled. Removing orders. Reason=disableRVDisableMMExternalQuoteForAllContractsIfEnabled", ErrorType::Critical);
        }

        return needToNotifyUser;
    }

    void disableMMExternalQuoting(uint32_t id) {
        getStrategy(StrategyId::MarketMaker)->sendGenericMessage(ViperMessageType::QuoteExternallyOff, id);
    }

    void notifyFrontUseGTCDisabled(int32_t id) {
        sendGenericMessageToFront(ViperMessageType::DisableUseGTC, id);
    }

    void addMessageToThrottler(uint64_t now, strat::om_queue_info_t* connection) {
        auto port_gid = strat::port_gid(*connection);
        auto it = _throttlers.find(port_gid.value());
        if (it == _throttlers.end()) {
            it = _throttlers.emplace(port_gid.value(), Throttler()).first;

            double percentMsgHedges = boost::lexical_cast<double>(_viperConfig.getGlobal(PERCENT_MSG_HEDGES));
            double percentMsgReduceQuote = boost::lexical_cast<double>(_viperConfig.getGlobal(PERCENT_MSG_REDUCE_QUOTE));
            uint32_t atCapacityPeriodInMics = boost::lexical_cast<uint32_t>(_viperConfig.getGlobal(MSG_HEDGES_INTERVAL_MICS));
            uint64_t atCapacityPeriod = GetCyclesFromMicroSeconds(atCapacityPeriodInMics, _frequency);
            uint32_t inDangerZonePeriodInMics = boost::lexical_cast<uint32_t>(_viperConfig.getGlobal(MSG_REDUCE_QUOTE_MICS));
            uint64_t inDangerZonePeriod = GetCyclesFromMicroSeconds(inDangerZonePeriodInMics, _frequency);

            it->second.initialize(_frequency, port_gid, 1, _transactionCapacity[strat::venue(*connection)], percentMsgReduceQuote, percentMsgHedges, inDangerZonePeriod, atCapacityPeriod);
        }
        addMessageToThrottler(now, it->second);
    }

    void addMessageToThrottler(uint64_t now, om::port_gid_t::value_type port_gid) {
        auto it = _throttlers.find(port_gid);
        if (it != _throttlers.end()) {
            addMessageToThrottler(now, it->second);
        }
    }

    void addMessageToThrottler(uint64_t now, Throttler& throttler) {
        bool isInDangerZone = false;
        bool isAtCapacity = false;
        throttler.addMessage(now, isInDangerZone, isAtCapacity);
        if (isAtCapacity) {
            throttler.setAtCapacity(now);
            setThrottlingBanner(throttler, THROTTLING_MODE_AT_CAPACITY);
        } else {
            if (!throttler.wasInDangerZone() & isInDangerZone) {
                throttler.setInDangerZone(now);
                setThrottlingBanner(throttler, THROTTLING_MODE_DANGER_ZONE);
            }
        }
    }

    om::order_t* newFAKOrder(uint32_t internalId, uint32_t side, hf::price_t price, hf::qty_t size, StrategyId strategy) {
        ContractState& state = _contractStates[internalId];
        om::order_t* order = nullptr;
        order =  this->context().createOrderExt(_iid, om::RISK_TYPE::TRADING, state._routingPolicy, om::ORDER_TYPE::FAK, ToRhoneSide(side), price, 0, size, 1, 0, om::STP_ACTION::CANCEL_PASSIVE);
        if (order == nullptr) {
            LOG(0, "Could not send FAK order. Contract={C[i1]} Side={i2} Price={d1} Size={i3}", getContract(internalId), side, ToDollars(price), size)
        } else {
            LOG(0, "Sent FAK order. Contract={C[i1]} Side={i2} Price={d1} Size={i3} OID={i4}", getContract(internalId), side, ToDollars(price), size, order->oid.value())
            order->debug.i[ORDER_OWNER] = UNASSIGNED_OWNER + toUInt(strategy);
            order->debug.i[INTERNAL_ID] = internalId;
            ++state._ime._messages._inserts;
        }
        applyQtyDiffToOrderThrottleAndCheck(clockUtil::GetCycles(), internalId, ToRhoneSide(side), price, size);
        return order;
    }

    void applyQtyDiffToOrderThrottleAndCheck(uint64_t now, uint32_t internalId, hf::SIDE side, hf::price_t price, hf::qty_t diff) {

        const auto& book = _contractStates[internalId]._books;
        _newOrderModifyOrderRateChecker.onNewOrModifyOrder(now, side, price, diff, book[BID]._price, book[ASK]._price);

        if(BOOST_UNLIKELY(_newOrderModifyOrderRateChecker.isOverLimit())) {
            LOG(0, "EXE NewOrderMofiyOrderRateChecker is over limit.  window(ms)={i1} numClips={i2}/{i3} sumTotalLimit={i4}/{i5}", GetMillisFromCycles(_newOrderModifyOrderRateChecker.throttleWindow(), _frequency),
                _newOrderModifyOrderRateChecker.clipCount(), _newOrderModifyOrderRateChecker.maxClips(), _newOrderModifyOrderRateChecker.sumTotal(), _newOrderModifyOrderRateChecker.sumTotalLimit());

            //Disable all non-MM strategies
            for(auto* strategy : _strategies) {
                if(strategy->strategyId() != StrategyId::MarketMaker) {
                    strategy->disable();
                }
            }

            //Disable all strategy quote external
            for(auto* strategy : _strategies) {
                if(strategy->canQuoteExternally()) {
                    strategy->disableQuoteExternal();
                }
            }

            //Disable all exchange quote external
            s_systemState._exchQuoteExternally.store(0, std::memory_order_release);

            sendErrorMessage("OrderRateChecker is over limit.  Disabling RV + RV/MM/Exchange ext quote. Window(ms)=" + std::to_string(GetMillisFromCycles(_newOrderModifyOrderRateChecker.throttleWindow(), _frequency)) +
                          " NumClips=" + std::to_string(_newOrderModifyOrderRateChecker.clipCount()) + "/" + std::to_string(_newOrderModifyOrderRateChecker.maxClips()) +
                          " SumTotalLimit=" + std::to_string(_newOrderModifyOrderRateChecker.sumTotal()) + "/" + std::to_string(_newOrderModifyOrderRateChecker.sumTotalLimit()), ErrorType::Critical);
        }
    }

    void modifyOrder(uint32_t internalId, const om::order_t* order, hf::price_t price, hf::qty_t size) {
        ContractState& state = _contractStates[internalId];
        const bool done = this->context().modifyOrder(order->oid, price, size, om::STP_ACTION::CANCEL_PASSIVE, om::RISK_TYPE::ENTERING);
        const uint64_t now = clockUtil::GetCycles();
        if (done) {
            ++state._ime._messages._modifies;
            LOG(0, "Sent modify order. Contract={C[i1]} Side={i2} Price={d1} Size={i3}", getContract(internalId), ToViperSide(order->side), ToDollars(price), size)
            addMessageToThrottler(now, order->debug.i[ROUTE]);
        } else {
            LOG(0, "Could not modify order. Contract={C[i1]} Side={i2} Price={d1} Size={i3}", getContract(internalId), ToViperSide(order->side),ToDollars(price), size)
        }
        applyQtyDiffToOrderThrottleAndCheck(now, internalId, order->side, price, size - order->qty);
    }

    void setThrottlingBanner(Throttler& throttler, THROTTLING_MODE mode) {
        for (auto& state : _contractStates) {
            if (state._contract._toMarketMake) {
                if (mode == throttler.getMode()) {
                    state._ime.setThrottlingBanner(mode);
                }
            }
        }
    }

    void onCriticalError(const std::string& reason) {
        std::string message = boost::str(boost::format("Critical Error: %1%") % reason);
        disableSystemViaShutdown(message);
    }

    void addFillBannerTimer(uint64_t expiryTime, uint32_t internalId, uint32_t side) {
        _fillBannerTimer.addBannerEntry(expiryTime, internalId, side, TimerType::FillBanner);
    }

    void addQuoteBannerTimer(uint64_t expiryTime, uint32_t internalId, uint32_t side, TimerType type) {
        _quoteBannerTimer.addBannerEntry(expiryTime, internalId, side, type);
    }

    void checkTimers(uint64_t now) {
        _checkQuotesTimer.expire(now);
        _fillBannerTimer.expire(now);
        _quoteBannerTimer.expire(now);
        for (auto& kvp : _throttlers) {
            Throttler& throttler = kvp.second;
            if (throttler.isBanOn()) {
                if (throttler.didBanExpire(now)) {
                    setThrottlingBanner(throttler, throttler.getNewMode(now));
                }
            }
        }
    }

    void expireTimer(TimerEntry& action) {
        switch (action._type) {
            case TimerType::FillBanner: {
                expireFillBanner(action._internalId, action._side);
                break;
            }
            case TimerType::QuoteBanner: {
                expireQuoteBanner(action._internalId, action._side);
                break;
            }
            case TimerType::HedgeBanner: {
                expireHedgeQuoteBanner(action._internalId, action._side);
                break;
            }
            case TimerType::DoNothing: {
                break;
            }
        }
    }

    void expireHedgeQuoteBanner(uint32_t internalId, uint32_t side) {
        ContractState& state = _contractStates[internalId];
        state._ime.expireHedgeQuoteBanner(side);
    }

    void expireFillBanner(uint32_t internalId, uint32_t side) {
        ContractState& state = _contractStates[internalId];
        state._ime.expireFillBanner(side);
    }

    void expireQuoteBanner(uint32_t internalId, uint32_t side) {
        ContractState& state = _contractStates[internalId];
        state._ime.expireQuoteBanner(side);
    }

    void switchTrader(GridUpdate& update) {
        for (int i = 0; i < update._update.cells_size(); ++i) {
            const proto::GridCell& gridCell = update._update.cells(i);
            TraderSwitchColumns columnType = static_cast<TraderSwitchColumns>(gridCell.col());
            if (columnType == TraderSwitchColumns::Trader) {
                Traders trader = static_cast<Traders>(atoi(gridCell.value().c_str()));
                std::string traderName = GetTraderName(trader);
                this->context().swapTrader(traderName, "");
                _viperConfig._currentTrader = trader;
            } else if (columnType == TraderSwitchColumns::Shift) {
                auto shift = atoi(gridCell.value().c_str());
                _viperConfig._currentShift = static_cast<Shifts>(shift);
                LOG(0, "Shift changed to {i1}", shift)
                getStrategy<MarketMaker>(StrategyId::MarketMaker)->sendGenericMessage(ViperMessageType::ShiftUpdate, shift);
            }
        }
    }

    bool onManualFills(GridUpdate& update) {
        int numberOfFills = update._update.cells_size() / 3;
        _manualFills.resize(numberOfFills);
        int currentManualFillIndex = -1;
        int priorGridCellRow = -1;
        for (int i = 0; i < update._update.cells_size(); ++i) {
            const proto::GridCell &gridCell = update._update.cells(i);
            if (priorGridCellRow != static_cast<int>(gridCell.row())) {
                ++currentManualFillIndex;
                priorGridCellRow = static_cast<int>(gridCell.row());
            }
            auto columnType = static_cast<ManualFillsEntryColumns>(gridCell.col());
            ManualFill &fill = _manualFills[currentManualFillIndex];
            switch (columnType) {
                case ManualFillsEntryColumns::Contract: {
                    fill._contract = atoi(gridCell.value().c_str());
                    break;
                }
                case ManualFillsEntryColumns::Size: {
                    fill._size = atoi(gridCell.value().c_str());
                    break;
                }
                case ManualFillsEntryColumns::Type: {
                    fill._type = atoi(gridCell.value().c_str());
                    break;
                }
            }
        }

        for (int index = 0; index < numberOfFills; ++index) {
            ManualFill &fill = _manualFills[index];
            if (fill._type == toUInt(ManualFillType::MarketMaker)) {
                auto marketMaker = getStrategy<MarketMaker>(StrategyId::MarketMaker);
                auto id = marketMaker->beginFillProcess(fill._contract);
                marketMaker->processManualFill(fill);
                onEndOfFills(fill._contract, fill._size > 0 ? BID : ASK, id, true, true);
            }
        }

        return true;
    }

    std::vector<ContractState> _contractStates;
    SystemState _oldState;

    GatewayStatus _gatewayStatus;

    uint32_t _orderRequestQueueSize;

    Timer<Executor> _fillBannerTimer;
    Timer<Executor> _quoteBannerTimer;
    Timer<Executor> _checkQuotesTimer;

    std::unordered_map<om::port_gid_t::value_type, Throttler> _throttlers;

    NewOrderModifyOrderRateChecker<> _newOrderModifyOrderRateChecker;

    uint64_t _nextInternalHeartbeat;
    uint64_t _internalHeartbeatPeriod;

    std::vector<ManualFill> _manualFills;

    std::string _strategyName;
    std::string _strategyVersion;

    bool _needToSetLogger = true;

    om::container_id_t _id;
    std::set<uint32_t> _updatedContracts;

    std::map<hf::VENUE, uint32_t> _transactionCapacity;
    om::instance_id_t _iid;

    uint32_t _rvHedgeRejectCount = 0;
    const uint32_t RV_HEDGE_REJECT_COUNT_LIMIT = 20;

    uint8_t _oldMDStatus[static_cast<uint8_t>(hf::VENUE::TEST)] = {0};

    std::unordered_map<om::port_gid_t::value_type, hf::VENUE> _mapGatewayVenue;
    std::set<hf::VENUE> _usedVenues;
    SharedSystemState::QuoteExtBitset _oldQuoteExt;

    std::unordered_set<om::port_gid_t::value_type> _pendingGatewayLogin;
    bool _rhoneLimitsReady = false;

    struct {
        IOPTracker<Executor> _iopTracking;
        uint64_t lastForceReconcileSeqNum = UINT64_MAX - 1;
        boost::dynamic_bitset<> _potentiallyGapped;
//        std::unordered_map<om::port_id_t, std::unordered_map<om::market_segment_id_index_t, std::unordered_set<om::order_id_t>>> _badReloadedGTCsToCancel;
        bool _allowStartupWithUncontrolledGTCs = false;
    } __attribute__((aligned(64))) _slow;

    std::set<om::strategy_order_id_t> _recoveredOrders;
};

}

#endif //VIPER_EXECUTOR_H
