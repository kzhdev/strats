#pragma once

#include <atomic>
#include <vector>
#include <unordered_map>
#include <hf/Service.h>
#include <hf/ConfigManager.h>
#include "Common.h"
#include "RingBufferQueue.h"
#include "ViperResource.h"
#include "ClockUtil.h"
#include "MarketDataConsumer.h"

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

    virtual void populateGUIInitialization(::google::protobuf::RepeatedPtrField< ::Python::proto::GridDefinition >* definitions) const = 0;
    virtual void sendStateToGUI(std::function<void(proto::GridUpdate&)> send) const = 0;

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

    void disable() noexcept {
        _state.store(SystemState::Disabled, std::memory_order_release);
    }

    bool canQuoteExternally() const noexcept {
        return _quoteExternal.load(std::memory_order_relaxed) == SystemState::Enabled;
    }

    void disableQuoteExternal() noexcept {
        _quoteExternal.store(SystemState::Disabled, std::memory_order_release);
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
    void sendRegisterOrderClientRequest(uint32_t internalId, const std::vector<StrategyId>& subscriptions) const {
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
 *      void populateGUIInitialization()
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
class ViperStrategyBaseImpl : public virtual hf::Service, public ViperStrategyBase, protected MarketDataConsumer<T> {

public:
    void onCreate(const libconfig::Setting& config) override {
        ViperResource::_onCreate(config);
    }

    void onPostCreate(const libconfig::Setting& config) override {
        ViperResource::_onPostCreate(config);
        _nextInternalHeartbeat = 0;
    }

    void onRun() override {
        logging::GetLogger().logInfo(boost::str(boost::format("%1% logger %2%") % toString(strategyId()) % _kdbLogger));
        _kdbLogger->resetW();
        logging::setKdbLogger(_kdbLogger);
        this->initMarkDataQueueState();
        underlying().registerOrderClient();

        LOG(0, "Strategy {i1} thread started.", strategyName())

        while (BOOST_LIKELY(!hf::Service::isExitAll())) {
            checkSystemState();
            drainInternalMessages();
            this->drainAllMarketData();
            underlying().runCycle();
            const uint64_t now = clockUtil::GetCycles();
            sendInternalHeartbeat(now);
        }
    }

    void populateGUIInitialization(::google::protobuf::RepeatedPtrField< ::Python::proto::GridDefinition >* definitions) const override {
        underlying().populateGUIInitialization(definitions);
    }

private:
    friend T;
    ViperStrategyBaseImpl() {}

    T& underlying() noexcept { return static_cast<T&>(*this); }
    T const& underlying() const noexcept { return static_cast<T const&>(*this); }

    void checkSystemState() {
        SystemState state = _state.load(std::memory_order_relaxed);
        if (state != _oldState) {
            onSystemStateChange(state);
            _oldState = state;
        }

        auto quoteExt = _quoteExternal.load(std::memory_order_relaxed);
        if (quoteExt != _oldQuoteExt) {
            LOG(0, "{i1} QuoteExternal Switch Changed. {i2} -> {i3}", strategyName(), toUInt(_oldQuoteExt), toUInt(quoteExt))
            underlying().onQuoteExternalChanged(quoteExt);

            sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Enabled),
                                      (quoteExt == SystemState::Enabled ? proto::SystemState::SystemStateEnabled
                                                                        : proto::SystemState::SystemStateDisabled),
                                      toUInt(strategyId()) + toUInt(StrategyId::Execution));
            _oldQuoteExt = quoteExt;
        }

        auto exchQuoteExt = s_systemState._exchQuoteExternally.load(std::memory_order_relaxed);
        if (exchQuoteExt != _oldExchangeQuoteExt) {
            underlying().onExchangeQuoteExternalChanged(exchQuoteExt);
            _oldExchangeQuoteExt = exchQuoteExt;
        }
    }

    void onSystemStateChange(SystemState state) {
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
                }
                break;
            }
            case SystemState::LoggedOut: {
                LOG(0, "{i1} Logged out.", strategyName())
                underlying().handleLogout();
                break;
            }
            case SystemState::LoggedIn: {
                LOG(0, "{i1} logged in.", strategyName())
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
    }

    void drainInternalMessages() {
        uint32_t drainSize = 0;
        while(_msgQueue.end() != _msgCursor) {
            auto& msg = reinterpret_cast<ViperMessage&>(_msgQueue[_msgCursor].data[0]);
            _msgCursor += msg._size;
            underlying().handleViperMessage(msg);
            ++drainSize;
        }

        if (drainSize) {
            DLOG(0, "{i1} msgQueue drain size. {i2}", strategyName(), drainSize)
        }
    }
};

}