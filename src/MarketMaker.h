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

class MarketMaker : public ViperStrategyBaseImpl<MarketMaker> {
    friend class SimpleMarketMaker;
    friend class ViperStrategyBaseImpl<MarketMaker>;
    friend class MarketDataConsumer<MarketMaker>;
    std::set<uint32_t> _updatedContracts;
    uint8_t _oldMDStatus[static_cast<uint8_t>(hf::VENUE::TEST)] = {0};
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

    // GUI update
    void populateGUIInitialization(::google::protobuf::RepeatedPtrField< ::Python::proto::GridDefinition >* definitions) const override;
    void sendStateToGUI(std::function<void(proto::GridUpdate&)>&& sendingFunc) const override;

private:
    // Fill processing
    void onFill(const FillMessage& fill);

    // MD handing
    void onMDUpdate(const md::MDUpdateEvent& event, uint32_t internalId);
    void onMDTrade(hf::VENUE venue, const md::MDTradeEvent& event);
    void onMDStatus(hf::VENUE venue, uint64_t extSeqNum, const md::MDStatusEvent& event, uint32_t internalId);
    void handleUncommonMDEvent(const md::MDEvent& event, const md::MDBatch& batch) {};
    void onEndMDBatch() {}

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
    void handleEUREXMDStatus(uint32_t internalId, const md::MDStatusEvent& event, SimpleMarketMaker& state);

    void onMarketDataChange(uint32_t internalId);

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
    bool isRVEnabled() const noexcept {
        return _strategies[toUInt(StrategyId::RelativeValue)]->isEnabled();
    }

    void disableMarketMakers();
    void disableMarketMaker(uint32_t internalId);

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
            message._rvStf = 0;
        });
    }

    bool processGridChange(GridUpdate& update);
    void onManualFill(ManualFill& fii);
    void onEndOfManualFills();
    void sendAcceptedManualFillToFront(uint32_t internalId, hf::qty_t size, bool isInternal);
    void onShiftUpdate(uint32_t shift);
};

}