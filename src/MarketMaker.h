#pragma once

#include <md/md_event.h>
#include <md/queue/MDQueue.h>
#include "Common.h"
#include "ViperConfig.h"
#include "ClockUtil.h"
#include "ViperStrategyBase.h"
#include "MarketDataConsumer.h"
#include <unordered_set>

namespace Python {

class SimpleMarketMaker;

class MarketMaker final : public ViperStrategyBaseImpl<MarketMaker> {
    friend class SimpleMarketMaker;
    friend class ViperStrategyBaseImpl<MarketMaker>;
    friend class MarketDataConsumer<MarketMaker>;
    std::unordered_set<uint32_t> _updatedContracts;
    uint8_t _oldMDStatus[static_cast<uint8_t>(hf::VENUE::ANY)] = {0};
    std::unordered_set<SimpleMarketMaker*> _manualFilledMarketMakers;

public:
    SimpleMarketMaker* _marketMakers;

    MarketMaker() {
    }

    ResourceId id() const noexcept override { return ResourceId::MarketMaker; }
    StrategyId strategyId() const noexcept override { return StrategyId::MarketMaker; }

    void init(double frequency, uint64_t internalHeartbeatPeriod, std::function<bool(uint32_t)> pnlLimitsChecker) override;
    void registerOrderClient();

    void runCycle(uint64_t now) {
        for (uint32_t id : _updatedContracts) {
            onMarketDataChange(id);
        }
        _updatedContracts.clear();
    }

    void handleViperMessage(const ViperMessage& msg);

    void disableMMExternalQuoting(uint32_t id);

    void notifyFrontMMExternalQuoteDisabled(uint32_t id) {
        sendGenericMessageToFront(ViperMessageType::QuoteExternallyOff, id, toUInt(WindowIds::MarketMakerParameters),
                                  toUInt(MarketMakerParametersColumns::QuoteExternal));
    }

    void notifyFrontMMExternalQuoteEnabled(uint32_t id) {
        sendGenericMessageToFront(ViperMessageType::QuoteExternallyOn, id, toUInt(WindowIds::MarketMakerParameters),
                                  toUInt(MarketMakerParametersColumns::QuoteExternal));
    }

    // GUI update
    void populateGUIInitialization(::google::protobuf::RepeatedPtrField< ::Python::proto::GridDefinition >* definitions) override;
    void sendStateToGUI(std::function<void(proto::GridUpdate&, WindowIds)>&& sendingFunc, std::function<bool(WindowIds)>&& hasSubscription) const override;
    proto::GridUpdate getStateUpdate() const;

    bool isContractQuoteExternal(uint32_t contractId) const noexcept override;

private:
    void populateStateParameterCells(::google::protobuf::RepeatedPtrField< ::Python::proto::GridCell >* cells,
                                     uint32_t internalId, const SimpleMarketMaker& mm) const;
    // Fill processing
    void onFill(const FillMessage& fill);

    // MD handing
    void onMDUpdate(const md::MDUpdateEvent& event, uint32_t internalId, hf::batch_id_t bid);
    void onMDTrade(const md::MDBatch& batch, const md::MDTradeEvent& event, uint32_t internalId);
    void onMDStatus(hf::VENUE venue, uint64_t extSeqNum, const md::MDStatusEvent& event, uint32_t internalId, hf::batch_id_t bid);
    void handleUncommonMDEvent(const md::MDEvent& event, const md::MDBatch& batch, uint32_t internalId) {}
    void onEndMDBatch(hf::batch_id_t bid);

    void handle_CME_B3_MDStatus(uint32_t internalId, hf::VENUE venue, const md::MDStatusEvent& event, SimpleMarketMaker& state);
    void handleICEMDStatus(uint32_t internalId, const md::MDStatusEvent& event, SimpleMarketMaker& state);
    void handleEUREXMDStatus(uint32_t internalId, const md::MDStatusEvent& event, SimpleMarketMaker& state);

    void onMarketDataChange(uint32_t internalId);
    void passPosToRV(uint32_t internalId);

private:
    // System State handling
    void handleHardShutdown() {}
    void handleCheckQuotes() {}
    void handleLogin() {}
    void handleLogout() {}

    void handleDisabled() {
        disableMarketMakers();
    }

    bool handleEnabled() {
        if (canEnable()) {
            enableMarketMakers();
            return true;
        }
        return false;
    }

    void cancelGtcAndDisable() {
        disableMarketMakers();
    }

    void onQuoteExternalChanged(SystemState quoteExt);
    void onExchangeQuoteExternalChanged(const SharedSystemState::QuoteExtBitset& quoteExt);

private:
    bool anyStrategyEnabled() const noexcept {
        bool anyEnabled = false;
        for (auto strategy : _strategies) {
            anyEnabled |= strategy->isEnabled();
            if (anyEnabled) {
                return true;
            }
        }
        return false;
    }

    void disableMarketMakers();
    void disableMarketMaker(uint32_t internalId, bool disableGTC = false);

    bool canEnable() const noexcept {
        return pricesAndQuantitiesValid();
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
        });
    }

    bool processGridChange(GridUpdate& update);
    COLD void onManualFill(ManualFill& fii);
    COLD void onEndOfManualFills();
    COLD void sendAcceptedManualFillToFront(uint32_t internalId, hf::qty_t size, hf::price_t price, bool isInternal);
    void onShiftUpdate(uint32_t shift);
    void onOrderUpdate(const OrderUpdate& msg);
    void onQueuePositionUpdate(const QueuePositionUpdate& msg);
};

}