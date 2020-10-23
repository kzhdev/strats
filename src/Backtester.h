#pragma once

#include "Executor.h"

namespace Python {

class BackTester final   : public Executor {

public:
    BackTester() = default;

    void onTradeCleanedMDUpdate(const md::MDUpdateEvent &/*event*/, const md::MDBatch &/*batch*/);
    void onMDTradeDetails(const md::MDTradeDetailsEvent &/*event*/, const md::MDBatch &/*batch*/);
    void onEndTradeSummary(const md::MDEndTradeSummary &/*event*/, const md::MDBatch &/*batch*/) {}
    void onMDAddOrder(const md::MDAddOrderEvent &/*event*/, const md::MDBatch &/*batch*/) {}
    void onMDSnapshotOrders(const md::MDSnapshotOrdersEvent &/*event*/, const md::MDBatch &/*batch*/);
    void onMDDeleteOrder(const md::MDDeleteOrderEvent &/*event*/, const md::MDBatch &/*batch*/) {}
    void onMDModifyOrder(const md::MDModifyOrderEvent &/*event*/, const md::MDBatch &/*batch*/) {}
    void onMDModifyOrderWithNewID(const md::MDModifyOrderWithNewIDEvent &/*event*/, const md::MDBatch &/*batch*/) {}
    void onMDLevelChange(const md::MDLevelChangeEvent &/*event*/, const md::MDBatch &/*batch*/);
    void onMDLevelClear(const md::MDLevelClearEvent &/*event*/, const md::MDBatch &/*batch*/) {}
    void onMDLevelDelete(const md::MDLevelDeleteEvent &/*event*/, const md::MDBatch &/*batch*/) {}
    void onMDLevelDeleteFrom(const md::MDLevelDeleteFromEvent &/*event*/, const md::MDBatch &/*batch*/) {}
    void onMDLevelNew(const md::MDLevelNewEvent &/*event*/, const md::MDBatch &/*batch*/);
    void onMDLevelOverlay(const md::MDLevelOverlayEvent &/*event*/, const md::MDBatch &/*batch*/) {}
};

}
