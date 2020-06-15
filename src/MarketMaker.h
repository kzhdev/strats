#pragma once

#include <md/md_event.h>
#include <md/queue/MDQueue.h>
#include "Common.h"
#include "ViperConfig.h"
#include "ClockUtil.h"
#include "ViperStrategyBase.h"
#include "MarketDataConsumer.h"

namespace Python {

class SimpleMarketMaker;

class MarketMaker : public ViperStrategyBaseImpl<MarketMaker> {
    friend class SimpleMarketMaker;
    friend class ViperStrategyBaseImpl<MarketMaker>;
    std::set<uint32_t> _updatedContracts;
    uint8_t _oldMDStatus[static_cast<uint8_t>(hf::VENUE::TEST)] = {0};

public:
    SimpleMarketMaker* _marketMakers;

    MarketMaker() {
    }

    ResourceId id() const noexcept override { return ResourceId::MarketMaker; }
    StrategyId strategyId() const noexcept override { return StrategyId::MarketMaker; }

    void init(double frequency, uint64_t internalHeartbeatPeriod, std::function<bool(uint32_t)> pnlLimitsChecker) override;
    void registerOrderClient();

    void runCycle() {
        for (uint32_t id : _updatedContracts) {
            onMarketDataChange(id);
        }
        _updatedContracts.clear();
    }

    void onMDBatch(const md::MDBatch &batch);
    void handleViperMessage(const ViperMessage& msg);

    void disableMMExternalQuoting(uint32_t id);

    void notifyFrontMMExternalQuoteDisabled(uint32_t id) {
        sendGenericMessageToFront(ViperMessageType::DisableExternalQuotingOnMarketMaker, id);
    }

    // GUI update
    void populateGUIInitialization(::google::protobuf::RepeatedPtrField< ::Python::proto::GridDefinition >* definitions) const override;
    void sendStateToGUI(std::function<void(proto::GridUpdate&)> sendingFunc) const override;

    // Fill processing
    uint32_t beginFillProcess(uint32_t internalId);
    hf::qty_t allocateExternalMMHedgerFill(uint32_t internalId, hf::price_t filledPrice, hf::qty_t filledQty, uint32_t side);
    void onFill(uint32_t internalId, Order* order, hf::price_t price, hf::qty_t size, bool isPayUp, FillType fillType);
    hf::qty_t takeOverFill(uint32_t internalId, uint32_t side, hf::price_t price, hf::qty_t size);
    void processManualFill(const ManualFill& fill);
    bool forceTakeOverFill(uint32_t internalId, uint32_t side, hf::price_t price, hf::qty_t size, FillSource source);

private:
    // MD handing
    void onMDUpdate(hf::VENUE venue, const md::MDUpdateEvent& event, uint_fast64_t sequenceNumber);
    void onMDTrade(hf::VENUE venue, const md::MDTradeEvent& event);
    void onMDStatus(hf::VENUE venue, const md::MDStatusEvent& event);

    static void copyBooks(SimpleMarketMaker& mm,
                          hf::price_t bidPrice,
                          hf::qty_t bidSize,
                          md::order_count_t bidOrderCount,
                          hf::price_t askPrice,
                          hf::qty_t askSize,
                          md::order_count_t askOrderCount,
                          bool bidChanged,
                          bool askChanged);

    static void copyBook(Book& book, hf::price_t price, hf::qty_t size, md::order_count_t orderCount);

    void handleCMEMDStatus(uint32_t internalId, const md::MDStatusEvent& event, SimpleMarketMaker& state);
    void handleICEMDStatus(uint32_t internalId, const md::MDStatusEvent& event, SimpleMarketMaker& state);

    void onMarketDataChange(uint32_t internalId);

private:
    // System State handling
    void handleHardShutdown() {
        LOG(0, "MM hard shutdown. {i1}", 0);
        sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Enabled), proto::SystemState::SystemStateHardShutdown);
    }

    void handleCheckQuotes() {}

    void handleDisabled() {
        LOG(0, "MM got disabled. {i1}", 0);
        disableMarketMakers();
        sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Enabled), proto::SystemState::SystemStateDisabled);
    }

    void handleEnabled() {
        if (canEnable()) {
            enableMarketMakers();
            sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Enabled), proto::SystemState::SystemStateEnabled, toInt(StrategyId::MarketMaker));
            LOG(0, "MM got enabled {i1}", 0)
        } else {
            _state.store(SystemState::Disabled, std::memory_order_release);
            sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Enabled), proto::SystemState::SystemStateDisabled, toInt(StrategyId::MarketMaker));
            LOG(0, "MM could NOT be enabled {i1}", 0)
        }
    }

    void handleLogin() {
        LOG(0, "MM logged in. {i1}", 0);
    }

    void handleLogout() {
        LOG(0, "MM Logged out {i1}", 0);
    }

    void cancelGtcAndDisable() {
        LOG(0, "MM Got message disable and cancel GTC. Reason=Requested {i1}", 0)
        disableMarketMakers();
        sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Enabled), proto::SystemState::SystemStateDisabled);
    }

    void onQuoteExternalChanged(const SharedSystemState::QuoteExtBitset& quoteExt);

private:
    bool isRVEnabled() const noexcept {
        return _strategies[toUInt(StrategyId::RelativeValue)]->isEnabled();
    }

    void disableMarketMakers();
    void disableMarketMaker(uint32_t internalId);

    bool canEnable() const noexcept {
        bool canEnable = true;
        if (!isPnlWithinLimits()) {
            canEnable = false;
        }

        if (!pricesAndQuantitiesValid()) {
            canEnable = false;
        }
        return canEnable;
    }

    bool isPnlWithinLimits() const noexcept {
        const bool withinLimits = _pnlLimitsChecker(toUInt(StrategyId::MarketMaker));
        if (!withinLimits) {
            sendErrorMessage("MM - not within pnl limits.",  ErrorType::Critical);
            LOG(0, "MM - is not within pnl limits {i1}", 0)
        }
        return withinLimits;
    }

    bool pricesAndQuantitiesValid() const noexcept;
    void enableMarketMakers();

    void sendFillToFront(uint32_t contractIndex,
                         uint32_t side,
                         hf::price_t price,
                         hf::qty_t size,
                         FillType type,
                         FillSource source,
                         hf::qty_t internalAllocation,
                         hf::qty_t externalAllocation) const {
        sendMessageToFront<FillMessage>(ViperMessageType::Fill, [=](FillMessage& message) {
            message._contractId = contractIndex;
            message._side = side;
            message._qty = size;
            message._strategy = StrategyId::MarketMaker;
            message._type = type;
            message._price = price;
            message._source = source;
            message._internalAllocation = internalAllocation;
            message._externalAllocation = externalAllocation;
            message._rvStf = 0;
        });
    }

    bool processGridChange(GridUpdate& update);
    bool onManualFills(GridUpdate& update);
    void sendAcceptedManualFillToFront(uint32_t internalId, hf::qty_t size, bool isInternal);
    void onShiftUpdate(uint32_t shift);
};

}