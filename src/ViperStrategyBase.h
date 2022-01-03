#pragma once

#include <atomic>
#include <vector>
#include <unordered_map>
#include <hf/Service.h>
#include <util/ConfigManager.h>
#include "Common.h"
#include "RingBufferQueue.h"
#include "ViperResource.h"
#include "ClockUtil.h"
#include "MarketDataConsumer.h"
#include "PerfMonitor.h"

namespace Python {

class ViperStrategyBase : public ViperResource {
public:
    std::atomic<SystemState> _state {SystemState::LoggedOut};
    std::atomic<SystemState> _quoteExternal {SystemState::Disabled};

protected:
    friend class MarketDataConsumer<ViperStrategyBase>;
    SystemState _oldState {SystemState::LoggedOut};
    std::function<bool(uint32_t)> _pnlLimitsChecker;

    uint64_t _nextInternalHeartbeat;
    uint64_t _internalHeartbeatPeriod;
    int64_t _autoDisableTime = -1;
    uint64_t _autoDisableTimeInFrequency = -1;
public:
    ViperStrategyBase() = default;
    virtual ~ViperStrategyBase() = default;

    virtual StrategyId strategyId() const noexcept = 0;

    virtual void init(double frequency,
                      uint64_t internalHeartbeatPeriod,
                      std::function<bool(uint32_t)> pnlLimitsChecker) {
        _frequency = frequency;
        _pnlLimitsChecker = std::move(pnlLimitsChecker);
        _internalHeartbeatPeriod = internalHeartbeatPeriod;
    }

    virtual void populateGUIInitialization(::google::protobuf::RepeatedPtrField< ::Python::proto::GridDefinition >* definitions) = 0;
    virtual void sendStateToGUI(std::function<void(proto::GridUpdate&, WindowIds)>&& send, std::function<bool(WindowIds)>&& hasSubscription) const = 0;
    virtual bool onManualFills(GridUpdate& update) = 0;
    virtual bool isContractQuoteExternal(uint32_t contractId) const noexcept = 0;

public:
    bool isEnabled() const noexcept {
        return _state.load(std::memory_order_relaxed) == SystemState::Enabled;
    }

    SystemState state() const noexcept {
        return _state.load(std::memory_order_relaxed);
    }

    void enable() noexcept {
        _state.store(SystemState::Enabled, std::memory_order_release);
    }

    SystemState disable() noexcept {
        return _state.exchange(SystemState::Disabled, std::memory_order_release);
    }

    bool canQuoteExternally() const noexcept {
        return _quoteExternal.load(std::memory_order_relaxed) == SystemState::Enabled;
    }

    void enableQuoteExternal() noexcept {
        return _quoteExternal.store(SystemState::Enabled, std::memory_order_release);
    }

    SystemState disableQuoteExternal() noexcept {
        return _quoteExternal.exchange(SystemState::Disabled, std::memory_order_release);
    }

    int32_t autoDisableTime() const noexcept {
        return _autoDisableTime;
    }

    bool setAutoDisableTime(int32_t time, uint64_t now, bool verify = true) noexcept {
        if (time == -1) {
            _autoDisableTime = -1;
            _autoDisableTimeInFrequency = -1;
            LOG(0, "{i1} AutoDisableTime=-1", toUInt(strategyId()))
            return true;
        }

        uint32_t hour, min, sec;
        if (!verify || verifyHHMMSSTime(time, hour, min, sec)){
            _autoDisableTime = time;
            _autoDisableTimeInFrequency = convertTimeToCycles(hour, min, sec, now, _frequency);
            LOG(0, "{i1} AutoDisableTime={i2}({i3}) now={i4}", toUInt(strategyId()), time, _autoDisableTimeInFrequency, now)
            return true;
        }
        return false;
    }

#ifdef IS_DEV
    std::string strategyName() const {
        return toString(strategyId());
    }
#else
    uint32_t strategyName() const {
        return toUInt(strategyId());
    }
#endif

protected:
    void sendRegisterOrderClientRequest(uint32_t internalId, std::vector<StrategyId>&& subscriptions) const {
        sendMessageToExecutor<RegisterOrderClient>(
                ViperMessageType::RegisterOrderClient,
                [this, internalId, &subscriptions](RegisterOrderClient& req) {
                    req._internalId = internalId;
                    req._strategy = strategyId();
                    req._subscriptionCount = subscriptions.size();
                    StrategyId* sub = &req._internalMarketSubscriptions[0];
                    for (auto strategyId : subscriptions) {
                        *sub = strategyId;
                        ++sub;
                    }
                },
                subscriptions.size());
    };

