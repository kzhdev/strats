#ifndef VIPER_EXECUTOR_H
#define VIPER_EXECUTOR_H

#include <util/ConfigManager.h>
#include <util/ResourceManager.h>
#include <cstdint>
#include <hf/hf.h>
#include <util/log.h>
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
#include <csignal>
#include <unordered_set>
#include <util/VenueUtils.h>
#include "OrderRequestManager.h"
#include "ViperStrategyBase.h"
#include "ViperResource.h"
#include "MarketMaker.h"
#include "MarketDataConsumer.h"
#include <boost/dynamic_bitset.hpp>
#include "FillRateChecker.h"
#include <thread>
#include "RhoneRouter.h"

namespace Python {

LOCAL_EXPLICIT_LOG_DECLARE("Executor.h", ExecutorSpace)

class Executor;

//This can be changed in testing to have the B3 router used for a non-B3 venue
static constexpr hf::VENUE B3_ROUTER_VENUE = hf::VENUE::B3;
#ifndef DEBUG
    static_assert(B3_ROUTER_VENUE == hf::VENUE::B3, "B3 venue not set correctly in RhoneRouter");
#endif


struct ContractState {
    hf::batch_id_t _lastMDbid = 0;
    InternalMatchingEngine<Executor> _ime;
    md::top_t _books[2];
    int8_t _mdPriceChangeIndex[2] = {-1, -1};
    int8_t _mdQtyChangeIndex[2] = {-1, -1};
    ConfigContract _contract;
    RhoneRouter<B3_ROUTER_VENUE> _rhoneRouter;

#ifdef IS_DEV
    std::string getContractName() {
        return _contract._name;
    }
#else
    uint32_t getContractName() {
        return _contract._internalId;
    }
#endif
};

struct GatewayStatus {
    std::string _message;
    bool _active = true;
};

class Executor : public strat::StrategyT<Executor>, public ViperResource {
    OrderRequestManager& _orderRequestManager = OrderRequestManager::get();
    bool _mmOrderFilled = false;

public:
    Executor()
        : _fillBannerTimer(*this)
        , _quoteBannerTimer(*this)
        , _checkQuotesTimer(*this)
    {
    }

    virtual ~Executor() = default;

    ResourceId id() const noexcept override { return ResourceId::Execution; }

    void onStratBegin() override {
        initialize();
        if (!this->context().strategyLogin(_strategyName, _strategyVersion, true)) {
            logging::GetLogger().logErrorAndThrow(boost::str(boost::format("Strategy login did not work. Name=%1% Version=%2%") % _strategyName % _strategyVersion));
        }
        for (const auto& contract : _viperConfig._allContracts) {
            this->context().subscribeMD(contract._venue, contract._sid, md::top_t::LEVEL_NUM,true, true);
        }
        LOG(0, "Executor: Started. sc_id={i1}", this->context().getContainerId())
    }

    void onStratEnd() override {
        LOG(0, "Executor: Ended. {i1}", 0)
        if (_shutdownMDAndGateway) {
            // shutdown MD and Gateways
            LOG(0, "Executor: Shutdown MD and Gateways. {i1}", 0)
            std::thread thrd([script = _killScript](){
                using namespace std::chrono_literals;
                std::this_thread::sleep_for(5s);
                system(script.c_str());
            });
            thrd.detach();
        }
    }

    void onCreate(const libconfig::Setting& config) override {
        ViperResource::_onCreate(config);
    }

