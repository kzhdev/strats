#ifndef VIPER_EXECUTOR_H
#define VIPER_EXECUTOR_H

#include <hf/ConfigManager.h>
#include <hf/Service.h>
#include <stdint.h>
#include <hf/hf.h>
#include <hf/log.h>
#include <md/md_event.h>
#include <om/om_event.h>
#include <strat/Strategy.hpp>
#include "SimpleMarketMaker.h"
#include "Common.h"
#include "RelativeValue.h"
#include "ViperConfig.h"
#include "InternalMatchingEngine.h"
#include <md/queue/MDQueue.h>
#include "Timer.h"
#include <hf/queue/DebugQueueLogger.h>
#include "Throttler.h"
#include "IOPTracker.h"
#include <hf/util/SocketUtils.h>
#include <sys/types.h>
#include <signal.h>
#include <unordered_set>
#include <ven/VenueUtils.h>
#include <bitset>

namespace Python {

LOCAL_EXPLICIT_LOG_DECLARE("Executor.h", ExecutorSpace)

template <typename T>
struct ContractState {

    using SimpleMarketMakerT = SimpleMarketMaker<InternalMatchingEngine<T>>;

    InternalMatchingEngine<T> _ime;
    SimpleMarketMakerT _marketMaker;
    Book _books[2];
    hf::VENUE _venue;
    uint32_t _throttlerIndex;
    uint_fast64_t _mdSequenceNumber;
    om::routing_id_t _rid[10];
};

struct GatewayStatus {
    std::string _message;
    bool _active = true;
};

template<typename StrategyT>
class Executor : public StrategyT {

public:
    typedef ContractState<Executor> ContractStateT;

    Executor()
    :
        _fillBannerTimer(*this),
        _quoteBannerTimer(*this),
        _checkQuotesTimer(*this)
    {
        _manualFills.resize(5);
    }

    void onStratBegin() override {
        resetQueues();
        initialize(*_config);
        if (!this->context().strategyLogin(_strategyName, _strategyVersion)) {
            logging::GetLogger().logErrorAndThrow(boost::str(boost::format("Strategy login did not work. Name=%1% Version=%2%") % _strategyName % _strategyVersion));
        }
        LOG(0, "Executor: Started {i1}", 0)
    }

    void resetQueues() {
        _inFromFront->resetR();
        _inFromRV->resetR();
        _outToRV->resetW();
        _outToFront->resetW();
    }

    void onStratEnd() override {
        LOG(0, "Executor: Ended. {i1}", 0)
    }

#ifdef IS_DEV
    std::string getContract(uint32_t id) {
        return getContractName(id);
    }
#else
    uint32_t getContract(uint32_t id) {
        return id;
    }
#endif

    void onPostCreate(const libconfig::Setting& config) override {
        {
            const auto queueName = hf::ConfigManager::getString(config, "queue_to_front");
            auto *const ptr = hf::ResourceManager::getResourceP(queueName);
            if (!ptr) {
                logging::GetLogger().logErrorAndThrow(boost::str(boost::format("Cannot Find Queue Resource by name '%1%'") % queueName));
            }
            _outToFront = dynamic_cast<md::queue::MDQueueResource *> (ptr);
            if (!_outToFront) {
                logging::GetLogger().logErrorAndThrow(boost::str(boost::format("Cannot Find Queue Resource by name '%1%'") % queueName));
            }
        }

        {
            const auto queueName = hf::ConfigManager::getString(config, "queue_from_front");
            auto *const ptr = hf::ResourceManager::getResourceP(queueName);
            if (!ptr) {
                logging::GetLogger().logErrorAndThrow(boost::str(boost::format("Cannot Find Queue Resource by name '%1%'") % queueName));
            }
            _inFromFront = dynamic_cast<md::queue::MDQueueResource *> (ptr);
            if (!_inFromFront) {
                logging::GetLogger().logErrorAndThrow(boost::str(boost::format("Cannot Find Queue Resource by name '%1%'") % queueName));
            }
        }

        {
            const auto queueName = hf::ConfigManager::getString(config, "queue_to_relative_value");
            auto *const ptr = hf::ResourceManager::getResourceP(queueName);
            if (!ptr) {
                logging::GetLogger().logErrorAndThrow(boost::str(boost::format("Cannot Find Queue Resource by name '%1%'") % queueName));
            }
            _outToRV = dynamic_cast<md::queue::MDQueueResource *> (ptr);
            if (!_outToRV) {
                logging::GetLogger().logErrorAndThrow(boost::str(boost::format("Cannot Find Queue Resource by name '%1%'") % queueName));
            }
        }

        {
            const auto queueName = hf::ConfigManager::getString(config, "queue_from_relative_value");
            auto *const ptr = hf::ResourceManager::getResourceP(queueName);
            if (!ptr) {
                logging::GetLogger().logErrorAndThrow(boost::str(boost::format("Cannot Find Queue Resource by name '%1%'") % queueName));
            }
            _inFromRV = dynamic_cast<md::queue::MDQueueResource *> (ptr);
            if (!_inFromRV) {
                logging::GetLogger().logErrorAndThrow(boost::str(boost::format("Cannot Find Queue Resource by name '%1%'") % queueName));
            }
        }

        _strategyName = hf::ConfigManager::getString(config, "strategy_name");
        _strategyVersion = hf::ConfigManager::getString(config, "strategy_version");

        _inFromRV->resetR();
        _inFromFront->resetR();
        _outToRV->resetW();
        _outToFront->resetW();

        const std::string loggerName = config["kdb_logger"];
        _kdbLogger = &(dynamic_cast<hf::DebugQueueLogger*> (hf::ResourceManager::getResourceP(loggerName)))->queue();

        _nextInternalHeartbeat = 0;

        if (config.exists("md-queues")) {
            const auto &queuesConfig = config["md-queues"];

            for (int i = 0; i < queuesConfig.getLength(); ++i) {
                const libconfig::Setting &queueConfig = queuesConfig[i];

                if (!hf::ConfigManager::getBool(queueConfig, "enabled", true)) continue;

                const auto name = hf::ConfigManager::getString(queueConfig, "name");

                auto *const ptr = dynamic_cast<md::queue::MDQueueResource *> (hf::ResourceManager::getResourceP(name));
                if (!ptr) {
                    logging::GetLogger().logErrorAndThrow(boost::str(boost::format("Cannot Find MDQueue by name '%1%'") % name));
                }

                if (!ptr->getPoolP()) {
                    logging::GetLogger().logErrorAndThrow(boost::str(boost::format("MDPool is not set for MDQueue with name '%1%'") % name));
                }

                addMarketDataQueue(ptr);

                _id = hf::ConfigManager::getLong(queueConfig, "id");
            }
        }
    }

    void addMarketDataQueue(md::queue::MDQueue* q) {
        _marketDataQueues. push_back(q);
    }

    virtual void onStratLoop() override {
        if (BOOST_UNLIKELY(_needToSetLogger)) {
            _needToSetLogger = false;
            _kdbLogger->resetW();
            logging::setKdbLogger(_kdbLogger);
            logging::GetLogger().logInfo(boost::str(boost::format("EX logger %1%") % _kdbLogger));
            for (auto queue : _marketDataQueues) {
                subscribeToMarketData(queue);
            }
        }
        processMessagesFromRV();
        processMessagesFromFront();
        checkSystemState();
        uint64_t now = clockUtil::GetCycles();
        checkTimers(now);
        sendInternalHeartbeat(now);
        drainAllMarketData();
    }

    void sendInternalHeartbeat(uint64_t now) {
        if (BOOST_UNLIKELY(now > _nextInternalHeartbeat)) {
            _nextInternalHeartbeat = now + _internalHeartbeatPeriod;
            sendGenericMessageToFront(ViperMessageType::InternalHeartbeat);
        }
    }

