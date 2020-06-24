#include "RelativeValue.h"
#include "GUIThings.h"

using namespace Python;

namespace {
GUIThings& guiThings = GUIThings::get();
}

void RelativeValue::populateGUIInitialization(::google::protobuf::RepeatedPtrField< ::Python::proto::GridDefinition >* definitions) const {
    guiThings.populateContractParameterRVColumns(definitions->Add(), _extraContractWidths);
    guiThings.populatePricingMatrix(definitions->Add(), _pricingMatrix);
    guiThings.populateRiskMatrix(definitions->Add(), _riskMatrix);
    guiThings.populateHedgeRatios(definitions->Add(), _hedgeRatios);
    guiThings.populateFactorHedged(definitions->Add(), _factorHedgedIndexes);
    guiThings.populateQuoteSizes(definitions->Add(), _quoteSizes);
    guiThings.populateCommissions(definitions->Add(), _commissions);
}

void RelativeValue::sendStateToGUI(std::function<void(proto::GridUpdate&)> send) const {
    proto::GridUpdate message;
    bool checkForCross = (state() == SystemState::CheckQuotes);
    guiThings.populateRelativeValueGridUpdate(message, this, checkForCross);
    send(message);

    sendVectorGridStateToGui(send);
}

void RelativeValue::sendVectorGridStateToGui(std::function<void(proto::GridUpdate&)>& send) const {
    proto::GridUpdate message;
    guiThings.populateVectorGridUpdate(message, this);
    send(message);
}