    void onPostCreate(const libconfig::Setting& config) override {
        ViperResource::_onPostCreate(config);
        _tradeName = hf::ConfigManager::getString(config, "trade_name");
        if (_tradeName.empty()) {
            logging::GetLogger().logErrorAndThrow("Missing trade_name in Executor config.");
        }
        _strategyName = hf::ConfigManager::getString(config, "strategy_name");
        _strategyVersion = hf::ConfigManager::getString(config, "strategy_version");
        _orderRequestQueueSize = hf::ConfigManager::getULong(config, "order_queue_size", 2048);
        _slow._allowStartupWithUncontrolledGTCs = hf::ConfigManager::getBool(config, "allow_start_with_uncontrolled_gtcs", false);
        logging::GetLogger().logInfo(boost::str(boost::format("Executor: order_queue_size=%1%") % _orderRequestQueueSize));

        if(_slow._allowStartupWithUncontrolledGTCs) {
            logging::GetLogger().logInfo("Allowing startup with uncontrolled GTCs");
        }

        _nextInternalHeartbeat = 0;
        _killScript = hf::ConfigManager::getString(config, "kill_script", "/home/pythonsvc/prodops/killViper.sh &");
        if (!_killScript.empty() && _killScript[_killScript.size() - 1] != '&') {
            _killScript.append(" &");
        }

        if (config.exists("md-queues")) {
            const auto& queuesConfig = config["md-queues"];

            for (int i = 0; i < queuesConfig.getLength(); ++i) {
                const libconfig::Setting& queueConfig = queuesConfig[i];

                if (!hf::ConfigManager::getBool(queueConfig, "enabled", true)) continue;

                const auto name = hf::ConfigManager::getString(queueConfig, "name");

                auto* const ptr = dynamic_cast<md::queue::MDQueueResource*> (rhone::util::ResourceManager::getResourceP(name));
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

        if(config.exists("b3_hb_contract")) {
            //Validation and logging done on initialize()
            _slow._b3_hb_contract_str = hf::ConfigManager::getString(config, "b3_hb_contract");
        }
    }

    void onStratLoop() override {
        uint64_t now = clockUtil::GetCycles();
        if (!_economicNumbers.empty()) {
            checkEconomicNumberSchedule(now);
        }
        checkSystemState();
        for (uint32_t id : _updatedContracts) {
            onMarketDataChange(id);
        }
        _updatedContracts.clear();
        processMessages();
        checkShutdownTime(now);
        checkTimers(now);
        sendInternalHeartbeat(now);
        _newOrderModifyOrderRateChecker.expireEntries(now);
        rhoneRouterPoll(now);
    }

    void checkEconomicNumberSchedule(uint64_t now) {
        if (_economicNumbers.empty()) {
            return;
        }

        do {
            auto it = _economicNumbers.begin();
            EconomicNumber* en = *it;
            auto nextCheckPoint = en->nextCheckPoint();
            if (now < nextCheckPoint) {
                if (nextCheckPoint == std::numeric_limits<uint64_t>::max()) {
                    assert(false);
                    _economicNumbers.erase(it);
                }
                return;
            }

            if (now >= en->_beginTime) {
                onEconomicNumber(*en);
                en->_beginTime = std::numeric_limits<uint64_t>::max();
            }

            if (now >= en->_mmEndTime) {
                onEconomicNumberEnded(*en);
                en->_mmEndTime = std::numeric_limits<uint64_t>::max();
            }

            if (now >= en->_rvEndTime) {
                enableRVAfterEconomicNumber(*en);
                en->_rvEndTime = std::numeric_limits<uint64_t>::max();
            }

            if (now >= en->_asEndTime) {
                enableASAfterEconomicNumber(*en);
                en->_asEndTime = std::numeric_limits<uint64_t>::max();
            }

            _economicNumbers.erase(it);
            if (en->nextCheckPoint() != std::numeric_limits<uint64_t>::max()) {
                _economicNumbers.emplace(en);
            }
        } while (!_economicNumbers.empty());
    }

    void onEconomicNumber(EconomicNumber& en) {
        LOG(0, "EXE. On {i1} EconomicNumber. Id={i2}", toUInt(en._color), en._id)
        sendErrorMessage(boost::str(boost::format("On %1% economic number. %2%. id=%3%") % to_string(en._color) % en._description % en._id),
                         ErrorType::Critical);

        en._currentShift = _viperConfig._currentShift;
        Shifts shift;

        auto disableStrategyAndStrategyQuoteExtWithEarlyGetOut = [this](EconomicNumber& en) {
            for (auto* strategy : _strategies) {
                if (strategy->strategyId() != StrategyId::MarketMaker) {
                    if (strategy->disable() == SystemState::Enabled) {
                        en._strategyDisabled.emplace_back(strategy);
                    }
                    if (strategy->disableQuoteExternal() == SystemState::Enabled) {
                        en._strategyQuoteExtDisabled.emplace_back(strategy);
                    }
                    // Not waiting for the cancel request from the strategy is to avoid
                    // placing the order back to market when disable GTC order
                    getStrategyOutOfMarket(strategy->strategyId());
                }
            }
        };
        switch (en._color) {
            case EconomicNumber::DarkRed:
                disableStrategyAndStrategyQuoteExtWithEarlyGetOut(en);
                shift = Shifts::SafeMode;

                for (auto& state : _contractStates) {
                    if (state._contract._toMarketMake) {
                        state._ime.onEconomicNumber(en._color);
                    }
                }
                break;
            case EconomicNumber::Red:
                disableStrategyAndStrategyQuoteExtWithEarlyGetOut(en);
                shift = Shifts::Asia;

                for (auto& state : _contractStates) {
                    if (state._contract._toMarketMake) {
                        state._ime.onEconomicNumber(en._color);
                    }
                }
                break;
            case EconomicNumber::Orange:
                for (auto* strategy : _strategies) {
                    if (strategy->strategyId() != StrategyId::MarketMaker) {
                        if (strategy->disable() == SystemState::Enabled) {
                            en._strategyDisabled.emplace_back(strategy);
                        }
                        if (strategy->disableQuoteExternal() == SystemState::Enabled) {
                            en._strategyQuoteExtDisabled.emplace_back(strategy);
                        }
                    }
                }
                shift =Shifts::Europe;
                break;
            case EconomicNumber::__end__:
                break;
        }

        onShiftChanged(toUInt(shift));
        sendGenericMessageToFront(ViperMessageType::ShiftChanged);
    }

    void onEconomicNumberEnded(EconomicNumber& en) {
        LOG(0, "EXE. {i1} EconomicNumber ended. Undo actions. Id={i2}", toUInt(en._color), en._id)
        sendErrorMessage(boost::str(boost::format("%1% economic number ended. Undo actions. Id=%2%") % to_string(en._color) % en._id), ErrorType::Critical);

        onShiftChanged(toUInt(en._currentShift));
        sendGenericMessageToFront(ViperMessageType::ShiftChanged);

        switch (en._color) {
            case EconomicNumber::DarkRed:
            case EconomicNumber::Red:
                for (auto& state : _contractStates) {
                    if (state._contract._toMarketMake) {
                        state._ime.onEconomicNumberEnded(en._color);
                    }
                }
                FALLTHROUGH;
            case EconomicNumber::Orange:
                for (auto iter = en._strategyQuoteExtDisabled.begin(); iter != en._strategyQuoteExtDisabled.end();) {
                    if ((*iter)->strategyId() == StrategyId::MarketMaker) {
                        (*iter)->enableQuoteExternal();
                        iter = en._strategyQuoteExtDisabled.erase(iter);
                    } else {
                        ++iter;
                    }
                }

                for (auto iter = en._strategyDisabled.begin(); iter != en._strategyDisabled.end();) {
                    if ((*iter)->strategyId() == StrategyId::MarketMaker) {
                        (*iter)->enable();
                        iter = en._strategyDisabled.erase(iter);
                    } else {
                        ++iter;
                    }
                }
                break;
            case EconomicNumber::__end__:
                break;
        }
    }

    void enableRVAfterEconomicNumber(EconomicNumber& en) {
        LOG(0, "EXE. {i1} EconomicNumber RV duration reached. Id={i2}", toUInt(en._color), en._id)
        enableStrategyAfterEconomicNumber(en, StrategyId::RelativeValue);
    }

    void enableASAfterEconomicNumber(EconomicNumber& en) {
        LOG(0, "EXE. {i1} EconomicNumber AS duration reached. Id={i2}", toUInt(en._color), en._id)
        enableStrategyAfterEconomicNumber(en, StrategyId::ArbieSpreader);
    }

    void enableStrategyAfterEconomicNumber(EconomicNumber& en, StrategyId strategy) {
        sendErrorMessage(boost::str(boost::format("%1% economic number %2% duration reached. Id=%3%") % to_string(en._color) % toString(strategy) % en._id), ErrorType::Critical);

        if (en._color == EconomicNumber::DarkRed) {
            sendErrorMessage(boost::str(boost::format("Not re-enable %1% on DartRed EconomicNumber") % toString(strategy)), ErrorType::Normal);
            return;
        }

        for (auto iter = en._strategyQuoteExtDisabled.begin(); iter != en._strategyQuoteExtDisabled.end();) {
            if ((*iter)->strategyId() == strategy) {
                (*iter)->enableQuoteExternal();
                iter = en._strategyQuoteExtDisabled.erase(iter);
                break;
            }
            ++iter;
        }

        for (auto iter = en._strategyDisabled.begin(); iter != en._strategyDisabled.end();) {
            if ((*iter)->strategyId() == strategy) {
                (*iter)->sendGenericMessage(ViperMessageType::EconomicNumberEnded, toUInt(en._color));
                break;
            }
            ++iter;
        }
    }

    void checkShutdownTime(uint64_t now) {
        if (_shutdownTime == (uint64_t)-1) {
            return;
        }

        if (BOOST_UNLIKELY(now >= _shutdownTime)) {
            static bool shutting_down = false;
            if (!shutting_down) {
                shutting_down = true;
                LOG(0, "On ShutdownTime. Shutting down. {i1}", 0)
                sendErrorMessage("On ShutdownTime. Shutting down.", ErrorType::Normal);
                shutting_down = true;
                for (auto strategy : _strategies) {
                    strategy->disable();
                }
                // Give some time to disable strategies
                _shutdownTime = now + GetCyclesFromSeconds(2, _frequency);
            } else {
                handleShutdown();
            }
        }
    }

    void sendInternalHeartbeat(uint64_t now) {
        if (BOOST_UNLIKELY(now > _nextInternalHeartbeat)) {
            _nextInternalHeartbeat = now + _internalHeartbeatPeriod;
            sendGenericMessageToFront(ViperMessageType::InternalHeartbeat);
        }
    }

    void rhoneRouterPoll(uint64_t now) {
        if(!_b3hb._rrc.empty()) {  //Attempt to short circuit in non-b3 strategies
            if (BOOST_LIKELY(_strategies[toUInt(StrategyId::MarketMaker)]->isEnabled() | _strategies[toUInt(StrategyId::RelativeValue)]->isEnabled())) {
                uint64_t threshold = now - RhoneRouterUtil::B3Router<B3_ROUTER_VENUE>::HeartbeatThreshold(_b3hb._oneMillisecondInTicks);
                bool actionTaken = false;
                for (int i = 0; i < static_cast<int>(_b3hb._rrc.size()); ++i) {
                    if (_b3hb._rrc[i].newestRelevantEvent() < threshold) {
                        this->context().createOrder(_iid, om::RISK_TYPE::ENTERING, _b3hb._hbPolicies[i],om::ORDER_TYPE::LMT, hf::SIDE::BUY, 0, 1,om::STP_ACTION::CANCEL_PASSIVE);
                        _b3hb._rrc[i].insertEvent(now);
                        actionTaken = true;
                    }
                }
                if (BOOST_LIKELY(actionTaken)) {
                    this->context().sendOrders();
                }
            }
        }
    }

    void processMessages() {
        uint32_t drainSize = 0;
        bool requiresContextFlush = false;
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

                // From MM
                case ViperMessageType::ChangeUseGTC: {
                    auto& msg = reinterpret_cast<GenericMessage &> (message._data[0]);
                    uint32_t internalId = msg._number_1;
                    bool enabled = msg._number_2;
                    auto& state = _contractStates[internalId];
                    state._ime.updateUseGtc(enabled);
                    _slow._iopTracking.cancelGTCs(internalId);
                    notifyFrontUseGTCChanged(internalId, false);
                    break;
                }

                case ViperMessageType::CanEnableUseGTC: {
                    auto& msg = reinterpret_cast<GenericMessage &> (message._data[0]);
                    uint32_t internalId = msg._number_1;
                    auto& state = _contractStates[internalId];
                    state._ime.tryEnableUseGTCAfterEconomicNumber();
                    break;
                }

                // From RV/AS
                case ViperMessageType::EndOfQuotes: {
                    auto& reply = reinterpret_cast<GenericMessage &> (message._data[0]);
                    getStrategy((StrategyId)(toUInt(message._source)))->sendGenericMessage(ViperMessageType::EndOfQuotes, reply._number_1);
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
                case ViperMessageType::ShiftUpdate: {
                    auto& msg = reinterpret_cast<GenericMessage&>(message._data[0]);
                    onShiftChanged(msg._number_1);
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

        if(requiresContextFlush) {
            this->context().sendOrders();
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

    void sendFAKHedgeInternalFillToFront(StrategyId strategy,
                                         uint32_t internalId,
                                         uint32_t side,
                                         hf::price_t price,
                                         hf::qty_t size,
                                         hf::price_t stf) {
        sendFillToFront(internalId, side, price, size, strategy, FillType::Internal, FillSource::Hedge, stf);
    }

    void onInternalFill(uint32_t internalId, Order* order, hf::price_t price, hf::qty_t size) {
        // send fill to FrondSide for negative scalp checking
        sendMessageToFront<FillMessage>(ViperMessageType::Fill, [=](FillMessage& message) {
            message._contractId = internalId;
            message._side = order->_side;
            message._qty = size;
            message._strategy = StrategyId::Execution;  // not used
            message._type = FillType::NegativeScalpCheck;
            message._price = price;
            message._source = FillSource::Quote;    // not used
            message._rvStf = 0;
        });

        FillSource source = FillSource::Quote;
        bool isHedge = false;
        if (order->_type == Order::Type::Hedging) {
            source = FillSource::Hedge;
            isHedge = true;
        }
        LOG(0, "EXE {C[i1]}. Internal fill. Sending to {i2}. Side={i3} Price={d1} Size={i4} IsHedge={i5}",
            getContract(internalId), strategyName(order->_strategy), order->_side, ToDollars(price), size, isHedge)
        sendFillToStrategy(internalId, order->_side, price, size, order->_strategy, FillType::Internal, source);
    }

    void onExternalFill(uint32_t internalId, StrategyId strategy, hf::price_t price, hf::qty_t size, uint32_t side, FillSource source, bool forceTakeOverFill = false, bool checkLimit = false) {
        LOG(0, "EXE {C[i1]}. External fill. Sending to {i2}. Side={i3} Price={d1} Size={i4} IsHedge={i5}",
            getContract(internalId), strategyName(strategy), side, ToDollars(price), size, (source == FillSource::Hedge))
        sendFillToStrategy(internalId, side, price, size, strategy, FillType::External, source, forceTakeOverFill, checkLimit);
    }

    void onExternalFillExt(uint32_t internalId, StrategyId strategy, hf::price_t price, hf::qty_t size, uint32_t side, FillSource source, uint64_t extid, om::order_id_t oid, uint32_t bid) {
        LOG(0, "EXE {C[i1]}. External fill Ext. Sending to {i2}. Side={i3} Price={d1} Size={i4} IsHedge={i5}",
            getContract(internalId), strategyName(strategy), side, ToDollars(price), size, (source == FillSource::Hedge))
        sendFillToStrategyExt(internalId, side, price, size, strategy, FillType::External, source, extid, oid, bid);
    }

    void sendFillToFront(uint32_t contractIndex, uint32_t side, hf::price_t price, hf::qty_t size, StrategyId strategyId,
                         FillType type, FillSource source, hf::price_t stf, uint32_t hedgeId = 0) {
        sendMessageToFront<FillMessage>(ViperMessageType::Fill, [=](FillMessage& message) {
            message._contractId = contractIndex;
            message._side = side;
            message._qty = size;
            message._strategy = strategyId;
            message._type = type;
            message._price = price;
            message._source = source;
            message._rvStf = stf;
            message._latencyTracking._extid = hedgeId;
        });
    }

    bool onEndOfFills(uint32_t internalId, uint32_t side, bool isInternal, uint32_t execToken) {
        if (_mmOrderFilled) {
            _mmOrderFilled = false;
            _strategies[toUInt(StrategyId::MarketMaker)]->sendMessage<EndOfFill>(
                    ViperMessageType::EndOfFill,
                    [=](EndOfFill& message) {
                        message._internalId = internalId;
                        message._side = side;
                        message._execToken = execToken;
                        message._isInternal = isInternal;
                    });
            return true;
        }
        return false;
    }

    bool sendExternalFAKHedge(StrategyId strategy, uint32_t internalId, uint32_t side, OrderRequest::Hedge& hedge) {
        om::order_t* rhoneOrder = newFAKOrder(internalId, side, hedge._externalPrice, hedge._size, hedge._hedgeId, strategy);
        if(BOOST_UNLIKELY(rhoneOrder == nullptr)) {
            //Note we DO NOT flip the side because the calee does it.
            sendHedgeMarketDataAdjustmentAckWithZeroSize(strategy, internalId, side, hedge);
            return false;
        }
        sendHedgeMarketDataAdjustmentAck(strategy, hedge._hedgeId, internalId, GetOtherSide(side), rhoneOrder->oid.value(), hedge._size);
        sendFillToFront(internalId, side, hedge._externalPrice, hedge._size, strategy, FillType::External, FillSource::Hedge, hedge._stf, hedge._hedgeId);
        return true;
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
                            FillSource source,
                            bool forceOverFill = false,
                            bool checkLimit = false) {
        _mmOrderFilled |= (strategyId == StrategyId::MarketMaker);
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
                    message._forceOverFill = forceOverFill;
                    message._checkLimit = checkLimit;
                });
    }

    void sendFillToStrategyExt(uint32_t contractIndex,
                            uint32_t side,
                            hf::price_t price,
                            hf::qty_t size,
                            StrategyId strategyId,
                            FillType type,
                            FillSource source,
                            uint64_t extid,
                            om::order_id_t oid,
                            uint32_t bid) {
        _mmOrderFilled |= (strategyId == StrategyId::MarketMaker);
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
                    message._forceOverFill = false;
                    message._checkLimit = false;
                    message._latencyTracking._extid = extid;
                    message._latencyTracking._oid = oid;
                    message._latencyTracking._bid = bid;
                });
    }


    void sendGenericMessageToRV(ViperMessageType type, int64_t value1 = 0, int64_t value2 = 0, int64_t value3 = 0) {
        _strategies[toUInt(StrategyId::RelativeValue)]->sendGenericMessage(type, value1, value2, value3);
    }

    InternalMatchingEngine<Executor>& getInternalMatchingEngine(const om::order_t& order) {
        ContractState& state = _contractStates[getInternalId(order)];
        return state._ime;
    }

    void sendReverseFillToStrategyExt(StrategyId strategy, uint32_t internalId, const om::order_t& order, FillSource source, uint32_t bid, uint64_t extid) {
        const hf::qty_t unfilledSize = order.qty - order.executedQty;
        const uint32_t reverseSide = order.side == hf::SIDE::BUY ? 1 : 0;
        LOG(0, "EXE {C[i1]}. Sending size {i2} as reverse to strategy {i3}. OID={i4} Type={i5}",
            getContract(internalId), unfilledSize, toUInt(strategy), order.oid.value(), toUInt(source))
        sendFillToStrategyExt(internalId, reverseSide, order.pr, unfilledSize, strategy, FillType::External, source, extid, order.oid.value(), bid);
    }

    COLD
    void onOrderRecoveryStarted(om::port_gid_t port_gid, om::instance_id_t strategy_instance_id) {
        LOG(0, "Start order recovery. port_gid_t={i1}", port_gid.value())

        _slow._recoveryNotGood = false;
        _slow._numSuccessfulRecoveredOrders = 0;
        _slow._totalSuccessfulRecoveredOrderQty = 0;
    }

    COLD
    bool willManageRecoveredOrder(const om::strategy_order_t& order) {
        const uint32_t internalId = _viperConfig.getInternalId(order.sid);
        if (internalId == INVALID_INTERNAL_ID) {
            using ExecutorSpace::s_log;
            LOG_ERROR(boost::str(boost::format("Recovered GTC Could not be matched to existing sym.  sid=%1% Side=%2% Price=%3% Qty(before unprocessed fills)=%4% oid=%5%") %
                                 static_cast<int>(order.sid) % static_cast<int>(order.side) % order.pr % order.qty % order.oid.value()));
            std::string message = boost::str(boost::format("GTC Order for not traded contract - need to cancel. OID=%1% SID=%2%") % order.oid.value() % order.sid);
            sendErrorMessage(message, ErrorType::Critical);

            //Until the Rhone OM can handle cancelling a GTC that is not in the secdb, we're just going to force exit
            if (!_slow._allowStartupWithUncontrolledGTCs) {
                _slow._recoveryNotGood = true;
            }

            const_cast<om::order_t &>(order).debug.i[ORDER_OWNER] = UNPROCESSABLE_RELOADED_ORDER;
            BOOST_VERIFY(_slow._badReloadedGTCsToCancel.insert(&order).second);

            return false;
        }

        return order.action_flags.is_active;
    }

    COLD
    void onOrderRecoveryFinished(om::port_gid_t port_gid, om::instance_id_t strategy_instance_id) {
        if(_slow._recoveryNotGood) {
            rhone::util::Service::exitAll();
        }

        if (_slow._numSuccessfulRecoveredOrders) {
            using ExecutorSpace::s_log;
            auto message = boost::str(boost::format("GTC Orders recovered.  PortGid=%1% OrderCountForGid=%2% CumulativeOrderCount=%3% Qty(before unprocessed fills)=%4%")
                    % static_cast<int>(port_gid.value()) % _slow._numSuccessfulRecoveredOrders % _slow._cumulativeSuccessfulRecoveredOrders % _slow._totalSuccessfulRecoveredOrderQty);
            sendErrorMessage(message, ErrorType::Normal);
            LOG_INFO(message);
        }

        LOG(0, "Order recovery completed. port_gid_t={i1} OrderCountForGid={i2} CumulativeOrderCount={i3} QtyBeforeUnprocessedFills={i4}", port_gid.value(), _slow._numSuccessfulRecoveredOrders, _slow._cumulativeSuccessfulRecoveredOrders, _slow._totalSuccessfulRecoveredOrderQty)
        _slow._iopTracking.forceReconcileGTCs(true);
    }

    COLD
    void onRecoveredOrder(om::strategy_order_t& order, const om::OMBatch& batch) {
        using ExecutorSpace::s_log;
        const uint32_t internalId = _viperConfig.getInternalId(order.sid);
        uint32_t side = order.side == hf::SIDE::BUY ? BID : ASK;
        ContractState& state = _contractStates[internalId];
        state._ime.addGtcOrder(side,  order);
        sendOrderUpdateToMM(order.oid.value(), ToViperSide(order.side), order.pr, order.qty, order.qp.qtyInFront, internalId);
        auto message = boost::str(boost::format(
                "GTC Order recovered.  Contract=%1% Side=%2% Price=%3% Qty=%4% ExecutedQty=%5% LeavesQty=%6% oid=%7% rhone_oid=%8% exch_oid=%9% unique_order_id=%10%")
                % getContractName(internalId) % hf::SIDE_NAMES.at(static_cast<unsigned char>(order.side))
                % ToDollars(order.pr) % order.qty % order.executedQty % order.leavesQty % order.oid.value()
                % order.rhone_oid.value() % order.exchangeOrderId.value() % order.unique_order_id.to_string());
        LOG_INFO(message);

        ++_slow._numSuccessfulRecoveredOrders;
        ++_slow._cumulativeSuccessfulRecoveredOrders;
        _slow._totalSuccessfulRecoveredOrderQty += order.leavesQty;
    }

    COLD
    void onRecoveredExecution(const om::OMRecoveredExecution& event, const om::OMBatch& /*batch*/) {
        // onRecoveredExecution is called inside onRecoveredFills loop
        using ExecutorSpace::s_log;
        const auto& orders = this->context().getOrders();
        bool found = false;
        if (event.is_leg_fill) {
            return;
        }
        for (auto it = orders.cbegin(); it != orders.cend(); ++it) {
            const auto& order = *it;
            if (order.unique_order_id == event.unique_order_id) {
                found = true;
                if (order.sid != event.sid) {
                    // this is the ledge order fill. Ignore it
                    continue;
                }
                auto internalId = _viperConfig.getInternalId(order.sid);

                if (internalId != INVALID_INTERNAL_ID) {
                    handleFillForUnassignedGtcOrder(event, event.venue, internalId, order);
                    auto message = boost::str(boost::format(
                            "GTC Order filled.  Contract=%1% Side=%2% FillQty=%3% FillPrice=%4% OrderPrice=%5% LeavesQty=%6%")
                            % getContractName(internalId) % hf::SIDE_NAMES.at(static_cast<unsigned char>(order.side))
                            % event.filledQty % ToDollars(event.filledPrice) % ToDollars(order.pr) % order.leavesQty);
                    LOG_INFO(message);
                    sendErrorMessage(message, ErrorType::Critical);
                } else {
                    std::string message = boost::str(
                            boost::format("Fill for not traded contract. Contract=%1% Price=%2% Size%3%") % order.sid %
                            ToRhonePrice(event.filledPrice) % event.filledQty);
                    sendErrorMessage(message, ErrorType::Critical);
                    LOG_ERROR(message);
                }
                return;
            }
        }

        if (found) {
            return;
        }

        auto order_id_components = event.unique_order_id.decode(event.venue);
        auto internalId = _viperConfig.getInternalId(event.sid);
        if (internalId != INVALID_INTERNAL_ID) {
            auto message = boost::str(boost::format(
                    "GTC Order fully filled. UniqueOrderId=%1% (session_id=%2% instance_id=%3% order_seqnum=%4% request_id=%5%) Contract=%6% Side=%7% FillQty=%8% FillPrice=%9%")
                    % event.unique_order_id.to_string() % order_id_components.m_session_id
                    % order_id_components.m_instance_id % order_id_components.m_order_sequence_number
                    % order_id_components.m_order_request_id % getContractName(internalId)
                    % hf::SIDE_NAMES.at(static_cast<unsigned char>(event.fill_side)) % event.filledQty
                    % ToDollars(event.filledPrice));

            if (_viperConfig._allContracts[internalId]._toMarketMake) {
                uint32_t side = (event.fill_side == hf::SIDE::BUY ? BID : ASK);
                handleFillForUnassignedGtcOrder(event, event.venue, internalId, side);
                sendErrorMessage(message, ErrorType::Normal);
            }
            LOG_INFO(message);
        } else {
            auto message = boost::str(boost::format(
                    "Unknown GTC Order filled. UniqueOrderId=%1% (session_id=%2% instance_id=%3% order_seqnum=%4% request_id=%5%) sid=%6% Side=%7% FillQty=%8% FillPrice=%9%")
                    % event.unique_order_id.to_string() % order_id_components.m_session_id
                    % order_id_components.m_instance_id % order_id_components.m_order_sequence_number
                    % order_id_components.m_order_request_id % event.sid
                    % hf::SIDE_NAMES.at(static_cast<unsigned char>(event.fill_side)) % event.filledQty
                    % ToDollars(event.filledPrice));
            LOG_INFO(message);
            sendErrorMessage(message, ErrorType::Normal);
        }
    }

    COLD
    void onRecoveredFills(const om::OMRecoveredFillsEvent& recoveredFills, const om::OMBatch& batch) {
        using ExecutorSpace::s_log;
        LOG_INFO(boost::format("Recovered %1% GTC fills") % recoveredFills.num_fills);

        //Cancel bad GTCs if necessary
        for (auto& order : _slow._badReloadedGTCsToCancel) {
            if (order->route) {
                this->context().cancelOrder(order->oid);
                sendErrorMessage("Cancelling GTC due to unknown symbol.  oid=" + std::to_string(order->oid.value()), ErrorType::Critical);
            } else {
                LOG(0, "BadGTC [need to be] cancelled.  oid={i1}", order->oid.value())
                sendErrorMessage("Need to cancel GTCs due to unknown symbol.  However this version of Rhone does not support that functionality.  oid=" + std::to_string(order->oid.value()), ErrorType::Critical);
            }
        }
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
                                    hf::price_t bidPrice,
                                    hf::qty_t bidSize,
                                    uint32_t bidMarketId,
                                    hf::price_t askPrice,
                                    hf::qty_t askSize,
                                    uint32_t askMarketId) {
        getStrategy(strategy)->template sendMessage<MarketUpdate>(
                ViperMessageType::MarketUpdate,
                [=](MarketUpdate& message) {
                    message._instrumentIndex = internalId;
                    message._bidPrice = bidPrice;
                    message._bidSize = bidSize;
                    message._bidMarketId = bidMarketId;
                    message._askPrice = askPrice;
                    message._askSize = askSize;
                    message._askMarketId = askMarketId;
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

    //Note the return value is ignored!  Theoretically the Stacker validation should return false?
    bool processGridChange(GridUpdate& update) {
        if (update._update.gridid() == toUInt(WindowIds::StackerParameters)) {
            for (proto::GridCell& cell : *update._update.mutable_cells()) {
                const uint32_t internalId = cell.row();
                ContractState& state = _contractStates[internalId];
                StackerParametersColumns columnType = static_cast<StackerParametersColumns>(cell.col());
                bool attemptToCancelRemainder = false;
                if(columnType == StackerParametersColumns::UseGTC) {
                    if (1 == atoi(cell.value().c_str())) {
                        if(!_viperConfig._allowGTCs) {
                            sendErrorMessage("EnableGTCs changed to true for contract" + _viperConfig._allContracts[internalId]._name + ". This shouldn't be possible!  overriding", ErrorType::Critical);
                            LOG(0, "EXE: Contract {C[i1]} EnableGTCs changed to true for contract, this shouldn't be possible.  Overriding", getContract(internalId));
                            cell.set_value("0");
                            continue;
                        }
                    }
                    else {
                        attemptToCancelRemainder = true;
                    }
                }

                //Note this can also change values if further validation fails
                std::string errorMessage;
                if(!state._ime.tryHandleStackerParameterUpdate(cell, errorMessage)) {
                    sendErrorMessage("StackerParameterUpdate failed: " + errorMessage, ErrorType::Critical);
                    LOG(0, "EXE: Contract {C[i1]} Stacker parameter update failed", getContract(internalId));
                }

                //As a safety, check to see if any GTCs remain in the market that we must cancel
                if(attemptToCancelRemainder) {
                    int remainder = _slow._iopTracking.cancelGTCs(internalId);
                    bool anyStrategyEnabled = false;
                    for(int i = 0; i < static_cast<int>(StrategyId::Execution); ++i) {
                        if(getStrategy(static_cast<StrategyId>(i))->isEnabled()) {
                            anyStrategyEnabled = true;
                            break;
                        }
                    }
                    if (remainder && anyStrategyEnabled) {
                        LOG(0, "EXE: Contract {C[i1]} GTCs still in market after attempting to disable.  Canceling remainder.  NumRemainderCancelled={i2}", getContract(internalId), remainder);
                        sendErrorMessage("Contract " + getContractName(internalId) + ": GTCs found in market after OM attempted to cancel all.  See previous rows for more details GTCs.  NumberFoundAndCancelled=" + std::to_string(remainder), ErrorType::Critical);
                    }
                }
            }
        }
        else if (update._update.gridid() == toUInt(WindowIds::GlobalConfigs)) {
            for (int i = 0; i < update._update.cells_size(); ++i) {
                proto::GridCell& cell = *update._update.mutable_cells(i);
                GlobalConfig& config = _viperConfig._globalConfigs[cell.row()];
                if (config._key == QUOTE_BANNER_PERIOD) {
                    uint64_t period = GetCyclesFromMicroSeconds(boost::lexical_cast<uint32_t>(cell.value()), _frequency);
                    auto shift = static_cast<Shifts>(cell.col() - 1);
                    if (_viperConfig._currentShift == shift) {
                        for (auto& state : _contractStates) {
                            state._ime.handleQuoteBannerUpdate(period);
                        }
                    }
                } else if (config._key == FILL_BANNER_PERIOD) {
                    uint64_t period = GetCyclesFromMicroSeconds(boost::lexical_cast<uint32_t>(cell.value()), _frequency);
                    auto shift = static_cast<Shifts>(cell.col() - 1);
                    if (_viperConfig._currentShift == shift) {
                        for (auto& state : _contractStates) {
                            state._ime.handleFillBannerUpdate(period);
                        }
                    }
                } else if (config._key == SELF_MATCH_BANNER_MICS) {
                    uint64_t period = GetCyclesFromMicroSeconds(boost::lexical_cast<uint32_t>(cell.value()), _frequency);
                    auto shift = static_cast<Shifts>(cell.col() - 1);
                    if (_viperConfig._currentShift == shift) {
                        for (auto& state : _contractStates) {
                            state._ime.handleRejectBannerUpdate(period);
                        }
                    }
                } else if (config._key == MSG_REDUCE_QUOTE_MICS) {
                    uint64_t inDangerZonePeriod = GetCyclesFromMicroSeconds(boost::lexical_cast<uint32_t>(cell.value()), _frequency);
                    auto shift = static_cast<Shifts>(cell.col() - 1);
                    if (_viperConfig._currentShift == shift) {
                        for (auto& kvp : _throttlers) {
                            kvp.second.updateInDangerZonePeriod(inDangerZonePeriod);
                        }
                    }
                }
                else if (config._key == MSG_HEDGES_INTERVAL_MICS) {
                    uint64_t atCapacityPeriod = GetCyclesFromMicroSeconds(boost::lexical_cast<uint32_t>(cell.value()), _frequency);
                    auto shift = static_cast<Shifts>(cell.col() - 1);
                    if (_viperConfig._currentShift == shift) {
                        for (auto& kvp : _throttlers) {
                            kvp.second.updateAtCapacityPeriod(atCapacityPeriod);
                        }
                    }
                }
                else if (config._key == PERCENT_MSG_HEDGES) {
                    double percent = boost::lexical_cast<double>(cell.value());
                    auto shift = static_cast<Shifts>(cell.col() - 1);
                    if (_viperConfig._currentShift == shift) {
                        for (auto& kvp : _throttlers) {
                            kvp.second.updateHedgePercentage(percent);
                        }
                    }
                }
                else if (config._key == PERCENT_MSG_REDUCE_QUOTE) {
                    double percent = boost::lexical_cast<double>(cell.value());
                    auto shift = static_cast<Shifts>(cell.col() - 1);
                    if (_viperConfig._currentShift == shift) {
                        for (auto& kvp : _throttlers) {
                            kvp.second.updateDangerZonePercentage(percent);
                        }
                    }
                }
                else if (config._key == HEDGE_BANNER_MICS) {
                    uint64_t period = GetCyclesFromMicroSeconds(boost::lexical_cast<uint32_t>(cell.value()), _frequency);
                    auto shift = static_cast<Shifts>(cell.col() - 1);
                    if (_viperConfig._currentShift == shift) {
                        for (auto& state : _contractStates) {
                            state._ime.handleHedgeQuoteBannerUpdate(period);
                        }
                    }
                }
                else if(config._key == ORDER_RATE_CLIPS) {
                    uint32_t clips = boost::lexical_cast<uint32_t>(cell.value());
                    auto shift = static_cast<Shifts>(cell.col() - 1);
                    if (_viperConfig._currentShift == shift) {
                        _newOrderModifyOrderRateChecker.changeMaxClips(clips, clockUtil::GetCycles());
                        LOG(0, "Order Rate Checker.  Update max clips={i1}", clips);
                    }
                }
                else if(config._key == ORDER_RATE_SUM_TOTAL) {
                    hf::qty_t sumTotal = boost::lexical_cast<hf::qty_t>(cell.value());
                    auto shift = static_cast<Shifts>(cell.col() - 1);
                    if (_viperConfig._currentShift == shift) {
                        _newOrderModifyOrderRateChecker.changeSumTotal(sumTotal, clockUtil::GetCycles());
                        LOG(0, "Order Rate Checker.  Update sum total={i1}", sumTotal);
                    }
                }
                else if(config._key == ORDER_RATE_PERIOD) {
                    uint32_t period = boost::lexical_cast<uint32_t>(cell.value());
                    uint64_t periodInCycles = GetCyclesFromMilliSeconds(period, _frequency);
                    auto shift = static_cast<Shifts>(cell.col() - 1);
                    if (_viperConfig._currentShift == shift) {
                        _newOrderModifyOrderRateChecker.changeThrottleWindowSize(periodInCycles, clockUtil::GetCycles());
                        LOG(0, "Order Rate Checker. Update period={i1} periodInCycles={i2}", period, periodInCycles)
                    }
                } else if (config._key == AUTO_SHUTDOWN_TIME) {
                    auto shift = cell.col() - 1;
                    const auto& orgShutdownTime = _viperConfig._globalConfigs[cell.row()]._shifts[shift];
                    int32_t orgValue = -1;
                    try {
                        orgValue = boost::lexical_cast<int32_t>(orgShutdownTime);
                    }
                    catch (boost::bad_lexical_cast&) {
                        _viperConfig._globalConfigs[cell.row()]._shifts[shift] = "-1";
                    }
                    try {
                        auto value = boost::lexical_cast<int32_t>(cell.value());
                        uint32_t hour, min, sec;
                        if (value == -1) {
                            _shutdownTime = -1;
                            LOG(0, "Shift {i1} AutoShutdownTime changed {i2} -> -1", shift, orgValue)
                        } else if (verifyHHMMSSTime(value, hour, min, sec)) {
                            LOG(0, "Shift {i1} AutoShutdownTime changed {i2} -> {i3}", shift, orgValue, value)
                            if ((Shifts)shift == _viperConfig._currentShift) {
                                _shutdownTime = convertTimeToCycles(hour, min, sec, clockUtil::GetCycles(), _frequency);
                            }
                        } else {
                            LOG(0, "Shift {i1} AutoShutdownTime {i2} invalid format. Revert value back to {i3}", shift, value, orgValue)
                            sendErrorMessage(boost::str(boost::format("AutoShutdownTime %1% invalid format. Revert value back to %2%") % value % orgValue),
                                             ErrorType::Normal);
                            cell.set_value(orgShutdownTime);
                        }
                    }
                    catch (boost::bad_lexical_cast&) {
                        LOG(0, "AutoShutdownTime value invalid. Revert value back to {i1}", orgValue)
                        sendErrorMessage(boost::str(boost::format("AutoShutdownTime %1% invalid. Revert value back to %2%") % cell.value() % orgValue),
                                         ErrorType::Normal);
                        cell.set_value(orgShutdownTime);
                    }
                } else if (config._key == AUTO_RV_DISABLE_TIME) {
                    auto* rv = getStrategy(StrategyId::RelativeValue);
                    setAutoDisableTime(rv, cell);
                } else if (config._key == AUTO_MM_DISABLE_TIME) {
                    auto* mm = getStrategy(StrategyId::MarketMaker);
                    setAutoDisableTime(mm, cell);
                } else if (config._key == AUTO_AS_DISABLE_TIME) {
                    auto* as = getStrategy(StrategyId::ArbieSpreader);
                    setAutoDisableTime(as, cell);
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
        else if (update._update.gridid() == toUInt(WindowIds::TraderSwitch)) {
            switchTrader(update);
        }
        else if(update._update.gridid() == toUInt(WindowIds::GTCManagement)) {
            _slow._iopTracking.handleGTCManagementGridUpdate(update);
        }
        else if (update._update.gridid() == toUInt(WindowIds::EconomicNumbers)) {
            handleEconomicNumbersUpdate(update._update);
        }
        return true;
    }
    void handleEconomicNumbersUpdate(proto::GridUpdate& update) {
        std::set<EconomicNumber*> pendingAddEconomicNumbers;
        std::unordered_map<uint32_t, EconomicNumber> newEconomicNumbers;
        auto now = hf::util::TimeUtils::clock_gettime();
        for (int i = 0; i < update.cells_size(); ++i) {
            proto::GridCell& cell = *update.mutable_cells(i);
            auto id = cell.row();
            auto iter = _viperConfig._mapIdEconomicNumber.find(id);
            if (iter != _viperConfig._mapIdEconomicNumber.end()) {
                auto& config = iter->second;
                assert(config._tradeName == _tradeName);
                auto columnType = static_cast<EconomicNumberColumns>(cell.col());
                switch (columnType) {
                    case EconomicNumberColumns::Time: {
                        auto value = cell.value();
                        boost::algorithm::trim(value);
                        if (value.empty() || value == "<ERROR>" || value == "-1") {
                            LOG(0, "EXEC. Remove {i1} economic number. Id={i2}", toUInt(config._color), config._id)
                            removeEconomicNumber(config._id);
                            sendErrorMessage(boost::str(boost::format("Remove %1% economic number at %2%. Id=%3%") % to_string(config._color) % config._datetimeString % config._id),
                                             ErrorType::Normal);
                            break;
                        }

                        if (value.size() != 8) {
                            sendErrorMessage(boost::str(boost::format("Datetime in economic number %1% must be in \"HH:MM:SS\" format") % config._id),
                                             ErrorType::Normal);
                            cell.set_value(config._datetimeString.substr(11));
                            break;
                        }
                        auto date = hf::util::TimeUtils::strftime("%Y-%m-%d ", now);
                        value = date + value;
                        time_t datetime;
                        if (!parseDateTime(value, datetime)) {
                            sendErrorMessage(boost::str(boost::format("Datetime in economic number %1% must be in \"YYYY-MM-DD HH:MM:SS\" format. got %2%") % config._id % value),
                                             ErrorType::Normal);
                            cell.set_value(config._datetimeString.substr(11));
                            break;
                        }

                        MAYBE_UNUSED auto oldValue = config._datetime;
                        MAYBE_UNUSED auto oldDateTimeStr = config._datetimeString;
                        config._datetimeString = value;
                        config._datetime = datetime;

                        if ((config._datetime * 1000000000) < now) {
                            // time already passed, add 24 hour
                            config._datetime += 86400;
                            config._datetimeString = hf::util::TimeUtils::strftime("%Y-%m-%d %H:%M:%S", config._datetime * 1000000000);
                        }
#ifdef IS_DEV
                        LOG(0, "EXE. Economic number {i1} time changed. {i2} -> {i3}({i4})", config._id, oldDateTimeStr, config._datetimeString, config._datetime)
#else
                        LOG(0, "EXE. Economic number {i1} time changed. {i2} -> {i3}", config._id, oldValue, config._datetime)
#endif

                        removeEconomicNumber(config._id);
                        pendingAddEconomicNumbers.emplace(&config);
                        break;
                    }
                    case EconomicNumberColumns::MMDuration: {
                        auto value = boost::lexical_cast<int32_t>(cell.value());
                        if (value <= 0) {
                            sendErrorMessage(boost::str(boost::format("EconomicNumber %1% invalid MM duration %2%, set it back to %3%.") %
                                                        config._id % value % config._mmDuration),  ErrorType::Normal);
                            cell.set_value(std::to_string(config._mmDuration));
                        } else {
                            auto oldValue = config._mmDuration;
                            config._mmDuration = value;
                            LOG(0, "EXE. Economic number {i1} MM duration changed {i2} -> {i3}", config._id, oldValue, value)
                            removeEconomicNumber(config._id);
                            pendingAddEconomicNumbers.emplace(&config);
                        }
                        break;
                    }
                    case EconomicNumberColumns::RVDuration: {
                        auto value = boost::lexical_cast<int32_t>(cell.value());
                        if (value <= 0) {
                            sendErrorMessage(boost::str(boost::format("EconomicNumber %1% invalid RV duration %2%, set it back to %3%.") %
                                                        config._id % value % config._rvDuration),  ErrorType::Normal);
                            cell.set_value(std::to_string(config._rvDuration));
                        } else {
                            auto oldValue = config._rvDuration;
                            config._rvDuration = value;
                            LOG(0, "EXE. Economic number {i1} RV duration changed {i2} -> {i3}", config._id, oldValue, value)
                            removeEconomicNumber(config._id);
                            pendingAddEconomicNumbers.emplace(&config);
                        }
                        break;
                    }
                    case EconomicNumberColumns::ASDuration: {
                        auto value = boost::lexical_cast<int32_t>(cell.value());
                        if (value <= 0) {
                            sendErrorMessage(boost::str(boost::format("EconomicNumber %1% invalid AS duration %2%, set it back to %3%.") %
                                                        config._id % value % config._asDuration),  ErrorType::Normal);
                            cell.set_value(std::to_string(config._asDuration));
                        } else {
                            auto oldValue = config._asDuration;
                            config._asDuration = value;
                            LOG(0, "EXE. Economic number {i1} AS duration changed {i2} -> {i3}", config._id, oldValue, value)
                            removeEconomicNumber(config._id);
                            pendingAddEconomicNumbers.emplace(&config);
                        }
                        break;
                    }
                    case EconomicNumberColumns::Color: {
                        auto value = boost::lexical_cast<uint32_t>(cell.value());
                        if (value >= toUInt(EconomicNumber::Color::__end__)) {
                            sendErrorMessage(boost::str(boost::format("EconomicNumber %1% invalid Color %2%, set it back to %3%.") %
                                             config._id % value % to_string(config._color)),  ErrorType::Normal);
                            cell.set_value(std::to_string(toUInt(config._color)));
                        } else {
                            auto oldColor = toUInt(config._color);
                            config._color = (EconomicNumber::Color)value;
                            LOG(0, "EXE. Economic number {i1} color changed {i2} -> {i3}", config._id, oldColor, value)
                            removeEconomicNumber(config._id);
                            pendingAddEconomicNumbers.emplace(&config);
                        }
                        break;
                    }
                    case EconomicNumberColumns::Description:
                       config._description = cell.value();
                       break;
                }
            } else {
                auto it = newEconomicNumbers.find(id);
                if (it == newEconomicNumbers.end()) {
                    LOG(0, "EXE. Add a new economic number. id={i1}", id)
                    it = newEconomicNumbers.emplace(id, EconomicNumber()).first;
                    it->second._tradeName = _tradeName;
                    it->second._id = id;
                }
                auto& config = it->second;
                auto columnType = static_cast<EconomicNumberColumns>(cell.col());
                switch (columnType) {
                    case EconomicNumberColumns::Time: {
                        auto value = cell.value();
                        boost::algorithm::trim(value);
                        if (value.size() != 8) {
                            sendErrorMessage(boost::str(boost::format("Datetime in economic number %1% must be in \"HH:MM:SS\" format") % id),
                                             ErrorType::Normal);
                            cell.set_value("");
                            newEconomicNumbers.erase(it);
                            break;
                        }
                        time_t datetime;
                        value = hf::util::TimeUtils::strftime("%Y-%m-%d ", now) + value;
                        if (!parseDateTime(value, datetime)) {
                            sendErrorMessage(boost::str(boost::format("Datetime in economic number %1% must be in \"YYYY-MM-DD HH:MM:SS\" format. got %2%") % id % value),
                                             ErrorType::Normal);
                            cell.set_value("");
                            newEconomicNumbers.erase(it);
                            break;
                        }

                        config._datetimeString = value;
                        config._datetime = datetime;

                        if (config._datetime * 1000000000 < now) {
                            // time already passed, add 24 hour
                            config._datetime += 86400;
                            config._datetimeString = hf::util::TimeUtils::strftime("%Y-%m-%d %H:%M:%S", config._datetime * 1000000000);
                        }
#ifdef IS_DEV
                        LOG(0, "EXE. Economic number {i1} time {i2}({i3})", config._id, config._datetimeString, config._datetime)
#else
                        LOG(0, "EXE. Economic number {i1} time {i2}", config._id, config._datetime)
#endif
                        break;
                    }
                    case EconomicNumberColumns::MMDuration: {
                        auto value = boost::lexical_cast<int32_t>(cell.value());
                        if (value <= 0) {
                            sendErrorMessage(boost::str(boost::format("EconomicNumber %1% invalid MM duration %2%, set it back to %3%.") %
                                                        config._id % value % config._mmDuration),  ErrorType::Normal);
                            cell.set_value("");
                            newEconomicNumbers.erase(it);
                        } else {
                            config._mmDuration = value;
                            LOG(0, "EXE. Economic number {i1} MM duration {i2}", config._id, value)
                        }
                        break;
                    }
                    case EconomicNumberColumns::RVDuration: {
                        auto value = boost::lexical_cast<int32_t>(cell.value());
                        if (value <= 0) {
                            sendErrorMessage(boost::str(boost::format("EconomicNumber %1% invalid RV duration %2%, set it back to %3%.") %
                                                        config._id % value % config._rvDuration),  ErrorType::Normal);
                            cell.set_value("");
                            newEconomicNumbers.erase(it);
                        } else {
                            config._rvDuration = value;
                            LOG(0, "EXE. Economic number {i1} RV duration {i2}", config._id, value)
                        }
                        break;
                    }
                    case EconomicNumberColumns::ASDuration: {
                        auto value = boost::lexical_cast<int32_t>(cell.value());
                        if (value <= 0) {
                            sendErrorMessage(boost::str(boost::format("EconomicNumber %1% invalid AS duration %2%, set it back to %3%.") %
                                                        config._id % value % config._asDuration),  ErrorType::Normal);
                            cell.set_value("");
                            newEconomicNumbers.erase(it);
                        } else {
                            config._asDuration = value;
                            LOG(0, "EXE. Economic number {i1} RV duration {i2}", config._id, value)
                        }
                        break;
                    }
                    case EconomicNumberColumns::Color: {
                        auto value = boost::lexical_cast<uint32_t>(cell.value());
                        if (value >= toUInt(EconomicNumber::Color::__end__)) {
                            sendErrorMessage(boost::str(boost::format("EconomicNumber %1% invalid Color %2%.") % id % value),
                                             ErrorType::Normal);
                            cell.set_value("");
                            newEconomicNumbers.erase(it);
                        } else {
                            config._color = (EconomicNumber::Color)value;
                            LOG(0, "EXE. Economic number {i1} color={i2}", id, value)
                        }
                        break;
                    }
                    case EconomicNumberColumns::Description:
                        config._description = cell.value();
                        break;
                }
            }
        }

        now /= 1000000000;  // change to second
        for (auto en : pendingAddEconomicNumbers) {
            if (addEconomicNumber(*en, now)) {
                sendErrorMessage(boost::str(boost::format("Expecting %1% economic number at %2%. Id=%3% %4%") % to_string(en->_color) % en->_datetimeString % en->_id % en->_description),
                                 ErrorType::Normal);
            }
        }

        for (auto& kvp : newEconomicNumbers) {
            auto& en = kvp.second;
            auto iter = _viperConfig._mapIdEconomicNumber.emplace(kvp.first, en).first;
            if (addEconomicNumber(iter->second, now)) {
                sendErrorMessage(boost::str(boost::format("Expecting %1% economic number at %2%. Id=%3% %4%") % to_string(en._color) % en._datetimeString % en._id % en._description),
                                 ErrorType::Normal);
            }
        }
    }

    void removeEconomicNumber(uint32_t id) {
        for (auto iter = _economicNumbers.begin(); iter != _economicNumbers.end(); ++iter) {
            auto* economicNumber = *iter;
            if(economicNumber->_id == id) {
                LOG(0, "EXE. remove economic number {i1}. color={i2}", id, toUInt(economicNumber->_color));
                _economicNumbers.erase(iter);
                break;
            }
        }
    }

    void setAutoDisableTime(ViperStrategyBase* strategy, proto::GridCell& cell) {
        auto shift = cell.col() - 1;
        auto orgValue = _viperConfig._globalConfigs[cell.row()]._shifts[shift];
        try {
            if (cell.value() != "-1" && cell.value().size() != 6) {
                sendErrorMessage(boost::str(boost::format("Auto%1%DisableTime must be in HHMMSS format.") % toString(strategy->strategyId())),
                                 ErrorType::Normal);
                cell.set_value(orgValue);
                return;
            }
            auto value = boost::lexical_cast<int32_t>(cell.value());
            if (shift == toUInt(_viperConfig._currentShift)) {
                if (!strategy->setAutoDisableTime(value, clockUtil::GetCycles())) {
                    sendErrorMessage(boost::str(
                            boost::format("Auto%1%DisableTime %2% invalid") % toString(strategy->strategyId()) %
                            cell.value()),
                                     ErrorType::Normal);
                    cell.set_value(std::to_string(strategy->autoDisableTime() % 240000));
                }
            } else {
                uint32_t hour, min, sec;
                if (value != -1 && !verifyHHMMSSTime(value, hour, min, sec)) {
                    sendErrorMessage(boost::str(boost::format("Auto%1%DisableTime %2% invalid") % toString(strategy->strategyId()) % cell.value()),
                                     ErrorType::Normal);
                    cell.set_value(orgValue);
                }
            }
        }
        catch (boost::bad_lexical_cast&) {
            sendErrorMessage(boost::str(boost::format("Auto%1%DisableTime %2% invalid") % toString(strategy->strategyId()) % cell.value()),
                             ErrorType::Normal);
            cell.set_value(orgValue);
        }
    }

    COLD
    void init(double frequency,
              uint64_t internalHeartbeatPeriod,
              std::map<hf::VENUE, uint32_t>& transactionCapacity,
              om::instance_id_t iid,
              const std::string& economicNumbersCsv) {
        _internalHeartbeatPeriod = internalHeartbeatPeriod;
        for(const auto& kvp : transactionCapacity) {
            const auto& candidateCapacity = reinterpret_cast<const int16_t&>(kvp.second);
            if(candidateCapacity > std::numeric_limits<std::remove_all_extents_t<decltype(_transactionCapacity)>>::max()) {
                logging::GetLogger().logInfo("Transaction capacity too large for venue=" + ven::VenueUtils::getName(kvp.first) + " value=" + std::to_string(candidateCapacity));
            }
            _transactionCapacity[toUInt(kvp.first)] = candidateCapacity;
        }
        _iid = iid;
        logging::GetLogger().logInfo(boost::str(boost::format("Executor initialized. Frequency=%1%") % _frequency));
    }

    bool addEconomicNumber(EconomicNumber& economicNumber, int64_t now) {
        auto diff = economicNumber._datetime - now;
        if (diff > 0 && diff < 86400) {
            economicNumber._beginTime = clockUtil::GetCycles() + GetCyclesFromSeconds(diff, _frequency);
            economicNumber._mmEndTime = economicNumber._beginTime + GetCyclesFromSeconds(economicNumber._mmDuration, _frequency);
            economicNumber._rvEndTime = economicNumber._beginTime + GetCyclesFromSeconds(economicNumber._rvDuration, _frequency);
            economicNumber._asEndTime = economicNumber._beginTime + GetCyclesFromSeconds(economicNumber._asDuration, _frequency);
            if (_shutdownTime == (uint64_t)-1 || economicNumber._beginTime < _shutdownTime) {
                if (economicNumber._mmDuration > economicNumber._rvDuration ||
                    economicNumber._mmDuration > economicNumber._asDuration) {
                    sendErrorMessage(boost::str(boost::format("Not adding economic number %1%. mmDuration must be <= rv/as Duration. mmDuration=%2% rvDuration=%3% asDuration=%4%")
                        % economicNumber._id % economicNumber._mmDuration % economicNumber._rvDuration % economicNumber._asDuration),
                        ErrorType::Critical);
                    return false;
                }
                _economicNumbers.insert(&economicNumber);
                return true;
            }
        }
        return false;
    }

    void initialize() {
        try {
            _updatedContracts.reserve(_viperConfig._allContracts.size());
            auto now = hf::util::TimeUtils::clock_gettime() / 1000000000;
            for (auto it = _viperConfig._mapIdEconomicNumber.begin(); it != _viperConfig._mapIdEconomicNumber.end(); ++it) {
                if (it->second._tradeName == _tradeName) {
                    auto& en = it->second;
                    logging::GetLogger().logInfo(boost::str(boost::format("EconomicNumber %1%. Id=%2% at %3% mmDuration=%4%Sec rvDuration=%5%Sec asDuration=%6%Sec dateTime=%7% now=%8%")
                        % to_string(en._color) % en._id % en._datetimeString % en._mmDuration % en._rvDuration
                        % en._asDuration % en._datetime % now));
                    en._strategyDisabled.reserve(_strategies.size());
                    en._strategyQuoteExtDisabled.reserve(_strategies.size());
                    addEconomicNumber(en, now);
                }
            }

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

            if(!_slow._b3_hb_contract_str.empty()) {
                _b3hb._b3_hb_contract = context().getSymbolID(_slow._b3_hb_contract_str);
                if(_b3hb._b3_hb_contract == hf::NULL_SYMBOLID) {
                    logging::GetLogger().logErrorAndThrow("Specified b3_hb_contract cannot be found in context.  symbol=" + _b3hb._b3_hb_contract);
                }
                logging::GetLogger().logInfo("b3_hb_contract specified.  symbol=" + _slow._b3_hb_contract_str + " sid=" + std::to_string(_b3hb._b3_hb_contract));
                const auto& symbolInfo = context().getSymbolInfo(_b3hb._b3_hb_contract);
                orderrouter::RoutingSpec spec(symbolInfo.getVenue(), _b3hb._b3_hb_contract, orderrouter::ROUTE_OPTIONS::NO_OPTIONS_SPECIFIED);
                orderrouter::RoutingPolicy policy = context().getRoutingPolicy(spec);
                for(const auto& connection : policy.getAllConnections()) {
                    std::vector<strat::om_queue_info_t *> piece(1, connection);
                    _b3hb._hbPolicies.emplace_back(context().getRoutingPolicyForGivenConnections(spec, piece));
                }
                _b3hb._rrc.resize(policy.getAllConnections().size());
            }
            _b3hb._oneMillisecondInTicks = GetCyclesFromMilliSeconds(1, _frequency);

            for (uint32_t i = 0; i < _viperConfig._allContracts.size(); ++i) {
                const ConfigContract &contract = _viperConfig._allContracts[i];
                auto& state = _contractStates[i];
                if (contract._toMarketMake) {
                    const StackerConfig &stackerConfig = _viperConfig._stackerConfigs[contract._marketMakeIndex];
                    const RelativeValueConfig &rvConfig = _viperConfig._relativeValueConfigs[contract._marketMakeIndex];
                    hf::price_t rvHedgeMatchTolerance = ToRhonePrice(rvConfig._hedgeTolerance);
                    state._rhoneRouter.init(contract._venue, _b3hb._b3_hb_contract == hf::NULL_SYMBOLID, contract._sid, _frequency, _b3hb._rrc, this->context());
                    if (!state._rhoneRouter.hasConnections(contract._venue)) {
                        for (const auto& conn : context().getAllExchangeConnections()) {
                            logging::GetLogger().logInfo(boost::str(boost::format("Conn=%1% venue=%2% port_gid=%3% is_convenient=%4%")
                                % conn.getName() % to_string(strat::venue(conn)) % strat::port_gid(conn).value() % strat::is_convenience(conn)));
                        }
#ifndef BACKTEST
                        logging::GetLogger().logErrorAndThrow(boost::str(boost::format("No viable connection for %1% %2%")
                            % to_string(contract._venue) % contract._name));
#endif
                    }
                    state._ime.initialize(this, contract, stackerConfig, rvHedgeMatchTolerance,
                                          fillBannerPeriod, quoteBannerPeriod, hedgeBannerPeriod, selfMatchBannerPeriod,
                                          state._books, _viperConfig._iceCancelResting);
                    state._contract = contract;
                }
                _usedVenues.insert(contract._venue);
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
            throw;
        }
    }

    COLD
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

        for (hf::VENUE venue : _usedVenues) {
            if (!_transactionCapacity[toUInt(venue)]) {
                logging::GetLogger().logErrorAndThrow(boost::str(boost::format("does not have transaction capacity for %1%") % (int)venue));
            }
        }

        //For the B3 round robin to work correctly, all B3 RoundRobin vectors must be in the same order
        RhoneRouter<B3_ROUTER_VENUE>* firstFoundRouter = nullptr;
        for(unsigned i = 0; i < _contractStates.size(); ++i) {
            ContractState& state = _contractStates[i];
            if(state._rhoneRouter.hasAssignedB3Router()) {
                if(firstFoundRouter == nullptr) {
                    firstFoundRouter = &state._rhoneRouter;
                }
                if(!state._rhoneRouter.sameOMQueueRoundRobinBehaviour(*firstFoundRouter)) {
                    logging::GetLogger().logErrorAndThrow("Not all assigned b3 routers have the same Round Robin setup");
                }
            }
        }

        //Also, this order must be consistent with the order of the heartbeat routs
        if(firstFoundRouter == nullptr) {
            if(!_b3hb._hbPolicies.empty()) {
                logging::GetLogger().logErrorAndThrow("No b3 routers found assigned to contracts, but heartbeat router exists");
            }
        }
        else {
            if(_b3hb._hbPolicies.size() != firstFoundRouter->numRoutes()) {
                logging::GetLogger().logErrorAndThrow("Number of routes in heartbeat route map does not match number of routes in router.  hbRouteMap=" + std::to_string(_b3hb._hbPolicies.size()) +
                    " firstFoundRouter=" + std::to_string(firstFoundRouter->numRoutes()));
            }
            else {
                for(unsigned i = 0; i < _b3hb._hbPolicies.size(); ++i) {
                    orderrouter::RoutingPolicy& hbPolicy = _b3hb._hbPolicies[i];
                    //We've already validated all are in the same order, so we only nee to do this check versus one
                    if(!firstFoundRouter->validateSameOMQueueAtIndex(i, &hbPolicy.getNextConnection().get())) {
                        logging::GetLogger().logErrorAndThrow("Heartbeat route queue order did not match round robin queue order");
                    }
                }
            }
        }
    }

    bool gatherRhoneLimitsAndSendToRV(om::port_gid_t port_gid) {
        std::unordered_map<uint32_t, int64_t> internalIdToLimit;

#ifndef BACKTEST
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
#endif

        //Send the limits
        if(!internalIdToLimit.empty()) {
            auto dataSize = internalIdToLimit.size() * sizeof(RhoneLimit);
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
                    }, dataSize);
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

    template<typename IteratorFunc>
    void foreachStrategy(IteratorFunc&& iteratorFunc) {
        for (auto* strategy : _strategies) {
            iteratorFunc(strategy);
        }
    }

    void onExchangeQuoteExtChange(const SharedSystemState::QuoteExtBitset& quoteExt) {
        LOG(0, "EXE. ExchangeQuoteExternal Switch Changed. {i1} -> {i2}", _oldQuoteExt.to_ulong(), quoteExt.to_ulong())
        for (uint8_t i = 1; i < static_cast<uint8_t>(hf::VENUE::ANY); ++i) {
            if (_oldQuoteExt[i] != quoteExt[i]) {
                sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Enabled),
                                          (quoteExt.test(i) ? proto::SystemState::SystemStateEnabled : proto::SystemState::SystemStateDisabled),
                                          i + toUInt(StrategyId::Execution) * 2);
            }
        }
    }

    void handleShutdown() {
        LOG(0, "Executor - shutting down {i1}", 0)
        rhone::util::Service::exitAll();
        _shutdownMDAndGateway = true;
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

        if(disableUseGTC) {
            int remainder = _slow._iopTracking.cancelGTCs(internalId);
            if(remainder) {
                LOG(0, "EXE: Contract {C[i1]} GTCs still in market after attempting to disable.  Canceling remainder.  NumRemainderCancelled={i2}", getContract(internalId), remainder);
                sendErrorMessage("Contract" + getContractName(internalId) + ": GTCs found in market after OM attempted to disable GTCs.  See previous rows for more details GTCs.  NumberFoundAndCancelled=" + std::to_string(remainder), ErrorType::Critical);
            }
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
    void onMDFirmOrderExecutionEventBeforeFillArbitration(const md::MDFirmOrderExecutionEvent& event, const om::strategy_order_t& order, const md::MDBatch& /*batch*/) {
        const uint32_t internalId = sidToInternalId(event.sid);
        if(BOOST_LIKELY(internalId != INVALID_INTERNAL_ID)) {
            LOG(0, "EXE {C[i1]}. onMDFirmOrderExecutionEventBeforeFillArbitration called.  This event should not be triggered.  oid={i2}", getContract(internalId), order.oid.value());
            //Conditional is md::MDEventType::FIRM_ORDER_EXECUTION path that leads to order.is_active = false
            if(BOOST_UNLIKELY(order.action_flags.is_active && event.totalExecutedQty > order.executedQty && event.totalExecutedQty >= order.qty)) {
                using ExecutorSpace::s_log;
                LOG(0, "EXE {C[i1]} onMDFirmOrderExecutionEventBeforeFillArbitration will likely suppress OE event!  oid={i2}", getContract(internalId), event.oid.value());
                std::string errorMessage = "MD Fill event likely suppressing fill processing by viper.  OM will be in a bad state for contract=" + getContractName(internalId) + " oid=" + std::to_string(order.oid.value());
                sendErrorMessage(errorMessage, ErrorType::Critical);
                LOG_ERROR(errorMessage);
            }
        }
        else {
            LOG(0, "EXE onMDFirmOrderExecutionEventBeforeFillArbitration called before with unknown contract.  oid={i1}", order.oid.value());
        }
    }
    void onMDFirmOrderExecutionEventWonFillArbitration(const md::MDFirmOrderExecutionEvent &/*event*/, const om::strategy_order_t &/*order*/, const md::MDBatch &/*batch*/) {}
    void onMDFirmOrderExecutionEventLostFillArbitration(const md::MDFirmOrderExecutionEvent &/*event*/, const om::strategy_order_t &/*order*/, const md::MDBatch &/*batch*/) {}

    void onPreMDBatch(const md::MDBatch &batch) override {
#ifdef IS_DEV
        DLOG(0, "EXE received MD batch. bid={i1} extSeqNum={i2} venue={i3}", batch.bid, batch.extSequenceNumber,
             toUInt(batch.venue))
#endif

        if (BOOST_UNLIKELY((batch.venue != hf::VENUE::ICE) && (batch.bid <= _lastMDbid[toUInt(batch.venue)]))) {
            uint64_t eventMask = 0;
            const auto* data = batch.data;
            for (auto i = batch.count; i; --i) {
                const auto& mde = reinterpret_cast<const md::MDEvent&>(*data);
                data += mde.size;
                eventMask |= (1ull << static_cast<uint8_t>(mde.type));
            }
            logging::GetLogger().logWarning(boost::str(boost::format(
                    "EXE DUPLICATE MD BATCH. bid=%1% count=%2% hwTS=%3% srcTS=%4% srcSentTS=%5% stratTS=%6% venue=%7% extSeqNum=%8% eventMask=%9%") %
                batch.bid % batch.count % batch.time.hwTS % batch.time.srcTS % batch.time.srcSentTS % batch.time.stratTS %
                static_cast<int>(batch.venue) % batch.extSequenceNumber % eventMask));
        }
        _lastMDbid[toUInt(batch.venue)] = batch.bid;
    }

    void onPostMDBatch(const md::MDBatch &batch, bool) override {
        for (auto internalId : _mdUpdated) {
            if (internalId < _contractStates.size()) {
                auto& state = _contractStates[internalId];
                state._lastMDbid = batch.bid;
            }
        }
        _mdUpdated.clear();
    }

    COLD
    void logMDUpdate(const md::MDBatch& batch, const md::MDUpdateEvent& event) const {
        LOG(0, "EXE requested to log MDUpdate bid={i1} extSeqNum={i2} implied={i3} cleanedLevelRemoved={i4} buy.upx={i5} buy.uq={i6} sell.upx={i7} sell.uq={i8} l0.bid={i9}@{i10}({i11}) l1.sell={i12}@{i13}({i14})",
            batch.bid, batch.extSequenceNumber, event.implied, event.cleanedLevelRemoved, event.buy.updatedPriceNum, event.buy.updatedQtyNum,
            event.sell.updatedPriceNum, event.sell.updatedQtyNum, event.buy.top.l[0].pr, event.buy.top.l[0].q, event.buy.top.l[0].o, event.sell.top.l[0].pr, event.sell.top.l[0].q, event.sell.top.l[0].o);

        std::stringstream buffer;
        auto id = sidToInternalId(event.sid);

        if(id != INVALID_INTERNAL_ID) {
            buffer << "Contract=" << _contractStates[id]._contract._name;
        }
        else {
            buffer << "Contract=UNKNWON(sid=" << event.sid << ")";
        }

        buffer << " bid=" << batch.bid << " extSeqNum=" << batch.extSequenceNumber << " isImplied=" << event.implied << " cleanedLevelRemoved="
          << event.cleanedLevelRemoved << " buy.upx=" << static_cast<int>(event.buy.updatedPriceNum) << " buy.uq=" << static_cast<int>(event.buy.updatedQtyNum)
          << " sell.upx=" << static_cast<int>(event.sell.updatedPriceNum) << " sell.uq=" << static_cast<int>(event.sell.updatedQtyNum);

        auto addBook =  [&buffer](const char* sideString, const md::top_t& book) {
            buffer << " " << sideString << "={";
            for(int i = 0; i < md::top_t::LEVEL_NUM; ++i) {
                buffer << "{ i= " << i << " p=" << std::to_string(book.l[i].pr) << " q=" + std::to_string(book.l[i].q) << " o=" << std::to_string(book.l[i].o) << "} ";
            }
        };
        addBook("BIDS", event.buy.top);
        addBook("ASKS", event.sell.top);
        logging::GetLogger().logError(buffer.str());
    }

    void onMDUpdate(const md::MDUpdateEvent& event, const md::MDBatch &batch) {
        const uint32_t internalId = sidToInternalId(event.sid);
        if (internalId == INVALID_INTERNAL_ID) {
            return;
        }

        ContractState& state = _contractStates[internalId];
        if (!state._contract._toMarketMake) {
            return;
        }

        _mdUpdated.emplace(internalId);

        auto& bestBid = event.buy.top.l[0];
        auto& bestAsk = event.sell.top.l[0];
        if(event.cleanedLevelRemoved != hf::NULL_PRICE) {
            DLOG(0, "EXE: Contract {C[i1]} Level cleaned from book.  Bid book adjustment due to trade. px={i2} oldBid:{i3}@{i4} newBid:{i5}@{i6} oldOffer:{i7}@{i8} offer:{i9}@{i10} seq={i11}",
                 getContract(internalId), event.cleanedLevelRemoved, state._books[BID].l[0].q, state._books[BID].l[0].pr, bestBid.q, bestBid.pr, state._books[ASK].l[0].q, state._books[ASK].l[0].pr,
                 bestAsk.q, bestAsk.pr, batch.extSequenceNumber);
        }
        if(BOOST_UNLIKELY(!copyBooks(*this, state, event, _updatedContracts, true))) {
            logMDUpdate(batch, event);
        }
    }

    void onMarketDataChange(uint32_t internalId) {
        ContractState& state = _contractStates[internalId];
        if (state._contract._toMarketMake) {
            if(BOOST_UNLIKELY(!state._ime._contractCanTrade)) {
                _slow._iopTracking.onMDUpdateDuringAuction(internalId, state._books[BID].l[0].pr, state._books[ASK].l[0].pr);
            }
        }

        if (state._mdQtyChangeIndex[BID] >= 0) {
            state._ime.template onMDUpdate<BID>(state._books[BID], state._mdPriceChangeIndex[BID], state._mdQtyChangeIndex[BID]);
            state._mdQtyChangeIndex[BID] = -1;
            state._mdPriceChangeIndex[BID] = -1;
        }

        if (state._mdQtyChangeIndex[ASK] >= 0) {
            state._ime.template onMDUpdate<ASK>(state._books[ASK], state._mdPriceChangeIndex[ASK], state._mdQtyChangeIndex[ASK]);
            state._mdQtyChangeIndex[ASK] = -1;
            state._mdPriceChangeIndex[ASK] = -1;
        }
    }

    hf::qty_t getMarketQty(uint32_t side, uint32_t internalId, hf::price_t price) const {
        const ContractState& state = _contractStates[internalId];
        return (side == BID) ? getMarketQty<BID>(state._books[BID], price) : getMarketQty<ASK>(state._books[ASK], price);
    }

    template<uint32_t>
    hf::qty_t getMarketQty(const md::top_t& book, hf::price_t price) const {
        for (const auto& l : book.l) {
            if (l.pr == hf::NULL_PRICE) {
                break;
            }

            if (l.pr == price) {
                return l.q;
            }
        }
        return 0;
    }

    void onMDTrade(const md::MDTradeEvent &event, const md::MDBatch &batch) {
        const uint32_t internalId = _viperConfig.getInternalId(event.sid);
        if (internalId != INVALID_INTERNAL_ID) {
            _mdUpdated.emplace(internalId);
            
            if (batch.venue == hf::VENUE::CME) {
                ContractState& state = _contractStates[internalId];
                if (!state._contract._toMarketMake) {
                    return;
                }

                for (uint32_t i = 0; i < event.num; ++i) {
                    auto& trade = event.trades[i];
                    if (trade.side == hf::SIDE::BUY) {
                        //LOG(0, "EXE {C[i1]}. Bid book adjustment due to trade. {i2}@{d1}", getLoggingContract(internalId), trade.q, ToDollars(trade.pr))
                        handleMDTrade<ContractState, BID>(state, trade, internalId, _updatedContracts);
                    } else if (trade.side == hf::SIDE::SELL) {
                        //LOG(0, "EXE: Contract {C[i1]} Ask book adjustment due to trade. {i2}@{d1}", getLoggingContract(internalId), trade.q, ToDollars(trade.pr))
                        handleMDTrade<ContractState, ASK>(state, trade, internalId, _updatedContracts);
                    }
                }
            }
        }
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
        _mdUpdated.emplace(internalId);
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
                    case hf::VENUE::EUREX:
                        handleEUREXMDStatus(internalId, event, state);
                        break;
                    case hf::VENUE::B3:
                        handleB3MDStatus(internalId, event, state);
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

    void handleB3MDStatus(uint32_t internalId, const md::MDStatusEvent& event, ContractState& state) {
        switch (event.extStatus) {
            case 17: {
                state._ime._contractCanTrade = true;
                state._ime._contractCanCancel = true;
                sendEndIOPTrackingToFront(internalId);
                LOG(0, "EXE {C[i1]}. market data - opened.", getContract(internalId))
                sendErrorMessage(boost::str(boost::format("Contract %1% market data - opened. B3 ExternalQuoting safe to enabled") % getContractName(internalId)), ErrorType::Normal);
                break;
            }
            case 2: {
                state._ime._contractCanTrade = false;
                state._ime._contractCanCancel = true;
                LOG(0, "EXE {C[i1]}. market data - halted.", getContract(internalId))
                if (canExchangeQuoteExternally(hf::VENUE::B3)) {
                    changeExchangeExternalQuoting(hf::VENUE::B3, SystemState::Disabled);
                    sendErrorMessage(boost::str(
                            boost::format("Contract %1% market data - halted. Disable B3 external quoting.") %
                            getContractName(internalId)), ErrorType::Critical);
                } else {
                    sendErrorMessage(boost::str(boost::format("Contract %1% market data - halted.") % getContractName(internalId)), ErrorType::Normal);
                }
                break;
            }
            case 18:
            case 101:
            case 4: {
                state._ime._contractCanTrade = false;
                state._ime._contractCanCancel = false;
                LOG(0, "EXE {C[i1]}. market data - closed({i2}).", getContract(internalId), (uint32_t)(event.extStatus))
                if (canExchangeQuoteExternally(hf::VENUE::B3)) {
                    changeExchangeExternalQuoting(hf::VENUE::B3, SystemState::Disabled);
                    sendErrorMessage(boost::str(
                            boost::format("Contract %1% market data - closed(%2%). Disable B3 external quoting.") %
                            getContractName(internalId) % (uint32_t) event.extStatus),
                                     ErrorType::Critical);
                } else {
                    sendErrorMessage(boost::str(boost::format("Contract %1% market data - closed(%2%).") % getContractName(internalId) % (uint32_t)event.extStatus), ErrorType::Normal);
                }
                break;
            }
            case 21: {
                state._ime._contractCanTrade = false;
                state._ime._contractCanCancel = false;
                LOG(0, "EXE {C[i1]}. market data - entered pre-open. canCancel {i2}", getContract(internalId), state._ime._contractCanCancel)
                if (canExchangeQuoteExternally(hf::VENUE::B3)) {
                    changeExchangeExternalQuoting(hf::VENUE::B3, SystemState::Disabled);
                    sendErrorMessage(boost::str(
                            boost::format("Contract %1% market data - pre-open. Disable B3 external quoting.") %
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

    void handleEUREXMDStatus(uint32_t internalId, const md::MDStatusEvent& event, ContractState& state) {
//            Ones marked with (*) have never been seen in two years of combing cert OMX data
//            2   = Trading Halt *
//            200 = Closed
//            201 = Restricted   *
//            202 = Book
//            203 = Continuous
//            204 = Opening Auction
//            205 = Opening Auction Freeze
//            206 = Intraday Auction
//            207 = Intraday Auction Freeze
//            208 = Circuit Breaker Auction
//            209 = Circuit Breaker Auction Freeze
//            210 = Closing Auction
//            211 = Closing Auction Freeze
//            212 = IPOAuction         *
//            213 = IPOAuction Freeze  *
//            214 = Pre Call           *
//            215 = Call               *
//            216 = Freeze             *
        switch(event.extStatus) {
            case 203: //Continuous
                state._ime._contractCanTrade = true;
                state._ime._contractCanCancel = true;
                sendEndIOPTrackingToFront(internalId);
                LOG(0, "EXE Contract {C[i1]} market data - opened.", getContract(internalId));
                sendErrorMessage(boost::str(boost::format("Contract %1% market data - opened.  EUREX external quoting safe to enable.") % getContractName(internalId)), ErrorType::Normal);
                break;
            case 200: //Closed
                state._ime._contractCanTrade = false;
                state._ime._contractCanCancel = false;
                LOG(0, "EXE Contract {C[i1]} market data - closed.", getContract(internalId))
                if (canExchangeQuoteExternally(hf::VENUE::EUREX)) {
                    changeExchangeExternalQuoting(hf::VENUE::EUREX, SystemState::Disabled);
                    sendErrorMessage(boost::str(
                            boost::format("Contract %1% market data - closed. Disable EUREX external quoting.") % getContractName(internalId)), ErrorType::Critical);
                } else {
                    sendErrorMessage(boost::str(boost::format("Contract %1% market data - closed.") % getContractName(internalId)), ErrorType::Normal);
                }
                break;
            case 2:   /* Halt */
            case 201: /* Restricted */
            case 216: /* Freeze */
            case 213: /* IPO Auction Freeze */
            case 214: /* Pre Call */
            case 215: /* Call */
                // These are states that as described we will handle as a freeze, but have never been seen in market data
                LOG(0, "EXE Contract {C[i1]}. market data - seen rare MD state we're treating as a halt  extStatus={i2}", getContract(internalId), event.extStatus);
                sendErrorMessage("Contract " + getContractName(internalId) + " market data - rare state we're treating as a halt.  extStatus=" + std::to_string(event.extStatus), ErrorType::Normal);
                FALLTHROUGH;
            case 204: /* Opening Auction */
            case 206: /* Intraday Auction */
            case 208: /* Circuit Breaker Auction */
            case 210: /* Closing Auction */
            case 212: /* IPO Auction */
                state._ime._contractCanTrade = false;
                state._ime._contractCanCancel = true;
                LOG(0, "EXE {C[i1]}. market data - auction.  extStatus={i2}", getContract(internalId), event.extStatus);
                if(canExchangeQuoteExternally(hf::VENUE::EUREX)) {
                    changeExchangeExternalQuoting(hf::VENUE::EUREX, SystemState::Disabled);
                    sendErrorMessage("Contract " + getContractName(internalId) + " market data - auction.  Disable Eurex external quoting.", ErrorType::Critical);
                }
                else {
                    sendErrorMessage("Contract " + getContractName(internalId) + " market data - halted due to auction", ErrorType::Normal);
                }
                break;
            case 205: /* Opening Auction Freeze */
            case 207: /* Intraday Auction Freeze */
            case 209: /* Circuit Breaker Auction Freeze */
            case 211: /* Closing Auction Freeze */
                //These states are similar to CME no-cancel, except instead of a cancel reject, the exchange will "hold" the cancel until the market re-opens and
                //cancel the order if it's not matched.  Note that this state is not guaranteed to occur before the market re-opens!
                state._ime._contractCanTrade = false;
                state._ime._contractCanCancel = true;
                LOG(0, "EXE {C[i1]}. market data - Auction Freeze.  extStatus={i2}", getContract(internalId), event.extStatus);
                break;
            case 202: //Book
                LOG(0, "EXE {C[i1]}. market data - Book.", getContract(internalId));
                break; //Explicitly doing nothing
            default: //Should never get here
                LOG(0, "EXE Contract {C[i1] market data - unknown state received.  state={i2}", getContract(internalId), static_cast<int>(event.extStatus));
                sendErrorMessage(boost::str(boost::format("Contract %1% market data - unknown state received.  Tell a dev! extStatus=%2%") % getContractName(internalId) %
                    static_cast<int>(event.extStatus)), ErrorType::Critical);
                break;
        }
    }

    void onMDIntraEventSweep(const md::MDIntraEventSweepEvent& event, const md::MDBatch& batch) {}

    void onMDPreOpenPrice(const md::MDPreOpenPriceEvent &event, const md::MDBatch &/*batch*/) {
        _slow._iopTracking.onIOPUpdate(event);
    }

    void onMDUnknown(const md::MDEvent &event, const md::MDBatch& batch) {
    }
    void onMDCurrencyEvent(const md::MDCurrencyEvent& event, const md::MDBatch& batch) {
    }

    void onMDStatistics(const md::MDStatisticsEvent &event, const md::MDBatch &/*batch*/) {
        for(int i = 0; i < static_cast<int>(event.num); ++i) {
            const auto& stat = event.statistics[i];
            if((stat.type == '4') & (stat.action == 0) & (stat.openCloseSettlFlag == 5)) {
                _slow._iopTracking.onIOPUpdate(event.sid, stat.price, stat.price);
            }
        }
    }

    void onMDQueuePositionUpdate(const md::MDQueuePositionUpdateEvent &event, const md::MDBatch &/*batch*/) {
        for (uint32_t i = 0; i < event.num; ++i) {
            auto& ord = this->context().getOrder(event.orders[i].oid);
            TLOG(0, "**** {i1} queue position: Prc={d1} QtyInfront={i2} OrdInFront={i3} qp_price={d2}", ord.oid.value(),
                 ToDollars(ord.pr), ord.qp.qtyInFront, ord.qp.numOrdersInFront, ToDollars(event.orders[i].price))
            if (!ord.action_flags.is_active) {
                TLOG(0, "**** {i1} queue position: order not active", ord.oid.value())
                continue;
            }

            SetQuantity(ord, event.orders[i].price, LAST_QP_PRICE);

            if (event.orders[i].price == ord.pr) {
                // If not same price, it means the order has changed to different price
                // ignore the qp update
                if (!TestAndSetBit(ord, STACKER_ORDER_FLAGS, SOF_QUEUE_POSITION_UPDATED)) {
                    auto& ime = getInternalMatchingEngine(ord);

                    if (ord.side == hf::SIDE::BUY) {
                        ime._om[BID].template onQueuePositionUpdate<BID>(ord);
                    } else {
                        ime._om[ASK].template onQueuePositionUpdate<ASK>(ord);
                    }
                }

                getStrategy(StrategyId::MarketMaker)->sendMessage<QueuePositionUpdate>(
                        ViperMessageType::QueuePositionUpdate,
                        [this, &ord](QueuePositionUpdate& msg) {
                            msg.contractId = getInternalId(ord);
                            msg.orderId = ord.oid.value();
                            msg.qp = ord.qp.qtyInFront;
                        });
            } else {
                LOG(0, "EXE. Skip old queue position for order OID={i2}. price={d1} qp_price={d2}", ord.oid.value(),
                    ToDollars(ord.pr), ToDollars(event.orders[i].price))
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
        onOrderExecution(event, batch, order);
    }

    void onOMQuote(const om::OMQuoteEvent& event, const om::OMBatch& batch) {}
    void onOMQuoteCancel(const om::OMQuoteCancelEvent& event, const om::OMBatch& batch) {}
    void onOMQuoteRequestCancel(const om::OMQuoteRequestCancelEvent& event, const om::OMBatch& batch) {}

    void onOrderMultiLegAccepted(const om::OMOrderMultiLegAcceptedEvent& event, const om::strategy_multi_leg_order_t& order, const om::OMBatch& batch) {}
    void onOrderMultiLegRejected(const om::OMOrderMultiLegRejectedEvent& event, const om::strategy_multi_leg_order_t& order, const om::OMBatch& batch) {}
    void onOrderRequestForQuoteReject(const om::OMRequestForQuoteReject& event, const om::OMBatch& batch) {}
    void onOrderRequestForQuoteCancelReject(const om::OMRequestForQuoteCancelReject& event, const om::OMBatch& batch) {}
    void onOrderTimedOut(const om::OMOrderTimedOutEvent& event, const om::strategy_order_t& order, const om::OMBatch& batch) {}
    void onOrderMultiLegTimedOut(const om::OMOrderMultiLegTimedOutEvent& event, const om::strategy_multi_leg_order_t& order, const om::OMBatch& batch) {}
    FillProcessingStatus onOMOrderMultiLegExecution(const om::OMMultiLegExecutionEvent& event, const om::strategy_multi_leg_order_t& order, const om::OMBatch& batch) { return FillAccepted; }

    void onOMOrderExecutionLostFillArbitration(const om::OMExecutionEvent& /*event*/, const om::strategy_order_t& /*order*/, const om::OMBatch& /*batch*/) {}
    FillProcessingStatus onOrderSpreadLegExecution(const om::OMSpreadLegExecutionEvent &/*event*/, const om::strategy_order_t &/*order*/, const om::OMBatch& /*batch*/) { return FillAccepted; }
    void onMassCancelAccepted(const om::OMMassCancelAcceptedEvent &/*event*/, const om::OMBatch& /*batch*/) {}
    void onMassCancelRejected(const om::OMMassCancelRejectedEvent &/*event*/, const om::OMBatch& /*batch*/) {}
    void onTraderLogInOutResponse(const om::TraderLogInOutResponseEvent &/*event*/, const om::OMBatch& /*batch*/) {}
    void onThrottleThresholdWarning(const om::ThrottleThresholdWarning &/*event*/, const om::OMBatch& /*batch*/) {}

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

    void onSessionReject(const om::OMSessionRejectEvent& event, const om::OMBatch& /*batch*/) {
        onCriticalError(boost::str(boost::format("onSessionReject. reason=%1%") % event.reasonText));
    }

    void onUnhandledOMEvent(const om::OMEvent &/*event*/, const om::OMBatch& /*batch*/) {}

    void onExecutionMissingInfo(const om::OMExecutionMissingInfoEvent& event, const om::OMBatch& /*batch*/) {
        if (event.is_leg_fill) {
            return;
        }

        auto components = event.unique_order_id.decode(event.venue);
        std::string msg = boost::str(boost::format("onExecutionMissingInfo: externalGWSeqNum=%1% VENUE=%2% Source=%3% "
                                                   "OrderType=%4% sessionId=%5% instance_id=%6% order_seq_num=%7% "
                                                   "order_request_id=%8% fillTimeStamp=%9% prot_gid=%10% sym_gid=%11% "
                                                   "sym_sid=%12% exchangeOrderId=%13% exchange_sequence_number=%14% "
                                                   "side=%15% fillPrice=%16% fillQty=%17% totalExecutedQty=%18% "
                                                   "isActive=%19% received_by_strategy=%20%")
                                                   % event.externalGWSequenceNo % ven::VenueUtils::getName(event.venue)
                                                   % toUInt(event.src) % om::to_string(event.order_type)
                                                   % components.m_session_id.value() % components.m_instance_id
                                                   % components.m_order_sequence_number.value()
                                                   % components.m_order_request_id.value() % event.fillTimestamp.to_string()
                                                   % event.port_gid % event.gid % event.sid
                                                   % event.exchangeOrderId.value() % event.exchange_sequence_number
                                                   % event.fill_side % ToDollars(event.filledPrice) % event.filledQty
                                                   % event.totalExecutedQty % event.is_active % event.received_by_strategy);
        logging::GetLogger().logWarning(msg);
        sendErrorMessage(msg, ErrorType::Critical);
    }

    void onSpreadLegExecutionMissingInfo(const om::OMSpreadLegExecutionMissingInfoEvent& event, const om::OMBatch& /*batch*/) {}

    void onRecoveredFillsMissingInfo(const om::OMRecoveredFillsEvent& event, const om::OMBatch& /*batch*/) {
        for (uint16_t i = 0; i < event.num_fills; ++i) {
            auto& execution = event.executions[i];
            if (execution.is_leg_fill) {
                continue;
            }

            auto components = execution.unique_order_id.decode(execution.venue);
            std::string msg = boost::str(boost::format("onRecoveredFillsMissingInfo: VENUE=%1% sessionId=%2% "
                                         "instance_id=%3% order_seq_num=%4% order_request_id=%5% fillTimeStamp=%6% "
                                         "prot_gid=%7% sym_gid=%8% sym_sid=%9% exchangeOrderId=%10% "
                                         "exchange_sequence_number=%11% side=%12% fillPrice=%13% fillQty=%14% "
                                         "totalExecutedQty=%15% isActive=%16% received_by_strategy=%17%")
                                         % ven::VenueUtils::getName(execution.venue)
                                         % components.m_session_id.value() % components.m_instance_id
                                         % components.m_order_sequence_number.value()
                                         % components.m_order_request_id.value() % execution.fillTimestamp.to_string()
                                         % execution.port_gid % execution.gid % execution.sid
                                         % execution.exchangeOrderId.value() % execution.exchange_sequence_number
                                         % execution.fill_side % ToDollars(execution.filledPrice) % execution.filledQty
                                         % execution.totalExecutedQty % execution.is_active % execution.received_by_strategy);
            logging::GetLogger().logWarning(msg);
            sendErrorMessage(msg, ErrorType::Critical);
        }
    }

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

        LOG(0, "EXE: {i1} gateway status changed. port_gid={i2} Active={i3}", static_cast<uint32_t>(venue), event.port_gid.value(), _gatewayStatus._active)
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
                        strncpy(message._message, _gatewayStatus._message.c_str(), sizeof(message._message));
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
            BOOST_ASSERT_MSG(order.leavesQty == event.acceptedQty,  "leavesQty managed by Strategy.hpp in Rhone4");
            const_cast<om::strategy_order_t&> (order).leavesQty = event.acceptedQty;
            if (!isFAKHedge(order)) {
                auto& ime = getInternalMatchingEngine(order);
                TLOG(0, "OrderAccepted: oid={i1} rhone_oid={i2} exch_oid={i3} qp={i4}", order.oid.value(),
                     order.rhone_oid.value(), order.exchangeOrderId.value(), order.qp.qtyInFront)
                ime.onOrderAccepted(event, order);
                // qp update might come before new order ack
                sendOrderUpdateToMM(order.oid.value(), ToViperSide(order.side), order.pr, order.qty, order.qp.qtyInFront, ime._internalId);
            }
        } else {
            LOG(0, "Received Order Accepted when logged out. {i1}", event.strat_oid.value())
        }
    }

    void sendOrderUpdateToMM(uint32_t oid, uint32_t side, hf::price_t price, hf::qty_t qty, hf::qty_t qp, uint32_t internalId) {
        getStrategy(StrategyId::MarketMaker)->sendMessage<OrderUpdate>(
                ViperMessageType::OrderUpdate,
                [=](OrderUpdate& msg) {
                    msg.orderId = oid;
                    msg.contractId = internalId;
                    msg.price = price;
                    msg.qty = qty;
                    msg.qp = qp;
                    msg.side = side;
                });
    }

    void onOrderRejected(const om::OMOrderRejectedEvent &event, const om::strategy_order_t &order, const om::OMBatch& batch) {
        if (isLoggedIn()) {
            if (isFAKHedge(order)) {
                auto internalId = getInternalId(order);
                LOG(0, "Hedge order rejected. Contract={C[i1]} OID={i2} Reason={i3}", getContract(internalId), order.oid.value(), event.reasonCodeExt)
                if (IsSelfMatchReject(order.venue, event.reasonCodeExt)) {
                    ++_rvHedgeRejectCount;
                    if (_rvHedgeRejectCount == RV_HEDGE_REJECT_COUNT_LIMIT) {
                        handleFAKHedgeRejectLimitBreach(order);
                    } else {
                        sendReverseFillToStrategyExt(getStrategyId(order), internalId, order, FillSource::SelfMatch, batch.bid, event.orderSnapshot.exchangeOrderId.value());
                    }
                } else {
                    reverseFAKHedgeFillToMarketMaker(order);
                    if (BOOST_LIKELY(!IsRejectDueToMarketClosed(order.venue, event.reasonCodeExt))) {
                        auto strategy = getStrategyId(order);
                        disableContractExternalQuoteOnAllStrategy(getInternalId(order), boost::str(boost::format("%1% hedge reject. Code=%2% Text=%3%") % toString(strategy) % event.reasonCodeExt % event.reasonText));
                    }
                }
            } else {
                if(BOOST_LIKELY(order.sid != _b3hb._b3_hb_contract)) {
                    auto &ime = getInternalMatchingEngine(order);
                    ime.onOrderRejected(event, order);
                }
            }
        } else {
            LOG(0, "Received Order Rejected when logged out. {i1}", event.strat_oid.value())
        }
    }

    void handleFAKHedgeRejectLimitBreach(const om::order_t& order) {
        _rvHedgeRejectCount = 0;
        _strategies[toUInt(getStrategyId(order))]->disable();
        reverseFAKHedgeFillToMarketMaker(order, true);
    }

    void reverseFAKHedgeFillToMarketMaker(const om::order_t& order, bool checkLimit = false) {
        const uint32_t internalId = getInternalId(order);
        const hf::qty_t unfilledSize = order.qty - order.executedQty;
        const uint32_t reverseSide = order.side == hf::SIDE::BUY ? 1 : 0;
        auto source = getStrategyId(order) == StrategyId::RelativeValue ? FillSource::RvHedgeReject : FillSource::AsHedgeReject;
        onExternalFill(internalId, StrategyId::MarketMaker, order.pr, unfilledSize, reverseSide, source, true, checkLimit);
        const auto& state = _contractStates[internalId];
        onEndOfFills(internalId, reverseSide, false, state._ime._lastMMOrderExecution);
    }

    void onOrderReplaced(const om::OMOrderModifiedEvent &event, const om::strategy_order_t &order, const om::OMBatch& /*batch*/) {
        if (isLoggedIn()) {
            auto& ime = getInternalMatchingEngine(order);
            TLOG(0, "OrderReplaced: oid={i1} rhone_oid={i2} exch_oid={i3}({i4})", order.oid.value(),
                 order.rhone_oid.value(), order.exchangeOrderId.value(), __bswap_64(order.exchangeOrderId.value()))
            ime.onOrderReplaced(event, order);
            hf::qty_t qp = om::strategy_order_t::queue_pos_t::NOT_SET;
            if (IsBitSet(order, STACKER_ORDER_FLAGS, SOF_QUEUE_POSITION_UPDATED)) {
                // Either only qty decreased or qp update received before order modify ack
                qp = order.qp.qtyInFront;
            }
            sendOrderUpdateToMM(order.oid.value(), ToViperSide(order.side), order.pr, order.leavesQty, qp, ime._internalId);
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

    void onOrderExecution(const om::OMExecutionEvent &event, const om::OMBatch& batch, const om::order_t &order) {
        if(BOOST_UNLIKELY(order.debug.i[ORDER_OWNER] == UNPROCESSABLE_RELOADED_ORDER)) {
            LOG(0, "EXE Fill for unprocessable GTC.  sid={i1} oid={i2} px={i3} fill_qty={i4} cumul_fill_qty={i5] total_qty={i6} leaves_qty={i7}", order.sid, order.oid.value(),
                    event.filledPrice, event.filledQty, event.totalExecutedQty, order.qty, order.leavesQty);
            sendErrorMessage(boost::str(boost::format("Fill for unprocessable GTC.  Cancel rejected for unprocessable GTC!  sid=%1% oid=%2% px=%3% fill_qty=%4% cumul_fill_qty=%5% total_qty=%6% leaves_qty=%7%")
                % order.sid % order.oid % event.filledPrice % event.filledQty % event.totalExecutedQty % order.qty % order.leavesQty), ErrorType::Critical);
            return;
        }

        if (BOOST_LIKELY(isLoggedIn())) {
            auto& ime = getInternalMatchingEngine(order);
            // send fill to FrondSide for negative scalp checking
            sendMessageToFront<FillMessage>(ViperMessageType::Fill, [=](FillMessage& message) {
                message._contractId = ime._internalId;
                message._side = ToViperSide(order.side);
                message._qty = event.filledQty;
                message._strategy = StrategyId::Execution;  // not used
                message._type = FillType::NegativeScalpCheck;
                message._price = event.filledPrice;
                message._source = FillSource::Quote;    // not used
                message._internalAllocation = 0;
                message._externalAllocation = 0;
                message._rvStf = 0;
            });

            if (!isFAKHedge(order)) {
                if (BOOST_UNLIKELY(order.debug.i[ORDER_OWNER] == UNASSIGNED_OWNER || order.debug.i[ORDER_OWNER] == GTC_OWNER)) {
                    handleFillForUnassignedGtcOrder(event, ime._contract._venue, ime._internalId, order);
                } else {
                    ime.onOrderExecution(event, batch, order);
                }
                sendOrderUpdateToMM(order.oid.value(), ToViperSide(order.side), order.pr, order.leavesQty, order.qp.qtyInFront, ime._internalId);
            } else {
                const uint32_t internalId = ime._internalId;
                LOG(0, "EXE {C[i1]}. FAK Hedge order filled. OID={i2}", getContract(internalId), order.oid.value())
            }
            ime._messages._fills += event.filledQty;
        } else {
            LOG(0, "Received Order Replace Executed when logged out. {i1}", event.strat_oid.value())
        }
    }

    template <typename FillT>
    void handleFillForUnassignedGtcOrder(const FillT& event, hf::VENUE /*venue*/, uint32_t internalId, uint32_t side) {
        onExternalFill(internalId, StrategyId::MarketMaker, event.filledPrice, event.filledQty, side, FillSource::ViperOff);
        const auto& state = _contractStates[internalId];
        onEndOfFills(internalId, side, false, state._ime._lastMMOrderExecution);
    }

    template <typename FillT>
    void handleFillForUnassignedGtcOrder(const FillT& event, hf::VENUE venue, uint32_t internalId, const om::order_t &order) {
        BOOST_ASSERT(order.leavesQty >= 0);
        uint32_t side = order.side == hf::SIDE::BUY ? BID : ASK;
        handleFillForUnassignedGtcOrder(event, venue, internalId, side);
        if((!order.action_flags.is_active) || (!order.leavesQty)) {
            getInternalMatchingEngine(order).tryRemoveGtcOrder(order);
            _slow._iopTracking.stopTrackingOrder(internalId, order.oid);
        }
    }

    void onOrderCanceled(const om::OMOrderCanceledEvent& event, const om::order_t& order, const om::OMBatch& batch) {
        if (BOOST_LIKELY(isLoggedIn())) {
            if (isFAKHedge(order)) {
                auto internalId = getInternalId(order);
                StrategyId strategy = getStrategyId(order);
                LOG(0, "EXE {C[i1]}. Hedge order cancelled. OID={i2}", getContract(internalId), order.oid.value())
                cleanBook(1 - ToViperSide(order.side), internalId, order.pr);
                sendReverseFillToStrategyExt(strategy, internalId, order, FillSource::MissedHedge, batch.bid, event.orderSnapshot.exchangeOrderId.value());
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
                sendOrderUpdateToMM(order.oid.value(), ToViperSide(order.side), order.pr, 0, order.qp.numOrdersInFront, ime._internalId);
            }
        } else {
            LOG(0, "Received Order Cancelled when logged out. {i1}", event.strat_oid.value())
        }
    }

    void cleanBook(uint32_t side, uint32_t internalId, hf::price_t px) {
        auto& state = _contractStates[internalId];
        auto& book = state._books[side];
        bool need_clean_up = false;
        uint32_t i = 0;
        for (; i < md::top_t::LEVEL_NUM; ++i) {
            auto& l = book.l[i];
            if (l.pr == hf::NULL_PRICE || !l.q || !IsBetterOrEqual(l.pr, px, side)) {
                break;
            }
            need_clean_up = true;
        }

        if (need_clean_up) {
            if (BOOST_LIKELY(i < md::top_t::LEVEL_NUM)) {
                auto n = md::top_t::LEVEL_NUM - i;
//                TLOG(0, "EXE {i1}. Side {i2} before clean book", getContract(internalId), side)
//                for (uint32_t i = 0; i < md::top_t::LEVEL_NUM; ++i) {
//                    auto& l = book.l[i];
//                    TLOG(0, "    - {i1}@{d1}", l.q, ToDollars(l.pr))
//                }
                memcpy(&book.l[0], &book.l[i], sizeof(md::top_t::level_t) * n);
                book.l[n].pr = hf::NULL_PRICE;
                book.l[n].q = 0;
//                TLOG(0, "EXE {i1}. Side {i2} after clean book", getContract(internalId), side)
//                for (uint32_t i = 0; i < md::top_t::LEVEL_NUM; ++i) {
//                    auto& l = book.l[i];
//                    TLOG(0, "    - {i1}@{d1}", l.q, ToDollars(l.pr))
//                }
            } else {
                book.l[0].pr = hf::NULL_PRICE;
                book.l[0].q = 0;
            }

            state._mdQtyChangeIndex[side] = 0;
            state._mdPriceChangeIndex[side] = 0;
            _updatedContracts.emplace(internalId);
        }

        foreachStrategy([&book, internalId, side, bid = state._lastMDbid](ViperStrategyBase* strategy) {
            strategy->sendMessage<InternalBookUpdate>(ViperMessageType::InternalBookUpdate, [&book, internalId, side, bid](InternalBookUpdate& msg) {
                msg._contractId = internalId;
                msg._side = side;
                msg._lastMDbid = bid;
                memcpy(&msg._book, &book, sizeof(book));
            });
        });

        LOG(0, "EXE {C[i1]}. Send InternalBookUpdate. lastMDbid={i2} Side={i3} Price={d1} book={i4}@{d2} levelRemoved={i5}", getContract(internalId),
            state._lastMDbid, side, ToDollars(px), book.l[0].q, ToDollars(book.l[0].pr), need_clean_up);
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
        //TODO, we relaly don't want to call the rdstc function here.  Find a better way, split the method up maybe into passed in ts and non-passed-in ts?
        state._rhoneRouter.orderCancelled(state._contract._venue, [](){ return clockUtil::GetCycles(); }, *order);
        ++state._ime._messages._cancels;
    }

    bool canSystemSendOrders() const noexcept {
        static_assert(toUInt(StrategyId::MarketMaker) == 0 && toUInt(StrategyId::RelativeValue) == 1 &&
                      toUInt(StrategyId::ArbieSpreader) == 2 && toUInt(StrategyId::Execution) == 3,
                      "Hard coding strategies in check, need to modify if new strategies");
        return _strategies[toUInt(StrategyId::MarketMaker)]->isEnabled() |
               _strategies[toUInt(StrategyId::RelativeValue)]->isEnabled() |
               _strategies[toUInt(StrategyId::ArbieSpreader)]->isEnabled();
    }

    om::order_t* newLimitOrder(uint32_t internalId, uint32_t side, hf::price_t price, hf::qty_t size, om::ORDER_TYPE orderType, bool isHedge = false) {
        if(BOOST_UNLIKELY(!canSystemSendOrders())) {
            LOG(0, "Attempt to create LMT order Contract={C[i1]} Side={i2} Price={d1} size={i3} disallowed due to Viper gate", getContract(internalId), side, ToDollars(price), size);
            sendErrorMessage(boost::str(boost::format("Attempt to create order Contract=%1% Side=%2% Price=%3% size=%4% disallowed due to Viper gate")
                % _viperConfig._allContracts[internalId]._name % side % ToDollars(price) % size), ErrorType::Critical);
            return nullptr;
        }

        ContractState& state = _contractStates[internalId];
        assert(state._contract._venue != hf::VENUE::B3 || (state._contract.toValidQty(size) == size));
        auto connection = state._rhoneRouter.getRouteForNextNewOrder(state._contract._venue);
        om::order_t* order = this->context().createOrder(_iid, om::RISK_TYPE::ENTERING, state._rhoneRouter.getRoutingPolicyForNextNewOrder(state._contract._venue), orderType,
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
            state._rhoneRouter.setNextRoute(state._contract._venue, now, _b3hb._oneMillisecondInTicks);
        }
        applyQtyDiffToOrderThrottleAndCheck(now, internalId, ToRhoneSide(side), price, size);
        return order;
    }

    void addRoutingStateToNewLimitOrder(om::order_t& order, uint32_t internalId) {
        ContractState& state = _contractStates[internalId];
        auto connection = state._rhoneRouter.getRouteForNextNewOrder(state._contract._venue);  //Assigning all GTCs the same connection?
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
    bool disableRVdisableRVQuoteExternal(hf::VENUE /*venue*/) {
        bool needToNotifyUser = false;
        auto* rv = getStrategy(StrategyId::RelativeValue);
        if (BOOST_LIKELY(rv->isEnabled())) {
            needToNotifyUser = true;
            rv->disable();
            handleStrategyDisabled(StrategyId::RelativeValue);
        }

        if (BOOST_LIKELY(rv->canQuoteExternally())) {
            needToNotifyUser = true;
            rv->disableQuoteExternal();
        }

        if (needToNotifyUser) {
            LOG(0, "EXE. Relative Value Disabled. RV Quote External Disabled. Removing orders. Reason=disableRVdisableRVQuoteExternal {i1}", 0)
            sendErrorMessage("EXE. Relative Value Disabled. RV Quote External Disabled. Removing orders. Reason=disableRVdisableRVQuoteExternal", ErrorType::Critical);
        }

        return needToNotifyUser;
    }

    void disableMMExternalQuoting(uint32_t id) {
        getStrategy(StrategyId::MarketMaker)->sendGenericMessage(ViperMessageType::QuoteExternallyOff, id);
    }

    void notifyFrontUseGTCChanged(int32_t id, bool enabled) {
        sendGenericMessageToFront(ViperMessageType::ChangeUseGTC, id, enabled);
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

            it->second.initialize(_frequency, port_gid, 1, _transactionCapacity[toUInt(strat::venue(*connection))], percentMsgReduceQuote, percentMsgHedges, inDangerZonePeriod, atCapacityPeriod);
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

    om::order_t* newFAKOrder(uint32_t internalId, uint32_t side, hf::price_t price, hf::qty_t size, uint32_t hedgeId, StrategyId strategy) {

        if(BOOST_UNLIKELY(!canSystemSendOrders())) {
            LOG(0, "Attempt to create FAK order Contract={C[i1]} Side={i2} Price={d1} size={i3} disallowed due to Viper gate", getContract(internalId), side, ToDollars(price), size);
            sendErrorMessage(boost::str(boost::format("Attempt to create order Contract=%1% Side=%2% Price=%3% size=%4% disallowed due to Viper gate")
                                        % _viperConfig._allContracts[internalId]._name % side % ToDollars(price) % size), ErrorType::Critical);
            return nullptr;
        }

        ContractState& state = _contractStates[internalId];
        assert(state._contract._venue != hf::VENUE::B3 || (state._contract.toValidQty(size) == size));
        om::order_t* order = this->context().createOrderExt(_iid, om::RISK_TYPE::TRADING, state._rhoneRouter.getRoutingPolicyForNextNewOrder(state._contract._venue), om::ORDER_TYPE::FAK, ToRhoneSide(side), price, 0, size, state._contract._orderIncrementQty, 0, om::STP_ACTION::CANCEL_PASSIVE);
        if (order == nullptr) {
            LOG(0, "Could not send FAK order. Contract={C[i1]} Side={i2} Price={d1} Size={i3}", getContract(internalId), side, ToDollars(price), size)
        } else {
            LOG(0, "Sent FAK order. Contract={C[i1]} Side={i2} Price={d1} Size={i3} OID={i4}", getContract(internalId), side, ToDollars(price), size, order->oid.value())
            order->debug.i[ORDER_OWNER] = UNASSIGNED_OWNER + toUInt(strategy);
            order->debug.i[INTERNAL_ID] = internalId;
            order->debug.i[HEDGE_ID] = hedgeId;
            ++state._ime._messages._inserts;
            state._rhoneRouter.setNextRoute(state._contract._venue, clockUtil::GetCycles(), _b3hb._oneMillisecondInTicks);
        }
        applyQtyDiffToOrderThrottleAndCheck(clockUtil::GetCycles(), internalId, ToRhoneSide(side), price, size);
        return order;
    }

    void applyQtyDiffToOrderThrottleAndCheck(uint64_t now, uint32_t internalId, hf::SIDE side, hf::price_t price, hf::qty_t diff) {

        const auto& book = _contractStates[internalId]._books;
        _newOrderModifyOrderRateChecker.onNewOrModifyOrder(now, side, price, diff, book[BID].l[0].pr, book[ASK].l[0].pr);

        if(BOOST_UNLIKELY(_newOrderModifyOrderRateChecker.isOverLimit())) {
            LOG(0, "EXE NewOrderModifyOrderRateChecker is over limit.  window(ms)={i1} numClips={i2}/{i3} sumTotalLimit={i4}/{i5}",
                GetMillisFromCycles(_newOrderModifyOrderRateChecker.throttleWindow(), _frequency),
                _newOrderModifyOrderRateChecker.clipCount(), _newOrderModifyOrderRateChecker.maxClips(),
                _newOrderModifyOrderRateChecker.sumTotal(), _newOrderModifyOrderRateChecker.sumTotalLimit());

            //Disable all strategies except MM
            for(auto* strategy : _strategies) {
                if (strategy->strategyId() != StrategyId::MarketMaker) {
                    strategy->disable();
                }
            }

            //Disable all strategy quote external except MM
            for(auto* strategy : _strategies) {
                if(strategy->strategyId() != StrategyId::MarketMaker && strategy->canQuoteExternally()) {
                    strategy->disableQuoteExternal();
                }
            }

            sendErrorMessage("OrderRateChecker is over limit.  Disabling RV + RV ext quote. Window(ms)=" + std::to_string(GetMillisFromCycles(_newOrderModifyOrderRateChecker.throttleWindow(), _frequency)) +
                          " NumClips=" + std::to_string(_newOrderModifyOrderRateChecker.clipCount()) + "/" + std::to_string(_newOrderModifyOrderRateChecker.maxClips()) +
                          " SumTotalLimit=" + std::to_string(_newOrderModifyOrderRateChecker.sumTotal()) + "/" + std::to_string(_newOrderModifyOrderRateChecker.sumTotalLimit()), ErrorType::Critical);
        }
    }

    bool modifyOrder(uint32_t internalId, const om::order_t* order, hf::price_t price, hf::qty_t size) {
        if(BOOST_UNLIKELY(!canSystemSendOrders())) {
            LOG(0, "Attempt to modify oid={i2} disallowed due to Viper gate", order->oid.value());
            sendErrorMessage("Attempt to modify oid=" + std::to_string(order->oid.value()) + " disallowed due to Viper gate", ErrorType::Critical);
            return false;
        }
        ContractState& state = _contractStates[internalId];
        assert(state._contract._venue != hf::VENUE::B3 || (state._contract.toValidQty(size) == size));
        const bool done = this->context().modifyOrder(order->oid, price, size, om::STP_ACTION::CANCEL_PASSIVE, om::RISK_TYPE::ENTERING);
        const uint64_t now = clockUtil::GetCycles();
        if (BOOST_LIKELY(done)) {
            ++state._ime._messages._modifies;
            state._rhoneRouter.orderModified(state._contract._venue, now, *order);
            LOG(0, "Sent modify order. Contract={C[i1]} Side={i2} Price={d1} Size={i3}", getContract(internalId), ToViperSide(order->side), ToDollars(price), size)
            addMessageToThrottler(now, order->debug.i[ROUTE]);
        } else {
            LOG(0, "Could not modify order. Contract={C[i1]} Side={i2} Price={d1} Size={i3}", getContract(internalId), ToViperSide(order->side),ToDollars(price), size)
            return false;
        }
        applyQtyDiffToOrderThrottleAndCheck(now, internalId, order->side, price, size - order->qty);
        return true;
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
        size_t nThrottlers = _throttlers.size();
        for (auto iter = _throttlers.begin(); iter != _throttlers.end(); ++iter) {
            Throttler& throttler = iter->second;
            auto port_gid = throttler.getPortGid().value();
            if (throttler.isBanOn()) {
                if (throttler.didBanExpire(now)) {
                    setThrottlingBanner(throttler, throttler.getNewMode(now));
                }
            }

            if (BOOST_UNLIKELY(nThrottlers != _throttlers.size())) {
                logging::GetLogger().logWarning(boost::str(boost::format("!!!! new throttler added while looping through _throllters in checkTimers. nThrottlers=%1% _throtters.size()=%2%") % nThrottlers % _throttlers.size()));
                // New throttler added when putting quote back to market
                // the iterator might be invalidated
                iter = _throttlers.find(port_gid);
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
            proto::GridCell& gridCell = *update._update.mutable_cells(i);
            TraderSwitchColumns columnType = static_cast<TraderSwitchColumns>(gridCell.col());
            if (columnType == TraderSwitchColumns::Trader) {
                Traders trader = static_cast<Traders>(atoi(gridCell.value().c_str()));
                std::string traderName = GetTraderName(trader);
                this->context().swapTrader(traderName, "");
                _viperConfig._currentTrader = trader;
            } else if (columnType == TraderSwitchColumns::Shift) {
                auto shift = atoi(gridCell.value().c_str());
                if (shift >= 0) {
                    onShiftChanged(shift);
                } else {
                    LOG(0, "Invalid Shift {i1}", shift);
                    sendErrorMessage(boost::str(boost::format("Invalid Shift %1%, set Shift to %2%") % shift % toUInt(_viperConfig._currentShift)), ErrorType::Critical);
                    gridCell.set_value(std::to_string(toUInt(_viperConfig._currentShift)));
                }
            }
        }
    }

    void onShiftChanged(uint32_t shift) {
        _viperConfig._currentShift = static_cast<Shifts>(shift);
        LOG(0, "Shift changed to {i1}", shift)
        for (const auto& config : _viperConfig._globalConfigs) {
            if (config._key == QUOTE_BANNER_PERIOD) {
                uint64_t period = GetCyclesFromMicroSeconds(boost::lexical_cast<uint32_t>(config._shifts[shift]), _frequency);
                for (auto& state : _contractStates) {
                    state._ime.handleQuoteBannerUpdate(period);
                }
            } else if (config._key == FILL_BANNER_PERIOD) {
                uint64_t period = GetCyclesFromMicroSeconds(boost::lexical_cast<uint32_t>(config._shifts[shift]), _frequency);
                for (auto& state : _contractStates) {
                    state._ime.handleFillBannerUpdate(period);
                }
            } else if (config._key == SELF_MATCH_BANNER_MICS) {
                uint64_t period = GetCyclesFromMicroSeconds(boost::lexical_cast<uint32_t>(config._shifts[shift]), _frequency);
                for (auto& state : _contractStates) {
                    state._ime.handleRejectBannerUpdate(period);
                }
            } else if (config._key == MSG_REDUCE_QUOTE_MICS) {
                uint64_t inDangerZonePeriod = GetCyclesFromMicroSeconds(boost::lexical_cast<uint32_t>(config._shifts[shift]), _frequency);
                for (auto& kvp : _throttlers) {
                    kvp.second.updateInDangerZonePeriod(inDangerZonePeriod);
                }
            }
            else if (config._key == MSG_HEDGES_INTERVAL_MICS) {
                uint64_t atCapacityPeriod = GetCyclesFromMicroSeconds(boost::lexical_cast<uint32_t>(config._shifts[shift]), _frequency);
                for (auto& kvp : _throttlers) {
                    kvp.second.updateAtCapacityPeriod(atCapacityPeriod);
                }
            }
            else if (config._key == PERCENT_MSG_HEDGES) {
                double percent = boost::lexical_cast<double>(config._shifts[shift]);
                for (auto& kvp : _throttlers) {
                    kvp.second.updateHedgePercentage(percent);
                }
            }
            else if (config._key == PERCENT_MSG_REDUCE_QUOTE) {
                double percent = boost::lexical_cast<double>(config._shifts[shift]);
                for (auto& kvp : _throttlers) {
                    kvp.second.updateDangerZonePercentage(percent);
                }
            }
            else if (config._key == HEDGE_BANNER_MICS) {
                uint64_t period = GetCyclesFromMicroSeconds(boost::lexical_cast<uint32_t>(config._shifts[shift]), _frequency);
                for (auto& state : _contractStates) {
                    state._ime.handleHedgeQuoteBannerUpdate(period);
                }
            }
            else if(config._key == ORDER_RATE_CLIPS) {
                uint32_t clips = boost::lexical_cast<uint32_t>(config._shifts[shift]);
                _newOrderModifyOrderRateChecker.changeMaxClips(clips, clockUtil::GetCycles());
                LOG(0, "Order Rate Checker.  Update max clips={i1}", clips);
            }
            else if(config._key == ORDER_RATE_SUM_TOTAL) {
                hf::qty_t sumTotal = boost::lexical_cast<hf::qty_t>(config._shifts[shift]);
                _newOrderModifyOrderRateChecker.changeSumTotal(sumTotal, clockUtil::GetCycles());
                LOG(0, "Order Rate Checker.  Update sum total={i1}", sumTotal);
            }
            else if(config._key == ORDER_RATE_PERIOD) {
                uint32_t period = boost::lexical_cast<uint32_t>(config._shifts[shift]);
                uint64_t periodInCycles = GetCyclesFromMilliSeconds(period, _frequency);
                _newOrderModifyOrderRateChecker.changeThrottleWindowSize(periodInCycles, clockUtil::GetCycles());
                LOG(0, "Order Rate Checker. Update period={i1} periodInCycles={i2}", period, periodInCycles);
            }
        }
        getStrategy(StrategyId::MarketMaker)->sendGenericMessage(ViperMessageType::ShiftUpdate, shift);
        getStrategy(StrategyId::RelativeValue)->sendGenericMessage(ViperMessageType::ShiftUpdate, shift);
        getStrategy(StrategyId::ArbieSpreader)->sendGenericMessage(ViperMessageType::ShiftUpdate, shift);

        auto shutdownTime = boost::lexical_cast<uint32_t>(_viperConfig.getGlobalByShift(AUTO_SHUTDOWN_TIME, toUInt(_viperConfig._currentShift)));
        uint32_t hour, min, sec;
        if (shutdownTime == (uint32_t)-1) {
            _shutdownTime = -1;
            LOG(0, "AutoShutdownTime disabled. {i1}", -1)
        } else if (verifyHHMMSSTime(shutdownTime, hour, min, sec)) {
            auto now = clockUtil::GetCycles();
            _shutdownTime = convertTimeToCycles(hour, min, sec, now, _frequency);
            LOG(0, "AutoShutdownTime set to {i1}({i2}), now={i3}", shutdownTime, _shutdownTime, now)
        }
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
    uint64_t _shutdownTime = -1;

    std::string _tradeName;
    std::string _strategyName;
    std::string _strategyVersion;

    std::unordered_set<uint32_t> _mdUpdated;
    std::unordered_set<uint32_t> _updatedContracts;

    int16_t _transactionCapacity[toUInt(hf::VENUE::EUREX) + 1] = {0};
    om::instance_id_t _iid;
    hf::batch_id_t _lastMDbid[toUInt(hf::VENUE::LAST)] = {0};

    uint32_t _rvHedgeRejectCount = 0;
    const uint32_t RV_HEDGE_REJECT_COUNT_LIMIT = 20;

    hf::timer_id_t _timerId = 0;
    bool _hasTimer = false;
    uint8_t _oldMDStatus[static_cast<uint8_t>(hf::VENUE::ANY)] = {0};

    std::unordered_map<om::port_gid_t::value_type, hf::VENUE> _mapGatewayVenue;
    std::set<hf::VENUE> _usedVenues;
    SharedSystemState::QuoteExtBitset _oldQuoteExt;

    std::unordered_set<om::port_gid_t::value_type> _pendingGatewayLogin;
    bool _rhoneLimitsReady = false;
    bool _shutdownMDAndGateway = false;
    std::string _killScript;

    //Only used by b3 heartbeat mechanism
    struct {
        hf::symbol_id_t _b3_hb_contract = hf::NULL_SYMBOLID;
        std::vector<RhoneRouterUtil::RoundRobinContainer<3>> _rrc;
        std::vector<orderrouter::RoutingPolicy> _hbPolicies;
        int _oneMillisecondInTicks;
    } _b3hb;

    struct {
        IOPTracker<Executor> _iopTracking;
        uint64_t lastForceReconcileSeqNum = UINT64_MAX - 1;
        boost::dynamic_bitset<> _potentiallyGapped;
        std::unordered_set<const om::strategy_order_t*> _badReloadedGTCsToCancel;
        bool _allowStartupWithUncontrolledGTCs = false;
        int _numSuccessfulRecoveredOrders = 0;
        int _cumulativeSuccessfulRecoveredOrders = 0;
        hf::qty_t _totalSuccessfulRecoveredOrderQty = 0;
        bool _recoveryNotGood = false;
        std::string _b3_hb_contract_str;
    } __attribute__((aligned(64))) _slow;

    std::set<om::strategy_order_id_t> _recoveredOrders;

    struct EconomicNumberComparer {
        bool operator()(const EconomicNumber* a, const EconomicNumber* b) const {
            return a->nextCheckPoint() < b->nextCheckPoint();
        }
    };
    std::set<EconomicNumber*, EconomicNumberComparer> _economicNumbers;
};

}

#endif //VIPER_EXECUTOR_H
