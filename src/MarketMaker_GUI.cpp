#include "MarketMaker.h"
#include "GUIThings.h"

using namespace Python;

namespace {
auto& guiThings = GUIThings::get();
}

void MarketMaker::populateGUIInitialization(::google::protobuf::RepeatedPtrField< ::Python::proto::GridDefinition >* definitions) const {
    guiThings.populateMarketMakerParameters(definitions->Add());
}

void MarketMaker::sendStateToGUI(std::function<void(proto::GridUpdate&)>&& send) const {
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

        std::unordered_set<SimpleMarketMaker*> mmNeedApplyUpdate;
        std::unordered_map<proto::GridCell*, SimpleMarketMaker*> mmHasEnabledChange;
        std::unordered_map<proto::GridCell*, SimpleMarketMaker*> mmHasQuoteExchangeChange;
        for (proto::GridCell& cell : *update._update.mutable_cells()) {
            const uint32_t internalId = cell.row();
            auto& mm = _marketMakers[internalId];

            if (BOOST_LIKELY(mm.handleParametersUpdate(cell))) {
                mmNeedApplyUpdate.emplace(&mm);
            }

            // UpdateGridForChangedMMState
            MarketMakerParametersColumns columnType = static_cast<MarketMakerParametersColumns>(cell.col());
            if(columnType == MarketMakerParametersColumns::Enabled) {
                mmHasEnabledChange.emplace(&cell, &mm);
            }
            else if(columnType == MarketMakerParametersColumns::QuoteExternal) {
                mmHasQuoteExchangeChange.emplace(&cell, &mm);
            }
        }

        for (auto it = mmNeedApplyUpdate.begin(); it != mmNeedApplyUpdate.end(); ++it) {
            (*it)->applyUpdates();
        }

        for (auto& kvp : mmHasEnabledChange) {
            bool enabled = 1 == atoi(kvp.first->value().c_str());
            kvp.second->handleEnableChange(enabled);
            kvp.first->set_value(std::to_string(kvp.second->_enabled));
        }

        for (auto& kvp : mmHasQuoteExchangeChange) {
            kvp.first->set_value(std::to_string(kvp.second->_quoteExternal));
        }

        return true;
    } else if (update._update.gridid() == toUInt(WindowIds::ManualFillsEntry)) {
        onManualFills(update);
    }
    return true;
}