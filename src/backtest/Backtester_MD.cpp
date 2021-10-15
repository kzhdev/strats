#ifdef VIPER_BACKTEST

#include "Backtester.h"
#include "src/logging/Coolog.h"
#include "src/FrontSide.h"
#include <chrono>

using namespace Python;

void BackTester::onMDAddOrder(const md::MDAddOrderEvent &event, const md::MDBatch &batch) {
    if (event.oid == 8031902790291) {
        LOG(0, "**** Add order {i1}", event.oid)
    }
    if (!_ready) {
        handleMDAddOrder(event, batch.venue);
    } else {
        pushEvent<md::MDAddOrderEvent>(
                ViperMessageType::MD_Add_Order,
                [&event, &batch](ViperMessage& viperMsg, md::MDAddOrderEvent& msg) {
                    viperMsg._timestamp = batch.time.srcTS;
                    memcpy(&msg, &event, event.size);
                    viperMsg._venue = batch.venue;
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
            [&event, &batch](ViperMessage& viperMsg, md::MDDeleteOrderEvent& msg) {
                viperMsg._timestamp = batch.time.srcTS;
                memcpy(&msg, &event, event.size);
                viperMsg._venue = batch.venue;
            });
}

void BackTester::handleMDDeleteOrder(const md::MDDeleteOrderEvent& event, hf::VENUE venue) {
    auto internalId = sidToInternalId(event.sid);
    auto& limitBook = _limitBooks[internalId];
    limitBook.onMDDeleteOrder(event, venue);
}

void BackTester::onMDModifyOrder(const md::MDModifyOrderEvent &event, const md::MDBatch &batch) {
    if (event.oid == 8031902761178) {
        LOG(0, "**** modify order {i1}", event.oid)
    }
    assert(_ready);
    pushEvent<md::MDModifyOrderEvent>(
            ViperMessageType::MD_Modify_Order,
            [&event, &batch](ViperMessage& viperMsg, md::MDModifyOrderEvent& msg) {
                viperMsg._timestamp = batch.time.srcTS;
                memcpy(&msg, &event, event.size);
                viperMsg._venue = batch.venue;
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
                [&event, &batch](ViperMessage& viperMsg, md::MDStatusEvent& msg) {
                    viperMsg._timestamp = batch.time.srcTS;
                    memcpy(&msg, &event, event.size);
                    viperMsg._venue = batch.venue;
                });
    }
}

void BackTester::onMDTradeDetails(const md::MDTradeDetailsEvent &event, const md::MDBatch &batch) {
    assert(_ready);
    pushEvent<md::MDTradeDetailsEvent>(
            ViperMessageType::MD_TradeDetails,
            [&event, &batch](ViperMessage& viperMsg, md::MDTradeDetailsEvent& msg) {
                viperMsg._timestamp = batch.time.srcTS;
                memcpy(&msg, &event, event.size);
                viperMsg._venue = batch.venue;
            }, event.numberOfOrders * sizeof(md::MDTradeDetailsEvent::trade_orders_t));
}

void BackTester::handleMDTradeDetails(const md::MDTradeDetailsEvent& event, hf::VENUE venue) {
    auto internalId = sidToInternalId(event.sid);
    auto& limitBook = _limitBooks[internalId];
    limitBook.onMDTradeDetails(event, venue);
}

void BackTester::onMDUpdate(const md::MDUpdateEvent& event, const md::MDBatch &batch) {
    if (!_ready) {
        return;
    }

    pushEvent<md::MDUpdateEvent>(
            ViperMessageType::MD_UpdateToLimitBook,
            [&event, &batch](ViperMessage& viperMsg, md::MDUpdateEvent& msg) {
                viperMsg._timestamp = batch.time.srcTS;
                memcpy(&msg, &event, event.size);
                viperMsg._venue = batch.venue;
            });
}

void BackTester::handleMDUpdate(const md::MDUpdateEvent& event, hf::VENUE venue) {
    auto internalId = sidToInternalId(event.sid);
    auto& limitBook = _limitBooks[internalId];
    limitBook.onMDUpdate(event, venue);
}

#endif