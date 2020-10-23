#include "Backtester.h"
#include "logging/Coolog.h"

using namespace Python;

void BackTester::onMDSnapshotOrders(const md::MDSnapshotOrdersEvent &/*event*/, const md::MDBatch &/*batch*/) {
    LOG(0 "**** onMDSnapshotOrders {i1}", 0);
}

void BackTester::onMDLevelNew(const md::MDLevelNewEvent &/*event*/, const md::MDBatch &/*batch*/) {
    LOG(0 "**** onMDLevelNew {i1}", 0);
}