    void sendInternalHeartbeat(uint64_t now) noexcept {
        if (BOOST_UNLIKELY(now > _nextInternalHeartbeat)) {
            _nextInternalHeartbeat = now + _internalHeartbeatPeriod;
            sendGenericMessageToFront(ViperMessageType::InternalHeartbeat);
        }
    }

    bool isPnlWithinLimits() const {
        const bool withinLimits = _pnlLimitsChecker(toUInt(strategyId()));
        if (!withinLimits) {
            sendErrorMessage(boost::str(boost::format("%1% - not within pnl limits.") % toString(strategyId())),ErrorType::Critical);
            LOG(0, "{i1} - is not within pnl limits", strategyName())
        }
        return withinLimits;
    }
};

/**
 * ViperStrategyBaseImpl class
 *
 * ViperStrategy need to derived from this class.
 *
 * NOTE:
 *      If Strategy override onCreate function, it MUST call ViperStrategyBaseImpl<Strategy>::onCreate function
 *      If Strategy override onPostCrate function, it MUST call ViperStrategyBaseImpl<Strategy>::onPostCreate function
 *
 * Strategy MUST implement following functions:
 *      void registerOrders()   // register quote/hedge order(s) in IME
 *      void runCycle()         // this is call after msgQueue and mdQueue are drained
 *      void onQuoteExternalChanged(const SharedSystemState::QuoteExtBitset& quoteExt)
 *      void handleHardShutdown()
 *      void handleDisabled()
 *      void handleEnabled()
 *      void handleLogout()
 *      void handleLogin()
 *      void handleCheckQuotes()
 *      void cancelGtcAndDisable()
 *      void onMDBach(const md::MDBatch& batch)
 *      void handleViperMessage(const ViperMessage& msg)
 */
template<typename T>
class ViperStrategyBaseImpl : public virtual hf::Service, public ViperStrategyBase, public MarketDataConsumer<T> {
    std::vector<ManualFill> _manualFills;

public:
    void onCreate(const libconfig::Setting& config) override {
        ViperResource::_onCreate(config);
    }

    void onPostCreate(const libconfig::Setting& config) override {
        ViperResource::_onPostCreate(config);
        _nextInternalHeartbeat = 0;
    }

    //FLATTEN //Somehow this breaks LTO
    void onRun() override {
        if (_kdbLogger) {
            logging::GetLogger().logInfo(boost::str(boost::format("%1% logger %2%") % toString(strategyId()) % _kdbLogger));
            _kdbLogger->resetW();
            logging::setKdbLogger(_kdbLogger);
        }
        this->initMarkDataQueueState(_viperConfig._allContracts.size());
        underlying().registerOrderClient();

        LOG(0, "Strategy {i1} thread started.", strategyName())

        while (BOOST_LIKELY(!hf::Service::isExitAll())) {
            checkSystemState();
            drainInternalMessages();
            this->drainAllMarketData();
            const uint64_t now = clockUtil::GetCycles();
            underlying().runCycle(now);
            sendInternalHeartbeat(now);
            checkAutoDisable(now);
        }
    }

    bool onManualFills(GridUpdate& update) override {
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
            ManualFill& fill = _manualFills[currentManualFillIndex];
            ManualFillsEntryColumns columnType = static_cast<ManualFillsEntryColumns>(gridCell.col());
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
            if (fill._type == toUInt(strategyId())) {
                underlying().onManualFill(fill);
            }
        }

        underlying().onEndOfManualFills();
        return true;
    }

protected:
    COLD NEVER_INLINE
    void handleDebugStatusEvents(const DebugStatusEvents& dses) {
        HandleDebugStatusEvents(dses, &underlying());
    }

private:
    friend T;
    ViperStrategyBaseImpl() {
        _manualFills.reserve(5);
    }

    T& underlying() noexcept { return static_cast<T&>(*this); }
    T const& underlying() const noexcept { return static_cast<T const&>(*this); }

