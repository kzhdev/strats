#ifdef VIPER_BACKTEST

#include "Backtester.h"
#include "src/logging/Coolog.h"
#include "src/FrontSide.h"
#include <chrono>

using namespace Python;

void BackTester::onMDAddOrder(const md::MDAddOrderEvent &event, const md::MDBatch &batch) {
    if (!_ready) {
        handleMDAddOrder(event, batch.venue);
    } else {
        pushEvent<md::MDAddOrderEvent>(
                ViperMessageType::MD_Add_Order,
                [&event, venue = toUInt(batch.venue)](md::MDAddOrderEvent& msg) {
                    memcpy(&msg, &event, sizeof(event));
                    msg.size = venue;
                });
    }
}

void BackTester::handleMDAddOrder(const md::MDAddOrderEvent& event, hf::VENUE venue) {
    auto internalId = sidToInternalId(event.sid);
    auto& limitBook = _limitBooks[internalId];
    limitBook.onMDAddOrder(event, venue);
}

void BackTester::onMDDeleteOrder(const md::MDDeleteOrderEvent& event, const md::MDBatch& batch) {
    assert(_ready);
    pushEvent<md::MDDeleteOrderEvent>(
            ViperMessageType::MD_Delete_Order,
            [&event, venue = toUInt(batch.venue)](md::MDDeleteOrderEvent& msg) {
                memcpy(&msg, &event, sizeof(event));
                msg.size = venue;
            });
}

void BackTester::handleMDDeleteOrder(const md::MDDeleteOrderEvent& event, hf::VENUE venue) {
    auto internalId = sidToInternalId(event.sid);
    auto& limitBook = _limitBooks[internalId];
    limitBook.onMDDeleteOrder(event, venue);
}

void BackTester::onMDModifyOrder(const md::MDModifyOrderEvent &event, const md::MDBatch &batch) {
    assert(_ready);
    pushEvent<md::MDDeleteOrderEvent>(
            ViperMessageType::MD_Delete_Order,
            [&event, venue = toUInt(batch.venue)](md::MDDeleteOrderEvent& msg) {
                memcpy(&msg, &event, sizeof(event));
                msg.size = venue;
            });
}

void BackTester::handleMDModifyOrder(const md::MDModifyOrderEvent& event, hf::VENUE venue) {
    auto internalId = sidToInternalId(event.sid);
    auto& limitBook = _limitBooks[internalId];
    limitBook.onMDModifyOrder(event, venue);
}

void BackTester::onMDStatus(const md::MDStatusEvent &event, const md::MDBatch &batch) {
    if (!_ready) {
        Executor::onMDStatus(event, batch);
        if (event.statusType == md::MDStatusEvent::TYPE::RECOVERED) {
            for (auto& state : _contractStates) {
                if (state._contract._toMarketMake && !state._ime._contractCanTrade) {
                    return;
                }
            }

            _ready = true;
            auto _frontSide = reinterpret_cast<FrontSide*>(getResource(ResourceId::FrontSide));

            for (uint8_t i = 1; i < toUInt(hf::VENUE::EUREX); ++i) {
                proto::SwitchChange enableExchExternal;
                auto switchCells = enableExchExternal.mutable_cell();
                switchCells->set_row(GUIThings::get().venueToMainControlRowIndex(i));
                switchCells->set_col(toUInt(MainControlAction::Enabled));
                switchCells->set_value("1");
                _frontSide->onSwitchChange(enableExchExternal);
            }

            _frontSide->onSystemStateChange(SystemState::CheckQuotes, "backtest");

            using namespace std::chrono_literals;
            std::this_thread::sleep_for(1s);

            foreachStrategy([this](ViperStrategyBase* strategy) {
                if (!strategy->isEnabled()) {
                    strategy->enableQuoteExternal();
                    strategy->enable();
                }
            });
        }
    } else {
        pushEvent<md::MDStatusEvent>(
                ViperMessageType::MD_Status,
                [&event, venue = toUInt(batch.venue)](md::MDStatusEvent& msg) {
                    memcpy(&msg, &event, sizeof(event));
                    msg.size = venue;
                });
    }
}

void BackTester::onMDTradeDetails(const md::MDTradeDetailsEvent &event, const md::MDBatch &batch) {
    assert(_ready);
    pushEvent<md::MDTradeDetailsEvent>(
            ViperMessageType::MD_TradeDetails,
            [&event, venue = toUInt(batch.venue)](md::MDTradeDetailsEvent& msg) {
                memcpy(&msg, &event, sizeof(event));
                msg.size = venue;
            });
}

void BackTester::handleMDTradeDetails(const md::MDTradeDetailsEvent& event, hf::VENUE venue) {
    auto internalId = sidToInternalId(event.sid);
    auto& limitBook = _limitBooks[internalId];
    limitBook.onMDTradeDetails(event, venue);
}

#endif