    void processMessagesFromRV() {
        while (true) {
            uint64_t idx = _inFromRV->getNext();

            if (BOOST_UNLIKELY(idx == 0))
                return;

            _inFromRV->freeNext();

            auto& message = *static_cast<ViperMessage*>(_inFromRV->getPoolP()->getData(idx));

            switch (message._type) {
                case ViperMessageType::QuoteOrder: {
                    QuoteOrderMessage& order = reinterpret_cast<QuoteOrderMessage &> (message._data[0]);
                    onQuoteOrderFromRv(order);
                    break;
                }
                case ViperMessageType::HedgeOrder: {
                    HedgeOrderMessage& order = reinterpret_cast<HedgeOrderMessage &> (message._data[0]);
                    onHedgeOrderFromRv(order);
                    break;
                }
                case ViperMessageType::Fill: {
                    FillMessage& fill = reinterpret_cast<FillMessage &> (message._data[0]);
                    handleReturnedFillFromRelativeValue(fill);
                    break;
                }
                case ViperMessageType::FirstOrdersDone: {
                    reconcileGtcOrdersAfterRvEnabled();
                    break;
                }
                case ViperMessageType::EndOfQuotes: {
                    GenericMessage& reply = reinterpret_cast<GenericMessage &> (message._data[0]);
                    sendGenericMessageToRV(ViperMessageType::EndOfQuotes, reply._number_1);
                    break;
                }
                case ViperMessageType::MMQuoteExternallyOffAll: {
                    if(BOOST_LIKELY(_systemState->_marketMaker.load(std::memory_order_relaxed) == SystemState::Enabled)) {
                        disableExternalQuotingOnAllMMInstrumentsKernel();
                        LOG(0, "EXE Disabled external quoting on all MarketMaker instruments due to RV request {i1}", 0);
                        sendErrorMessage("Disabling external quoting on all MarketMaker instruments in response to RV request", ErrorType::Critical);
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }

    void handleReturnedFillFromRelativeValue(FillMessage& fill) {
        LOG(0, "Executor: Handling fill returned from RV. Contract={C[i1]} Side={i2} Price={d1} Size={i3}", getLoggingContract(fill._contractId), fill._side, ToDollars(fill._price), fill._qty)
        ContractStateT& state = _contractStates[fill._contractId];
        state._marketMaker.forceTakeOverFill(fill._side, fill._qty);
        sendFillToFront(fill._contractId, fill._side, fill._price, fill._qty, StrategyId::MarketMaker, FillType::External, FillSource::RvDisable, 0, fill._qty, 0);
        if (!state._marketMaker.isWithinPositionLimits()) {
            auto formatStr = CREATE_SAFE_FORMATTER("Handling fill returned from RV.  Contract=%1% Side=%2% Price=%3% Size=%4%");
            disableMMExternalQuoteWithMessage(fill._contractId, static_cast<std::string>(formatStr % getLoggingContract(fill._contractId) % fill._side % ToDollars(fill._price) % fill._qty));
        } else {
            state._marketMaker.onEndOfFills();
        }
    }

    void onQuoteOrderFromRv(QuoteOrderMessage& order) {
        if (_systemState->_relativeValue.load(std::memory_order_relaxed) == SystemState::Enabled) {
            ContractStateT& state = _contractStates[order._instrumentIndex];
            state._ime.onQuoteOrderFromRelativeValue(order);
        }
    }

    void sendRelativeValueHedgeInternalFillToFront(uint32_t internalId, uint32_t side, hf::price_t price, hf::qty_t size, hf::price_t stf) {
        sendFillToFront(internalId, side, price, size, StrategyId::RelativeValue, FillType::Internal, FillSource::Hedge, 0, 0, stf);
    }

    void onInternalFill(uint32_t internalId, Order* order, hf::price_t price, hf::qty_t size, bool payUp = false) {
        switch (order->_strategy) {
            case Order::Strategy::RelativeValue: {
                sendFillToRV(internalId, order->_side, price, size, StrategyId::RelativeValue, FillType::Internal, FillSource::Quote);
                break;
            }
            case Order::Strategy::MarketMaker: {
                ContractStateT& state = _contractStates[internalId];
                hf::qty_t allocatedInternally = 0;
                hf::qty_t allocatedExternally = 0;
                state._marketMaker.onInternalFill(order, price, size, state._books[BID], state._books[ASK], payUp, allocatedInternally, allocatedExternally);
                FillSource fillSource = order->_type == Order::Type::Hedging ? FillSource::Hedge : FillSource::Quote;
                sendFillToFront(internalId, order->_side, price, size, StrategyId::MarketMaker, FillType::Internal, fillSource, allocatedInternally, allocatedExternally, 0);
                break;
            }
            default:
                break;
        }
    }

    void onExternalFill(uint32_t internalId, Order* order, hf::price_t price, hf::qty_t size, bool payUp = false) {
        switch (order->_strategy) {
            case Order::Strategy::RelativeValue: {
                LOG(0, "Executor: External fill. Sending to RV. Contract={C[i1]} Side={i2} Price={d1} Size={i3}", getLoggingContract(internalId), order->_side, ToDollars(price), size)
                sendFillToRV(internalId, order->_side, price, size, StrategyId::RelativeValue, FillType::External, FillSource::Quote);
                break;
            }
            case Order::Strategy::MarketMaker: {
                ContractStateT& state = _contractStates[internalId];
                hf::qty_t allocatedInternally = 0;
                hf::qty_t allocatedExternally = 0;
                state._marketMaker.onExternalFill(order, price, size, state._books[BID], state._books[ASK], payUp, allocatedInternally, allocatedExternally);
                FillSource fillSource = order->_type == Order::Type::Hedging ? FillSource::Hedge : FillSource::Quote;
                sendFillToFront(internalId, order->_side, price, size, StrategyId::MarketMaker, FillType::External, fillSource, allocatedInternally, allocatedExternally, 0);
                break;
            }
            default:
                break;
        }
    }

    hf::qty_t allocateExternalMMHedgeFills(uint32_t internalId, Order* order, hf::price_t price, hf::qty_t size) {
        ContractStateT& state = _contractStates[internalId];
        hf::qty_t allocatedInternally = 0;
        hf::qty_t allocatedExternally = 0;
        hf::qty_t remaining = state._marketMaker.allocateHedgingFill(order->_side, size, false, allocatedInternally, allocatedExternally);
        if (allocatedInternally || allocatedExternally) {
            sendFillToFront(internalId, order->_side, price, size - remaining, StrategyId::MarketMaker, FillType::External, FillSource::Hedge, allocatedInternally, allocatedExternally, 0);
        }
        return remaining;
    }

    void onExternalOverFill(uint32_t internalId, uint32_t side, hf::price_t price, hf::qty_t size) {
       LOG(0, "Executor: External overfill. Contract={C[i1]} Side={i2} Price={d1} Size={i3}", getLoggingContract(internalId), side, ToDollars(price), size)
        ContractStateT& state = _contractStates[internalId];
        hf::qty_t remainingSize = size;
        if (state._marketMaker.isEnabled()) {
            hf::qty_t takenSize = state._marketMaker.takeOverFill(side, remainingSize);
            if (takenSize > 0) {
                sendFillToFront(internalId, side, price, takenSize, StrategyId::MarketMaker, FillType::External, FillSource::Overfill, 0, takenSize, 0);
            }
            remainingSize -= takenSize;
        }
        if (remainingSize > 0) {
            if (_systemState->_relativeValue.load(std::memory_order_relaxed) == SystemState::Enabled) {
                sendFillToRV(internalId, side, price, remainingSize, StrategyId::RelativeValue, FillType::External, FillSource::Overfill);
            } else {
                state._marketMaker.forceTakeOverFill(side, remainingSize);
                sendFillToFront(internalId, side, price, remainingSize, StrategyId::MarketMaker, FillType::External, FillSource::Overfill, 0, remainingSize,  0);
                disableMMExternalQuoteWithMessage(internalId, static_cast<std::string>(CREATE_SAFE_FORMATTER("Exectuor: External overfill.  Contract=%1% remainingSize after MM takes=%2% Price=%3%")
                     % getLoggingContract(internalId) % remainingSize % ToDollars(price)));
            }
        }
    }

    void sendFillToFront(uint32_t contractIndex, uint32_t side, hf::price_t price, hf::qty_t size, StrategyId strategyId, FillType type, FillSource source,
                         hf::qty_t internalAllocation, hf::qty_t externalAllocation, hf::price_t stf) {
        auto& outMessage = *static_cast<ViperMessage*>(_outToFront->getPoolP()->getNextP());
        outMessage._type = ViperMessageType::Fill;
        FillMessage& message = reinterpret_cast<FillMessage &> (outMessage._data[0]);
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
        const auto outIdx = _outToFront->getPoolP()->append(sizeof(ViperMessageType) + sizeof(FillMessage));
        mb();
        _outToFront->appendNew(outIdx);
    }

    void onEndOfExternalFills(uint32_t internalId, hf::SIDE side) {
        ContractStateT& state = _contractStates[internalId];
        uint32_t s = ToViperSide(side);
        state._marketMaker.onEndOfExternalFills(s);
    }

    void onEndOInternalFills(uint32_t internalId) {
        ContractStateT& state = _contractStates[internalId];
        state._marketMaker.onEndOInternalFills();
    }

    void onHedgeOrderFromRv(HedgeOrderMessage& order) {
       LOG(0, "MM received hedge order from RV. Contract={C[i1]} Side={i2} Price={d1} Size={i3} HedgeId={i4}", getLoggingContract(order._instrumentIndex), order._side, ToDollars(order._price), order._size, order._hedgeId)
        ContractStateT& state = _contractStates[order._instrumentIndex];
        const hf::qty_t unmatchedSize = state._ime.onHedgeOrderFromRelativeValue(order);
        if ((unmatchedSize > 0) & (!order._isExternal)) {
            const uint32_t otherSide = GetOtherSide(order._side);
            if (!order._isExternal) {
                LOG(0, "Missed internal only hedge. Back to RV. Contract={C[i1]} Unfilled={i2}",getLoggingContract(order._instrumentIndex), unmatchedSize)
                sendFillToRV(order._instrumentIndex, otherSide, order._price, unmatchedSize, StrategyId::RelativeValue, FillType::Internal, FillSource::MissedHedge);
            }
            else {
                LOG(0, "Missed external hedge due to market maker disabled. Back to RV. Contract={C[i1]} Unfilled={i2}",getLoggingContract(order._instrumentIndex), unmatchedSize)
                sendFillToRV(order._instrumentIndex, otherSide, order._price, unmatchedSize, StrategyId::RelativeValue, FillType::External, FillSource::MissedHedge);
            }
        }
        state._ime.sendInternalFills();
    }

    std::string& getContractName(uint32_t id) {
        return _config->_allContracts[id]._name;
    }

#ifdef IS_DEV
    std::string getLoggingContract(uint32_t id) {
        return _config->_allContracts[id]._name;
    }
#else
    uint32_t getLoggingContract(uint32_t id) {
            BOOST_ASSERT_MSG(id == _config->_allContracts[id]._internalId, "If this isn't true . . . a lot of the log statements are broken");
            return _config->_allContracts[id]._internalId;
        }
#endif

    void sendExternalRvHedge(HedgeOrderMessage& order) {
        om::order_t* rhoneOrder = newFAKOrder(order._instrumentIndex, order._side, order._price, order._size);
        sendHedgeMarketDataAdjustmentAck(order._hedgeId, order._instrumentIndex, GetOtherSide(order._side), rhoneOrder->oid, order._size);
        sendFillToFront(order._instrumentIndex, order._side, order._price, order._size, StrategyId::RelativeValue, FillType::External, FillSource::Hedge, 0, 0, order._stf);
    }

    void sendHedgeMarketDataAdjustmentAckWithZeroSize(HedgeOrderMessage& order) {
        sendHedgeMarketDataAdjustmentAck(order._hedgeId, order._instrumentIndex, GetOtherSide(order._side), 0, 0);
    }

    void onInternalHedgeDone(uint32_t instrumentId, uint32_t marketId, uint32_t side, uint32_t hedgeId) {
        auto& outMessage = *static_cast<ViperMessage*>(_outToRV->getPoolP()->getNextP());
        outMessage._type = ViperMessageType::InternalHedgeDone;
        InternalHedgeDone& message = reinterpret_cast<InternalHedgeDone &> (outMessage._data[0]);
        message._hedgeId = hedgeId;
        message._instrumentIndex = instrumentId;
        message._side = side;
        message._marketIndex = marketId;
        const auto outIdx = _outToRV->getPoolP()->append(sizeof(ViperMessageType) + sizeof(InternalHedgeDone));
        mb();
        _outToRV->appendNew(outIdx);
    }

    void sendHedgeMarketDataAdjustmentAck(uint32_t hedgeId, uint32_t instrumentIndex, uint32_t side, om::order_id_t orderId, hf::qty_t size) {
        auto& outMessage = *static_cast<ViperMessage*>(_outToRV->getPoolP()->getNextP());
        outMessage._type = ViperMessageType::HedgeMDAdjsutAck;
        HedgeMarketDataAdjustmentAck& message = reinterpret_cast<HedgeMarketDataAdjustmentAck &> (outMessage._data[0]);
        message._hedgeId = hedgeId;
        message._instrumentIndex = instrumentIndex;
        message._side = side;
        message._size = size;
        message._orderId = orderId;
        const auto outIdx = _outToRV->getPoolP()->append(sizeof(ViperMessageType) + sizeof(HedgeMarketDataAdjustmentAck));
        mb();
        _outToRV->appendNew(outIdx);
    }

    void sendFillToRV(uint32_t contractIndex, uint32_t side, hf::price_t price, hf::qty_t size, StrategyId strategyId, FillType type, FillSource source) {
        auto& outMessage = *static_cast<ViperMessage*>(_outToRV->getPoolP()->getNextP());
        outMessage._type = ViperMessageType::Fill;
        FillMessage& message = reinterpret_cast<FillMessage &> (outMessage._data[0]);
        message._contractId = contractIndex;
        message._side = side;
        message._qty = size;
        message._strategy = strategyId;
        message._type = type;
        message._price = price;
        message._source = source;
        const auto outIdx = _outToRV->getPoolP()->append(sizeof(ViperMessageType) + sizeof(FillMessage));
        mb();
        _outToRV->appendNew(outIdx);
    }

    void sendGenericMessageToRV(ViperMessageType type, int64_t value1 = 0, int64_t value2 = 0, int64_t value3 = 0) {
        auto& outMessage = *static_cast<ViperMessage*>(_outToRV->getPoolP()->getNextP());
        outMessage._type = type;
        GenericMessage& message = reinterpret_cast<GenericMessage &> (outMessage._data[0]);
        message._number_1 = value1;
        message._number_2 = value2;
        message._number_3 = value3;
        const auto outIdx = _outToRV->getPoolP()->append(sizeof(ViperMessageType) + sizeof(GenericMessage));
        mb();
        _outToRV->appendNew(outIdx);
    }

    InternalMatchingEngine<Executor>& getInternalMatchingEngine(const om::order_t& order) {
        ContractStateT& state = _contractStates[getInternalId(order)];
        return state._ime;
    }

    void sendReverseFillToRelativeValue(uint32_t internalId, const om::order_t& order, FillSource source) {
        const hf::qty_t unfilledSize = order.qty - order.executedQty;
        const uint32_t reverseSide = order.side == hf::SIDE::BUY ? 1 : 0;
        LOG(0, "Sending size as reverse to RV. Contract={C[i1]} OID={i2} Type={i3}", getLoggingContract(internalId), order.oid, toUInt(source))
        sendFillToRV(internalId, reverseSide, order.pr, unfilledSize, StrategyId::RelativeValue, FillType::External, source);
    }

    void processMessagesFromFront() {
        while (true) {
            uint64_t idx = _inFromFront->getNext();

            if (BOOST_UNLIKELY(idx == 0))
                return;

            _inFromFront->freeNext();

            auto& message = *static_cast<ViperMessage*>(_inFromFront->getPoolP()->getData(idx));

            switch (message._type) {
                case ViperMessageType::GridUpdate: {
                    GridUpdateMessage& gridChange = reinterpret_cast<GridUpdateMessage &> (message._data[0]);
                    processGridChange(*gridChange._gridUpdate);
                    updateGridForChangedMMState(*gridChange._gridUpdate);
                    sendGenericMessageToFront(ViperMessageType::GridUpdateAccepted, gridChange._gridUpdate->_update.gridid());
                    break;
                }
                case ViperMessageType::GuiInitialized: {
                    handleGuiInitialized();
                    break;
                }
                case ViperMessageType::DisableExternalQuotingOnMarketMaker: {
                    disableExternalQuotingOnAllMMInstruments();
                    break;
                }
                case ViperMessageType::SweepConfigUpdate : {
                    const SweepConfig& sweepConfig = reinterpret_cast<SweepConfig &>(message._data[0]);
                    handleSweepConfigChange(sweepConfig);
                    break;
                }
                default:
                    break;
            }
        }
    }

    void disableExternalQuotingOnAllMMInstruments() {
        if (disableExternalQuotingOnAllMMInstrumentsKernel()) {
            LOG(0, "Disabling external quoting", 0);
            sendErrorMessage("Disabling ExternalQuoting", ErrorType::Critical);
        }
    }

    bool disableExternalQuotingOnAllMMInstrumentsKernel() {
        return _systemState->_quoteExternally.exchange(0, std::memory_order_release).any();
    }

    bool changeExternalQuoting(hf::VENUE venue, SystemState newState) {
        LOG(0, "Executor. Change {i1} external quoting to {i2}", static_cast<uint32_t>(venue), static_cast<uint32_t>(newState))
        auto currentExtQuote = _systemState->_quoteExternally.load(std::memory_order_relaxed);
        auto index = static_cast<uint8_t>(venue);
        bool rt = currentExtQuote.test(index);
        std::bitset<static_cast<uint8_t>(hf::VENUE::TEST)> newExtQuote;
        do {
            newExtQuote = currentExtQuote;
            if (newState == SystemState::Enabled) {
                newExtQuote.set(index);
            }
            else
            {
                newExtQuote.set(index, 0);
            }
        }
        while (!_systemState->_quoteExternally.compare_exchange_strong(currentExtQuote, newExtQuote, std::memory_order_release,std::memory_order_relaxed));
        return rt;
    }

    bool canQuoteExternally(hf::VENUE venue) {
        return _systemState->_quoteExternally.load(std::memory_order_relaxed).test(static_cast<uint8_t>(venue));
    }

    bool handleLogin() {
        LOG(0, "Market makers logging in. {i1}", 0)
        bool success = true;
        for (auto& state : _contractStates) {
            success &= state._marketMaker.handleLogin();
        }
        return true;
    }

    COLD
    void onRecoveredOrders(const om::OMRecoveredOrdersEvent& recoveredOrders) {
        LOG(0, "Recovered GTC orders. {i1}", recoveredOrders.num_orders);
        if (recoveredOrders.num_orders > 0) {
            const uint8_t* ptr = recoveredOrders.data;
            for (uint16_t i(0); i < recoveredOrders.num_orders; ++i) {
                const auto oid = reinterpret_cast<const om::order_id_t&>(*ptr);
                const om::order_t& order = this->context().getOrder(oid);
                auto it = _sidToInternalId.find(order.sid);
                if (it != _sidToInternalId.end()) {
                    const uint32_t internalId = it->second;
                    LOG(0, "Order. OID={i1} ID={2}", oid, internalId);
                    ContractStateT& state = _contractStates[internalId];
                    uint32_t side = order.side == hf::SIDE::BUY ? 0 : 1;
                    state._ime.addGtcOrder(side, _slow._iopTracking.getRouteFromPid(internalId, recoveredOrders.pid), order);
                    auto message = boost::str(boost::format("GTC Order recovered.  Contract=%1% Side=%2% Price=%3% Qty(before unprocessed fills)=%4%") %
                                                      getContractName(internalId) % hf::SIDE_NAMES.at(static_cast<unsigned char>(order.side)) % ToDollars(order.pr) % order.leavesQty);
                    sendErrorMessage(message, ErrorType::Normal);
                } else {
                    LOG(0, "Order. OID={i1} SID={i3}", oid, order.sid);
                    std::string message = boost::str(boost::format("GTC Order for not traded contract - need to cancel. OID=%1% SID=%2%") % oid % order.sid);
                    sendErrorMessage(message, ErrorType::Critical);
                }
                ptr += sizeof(om::order_id_t);
            }
        }
        this->context().ackRecoveryBatch(_iid, recoveredOrders.pid, recoveredOrders.msgw_idx);
        _slow._iopTracking.forceReconcileGTCs();
    }

    COLD
    void onRecoveredFills(const om::OMRecoveredFillsEvent& recoveredFills) {
        LOG(0, "Recovered GTC fills. {i1}", recoveredFills.num_outright_fills);
        const uint8_t *ptr = recoveredFills.data;
        for (uint16_t i(0); i < recoveredFills.num_outright_fills; ++i) {
            const auto& fill = reinterpret_cast<const om::OMRecoveredExecution &>(*ptr);
            const om::order_t& order = this->context().getOrder(fill.oid);
            om::order_t& mutableOrder = const_cast<om::order_t &>(order);
            mutableOrder.executedQty += fill.filledQty;
            mutableOrder.leavesQty -= fill.filledQty;
            auto it = _sidToInternalId.find(order.sid);
            if (it != _sidToInternalId.end()) {
                handleFillForUnassignedGtcOrder(fill, it->second, order);
                auto message = boost::str(boost::format("GTC Order filled.  Contract=%1% Side=%2% FillQty=%3% FillPrice=%4% OrderPrice=%5% LeavesQty=%6%")
                        % getContractName(it->second) % hf::SIDE_NAMES.at(static_cast<unsigned char>(order.side)) % fill.filledQty % ToDollars(fill.filledPrice) % ToDollars(order.pr) % order.leavesQty);
                sendErrorMessage(message, ErrorType::Critical);
            } else {
                std::string message = boost::str(boost::format("Fill for not traded contract. Contract=%1% Price=%2% Size%3%") % order.sid % ToRhonePrice(fill.filledPrice) % fill.filledQty);
                sendErrorMessage(message, ErrorType::Critical);
            }
            ptr += sizeof(fill);
        }
    }

    bool handleLogout() {
       LOG(0, "Market makers logging out. {i1}", 0);
        handleSystemDisabled();
        return true;
    }

    void sendMarketUpdateToRelativeValue(uint32_t internalId, OrderIds id, hf::price_t bidPrice, hf::qty_t bidSize, hf::price_t askPrice, hf::qty_t askSize) {
        auto& outMessage = *static_cast<ViperMessage*>(_outToRV->getPoolP()->getNextP());
        outMessage._type = ViperMessageType::MarketUpdate;
        MarketUpdate& message = reinterpret_cast<MarketUpdate &> (outMessage._data[0]);
        message._instrumentIndex = internalId;
        message._marketId = id;
        message._bidPrice = bidPrice;
        message._bidSize = bidSize;
        message._askPrice = askPrice;
        message._askSize = askSize;
        const auto outIdx = _outToRV->getPoolP()->append(sizeof(ViperMessageType) + sizeof(GenericMessage));
        mb();
        _outToRV->appendNew(outIdx);
    }

    void sendGenericMessageToFront(ViperMessageType type, int64_t value1 = 0, int64_t value2 = 0, int64_t value3 = 0) {
        auto& outMessage = *static_cast<ViperMessage*>(_outToFront->getPoolP()->getNextP());
        outMessage._type = type;
        GenericMessage& message = reinterpret_cast<GenericMessage &> (outMessage._data[0]);
        message._number_1 = value1;
        message._number_2 = value2;
        message._number_3 = value3;
        const auto outIdx = _outToFront->getPoolP()->append(sizeof(ViperMessageType) + sizeof(GenericMessage));
        mb();
        _outToFront->appendNew(outIdx);
    }

    void sendErrorMessage(const std::string& s, ErrorType type) {
        auto& outMessage = *static_cast<ViperMessage*>(_outToFront->getPoolP()->getNextP());
        outMessage._type = ViperMessageType::ErrorMessage;
        ErrorMessage& message = reinterpret_cast<ErrorMessage &> (outMessage._data[0]);
        const uint32_t size = std::min(CRITICAL_MESSAGE_MAX_SIZE, (uint32_t) s.size());
        strncpy(message._message, s.c_str(), size);
        message._size = size;
        message._type = type;
        const auto outIdx = _outToFront->getPoolP()->append(sizeof(ViperMessageType) + sizeof(ErrorMessage));
        mb();
        _outToFront->appendNew(outIdx);
    }

    void sendIOPUpdateToFront(uint32_t internalId, hf::price_t price, int numBidGTCs, int numOfferGTCs, int ticksThroughBid, int ticksThroughAsk) {
        auto& outMessage = *static_cast<ViperMessage *>(_outToFront->getPoolP()->getNextP());
        outMessage._type = ViperMessageType::IOPUpdate;
        IOPUpdate& iopUpdate = reinterpret_cast<IOPUpdate &>(outMessage._data[0]);
        iopUpdate._internalId = internalId;
        iopUpdate.price = price;
        iopUpdate.numBidGTCs = numBidGTCs;
        iopUpdate.numOfferGTCs = numOfferGTCs;
        iopUpdate.ticksThroughBid = ticksThroughBid;
        iopUpdate.ticksThroughAsk = ticksThroughAsk;
        const auto outIdx = _outToFront->getPoolP()->append(sizeof(ViperMessageType) + sizeof(IOPUpdate));
        mb();
        _outToFront->appendNew(outIdx);
    }

    void sendAuctionMDUpdateToFront(uint32_t internalId, hf::price_t bid, hf::price_t offer) {
        auto& outMessage = *static_cast<ViperMessage *>(_outToFront->getPoolP()->getNextP());
        outMessage._type = ViperMessageType::AuctionMDUpdate;
        AuctionMDUpdate& update = reinterpret_cast<AuctionMDUpdate &>(outMessage._data[0]);
        update._internalId = internalId;
        update._bid = bid;
        update._offer = offer;
        const auto outIdx = _outToFront->getPoolP()->append(sizeof(ViperMessageType) + sizeof(AuctionMDUpdate));
        mb();
        _outToFront->appendNew(outIdx);
    }

    void sendEndIOPTrackingToFront(uint32_t instrumentId) {
        sendGenericMessageToFront(ViperMessageType::EndIOPTracking, instrumentId);
    }

    bool processGridChange(GridUpdate& update) {
        if (update._update.gridid() == toUInt(WindowIds::MarketMakerParameters)) {
            bool valid = true;
            for (const proto::GridCell& cell : update._update.cells()) {
                const uint32_t internalId = cell.row();
                ContractStateT& state = _contractStates[internalId];
                valid &= state._marketMaker.validateParametersUpdate(cell);
            }

            if (!valid)
                return valid;

            for (int i = 0; i < update._update.cells_size(); ++i) {
                proto::GridCell* cell = update._update.mutable_cells(i);
                const uint32_t internalId = cell->row();
                ContractStateT& state = _contractStates[internalId];
                state._marketMaker.handleParametersUpdate(*cell);
            }
            return true;
        }
        else if (update._update.gridid() == toUInt(WindowIds::StackerParameters)) {
            for (const proto::GridCell& cell : update._update.cells()) {
                const uint32_t internalId = cell.row();
                ContractStateT& state = _contractStates[internalId];
                state._ime.handleStackerParameterUpdate(cell);
            }
        }
        else if (update._update.gridid() == toUInt(WindowIds::GlobalConfigs)) {
            for (const proto::GridCell& cell : update._update.cells()) {
                GlobalConfig& config = _config->_globalConfigs[cell.row()];
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
                    for (uint32_t index = 0; index < _numberOfThrottlers; ++index) {
                        Throttler& throttler = _throttler[index];
                        throttler.updateInDangerZonePeriod(inDangerZonePeriod);
                    }
                }
                else if (config._key == MSG_HEDGES_INTERVAL_MICS) {
                    uint64_t atCapacityPeriod = GetCyclesFromMicroSeconds(boost::lexical_cast<uint32_t>(cell.value()), _frequency);
                    for (uint32_t index = 0; index < _numberOfThrottlers; ++index) {
                        Throttler& throttler = _throttler[index];
                        throttler.updateAtCapacityPeriod(atCapacityPeriod);
                    }
                }
                else if (config._key == PERCENT_MSG_HEDGES) {
                    double percent = boost::lexical_cast<double>(cell.value());
                    for (uint32_t index = 0; index < _numberOfThrottlers; ++index) {
                        Throttler& throttler = _throttler[index];
                        throttler.updateHedgePercentage(percent);
                    }
                }
                else if (config._key == PERCENT_MSG_REDUCE_QUOTE) {
                    double percent = boost::lexical_cast<double>(cell.value());
                    for (uint32_t index = 0; index < _numberOfThrottlers; ++index) {
                        Throttler& throttler = _throttler[index];
                        throttler.updateDangerZonePercentage(percent);
                    }
                }
                else if (config._key == HEDGE_BANNER_MICS) {
                    uint64_t period = GetCyclesFromMicroSeconds(boost::lexical_cast<uint32_t>(cell.value()), _frequency);
                    for (auto& state : _contractStates) {
                        state._ime.handleHedgeQuoteBannerUpdate(period);
                    }
                }
                else if (config._key == MM_WORSEN_PAYUP_COND) {
                    double percent = boost::lexical_cast<double>(cell.value());
                    for (auto& state : _contractStates) {
                        if (state._marketMaker._toMarketMake) {
                            state._marketMaker.handleWorsenPayupConditionUpdate(percent);
                        }
                    }
                }
            }
        }
        else if (update._update.gridid() == toUInt(WindowIds::ContractParametersRv)) {
            for (const proto::GridCell& cell : update._update.cells()) {
                ContractParameterRVColumns columnType = static_cast<ContractParameterRVColumns>(cell.col());
                if (columnType == ContractParameterRVColumns::HedgeTolerance) {
                    const uint32_t internalId = cell.row();
                    ContractStateT& state = _contractStates[internalId];
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

    void updateGridForChangedMMState(GridUpdate& gridUpdate) {
        if (gridUpdate._update.gridid() == toUInt(WindowIds::MarketMakerParameters)) {
            for (proto::GridCell& cell : *gridUpdate._update.mutable_cells()) {
                const uint32_t internalId = cell.row();
                ContractStateT& state = _contractStates[internalId];

                MarketMakerParametersColumns columnType = static_cast<MarketMakerParametersColumns>(cell.col());
                if(columnType == MarketMakerParametersColumns::Enabled) {
                    cell.set_value(std::to_string(state._marketMaker._enabled));
                }
                else if(columnType == MarketMakerParametersColumns::QuoteExternal) {
                    cell.set_value(std::to_string(state._marketMaker._quoteExternal));
                }
            }
        }
    }

    void init(ViperConfig& viperConfig, double frequency, SharedSystemState& systemState, uint64_t internalHeartbeatPeriod,
              std::function<bool(uint32_t)> pnlLimitsChecker, std::map<hf::VENUE, uint32_t>& transactionCapacity, om::instance_id_t iid, Vector<bool>* rvQuoteExternal,
              Vector<RelativeValue::SimpleSweepDefinition>* simpleSweepDefinition) {
        _config = &viperConfig;
        _frequency = frequency;
        _systemState = &systemState;
        _pnlLimitsChecker = pnlLimitsChecker;
        _internalHeartbeatPeriod = internalHeartbeatPeriod;
        _transactionCapacity = transactionCapacity;
        _iid = iid;
        _quoteRvExternal = rvQuoteExternal;
        _sweepDefinition = simpleSweepDefinition;
        logging::GetLogger().logInfo(boost::str(boost::format("Executor initialized. Frequency=%1%") % _frequency));
    }

    bool canExecute() {
        return  isEnabled() & _gatewayStatus._active;
    }

    bool isEnabled() {
        return _systemState->_marketMaker.load(std::memory_order_relaxed) == SystemState::Enabled;
    }

    bool isWithinPnlLimits() {
        return _pnlLimitsChecker(toUInt(StrategyId::MarketMaker));
    }

    bool isGatewayReady() {
        return _gatewayStatus._active;
    }

    bool pricesAndQuantitiesValid() {
        bool retVal = true;
        for(const auto& contract : _contractStates) {
            auto check = [this](const Book& book, uint32_t side, uint32_t bookid) -> bool {
                  if(book._price != hf::NULL_PRICE && ((book._otherQuantity < 0) || (book._otherOrderCount < 0))) {
                      LOG(0, "EXE: Contract {C[i1]} Failed pricesAndQuantity check, refusing to start.  side={i2} px={i3} q={i4} o={i5}", getLoggingContract(bookid), side, book._price, book._otherQuantity, book._price);
                      std::string errorMessage = boost::str(boost::format("Quantity Check Failed.  Refusing to start.  Contract=%1% side=%2% px=%3% q=%4% o=%5%")
                                                     % getContractName(bookid) % side % book._price % book._otherQuantity % book._otherOrderCount);
                      sendErrorMessage(errorMessage, ErrorType::Critical);
                      logging::GetLogger().logError(errorMessage);
                      return false;
                  }
                  return true;
            };

            retVal &= check(contract._books[BID], BID, contract._ime._internalId);
            retVal &= check(contract._books[ASK], ASK, contract._ime._internalId);
        }

        return retVal;
    }

    void initialize(ViperConfig& viperConfig) {
        _contractStates.resize(viperConfig._allContracts.size());
        uint32_t fillBannerMicros = boost::lexical_cast<uint32_t>(viperConfig.getGlobal(FILL_BANNER_PERIOD));
        uint64_t fillBannerPeriod = GetCyclesFromMicroSeconds(fillBannerMicros, _frequency);
        uint32_t quoteBannerMicros = boost::lexical_cast<uint32_t>(viperConfig.getGlobal(QUOTE_BANNER_PERIOD));
        uint64_t quoteBannerPeriod = GetCyclesFromMicroSeconds(quoteBannerMicros, _frequency);
        uint32_t hedgeBannerMicos = boost::lexical_cast<uint32_t>(viperConfig.getGlobal(HEDGE_BANNER_MICS));
        uint64_t hedgeBannerPeriod = GetCyclesFromMicroSeconds(hedgeBannerMicos, _frequency);
        double worsenPayupCondPercent = boost::lexical_cast<double>(viperConfig.getGlobal(MM_WORSEN_PAYUP_COND));
        uint32_t selfMatchBannerMics = boost::lexical_cast<uint32_t>(viperConfig.getGlobal(SELF_MATCH_BANNER_MICS));
        uint64_t selfMatchBannerPeriod = GetCyclesFromMicroSeconds(selfMatchBannerMics, _frequency);
        for (uint32_t i = 0; i < viperConfig._allContracts.size(); ++i) {
            ConfigContract& contract = viperConfig._allContracts[i];
            _sidToInternalId.insert({contract._sid, contract._internalId});
            auto& state = _contractStates[i];
            uint32_t index;
            bool toMarketMake = viperConfig.isContractToMarketMake(contract._name, index);
            if (toMarketMake) {
                StackerConfig& stackerConfig = viperConfig._stackerConfigs[index];
                MarketMakerConfig& mmConfig = viperConfig._marketMakerConfigs[index];
                RelativeValueConfig& rvConfig = viperConfig._relativeValueConfigs[index];
                hf::price_t rvHedgeMatchTolerance = ToRhonePrice(rvConfig._hedgeTolerance);
                const auto& rids = getRoutingIds(contract);
                state._ime.initialize(this, contract, stackerConfig, rvHedgeMatchTolerance,
                                      fillBannerPeriod, quoteBannerPeriod, hedgeBannerPeriod, selfMatchBannerPeriod,
                                      state._books, viperConfig._iceCancelResting);
                for (size_t i = 0; i < rids.size(); ++i) {
                    state._rid[i] =  rids[i];
                }
                state._marketMaker.initialize(contract, mmConfig, &state._ime, worsenPayupCondPercent);
                state._venue = contract._venue;
            }
            subscribeAll(contract._venue, contract._sid);
            _usedVenues.insert(contract._venue);
        }

        double percentMsgHedges = boost::lexical_cast<double>(viperConfig.getGlobal(PERCENT_MSG_HEDGES));
        double percentMsgReduceQuote = boost::lexical_cast<double>(viperConfig.getGlobal(PERCENT_MSG_REDUCE_QUOTE));
        uint32_t atCapacityPeriodInMics = boost::lexical_cast<uint32_t>(viperConfig.getGlobal(MSG_HEDGES_INTERVAL_MICS));
        uint64_t atCapacityPeriod = GetCyclesFromMicroSeconds(atCapacityPeriodInMics, _frequency);
        uint32_t inDangerZonePeriodInMics = boost::lexical_cast<uint32_t>(viperConfig.getGlobal(MSG_REDUCE_QUOTE_MICS));
        uint64_t inDangerZonePeriod = GetCyclesFromMicroSeconds(inDangerZonePeriodInMics, _frequency);

        uint32_t throttlerIndex = 0;
        std::map<hf::VENUE, uint32_t> venueToThrottlerIndex;
        for (hf::VENUE venue : _usedVenues) {
            const uint32_t nRoutes = this->context().getNumberOfStrategyOMPorts(venue);
            logging::GetLogger().logInfo("Will round-robin between " + std::to_string(nRoutes) + " routes.");
            LOG(0, "Will round-robin between {i1} routes.", nRoutes);
            auto it = _transactionCapacity.find(venue);
            if (it == _transactionCapacity.end()) {
                logging::GetLogger().logErrorAndThrow(boost::str(boost::format("does not have transaction capacity for %1%") % (int) venue));
            }
            const uint32_t transactionCapacity = it->second;
            _throttler[throttlerIndex].initialize(_frequency, nRoutes, 1, transactionCapacity, percentMsgReduceQuote, percentMsgHedges, inDangerZonePeriod, atCapacityPeriod);
            venueToThrottlerIndex.insert({venue, throttlerIndex});
            ++throttlerIndex;
            _pendingGatewayLogin.set(toUInt(venue));
        }
        _numberOfThrottlers = _usedVenues.size();

        if (venueToThrottlerIndex.size() > MAX_NUMBER_OF_THROTTLERS) {
            logging::GetLogger().logErrorAndThrow("Number of throttlers more than preallocated.");
        }

        for (uint32_t i = 0; i < viperConfig._allContracts.size(); ++i) {
            auto& state = _contractStates[i];
            state._throttlerIndex = venueToThrottlerIndex[state._venue];
            ConfigContract& contract = viperConfig._allContracts[i];
            logging::GetLogger().logInfo(boost::str(boost::format("Route Contract=%1% Port=%2%") % contract._name % state._throttlerIndex));
        }

        validateContracts(viperConfig._allContracts);

        for (const auto& gateway : this->context().getGateways()) {
            _mapGatewayVenue.emplace(gateway._id, gateway._venue);
        }

        _slow._iopTracking.init(this, viperConfig._allContracts.size());
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

    std::vector<om::routing_id_t> getRoutingIds(ConfigContract& contract) {
        std::vector<om::routing_id_t> ids;

        for(unsigned i = 0; i < om::PORT_NUM; ++i) {
            om::routing_id_t id = this->context().getRoutingId(contract._venue, contract._sid, i);
            if(id != om::NULL_ROUTING_ID) {
                logging::GetLogger().logInfo(boost::str(boost::format("Route Contract=%1% Port=%2%") % contract._name % id));
                ids.push_back(id);
            }
        }

        if(ids.empty()) {
            logging::GetLogger().logErrorAndThrow(boost::str(boost::format("No routing ids found for Contract=%1%") % contract._name));
        }

        if(ids.size() != std::unordered_set<om::routing_id_t>(ids.begin(), ids.end()).size()) {
            logging::GetLogger().logErrorAndThrow(boost::str(boost::format("Duplicate routes found for Contract=%1%.  This should not happen.")));
        }

        return ids;
    }

    void gatherRhoneLimitsAndSendToRV(om::port_id_t pid) {
        std::unordered_map<uint32_t, int64_t> internalIdToLimit;

        for(const auto kvp : _sidToInternalId) {
            const auto* limit = this->context().getRiskLimits(pid, kvp.first);
            if(limit && limit->enabled) {
                auto maxOrderQty = limit->params.max_order_qty;
                auto iter = internalIdToLimit.find(kvp.second);
                if(iter == std::end(internalIdToLimit)) {
                    if(maxOrderQty > std::numeric_limits<int>::max()) {
                        throw std::runtime_error("Discovered limit too large for RV to handle. sid=" + std::to_string(kvp.first) + " limit=" + std::to_string(maxOrderQty));
                    }
                    internalIdToLimit[kvp.second] = maxOrderQty;
                }
                else {
                    if(iter->second != maxOrderQty) {
                        throw std::runtime_error("Mismatch in OM limits between enabled ports.  sid=" + std::to_string(kvp.first));
                    }
                }
            }
        }

        //Send the limits
        if(!internalIdToLimit.empty()) {
            auto &outMessage = *static_cast<ViperMessage *>(_outToRV->getPoolP()->getNextP());
            outMessage._type = ViperMessageType::SetRhoneLimits;
            SetRhoneLimits &limits = reinterpret_cast<SetRhoneLimits &>(outMessage._data[0]);
            limits.numLimits = internalIdToLimit.size();
            RhoneLimit* rhoneLimit = limits._limits;
            for (const auto &limit : internalIdToLimit) {
                rhoneLimit->_internalId = limit.first;
                rhoneLimit->_max_order_qty = limit.second;
                ++rhoneLimit;
            }

            const auto outIdx = _outToRV->getPoolP()->append(sizeof(ViperMessageType) + sizeof(SetRhoneLimits) + (limits.numLimits * sizeof(RhoneLimit)));
            mb();
            _outToRV->appendNew(outIdx);
        }
    }

    //Technically not needed since updated on RV thread as well
    void handleSweepConfigChange(const SweepConfig& config) {
        auto& definition = _sweepDefinition->getRef(config._internalId);
        if(config._sweepTicks) {
            definition._sweepTicksAtPrice = config._sweepTicks;
            LOG(0, "EXE: Updating sweepTicks for {C[i1]} to {i2}", getLoggingContract(config._internalId), config._sweepTicks);
        }
        if(config._sweepQtyUpdated) {
            definition._tradedQty = config._sweepQty;
            LOG(0, "EXE: Updating sweepQty for {C[i2]} to {i2}", getLoggingContract(config._internalId), config._sweepQty);
        }
    }

    void subscribeAll(hf::VENUE venue, hf::symbol_id_t sid) {
        this->context().subscribeOM(sid, true, true, true, true, true, true, true, true);
    }

    void checkSystemState() {
        SystemState mmState = _systemState->_marketMaker.load(std::memory_order_relaxed);
        if (mmState != _executorOldSystemState) {
            onExecutorStateChange(mmState);
            _executorOldSystemState = mmState;
        }

        SystemState rvState = _systemState->_relativeValue.load(std::memory_order_relaxed);
        if (rvState != _relativeValueOldSystemState) {
            onRelativeValueStateChange(rvState);
            _relativeValueOldSystemState = rvState;
        }

        auto quoteExt = _systemState->_quoteExternally.load(std::memory_order_relaxed);
        if (quoteExt != _oldQuoteExt)
        {
            onQuoteExtChange(quoteExt);
            _oldQuoteExt = quoteExt;
        }
    }

    void onRelativeValueStateChange(SystemState state) {
        switch (state) {
            case SystemState::HardShutdown: {
                handleShutdown();
                break;
            }
            case SystemState::Disabled: {
                handleRelativeValueDisabled();
                break;
            }
            case SystemState::CancelGtcAndDisable: {
                cancelGtcAndDisable();
                break;
            }
            default:
                break;
        }
    }

    void onExecutorStateChange(SystemState state) {
        switch (state) {
            case SystemState::HardShutdown: {
                handleShutdown();
                break;
            }
            case SystemState::Disabled: {
                handleExecutorDisabled();
                break;
            }
            case SystemState::Enabled: {
                handleExecutorEnabled();
                break;
            }
            case SystemState::LoggedIn: {
                handleLogin();
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
    }

    void onQuoteExtChange(const SharedSystemState::QuoteExtBitset& quoteExt)
    {
        LOG(0, "Executor. QuoteExternal Switch Changed. {i1} -> {i2}", _oldQuoteExt.to_ulong(), quoteExt.to_ulong())
        for (uint8_t i = 1; i < static_cast<uint8_t>(hf::VENUE::TEST); ++i) {
            if (_oldQuoteExt[i] != quoteExt[i])
            {
                if (quoteExt.test(i)) {
                    sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Enabled), proto::SystemState::SystemStateEnabled, i);
                }
                else {
                    sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Enabled), proto::SystemState::SystemStateDisabled, i);
                }

                if(BOOST_LIKELY(_systemState->_marketMaker.load(std::memory_order_relaxed) == SystemState::Enabled)) {
                    for (auto& contractState : _contractStates) {
                        if ((static_cast<uint8_t>(contractState._venue) == i) & contractState._marketMaker._toMarketMake) {
                            contractState._marketMaker.applyUpdates();
                        }
                    }
                }
            }
        }
    }

    void handleShutdown() {
        sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Enabled), proto::SystemState::SystemStateHardShutdown, toInt(StrategyId::MarketMaker));
        LOG(0, "Executor - shutting down {i1}", 0)
        hf::Service::exitAll();
    }

    void disableSystem(const std::string& reason) {
        getAllOutOfMarket();
        LOG(0, "Disable System from Executor.  {i1}", 0)
        sendErrorMessage(boost::str(boost::format("Disable System from Executor. Reason=%1%") % reason), ErrorType::Critical);
        disableMarketMakers();
        _systemState->_marketMaker.store(SystemState::Disabled, std::memory_order_release);
        _systemState->_relativeValue.store(SystemState::Disabled, std::memory_order_release);
        sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Enabled), proto::SystemState::SystemStateDisabled, toInt(toInt(StrategyId::MarketMaker)));
    }

    void disableSystemViaShutdown(const std::string& reason) {
        LOG(0, "Shutdown System from Executor. {i1}", 0)
        sendErrorMessage(boost::str(boost::format("Shutdown System from Executor. Reason=%1%") % reason), ErrorType::Critical);
        handleShutdown();
    }

    void handleExecutorDisabled() {
        LOG(0, "Executor. Market Makers disabled. Removing orders. Reason=Requested {i1}", 0)
        if (_systemState->_relativeValue.load(std::memory_order_relaxed) == SystemState::Enabled) {
            getMarketMakersOutOfMarket();
        } else {
            getAllOutOfMarket();
        }
        sendErrorMessage("Executor. Market Makers disabled. Removing orders. Reason=Requested", ErrorType::Critical);
        disableMarketMakers();
        sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Enabled), proto::SystemState::SystemStateDisabled, toInt(StrategyId::MarketMaker));
    }