    void checkSystemState() {
        CONDITIONAL_SCOPED_PERF_POINT(AS_CheckSysState, [this]() -> bool { return strategyId() == StrategyId::ArbieSpreader; });
        SystemState state = _state.load(std::memory_order_relaxed);
        if (state != _oldState) {
            if (onSystemStateChange(state)) {
                _oldState = state;
            } else {
                _state.store(_oldState, std::memory_order_release);
            }
        }

        auto quoteExt = _quoteExternal.load(std::memory_order_relaxed);
        if (quoteExt != _oldQuoteExt) {
            LOG(0, "{i1} QuoteExternal Switch Changed. {i2} -> {i3}", strategyName(), toUInt(_oldQuoteExt), toUInt(quoteExt))
            underlying().onQuoteExternalChanged(quoteExt);

            sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Enabled),
                                      (quoteExt == SystemState::Enabled ? proto::SystemState::SystemStateEnabled
                                                                        : proto::SystemState::SystemStateDisabled),
                                      toUInt(strategyId()) + toUInt(StrategyId::MAX));
            _oldQuoteExt = quoteExt;
        }

        auto exchQuoteExt = s_systemState._exchQuoteExternally.load(std::memory_order_relaxed);
        if (exchQuoteExt != _oldExchangeQuoteExt) {
            LOG(0, "{i1} ExchangeQuoteExternal Switch Changed. {i2} -> {i3}", strategyName(), _oldExchangeQuoteExt.to_ulong(), exchQuoteExt.to_ulong())
            underlying().onExchangeQuoteExternalChanged(exchQuoteExt);
            _oldExchangeQuoteExt = exchQuoteExt;
        }
    }

    bool onSystemStateChange(SystemState state) {
        bool canChange = true;
        switch (state) {
            case SystemState::HardShutdown: {
                LOG(0, "{i1} hard shutdown.", strategyName())
                underlying().handleHardShutdown();
                sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Enabled),
                                          proto::SystemState::SystemStateHardShutdown, toUInt(strategyId()));
                break;
            }
            case SystemState::Disabled: {
                LOG(0, "{i1} got disabled.", strategyName())
                underlying().handleDisabled();
                sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Enabled),
                                          proto::SystemState::SystemStateDisabled, toUInt(strategyId()));
                break;
            }
            case SystemState::Enabled: {
                if (underlying().handleEnabled()) {
                    sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Enabled),
                                              proto::SystemState::SystemStateEnabled, toUInt(strategyId()));
                    LOG(0, "{i1} got enabled.", strategyName())
                } else {
                    sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Enabled),
                                              proto::SystemState::SystemStateDisabled, toUInt(strategyId()));
                    LOG(0, "{i1} could NOT be enabled.", strategyName())
                    canChange = false;
                }
                break;
            }
            case SystemState::LoggedOut: {
                LOG(0, "{i1} Logged out.", strategyName())
                underlying().handleLogout();
                break;
            }
            case SystemState::LoggedIn: {
                LOG(0, "{i1} Logged in.", strategyName())
                underlying().handleLogin();
                break;
            }
            case SystemState::CheckQuotes: {
                underlying().handleCheckQuotes();
                break;
            }
            case SystemState::CancelGtcAndDisable: {
                LOG(0, "{i1} Got message disable and cancel GTC. Reason=Requested", strategyName())
                underlying().cancelGtcAndDisable();
                sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Enabled),
                                          proto::SystemState::SystemStateDisabled, toUInt(strategyId()));
                break;
            }
        }
        return canChange;
    }

    void drainInternalMessages() {
#ifdef ENABLE_PERF
        uint64_t begin = 0;
        if (underlying().strategyId() == StrategyId::ArbieSpreader) {
            begin = clockUtil::getCpuTicksStart();
        }
        uint32_t drainSize = 0;
#endif
        while(_msgQueue.end() != _msgCursor) {
            auto& msg = reinterpret_cast<ViperMessage&>(_msgQueue[_msgCursor].data[0]);
            assert(msg._size);
            _msgCursor += msg._size;
            underlying().handleViperMessage(msg);
#ifdef ENABLE_PERF
            ++drainSize;
#endif
        }

#ifdef ENABLE_PERF
        if (drainSize & begin) {
            auto end = clockUtil::getCpuTicksEnd();
            PerfMonitor::get()->addPerfPoint(PerfId::AS_DrainInternalMessage, PerfType::Begin, begin);
            PerfMonitor::get()->addPerfPoint(PerfId::AS_DrainInternalMessage, PerfType::End, end);
            ADD_PERF_VALUE(AS_DrainInternalMessageCount, drainSize);
        }
#endif
    }

    void checkAutoDisable(uint64_t now) {
        if (_autoDisableTimeInFrequency == (uint64_t)-1) {
            return;
        }

        if (BOOST_UNLIKELY(now >= _autoDisableTimeInFrequency)) {
            LOG(0, "{i1} On DisableTime.", toUInt(strategyId()))
            sendErrorMessage(boost::str(boost::format("Disable %1% on AutoDisableTime.") % toString(strategyId())), ErrorType::Normal);
            disable();
            disableQuoteExternal();
            setAutoDisableTime(_autoDisableTime + 240000, now, false);
        }
    }
};

}