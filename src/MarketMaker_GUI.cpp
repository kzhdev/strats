#include "MarketMaker.h"
#include "GUIThings.h"
#include "SimpleMarketMaker.h"

using namespace Python;

namespace {
auto& guiThings = GUIThings::get();
}

void MarketMaker::populateGUIInitialization(::google::protobuf::RepeatedPtrField< ::Python::proto::GridDefinition >* definitions) const {
    guiThings.populateMarketMakerParameters(definitions->Add());
}

void MarketMaker::sendStateToGUI(std::function<void(proto::GridUpdate&)> send) const {
    proto::GridUpdate message;
    guiThings.populateMarketMakerState(message, this);
    send(message);
}

bool MarketMaker::processGridChange(GridUpdate& update) {
    if (update._update.gridid() == toUInt(WindowIds::MarketMakerParameters)) {
        bool valid = true;
        for (const proto::GridCell& cell : update._update.cells()) {
            const uint32_t internalId = cell.row();
            auto& mm = _marketMakers[internalId];
            valid &= mm.validateParametersUpdate(cell);
        }

        if (!valid)
            return valid;

        for (proto::GridCell& cell : *update._update.mutable_cells()) {
            const uint32_t internalId = cell.row();
            auto& mm = _marketMakers[internalId];

            mm.handleParametersUpdate(cell);

            // UpdateGridForChangedMMState
            MarketMakerParametersColumns columnType = static_cast<MarketMakerParametersColumns>(cell.col());
            if(columnType == MarketMakerParametersColumns::Enabled) {
                cell.set_value(std::to_string(mm._enabled));
            }
            else if(columnType == MarketMakerParametersColumns::QuoteExternal) {
                cell.set_value(std::to_string(mm._quoteExternal));
            }
        }
        return true;
    }
    else if (update._update.gridid() == toUInt(WindowIds::GlobalConfigs)) {
        for (const proto::GridCell& cell : update._update.cells()) {
            GlobalConfig& config = _viperConfig._globalConfigs[cell.row()];
            if (config._key == MM_WORSEN_PAYUP_COND) {
                double percent = boost::lexical_cast<double>(cell.value());
                for (size_t i = 0; i < _viperConfig._allContracts.size(); ++i) {
                    auto& mm = _marketMakers[i];
                    if (mm._contract._toMarketMake) {
                        mm.handleWorsenPayupConditionUpdate(percent);
                    }
                }
            }
        }
    }
    return true;
}