    void cancelGtcAndDisable() {
        LOG(0, "Executor. Market Makers disable and cancel GTC. Reason=Requested {i1}", 0)
        getAllOutOfMarket(true);
        sendErrorMessage("Executor. Market Makers disabled. Removing als GTC orders. Reason=Requested", ErrorType::Critical);
        disableMarketMakers();
        sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Enabled), proto::SystemState::SystemStateDisabled, toInt(StrategyId::MarketMaker));
    }

    void handleRelativeValueDisabled() {
        LOG(0, "Executor. Relative Value Disabled. Removing orders. Reason=Requested {i1}", 0)
        sendErrorMessage("Executor. Relative Value Disabled. Removing orders. Reason=Requested", ErrorType::Critical);
        handleRelativeValueDisabledKernel();
    }

    void handleRelativeValueDisabledKernel() {
        for (auto& state : _contractStates) {
            if (state._marketMaker._toMarketMake) {
                if (state._marketMaker._enabled) {
                    state._ime.getRelativeValueOutOfMarket();
                } else {
                    state._ime.getAllOutOfMarket();
                }
            }
        }
    }

    void handleSystemDisabled() {
        disableSystem("Logout");
    }

    void disableMarketMakers() {
        for (auto& state : _contractStates) {
            state._marketMaker.disable(false);
        }
    }

    bool isPnlWithinLimits() {
        const bool withinLimits = _pnlLimitsChecker(toUInt(StrategyId::MarketMaker));
        if (!withinLimits) {
            sendErrorMessage("MM - not within pnl limits.",  ErrorType::Critical);
            LOG(0, "MM - is not within pnl limits {i1}", 0)
        }
        return withinLimits;
    }

    bool canEnable() {
        bool canEnable = true;
        if (!isPnlWithinLimits()) {
            canEnable = false;
        }

        if (!pricesAndQuantitiesValid()) {
            canEnable = false;
        }
        return canEnable;
    }

    void handleExecutorEnabled() {
        if (canEnable()) {
            enableMarketMakers();
            _reconcileGtcOrders = false;
            sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Enabled), proto::SystemState::SystemStateEnabled, toInt(StrategyId::MarketMaker));
            LOG(0, "Executor got enabled {i1}", 0)
        } else {
            _systemState->_marketMaker.store(SystemState::Disabled, std::memory_order_release);
            sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Enabled), proto::SystemState::SystemStateDisabled, toInt(StrategyId::MarketMaker));
            LOG(0, "Executor could NOT be enabled {i1}", 0)
        }
    }

    void enableMarketMakers() {
        for (auto& state : _contractStates) {
            state._marketMaker.start(state._books[BID], state._books[ASK]);
            if (_reconcileGtcOrders) {
                state._ime.cancelRemainingGtc();
            }
        }
    }

    void getAllOutOfMarket(bool cancelGtc = false) {
        LOG(0, "Executor. Removing all orders from market. {i1}", 0)
        for (auto& state : _contractStates) {
            if (state._marketMaker._toMarketMake) {
                state._ime.getAllOutOfMarket(cancelGtc);
            }
        }
        if(cancelGtc) {
            _slow._iopTracking.cancelAllGTCs();
        }
    }

    void getMarketMakersOutOfMarket() {
        LOG(0, "Executor. Removing all market makers orders from market. {i1}", 0)
        for (auto& state : _contractStates) {
            if (state._marketMaker._toMarketMake) {
                state._ime.getMarketMakerOutOfMarket();
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

        for (uint32_t i = 0; i < _config->_allContracts.size(); ++i) {
            ConfigContract& contract = _config->_allContracts[i];
            uint32_t index;
            bool toMarketMake = _config->isContractToMarketMake(contract._name, index);
            if (toMarketMake) {
                struct md::request::MDBookRequestMsg bookRequestMsg (_id, contract._venue, contract._sid, 1, true, true);
                sendMessageToMDSocket(fd, bookRequestMsg);
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

    void drainAllMarketData() {
        uint32_t drainSize = 0;
        for (md::queue::MDQueue* queue : _marketDataQueues) {
            while (true) {
                const auto idx = queue->getNext();

                if (BOOST_UNLIKELY(!idx))
                    break;

                queue->freeNext();

                auto& batch = queue->getPoolP()->getBatch(idx);
                onMDBatch(batch);

                {
                    queue->poolBytesRead(batch.size);
                    uint64_t usage;
                    if (BOOST_UNLIKELY(queue->poolThresholdExceeded(usage))) {
                        onPoolThresholdExceeded(queue->getQueueID(), usage);
                    }
                    if (BOOST_UNLIKELY(queue->queueThresholdExceeded(usage))) {
                        onQueueThresholdExceeded(queue->getQueueID(), usage);
                    }
                }

                ++drainSize;
            }
        }

        if (drainSize > 0) {
            LOG(0, "MM MD Drain size. {i1}", drainSize)
        }

        for (uint32_t id : _updatedContracts) {
            onMarketDataChange(id);
        }
        _updatedContracts.clear();
    }

    void onPoolThresholdExceeded(const std::string& queueName, uint64_t usage) {
        logging::GetLogger().logError("MM Pool_threshold_exceeded");
    }

    void onQueueThresholdExceeded(const std::string& queueName, uint64_t usage) {
        logging::GetLogger().logError("MM Queue_threshold_exceeded");
    }

    void onMDBatch(const md::MDBatch& batch) override {
        LOG(0, "MM received MD batch. bid={i1} extSeqNum={i2}", batch.bid, batch.extSequenceNumber)
        const auto* data = batch.data;
        for (auto i = batch.count; i; --i) {
            const auto& mde = reinterpret_cast<const md::MDEvent&> (*data);
            data += mde.size;
            switch (mde.type) {
                case md::MDEvent::TYPE::UPDATE: {
                    const auto& event = static_cast<const md::MDUpdateEvent&> (mde);
                    onMDUpdate(batch.venue, event, batch.extSequenceNumber);
                    break;
                }
                case md::MDEvent::TYPE::TRADE: {
                    const auto& event = static_cast<const md::MDTradeEvent&> (mde);
                    onMDTrade(batch.venue, event);
                    break;
                }
                case md::MDEvent::TYPE::GAP: {
                    const auto& event = static_cast<const md::MDGapEvent&> (mde);
                    onMDGap(batch.venue, event);
                    break;
                }
                case md::MDEvent::TYPE::STATUS: {
                    const auto& event = static_cast<const md::MDStatusEvent&> (mde);
                    onMDStatus(batch.venue, event);
                    break;
                }
                case md::MDEvent::TYPE::INTRA_EVENT_SWEEP: {
                    const auto& event = static_cast<const md::MDIntraEventSweepEvent&> (mde);
                    onMDIntraEventSweep(batch.venue, event);
                    break;
                }
                case md::MDEvent::TYPE::PREOPEN_PRICE: {
                    const auto& event = static_cast<const md::MDPreOpenPriceEvent&> (mde);
                    _slow._iopTracking.onIOPUpdate(event);
                    break;
                }
                case md::MDEvent::TYPE::STATISTICS: {
                    const auto& event = static_cast<const md::MDStatisticsEvent&>(mde);
                    for(int i = 0; i < static_cast<int>(event.num); ++i) {
                        const auto& stat = event.statistics[i];
                        if((stat.type == '4') & (stat.action == 0) & (stat.openCloseSettlFlag == 5)) {
                            _slow._iopTracking.onIOPUpdate(event.sid, stat.price, stat.price);
                        }
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }

    void onMDUpdate(hf::VENUE venue, const md::MDUpdateEvent& event, uint_fast64_t sequenceNumber) {
        auto it = _sidToInternalId.find(event.sid);
        if (it != _sidToInternalId.end()) {
            const uint32_t internalId = it->second;
            ContractStateT& state = _contractStates[internalId];
            const bool bidChanged = event.buy.updatedQtyNum == 1;
            const bool askChanged = event.sell.updatedQtyNum == 1;
            state._mdSequenceNumber = sequenceNumber;
            auto& bid = event.buy.top.l[0];
            auto& ask = event.sell.top.l[0];
            if(event.cleanedLevelRemoved != hf::NULL_PRICE) {
                LOG(0, "EXE: Contract {C[i1]} Level cleaned from book.  Bid book adjustment due to trade. px={i2} oldBid:{i3}@{i4} newBid:{i5}@{i6} oldOffer:{i7}@{i8} offer:{i9}@{i10} seq={i11}",
                    getLoggingContract(it->second), event.cleanedLevelRemoved, state._books[BID]._otherQuantity, state._books[BID]._price, bid.q, bid.pr, state._books[ASK]._otherQuantity, state._books[ASK]._price,
                    ask.q, ask.pr, sequenceNumber);
            }
            copyBooks(state, bid.pr, bid.q, bid.o, ask.pr, ask.q, ask.o, bidChanged, askChanged);
            if (bidChanged | askChanged) {
                _updatedContracts.insert(internalId);
            }
        }
    }

    void onMarketDataChange(uint32_t internalId) {
        ContractStateT& state = _contractStates[internalId];
        if (state._marketMaker._toMarketMake) {
            state._marketMaker.onChangeInOtherBooks(state._books[BID], state._books[ASK]);
            if(BOOST_UNLIKELY(!state._ime._contractCanTrade)) {
                _slow._iopTracking.onMDUpdateDuringAuction(internalId, state._books[BID]._price, state._books[ASK]._price);
            }
        }
    }

    void copyBooks(ContractStateT& state, hf::price_t bidPrice, hf::qty_t bidSize, md::order_count_t bidOrderCount,
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

    void onMDTrade(hf::VENUE venue, const md::MDTradeEvent& event) {
        if (venue == hf::VENUE::CME) {
            for (uint32_t i = 0; i < event.num; ++i) {
                auto& trade = event.trades[i];
                auto it = _sidToInternalId.find(event.sid);
                if (it != _sidToInternalId.end()) {
                    const uint32_t internalId = it->second;
                    ContractStateT& state = _contractStates[internalId];
                    if (trade.side == hf::SIDE::BUY) {
                        //LOG(0, "EXE: Contract {C[i1]} Bid book adjustment due to trade. {i2}@{d1}", getLoggingContract(internalId), trade.q, ToDollars(trade.pr))
                        copyBook(state._books[BID], trade.pr, trade.q, trade.ext.cme_python_book_cleaning.orderCount);
                        _updatedContracts.insert(internalId);
                    } else if (trade.side == hf::SIDE::SELL) {
                        //LOG(0, "EXE: Contract {C[i1]} Ask book adjustment due to trade. {i2}@{d1}", getLoggingContract(internalId), trade.q, ToDollars(trade.pr))
                        copyBook(state._books[ASK], trade.pr, trade.q, trade.ext.cme_python_book_cleaning.orderCount);
                        _updatedContracts.insert(internalId);
                    }
                }
            }
        }
    }

    void onMDStatus(hf::VENUE venue, const md::MDStatusEvent& event) {
        auto it = _sidToInternalId.find(event.sid);
        if (it != _sidToInternalId.end()) {
            const uint32_t internalId = it->second;
            ContractStateT& state = _contractStates[internalId];
            bool needToNotify = _oldMDStatus[static_cast<uint8_t>(venue)] != event.extStatus;
            _oldMDStatus[static_cast<uint8_t>(venue)] = event.extStatus;
            switch (event.statusType) {
                case md::MDStatusEvent::TYPE::RECOVERING:
                case md::MDStatusEvent::TYPE::TIMEOUT:
                case md::MDStatusEvent::TYPE::NOT_CONFIGURED: {
                    LOG(0, "Contract {C[i1]} market data not working. Reason={i2}", getLoggingContract(internalId), toUInt(event.statusType))
                    if (needToNotify) {
                        stopContract(state,  boost::str(boost::format("Contract %1% market data not working. Reason=%2%") % state._ime._name % toUInt(event.statusType)));
                    }
                    break;
                }
                case md::MDStatusEvent::TYPE::RECOVERED: {
                    if (canQuoteExternally(venue)) {
                        changeExternalQuoting(venue, SystemState::Disabled);
                        sendErrorMessage(boost::str(boost::format("Disable %1% external quoting. MDStatus=RECOVERED Reason=%2% changed from NotTradable to Tradable") % ven::VenueUtils::getName(venue) % state._ime._name), ErrorType::Critical);
                    }
                    state._ime._contractCanTrade = true;
                    LOG(0, "Contract {C[i1]} market data recovered.", getLoggingContract(internalId))
                    break;
                }
                case md::MDStatusEvent::TYPE::TRADING_STATUS: {
                    switch (venue) {
                        case hf::VENUE::CME:
                            handleCMEMDStatus(internalId, event, state);
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
                    LOG(0, "Contract {C[i1]} market data - received unknown md status.", getLoggingContract(internalId))
                    sendErrorMessage(boost::str(boost::format("Contract %1% market data - received unknown md status.") % state._ime._name), ErrorType::Critical);
                    break;
                }

                // remove compile warning
                case md::MDStatusEvent::TYPE::UNRECOVERABLE:
                    break;
            }
        }
    }

    void handleICEMDStatus(uint32_t internalId, const md::MDStatusEvent& event, ContractStateT& state) {
        switch (event.extStatus) {
            case 'O': {
                state._ime._contractCanTrade = true;
                sendEndIOPTrackingToFront(internalId);
                LOG(0, "Contract {C[i1]} market data - opened.", getLoggingContract(state._ime._internalId));
                sendErrorMessage(boost::str(boost::format("Contract %1% market data - opened. ICE external quoting save to enable.") % _config->_allContracts[state._ime._internalId]._name), ErrorType::Normal);
                break;
            }
            case 'C': {
                state._ime._contractCanTrade = false;
                LOG(0, "Contract {C[i1]} market data - closed.", getLoggingContract(state._ime._internalId))
                if (canQuoteExternally(hf::VENUE::ICE)) {
                    changeExternalQuoting(hf::VENUE::ICE, SystemState::Disabled);
                    sendErrorMessage(boost::str(
                            boost::format("Contract %1% market data - closed. Disable ICE external quoting.") %
                            _config->_allContracts[state._ime._internalId]._name), ErrorType::Critical);
                } else {
                    sendErrorMessage(boost::str(boost::format("Contract %1% market data - closed.") % _config->_allContracts[state._ime._internalId]._name), ErrorType::Normal);
                }
                break;
            }
            case 'E': {
                LOG(0, "Contract {C[i1]} market data - expired.", getLoggingContract(state._ime._internalId))
                if (canQuoteExternally(hf::VENUE::ICE)) {
                    changeExternalQuoting(hf::VENUE::ICE, SystemState::Disabled);
                    sendErrorMessage(boost::str(
                            boost::format("Contract %1% market data - expired. Disable ICE external quoting.") %
                            _config->_allContracts[state._ime._internalId]._name), ErrorType::Critical);
                } else {
                    stopContract(state, boost::str(boost::format("Contract %1% market data - expired.") % _config->_allContracts[state._ime._internalId]._name));
                }
                break;
            }
            case '1': {
                state._ime._contractCanTrade = false;
                LOG(0, "Contract {C[i1]} market data - entered pre-open.", getLoggingContract(state._ime._internalId))
                if (canQuoteExternally(hf::VENUE::ICE)) {
                    changeExternalQuoting(hf::VENUE::ICE, SystemState::Disabled);
                    sendErrorMessage(boost::str(
                            boost::format("Contract %1% market data - pre-open. Disable ICE external quoting.") %
                            _config->_allContracts[state._ime._internalId]._name), ErrorType::Critical);
                } else {
                    sendErrorMessage(boost::str(boost::format("Contract %1% market data - pre-open.") % _config->_allContracts[state._ime._internalId]._name), ErrorType::Normal);
                }
                break;
            }
            default: {
                break;
            }
        }
    }

    void handleCMEMDStatus(uint32_t internalId, const md::MDStatusEvent& event, ContractStateT& state) {
        switch (event.extStatus) {
            case 17: {
                state._ime._contractCanTrade = true;
                sendEndIOPTrackingToFront(internalId);
                LOG(0, "Contract {C[i1]} market data - opened.", getLoggingContract(state._ime._internalId))
                sendErrorMessage(boost::str(boost::format("Contract %1% market data - opened. CME ExternalQuoting safe to enabled") % _config->_allContracts[state._ime._internalId]._name), ErrorType::Normal);
                break;
            }
            case 2: {
                state._ime._contractCanTrade = false;
                LOG(0, "Contract {C[i1]} market data - halted.", getLoggingContract(state._ime._internalId))
                if (canQuoteExternally(hf::VENUE::CME)) {
                    changeExternalQuoting(hf::VENUE::CME, SystemState::Disabled);
                    sendErrorMessage(boost::str(
                            boost::format("Contract %1% market data - halted. Disable CME external quoting.") %
                            _config->_allContracts[state._ime._internalId]._name), ErrorType::Critical);
                } else {
                    sendErrorMessage(boost::str(boost::format("Contract %1% market data - halted.") % _config->_allContracts[state._ime._internalId]._name), ErrorType::Normal);
                }
                break;
            }
            case 18:
            case 4: {
                state._ime._contractCanTrade = false;
                LOG(0, "Contract {C[i1]} market data - closed({i2}).", getLoggingContract(state._ime._internalId), (uint32_t)(event.extStatus))
                if (canQuoteExternally(hf::VENUE::CME)) {
                    changeExternalQuoting(hf::VENUE::CME, SystemState::Disabled);
                    sendErrorMessage(boost::str(
                            boost::format("Contract %1% market data - closed(%2%). Disable CME external quoting.") %
                            _config->_allContracts[state._ime._internalId]._name % (uint32_t) event.extStatus),
                            ErrorType::Critical);
                } else {
                    sendErrorMessage(boost::str(boost::format("Contract %1% market data - closed(%2%).") % _config->_allContracts[state._ime._internalId]._name % (uint32_t)event.extStatus), ErrorType::Normal);
                }
                break;
            }
            case 21: {
                state._ime._contractCanTrade = false;
                LOG(0, "Contract {C[i1]} market data - entered pre-open.", getLoggingContract(state._ime._internalId))
                if (canQuoteExternally(hf::VENUE::CME)) {
                    changeExternalQuoting(hf::VENUE::CME, SystemState::Disabled);
                    sendErrorMessage(boost::str(
                            boost::format("Contract %1% market data - pre-open. Disable CME external quoting.") %
                            _config->_allContracts[state._ime._internalId]._name), ErrorType::Critical);
                } else {
                    sendErrorMessage(boost::str(boost::format("Contract %1% market data - pre-open.") % _config->_allContracts[state._ime._internalId]._name), ErrorType::Normal);
                }
                break;
            }
            default: {
                break;
            }
        }
    }

    void onMDIntraEventSweep(hf::VENUE venue, const md::MDIntraEventSweepEvent& event) {
        const auto& it = _sidToInternalId.find(event.sid);
        if(BOOST_LIKELY(it != _sidToInternalId.end())) {
            const auto& sweepLimits = _sweepDefinition->getRef(it->second);
            if(sweepLimits.sweepDetected(event)) {
                LOG(0, "Sweep detected, disabling RV disabling ExternalQuote.  Contract={C[i1]} PriceLimit={i2} QtyLimit={i3} event.diff={i4} event.qty={i5}",
                    getLoggingContract(it->second), sweepLimits._sweepTicksAtPrice, sweepLimits._tradedQty, event.diff, event.qty)
                if(BOOST_LIKELY( disableRVDisableMMExternalQuoteForAllContractsIfEnabled(venue))) {
                    sendErrorMessage("Sweep detected, disabling RV disabling ExternalQuote.  Contract=" + getContractName(it->second) + " PriceLimit=" +
                                 std::to_string(sweepLimits._sweepTicksAtPrice) + " QtyLimit=" + std::to_string(sweepLimits._tradedQty) +
                                 " event.diff=" + std::to_string(event.diff) + " event.qty=" + std::to_string(event.qty), ErrorType::Critical);
                }
            }
        }
    }

    template <typename T>
    void stopContract(T& state, const std::string& message) {
        state._ime._contractCanTrade = false;
        if (isEnabled()) {
            state._marketMaker.disable();
        }
        sendErrorMessage(message, ErrorType::Critical);
    }

    void onMDGap(hf::VENUE venue, const md::MDGapEvent& event) {
        disableSystem("MD feed gapped.");
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
                    if (event.totalExecutedQty > order.executedQty) {
                        const_cast<om::order_t&> (order).is_active = event.is_active; //Is this set on full fill?
                        const_cast<om::order_t&> (order).executedQty = event.totalExecutedQty;
                        onOrderExecution(event, order);
                    }
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
                    const_cast<om::order_t &> (order).is_active = false;
                    onOrderRejected(event, order);
                    break;
                }
                case om::OMEvent::TYPE::OC: {
                    const auto &event = static_cast<const om::OMOrderCanceledEvent &> (ome);
                    const auto &order = this->context().getOrder(event.oid);
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
                    onOrderReplaced(event, order);
                    break;
                }
                case om::OMEvent::TYPE::MR: {
                    const auto &event = static_cast<const om::OMOrderModifyRejectedEvent &> (ome);
                    const auto &order = this->context().getOrder(event.oid);
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
                case om::OMEvent::TYPE::STRATEGY_LOGIN_RESPONSE: {
                    const auto &event = static_cast<const om::StrategyLoginResponseEvent &> (ome);
                    onStrategyLoginResponse(event);
                    break;
                }
                case om::OMEvent::TYPE::RECOVERED_ORDERS: {
                    const auto &event = static_cast<const om::OMRecoveredOrdersEvent &> (ome);
                    onRecoveredOrders(event);
                    break;
                }
                case om::OMEvent::TYPE::RECOVERED_FILLS: {
                    const auto &event = static_cast<const om::OMRecoveredFillsEvent &> (ome);
                    onRecoveredFills(event);
                    break;
                }
                default:
                    break;
            }
        }
    }

    void onMDMonitorStatusUpdate(const om::MDMonitorStatusUpdate &event) override {
        if (!event.active) {
            sendErrorMessage("MD/OM died - killing viper.", ErrorType::Critical);
            kill(getpid(), SIGKILL);
        }
    }

    virtual void onStrategyLoginResponse(const om::StrategyLoginResponseEvent &event) {
        LOG(0, "Received Strategy login response. Success={i1} PID={i2} AlgoId={i3}", event.success, event.pid, event.algo_id)

        //If the gateway was already connected prior to launching Viper, we will not get a port update status so we will explicitly update the Rhone limits here.  In case we are not connected yet,
        //this becomes a no-op since all get(...) calls will return nullPtr and structure will be properly populated on the {GW,Port}StatusUpdate messages.
        for(om::port_id_t pid = 0; pid < om::PORT_NUM; ++pid) {
            gatherRhoneLimitsAndSendToRV(pid);
        }

        if (_pendingGatewayLogin.any()) {
            auto venue = getGatewayVenue(event.pid);
            _pendingGatewayLogin.reset(static_cast<uint8_t>(venue));

            if (!_pendingGatewayLogin.any()) {
                static bool logged = false;
                if (!logged) {
                    logged = true;
                    using ExecutorSpace::s_log;
                    LOG_INFO("All Gateway Logged in");

                    // Hack: startViper.sh looking for "Wanted to send GridUpdate but do not have connection" in rhone.log
                    // The problem is that the string is not always logged.
                    // Log a hack string here so that the startViper.sh script can reliably pass
                    LOG_INFO("Wanted to send GridUpdate but do not have connection. Hack for startViper.sh backward compatibility");
                }
            }
        }
    }

    void onOMGWStatusUpdate(const om::GWStatusUpdate& event) override {
        handleGatewayStatusChange(event);

        if (!event.active && isEnabled()) {
            sendErrorMessage("OMS not running - killing viper.", ErrorType::Critical);
            kill(getpid(), SIGKILL);
        }
    }

    void onOMPortStatusUpdate(const om::PortStatusUpdate& event) override {
        handleGatewayStatusChange(event);
    }

    template <typename T>
    void handleGatewayStatusChange(T& event) {
        _gatewayStatus._active = event.active;
        _gatewayStatus._message = event.reasonText;
        hf::VENUE venue = getGatewayVenue(event.pid);

        if (_systemState->_marketMaker.load(std::memory_order_relaxed) != SystemState::LoggedOut) {
            sendGatewayStatusToFront();
            std::string message = boost::str(
                    boost::format("%1% Gateway status change. pid=%2% Running=%3%") % ven::VenueUtils::getName(venue) %
                    event.pid % (event.active ? "YES" : "NO"));
            sendErrorMessage(message, ErrorType::Critical);
        }

        LOG(0, "EXE: {i1} gateway status changed. pid={i2} Active={i2}", static_cast<uint32_t>(venue), event.pid, _gatewayStatus._active)
        for (auto& state : _contractStates) {
            if (state._venue == venue) {
                state._ime._gatewayActive = _gatewayStatus._active;
            }
        }

        if (!_gatewayStatus._active) {
            _systemState->_relativeValue.store(SystemState::Disabled, std::memory_order_release);
            changeExternalQuoting(venue, SystemState::Disabled);
            std::string message = boost::str(boost::format("%1% OMS not running. Disable RV and %1% ExternalQuoting.") % ven::VenueUtils::getName(venue));
            sendErrorMessage(message, ErrorType::Critical);
        }
        else {
            //TODO: When we're on 3.55 we can iterate over just the pid that generated the event.  Gateway Containers are messing this up since containers have a dedicated pid that is sent in the event.
            for(om::port_id_t pid = 0; pid < om::PORT_NUM; ++pid) {
                gatherRhoneLimitsAndSendToRV(pid);
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
        auto& outMessage = *static_cast<ViperMessage*>(_outToFront->getPoolP()->getNextP());
        outMessage._type = ViperMessageType::GatewayStatus;
        GatewayStatusMessage& message = reinterpret_cast<GatewayStatusMessage &> (outMessage._data[0]);
        message._active = _gatewayStatus._active;
        if (!_gatewayStatus._message.empty()) {
            strcpy(message._message, _gatewayStatus._message.c_str());
        }
        const auto outIdx = _outToFront->getPoolP()->append(sizeof(ViperMessageType) + sizeof(GatewayStatusMessage));
        mb();
        _outToFront->appendNew(outIdx);
    }

    void handleGuiInitialized() {
        sendGatewayStatusToFront();
        this->context().traderConnect("", _iid);
    }

    bool isRvHedge(const om::order_t& order) {
        return order.ot == om::ORDER_TYPE::FAK;
    }

    uint32_t getInternalId(const om::order_t& order) {
        return order.debug.i[INTERNAL_ID];
    }

    bool isLoggedIn() {
        return _systemState->_marketMaker.load(std::memory_order_relaxed) != SystemState::LoggedOut;
    }

    void onOrderAccepted(const om::OMOrderAcceptedEvent& event, const om::order_t& order) override {
        if (isLoggedIn()) {
            const_cast<om::order_t&> (order).leavesQty = event.acceptedQty;
            if (!isRvHedge(order)) {
                auto& ime = getInternalMatchingEngine(order);
                ime.onOrderAccepted(event, order);
            }
        } else {
            LOG(0, "Received Order Accepted when logged out. {i1}", event.oid)
        }
    }

    void onOrderRejected(const om::OMOrderRejectedEvent& event, const om::order_t& order) override {
        if (isLoggedIn()) {
            if (isRvHedge(order)) {
                LOG(0, "Hedge order rejected. Contract={C[i1]} OID={i2} Reason={i3}", getLoggingContract(getInternalId(order)), order.oid, event.reasonCodeExt)
                if (IsSelfMatchReject(order.venue, event.reasonCodeExt)) {
                    ++_rvHedgeRejectCount;
                    if (_rvHedgeRejectCount == RV_HEDGE_REJECT_COUNT_LIMIT) {
                        handleRvHedgeRejectLimitBreach(order);
                    } else {
                        sendReverseFillToRelativeValue(getInternalId(order), order, FillSource::SelfMatch);
                    }
                } else {
                    reverseRvHedgeFillToMarketMaker(order);
                    if (BOOST_LIKELY(!IsRejectDueToMarketClosed(order.venue, event.reasonCodeExt))) {
                        disableRVDisableMMExternalQuote(getInternalId(order), boost::str(boost::format("RV hedge reject. Code=%1% Text=%2%") % event.reasonCodeExt % event.reasonText));
                    }
                }
            } else {
                auto& ime = getInternalMatchingEngine(order);
                ime.onOrderRejected(event, order);
            }
        } else {
            LOG(0, "Received Order Rejected when logged out. {i1}", event.oid)
        }
    }

    void handleRvHedgeRejectLimitBreach(const om::order_t& order) {
        _rvHedgeRejectCount = 0;
        _systemState->_relativeValue.store(SystemState::Disabled, std::memory_order_release);
        moveSelfMatchRejectToMarketMaker(order);
    }

    void reverseRvHedgeFillToMarketMaker(const om::order_t& order) {
        const uint32_t internalId = getInternalId(order);
        const hf::qty_t unfilledSize = order.qty - order.executedQty;
        const uint32_t reverseSide = order.side == hf::SIDE::BUY ? 1 : 0;
        ContractStateT& state = _contractStates[internalId];
        state._marketMaker.forceTakeOverFill(reverseSide, unfilledSize);
        sendFillToFront(internalId, reverseSide, order.pr, unfilledSize, StrategyId::MarketMaker, FillType::External, FillSource::RvSelfMatch, 0, unfilledSize, 0);
    }

    void moveSelfMatchRejectToMarketMaker(const om::order_t& order) {
        reverseRvHedgeFillToMarketMaker(order);
        auto internalId = getInternalId(order);
        ContractStateT& state = _contractStates[internalId];
        if (!state._marketMaker.isWithinPositionLimits()) {
            disableMMExternalQuoting(internalId);
        } else {
            state._marketMaker.onEndOfFills();
        }
    }

    void reconcileGtcOrdersAfterRvEnabled() {
        if (_reconcileGtcOrders) {
            _reconcileGtcOrders = false;
            for (auto& state : _contractStates) {
                state._ime.cancelRemainingGtc();
            }
        }
    }

    void onOrderReplaced(const om::OMOrderModifiedEvent& event, const om::order_t& order) override {
        if (isLoggedIn()) {
            auto& ime = getInternalMatchingEngine(order);
            ime.onOrderReplaced(event, order);
        } else {
            LOG(0, "Received Order Replaced when logged out. {i1}", event.oid)
        }
    }

    void onOrderReplaceRejected(const om::OMOrderModifyRejectedEvent& event, const om::order_t& order) override {
        if (isLoggedIn()) {
            auto& ime = getInternalMatchingEngine(order);
            ime.onOrderReplaceRejected(event, order);
            ++ime._messages._modifyRejects;
        } else {
            LOG(0, "Received Order Replace Rejected when logged out. {i1}", event.oid)
        }
    }

    void onOrderExecution(const om::OMExecutionEvent &event, const om::order_t &order) override {
        if (isLoggedIn()) {
            auto& ime = getInternalMatchingEngine(order);
            if (!isRvHedge(order)) {
                if (order.debug.i[ORDER_OWNER] == UNASSIGNED_OWNER) {
                    handleFillForUnassignedGtcOrder(event, ime._internalId, order);
                } else {
                    ime.onOrderExecution(event, order);
                }
            } else {
                const uint32_t internalId = ime._internalId;
                LOG(0, "Hedge order filled. Contract={C[i1]} OID={i2}", getLoggingContract(internalId), order.oid)
            }
            ime._messages._fills += event.filledQty;
        } else {
            LOG(0, "Received Order Replace Executed when logged out. {i1}", event.oid)
        }
    }

    template <typename FillT>
    void handleFillForUnassignedGtcOrder(const FillT& event, uint32_t internalId, const om::order_t &order) {
        ContractStateT& state = _contractStates[internalId];
        uint32_t side = order.side == hf::SIDE::BUY ? 0 : 1;
        state._marketMaker.forceTakeOverFill(side, event.filledQty);
        sendFillToFront(internalId, side, event.filledPrice, event.filledQty, StrategyId::MarketMaker, FillType::External, FillSource::ViperOff, 0, event.filledQty, 0);
        if((!order.is_active) || (!order.leavesQty)) {
            getInternalMatchingEngine(order).tryRemoveGtcOrder(order);
        }
    }

    void onOrderCanceled(const om::OMOrderCanceledEvent& event, const om::order_t& order) override {
        if (isLoggedIn()) {
            if (isRvHedge(order)) {
                auto internalId = getInternalId(order);
                LOG(0, "Hedge order cancelled. Contract={C[i1]} OID={i2}", getLoggingContract(internalId), order.oid)
                sendReverseFillToRelativeValue(internalId, order, FillSource::MissedHedge);
            } else {
                auto& ime = getInternalMatchingEngine(order);
                ime.onOrderCanceled(event, order);
                if (BOOST_UNLIKELY(order.debug.i[ORDER_OWNER] == UNASSIGNED_OWNER)) {
                    _slow._iopTracking.onOrderCancelled(getInternalId(order), order.oid);
                }
                else if (BOOST_UNLIKELY(IsExternalUnsolicitedCancel(order.venue, event.reasonCodeExt, event.reasonText))) {
                    auto internalId = getInternalId(order);
                    LOG(0, "Received external cancel.  Contract={C[i1]} OID={i2} px={i3} leavesQty={i4}", getLoggingContract(internalId), order.oid, order.pr, order.leavesQty);
                    sendErrorMessage("Received external cancel.  Contract=" + getContractName(internalId) + " OID=" + std::to_string(order.oid) + " px=" + std::to_string(order.pr) +
                       " leavesQty=" + std::to_string(order.leavesQty), ErrorType::Critical);
                }
            }
        } else {
            LOG(0, "Received Order Cancelled when logged out. {i1}", event.oid)
        }
    }

    void onOrderCancelRejected(const om::OMOrderCancelRejectedEvent &event, const om::order_t &order) override {
        if (isLoggedIn()) {
            auto& ime = getInternalMatchingEngine(order);
            ime.onOrderCancelRejected(event, order);
            ++ime._messages._cancelRejects;
        } else {
            LOG(0, "Received Order Cancel Rejected when logged out. {i1}", event.oid)
        }
    }

    void onOrderSpreadLegExecution(const om::OMSpreadLegExecutionEvent &event, const om::order_t &order) {}
    void onMassCancelAccepted(const om::OMMassCancelAcceptedEvent &event) {}
    void onMassCancelRejected(const om::OMMassCancelRejectedEvent &event) {}

    void cancelOrder(uint32_t internalId, const om::order_t* order) {
        this->context().cancelOrder(order->oid);
        ContractStateT& state = _contractStates[internalId];
        ++state._ime._messages._cancels;
    }

    om::order_t* newLimitOrder(uint32_t internalId, uint32_t side, hf::price_t price, hf::qty_t size, om::ORDER_TYPE orderType, bool isHedge = false) {
        ContractStateT& state = _contractStates[internalId];
        Throttler& throttler = _throttler[state._throttlerIndex];
        const uint32_t route = throttler.getRoute();
        om::order_t* order = this->context().createOrder(_iid, om::RISK_TYPE::UNKNOWN, state._rid[route],
                                                         state._marketMaker._contract._sid, orderType,
                                                         ToRhoneSide(side), price, size, om::STP_ACTION::CANCEL_PASSIVE);
        if (BOOST_UNLIKELY(order == nullptr)) {
            LOG(0, "Could not send limit order. Contract={C[i1]} Side={i2} Price={d1} Size={i3}", getLoggingContract(internalId), side, ToDollars(price), size)
        } else {
            LOG(0, "Sent limit order. Contract={C[i1]} Side={i2} Price={d1} Size={i3} OID={i4}", getLoggingContract(internalId), side, ToDollars(price), size, order->oid)
            addRoutingStateToNewLimitOrder(*order, internalId, route);
            ++state._ime._messages._inserts;
            if (!isHedge) {
                addMessageToThrottler(state._throttlerIndex, route);
            }
        }
        return order;
    }

    void addRoutingStateToNewLimitOrder(om::order_t& order, uint32_t internalId) {
        ContractStateT& state = _contractStates[internalId];
        Throttler& throttler = _throttler[state._throttlerIndex];
        addRoutingStateToNewLimitOrder(order, internalId, throttler.getRoute());
    }

    void addRoutingStateToNewLimitOrder(om::order_t& order, uint32_t internalId, uint32_t route) {
        order.debug.i[INTERNAL_ID] = internalId;
        order.debug.i[ROUTE] = route;
    }

    void disableMarketMakerAndRV(uint32_t internalId, const std::string& reason) {
        std::string message = boost::str(boost::format("Disabled MM *COMPLETELY*.  Disable RV quote external. Contract=%1% Reason=%2%") % _config->_allContracts[internalId]._name % reason);
        sendErrorMessage(message, ErrorType::Critical);
        LOG(0, "Disabling MM and RV due to rejects. Contract={C[i1]}", getLoggingContract(internalId))
        ContractStateT& state = _contractStates[internalId];
        state._marketMaker.disable(true, true);
        _quoteRvExternal->setValue(internalId, false);
        sendGenericMessageToFront(ViperMessageType::RVQuoteExternallyOff, internalId);
        state._ime.getAllOutOfMarket();
    }

    void disableRVDisableMMExternalQuote(uint32_t internalId, const std::string& reason) {
        std::string message = boost::str(boost::format("Disabled MM quote external.  Disable RV quote external. Contract=%1% Reason=%2%") % _config->_allContracts[internalId]._name % reason);
        sendErrorMessage(message, ErrorType::Critical);
        LOG(0, "Disabling external quoting on MM and RV due to rejects. Contract={C[i1]}", getLoggingContract(internalId))
        ContractStateT& state = _contractStates[internalId];
        _quoteRvExternal->setValue(internalId, false);
        state._ime.getRelativeValueOutOfMarket();
        sendGenericMessageToFront(ViperMessageType::RVQuoteExternallyOff, internalId);
        disableMMExternalQuoting(internalId);
    }

    void disableMMExternalQuoteWithMessage(uint32_t internalId, const std::string& reason) {
        std::string message = boost::str(boost::format("Disabled MM quote external. Contract=%1% Reason=%2%") % _config->_allContracts[internalId]._name % reason);
        sendErrorMessage(message, ErrorType::Critical);
        LOG(0, "Disabling external quoting on MM due to rejects. Contract={C[i1]}", getLoggingContract(internalId))
        disableMMExternalQuoting(internalId);
    }

    //No message, this is critical enough that we're going to forgo building the string and log manually
    //returns if we took action
    bool disableRVDisableMMExternalQuoteForAllContractsIfEnabled(hf::VENUE venue) {
        bool needToNotifyUser = false;
        if(BOOST_LIKELY(_systemState->_relativeValue.load(std::memory_order_relaxed) == SystemState::Enabled)) {
            needToNotifyUser = true;
            _systemState->_relativeValue.store(SystemState::Disabled, std::memory_order_release);
            handleRelativeValueDisabledKernel();
        }

        //TODO: This calls synchronize() twice, once in getRelativeValueOutOfMarket(), once in disableExternalQuoting().  Should
        //really only happen once.
        if (BOOST_LIKELY((_systemState->_marketMaker.load(std::memory_order_relaxed) == SystemState::Enabled) &&
                         _systemState->_quoteExternally.load(std::memory_order_relaxed).any())) {
            for (int i = 0; i < static_cast<int>(_contractStates.size()); ++i) {
                _contractStates[i]._ime.getRelativeValueOutOfMarket();
                if (_contractStates[i]._marketMaker._toMarketMake) {
                    auto oldExtQuote = _contractStates[i]._marketMaker.disableExternalQuoting(true);
                    needToNotifyUser |= oldExtQuote;
                    // revert to original value, will disable QuoteExternal switch
                    _contractStates[i]._marketMaker._quoteExternal = oldExtQuote;
                }
            }
        }

        _systemState->_quoteExternally.store(0, std::memory_order_release);

        if (needToNotifyUser) {
            LOG(0, "Executor. Relative Value Disabled Quote External Disabled. Removing orders. Reason=disableRVDisableMMExternalQuoteForAllContractsIfEnabled {i1}", 0)
            sendErrorMessage("Executor. Relative Value Disabled Quote External Disabled. Removing orders. Reason=disableRVDisableMMExternalQuoteForAllContractsIfEnabled", ErrorType::Critical);
        }

        return needToNotifyUser;
    }

    void disableRVCompletelyGateRVDisableMMForInstrument(uint32_t id, const std::string& reason) {
        _contractStates[id]._marketMaker.disable();
        _systemState->_rvGated.store(true, std::memory_order_release);
        _systemState->_relativeValue.store(SystemState::Disabled, std::memory_order_release);
        handleRelativeValueDisabledKernel();
        notifyFrontMMDisabled(id);
        sendErrorMessage("Relative Value Disabled AND GATED.  MM Disabled for instrument=" + getContractName(id) + " reason=" + reason, ErrorType::Critical);
    }

    void disableMMExternalQuoting(uint32_t id) {
        _contractStates[id]._marketMaker.disableExternalQuoting();
        notifyFrontMMExternalQuoteDisabled(id);
    }

    void notifyFrontMMExternalQuoteDisabled(uint32_t id) {
        sendGenericMessageToFront(ViperMessageType::DisableExternalQuotingOnMarketMaker, id);
    }

    void notifyFrontMMDisabled(uint32_t id) {
        sendGenericMessageToFront(ViperMessageType::DisableMarketMakerInstrument, id);
    }

    void notifyFrontUseGTCDisabled(int32_t id) {
        sendGenericMessageToFront(ViperMessageType::DisableUseGTC, id);
    }

    void addMessageToThrottler(uint32_t throttlerIndex, uint32_t route) {
        const uint64_t now = clockUtil::GetCycles();
        bool isInDangerZone = false;
        bool isAtCapacity = false;
        Throttler& throttler = _throttler[throttlerIndex];
        throttler.addMessage(route, now, isInDangerZone, isAtCapacity);
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

    om::order_t* newFAKOrder(uint32_t internalId, uint32_t side, hf::price_t price, hf::qty_t size) {
        ContractStateT& state = _contractStates[internalId];
        Throttler& throttler = _throttler[state._throttlerIndex];
        const uint32_t route = throttler.getRoute();
        om::order_t* order = nullptr;
        order =  this->context().createOrderExt(_iid, om::RISK_TYPE::UNKNOWN, state._rid[route], state._marketMaker._contract._sid, om::ORDER_TYPE::FAK, ToRhoneSide(side), price, 0, size, 1, 0, om::STP_ACTION::CANCEL_PASSIVE);
        if (order == nullptr) {
            LOG(0, "Could not send FAK order. Contract={C[i1]} Side={i2} Price={d1} Size={i3}", getLoggingContract(internalId), side, ToDollars(price), size)
        } else {
            LOG(0, "Sent FAK order. Contract={C[i1]} Side={i2} Price={d1} Size={i3} OID={i4}", getLoggingContract(internalId), side, ToDollars(price), size, order->oid)
            order->debug.i[INTERNAL_ID] = internalId;
            ++state._ime._messages._inserts;
        }
        return order;
    }

    void modifyOrder(uint32_t internalId, const om::order_t* order, hf::price_t price, hf::qty_t size) {
        ContractStateT& state = _contractStates[internalId];
        const bool done = this->context().modifyOrder(order->oid, price, size, om::STP_ACTION::CANCEL_PASSIVE);
        if (done) {
            ++state._ime._messages._modifies;
            LOG(0, "Sent modify order. Contract={C[i1]} Side={i2} Price={d1} Size={i3}", getLoggingContract(internalId), ToViperSide(order->side), ToDollars(price), size)
            addMessageToThrottler(state._throttlerIndex, order->debug.i[ROUTE]);
        } else {
            LOG(0, "Could not modify order. Contract={C[i1]} Side={i2} Price={d1} Size={i3}", getLoggingContract(internalId), ToViperSide(order->side),ToDollars(price), size)
        }
    }

    void setThrottlingBanner(Throttler& throttler, THROTTLING_MODE mode) {
        for (auto& state : _contractStates) {
            if (state._marketMaker._toMarketMake) {
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
        for (uint32_t index = 0; index < _numberOfThrottlers; ++index) {
            Throttler& throttler = _throttler[index];
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
        ContractStateT& state = _contractStates[internalId];
        state._ime.expireHedgeQuoteBanner(side);
    }

    void expireFillBanner(uint32_t internalId, uint32_t side) {
        ContractStateT& state = _contractStates[internalId];
        state._ime.expireFillBanner(side);
    }

    void expireQuoteBanner(uint32_t internalId, uint32_t side) {
        ContractStateT& state = _contractStates[internalId];
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
                _currentTrader = trader;
            }
        }
    }

    bool onManualFills(GridUpdate& update) {
        int numberOfFills = update._update.cells_size() / 3;
        _manualFills.resize(numberOfFills);
        int currentManualFillIndex = -1;
        int priorGridCellRow = -1;
        for (int i = 0; i < update._update.cells_size(); ++i) {
            const proto::GridCell& gridCell = update._update.cells(i);
            if(priorGridCellRow != static_cast<int>(gridCell.row())) {
                ++currentManualFillIndex;
                priorGridCellRow = static_cast<int>(gridCell.row());
            }
            ManualFillsEntryColumns columnType = static_cast<ManualFillsEntryColumns>(gridCell.col());
            ManualFill& fill = _manualFills[currentManualFillIndex];
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
            ManualFill& fill = _manualFills[index];
            if (fill._type == toUInt(ManualFillType::MarketMaker)) {
                ContractStateT& state = _contractStates[fill._contract];
                uint32_t size = std::abs(fill._size);
                uint32_t side = fill._size > 0 ? BID : ASK;
                hf::qty_t allocatedInternally = 0;
                hf::qty_t allocatedExternally = 0;
                state._marketMaker.addManualPosition(size, side, allocatedInternally, allocatedExternally);
                sendAcceptedManualFillToFront(fill._contract, allocatedInternally, true);
                sendAcceptedManualFillToFront(fill._contract, allocatedExternally, false);
            }
        }

        return true;
    }

    void sendAcceptedManualFillToFront(uint32_t internalId, hf::qty_t size, bool isInternal) {
        sendGenericMessageToFront(ViperMessageType::AcceptedManualFill, internalId, size, (isInternal ? 0 : 1));
    }

    auto& getMarketMaker(uint32_t id) {
        ContractStateT& state = _contractStates[id];
        return state._marketMaker;
    }

    std::vector<ContractStateT> _contractStates;
    std::unordered_map<hf::symbol_id_t, uint32_t> _sidToInternalId;

    md::queue::MDQueueResource* _inFromFront;
    md::queue::MDQueueResource* _inFromRV;
    md::queue::MDQueueResource* _outToRV;
    md::queue::MDQueueResource* _outToFront;

    SharedSystemState* _systemState;
    SystemState _executorOldSystemState = SystemState::LoggedOut;
    SystemState _relativeValueOldSystemState = SystemState::LoggedOut;
    Vector<bool>* _quoteRvExternal;
    Vector<RelativeValue::SimpleSweepDefinition>* _sweepDefinition;

    GatewayStatus _gatewayStatus;

    ViperConfig* _config;
    double _frequency;

    Timer<Executor> _fillBannerTimer;
    Timer<Executor> _quoteBannerTimer;
    Timer<Executor> _checkQuotesTimer;

    static const uint32_t MAX_NUMBER_OF_THROTTLERS = 2;
    Throttler _throttler[MAX_NUMBER_OF_THROTTLERS];
    uint32_t _numberOfThrottlers = 0;

    uint64_t _nextInternalHeartbeat;
    uint64_t _internalHeartbeatPeriod;

    std::vector<ManualFill> _manualFills;

    std::function<bool(uint32_t)> _pnlLimitsChecker;

    std::string _strategyName;
    std::string _strategyVersion;

    hf::queue::DebugQueue* _kdbLogger;
    bool _needToSetLogger = true;

    std::vector<md::queue::MDQueue*> _marketDataQueues;
    om::client_id_t _id;
    std::set<uint32_t> _updatedContracts;

    std::map<hf::VENUE, uint32_t> _transactionCapacity;
    om::instance_id_t _iid;

    uint32_t _rvHedgeRejectCount = 0;
    const uint32_t RV_HEDGE_REJECT_COUNT_LIMIT = 20;

    bool _reconcileGtcOrders = true;
    uint8_t _oldMDStatus[static_cast<uint8_t>(hf::VENUE::TEST)] = {0};

    Traders _currentTrader = Traders::Steven;

    std::unordered_map<om::port_id_t, hf::VENUE> _mapGatewayVenue;
    std::set<hf::VENUE> _usedVenues;
    SharedSystemState::QuoteExtBitset _oldQuoteExt;

    std::bitset<static_cast<uint8_t>(hf::VENUE::TEST)> _pendingGatewayLogin = {0};

    struct {
        IOPTracker<Executor<::strat::Strategy>> _iopTracking;
    } __attribute__((aligned(64))) _slow;
};

typedef Executor<::strat::Strategy> ExecutorT;

}

#endif //VIPER_EXECUTOR_H
