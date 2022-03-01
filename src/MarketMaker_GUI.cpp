#include "MarketMaker.h"
#include "GUIThings.h"

using namespace Python;

namespace {
auto& guiThings = GUIThings::get();
}

void MarketMaker::populateGUIInitialization(::google::protobuf::RepeatedPtrField< ::Python::proto::GridDefinition >* definitions) {
    auto [definition, rowHeader, cells] = guiThings.addWindowWithSubmitButton(definitions, WindowIds::MarketMakerParameters, "Market Maker Parameters", proto::GridType::Table);
    guiThings.addHeader(definition, {
            headerCell(MarketMakerParametersColumns::QuoteExternal, "QuoteExternal", false, proto::DataType::bool_type),
            headerCell(MarketMakerParametersColumns::QuoteSize, "QuoteSize", false, proto::DataType::unsigned_type),
            headerCell(MarketMakerParametersColumns::QuoteGap, "QuoteGap", false, proto::DataType::unsigned_type),
            headerCell(MarketMakerParametersColumns::QuotePctTob, "Quote%TOB", false, proto::DataType::double_type),
            headerCell(MarketMakerParametersColumns::MaxTicksWideStopExtQuoting, "MaxTicksWideStopExtQuoting", false, proto::DataType::signed_type),
            headerCell(MarketMakerParametersColumns::MaxTicksWideDisable, "MaxTicksWideDisable", false, proto::DataType::signed_type),
            headerCell(MarketMakerParametersColumns::MaxPayUpTicks, "MaxPayUpTicks", false, proto::DataType::signed_type),
            headerCell(MarketMakerParametersColumns::Width, "WidthPercentage", true, proto::DataType::double_type),
            headerCell(MarketMakerParametersColumns::AsiaWidth, "AsiaWidthPercentage", false, proto::DataType::double_type),
            headerCell(MarketMakerParametersColumns::EuroWidth, "EuroWidthPercentage", false, proto::DataType::double_type),
            headerCell(MarketMakerParametersColumns::USWidth, "USWidthPercentage", false, proto::DataType::double_type),
            headerCell(MarketMakerParametersColumns::SafeModeWidth, "SafeModeWidthPercentage", false, proto::DataType::double_type),
            headerCell(MarketMakerParametersColumns::MinInternalPriceChange, "MinInternalPriceChange", false, proto::DataType::double_type),
            headerCell(MarketMakerParametersColumns::EstablishedLevel, "EstLevelEMA", true, proto::DataType::unsigned_type),
            headerCell(MarketMakerParametersColumns::EstLevelEMASeconds, "EstLevelEMASeconds", false, proto::DataType::unsigned_type),
            headerCell(MarketMakerParametersColumns::EstLevelPctEMA, "EstLevel%EMA", false, proto::DataType::double_type),
            headerCell(MarketMakerParametersColumns::HedgeOffset, "HedgeOffset", false, proto::DataType::double_type),
            headerCell(MarketMakerParametersColumns::JoinSize, "JoinSize", false, proto::DataType::unsigned_type),
            headerCell(MarketMakerParametersColumns::LeaveSize, "LeaveSize", false, proto::DataType::unsigned_type),
            headerCell(MarketMakerParametersColumns::PayUpQty, "PayUpQty", false, proto::DataType::unsigned_type),
            headerCell(MarketMakerParametersColumns::PayUpOrders, "PayUpOrders", false, proto::DataType::unsigned_type),
            headerCell(MarketMakerParametersColumns::PayUpMultiple, "PayUpMultiple", false, proto::DataType::double_type),
            headerCell(MarketMakerParametersColumns::MaxPosInterrnal, "MaxPosInternal", false, proto::DataType::unsigned_type),
            headerCell(MarketMakerParametersColumns::MaxPosExternal, "MaxPosExternal", false, proto::DataType::unsigned_type),
            headerCell(MarketMakerParametersColumns::Commission, "Commission", false, proto::DataType::double_type),
            headerCell(MarketMakerParametersColumns::Enabled, "Enabled", false, proto::DataType::bool_type),
            headerCell(MarketMakerParametersColumns::InternalPosition, "InternalPosition", true, proto::DataType::signed_type),
            headerCell(MarketMakerParametersColumns::ExternalPosition, "ExternalPosition", true, proto::DataType::signed_type),
            headerCell(MarketMakerParametersColumns::BidInternalPrice, "BidIntPrice", true, proto::DataType::rhone_price),
            headerCell(MarketMakerParametersColumns::AskInternalPrice, "AskIntPrice", true, proto::DataType::rhone_price),
            headerCell(MarketMakerParametersColumns::BidInternalQty, "BidIntQty", true, proto::DataType::unsigned_type),
            headerCell(MarketMakerParametersColumns::AskInternalQty, "AskIntQty", true, proto::DataType::unsigned_type),
            headerCell(MarketMakerParametersColumns::BidExternalPrice, "BidExtPrice", true, proto::DataType::rhone_price),
            headerCell(MarketMakerParametersColumns::AskExternalPrice, "AskExtPrice", true, proto::DataType::rhone_price),
            headerCell(MarketMakerParametersColumns::BidExternalQty, "BidExtQty", true, proto::DataType::unsigned_type),
            headerCell(MarketMakerParametersColumns::AskExternalQty, "AskExtQty", true, proto::DataType::unsigned_type),
            headerCell(MarketMakerParametersColumns::MidPoint, "MidPoint", true, proto::DataType::rhone_price),
            headerCell(MarketMakerParametersColumns::BidHedgeInternalPrice, "BidHedgeIntPrice", true, proto::DataType::rhone_price),
            headerCell(MarketMakerParametersColumns::BidHedgeExternalPrice, "BidHedgeExtPrice", true, proto::DataType::rhone_price),
            headerCell(MarketMakerParametersColumns::BidHedgeSize, "BidHedgeSize", true, proto::DataType::unsigned_type),
            headerCell(MarketMakerParametersColumns::AskHedgeInternalPrice, "AskHedgeIntPrice", true, proto::DataType::rhone_price),
            headerCell(MarketMakerParametersColumns::AskHedgeExternalPrice, "AskHedgeExtPrice", true, proto::DataType::rhone_price),
            headerCell(MarketMakerParametersColumns::AskHedgeSize, "AskHedgeSize", true, proto::DataType::unsigned_type),
            headerCell(MarketMakerParametersColumns::WorsenPayupCondPercent, "WorsenPayupCond%", false, proto::DataType::double_type),
            headerCell(MarketMakerParametersColumns::QueueImportance, "QueueImportance", false, proto::DataType::enum_type, "queue_importance"),
            headerCell(MarketMakerParametersColumns::TransferPositionToRV, "PassPosToRV", false, proto::DataType::button, ""),
    });

    for (uint32_t contractIndex = 0; contractIndex < _viperConfig._marketMakerConfigs.size(); ++contractIndex) {
        MarketMakerConfig& config = _viperConfig._marketMakerConfigs.at(contractIndex);
        uint32_t internalId = _viperConfig.getInternalId(config._contract);
        guiThings.addRowHeaderCell(WindowIds::MarketMakerParameters, rowHeader,internalId, config._contract);
        guiThings.addCells(cells, internalId, {
                Cell(MarketMakerParametersColumns::Enabled, std::to_string(config._enabled)),
                Cell(MarketMakerParametersColumns::QuoteExternal, std::to_string(config._quoteExternal)),
                Cell(MarketMakerParametersColumns::QuoteSize, std::to_string(config._quoteSize)),
                Cell(MarketMakerParametersColumns::QuoteGap, std::to_string(config._quoteGap)),
                Cell(MarketMakerParametersColumns::QuotePctTob, std::to_string(config._quotePctTob)),
                Cell(MarketMakerParametersColumns::MaxTicksWideStopExtQuoting, std::to_string(config._maxTicksWideStopExtQuoting)),
                Cell(MarketMakerParametersColumns::MaxTicksWideDisable, std::to_string(config._maxTicksWideDisable)),
                Cell(MarketMakerParametersColumns::MaxPayUpTicks, std::to_string(config._maxPayUpTicks)),
                Cell(MarketMakerParametersColumns::Width, std::to_string(config._widthPercentage[toUInt(Shifts::__current_marker__)])),

        });
        for (uint32_t i = 0; i < toUInt(Shifts::__current_marker__); ++i) {
            guiThings.addCells(cells, internalId, {
                    Cell((MarketMakerParametersColumns) (toUInt(MarketMakerParametersColumns::Width) + i + 1),
                            std::to_string(config._widthPercentage[i]))
            });
        }
        guiThings.addCells(cells, internalId, {
                Cell(MarketMakerParametersColumns::MinInternalPriceChange, std::to_string(config._minInternalPriceChange)),
                Cell(MarketMakerParametersColumns::EstLevelEMASeconds, std::to_string(config._estLevelEMASeconds)),
                Cell(MarketMakerParametersColumns::EstLevelPctEMA, std::to_string(config._estLevelPctEMA)),
                Cell(MarketMakerParametersColumns::HedgeOffset, std::to_string(config._hedgeOffset)),
                Cell(MarketMakerParametersColumns::JoinSize, std::to_string(config._joinSize)),
                Cell(MarketMakerParametersColumns::LeaveSize, std::to_string(config._leaveSize)),
                Cell(MarketMakerParametersColumns::PayUpQty, std::to_string(config._payUpQty)),
                Cell(MarketMakerParametersColumns::PayUpOrders, std::to_string(config._payUpOrders)),
                Cell(MarketMakerParametersColumns::PayUpMultiple, std::to_string(config._payUpMultiple)),
                Cell(MarketMakerParametersColumns::MaxPosInterrnal, std::to_string(config._maxPosInternal)),
                Cell(MarketMakerParametersColumns::MaxPosExternal, std::to_string(config._maxPosExternal)),
                Cell(MarketMakerParametersColumns::Commission, std::to_string(config._commission)),
                Cell(MarketMakerParametersColumns::WorsenPayupCondPercent, std::to_string(config._worsenPayupCondPercent)),
                Cell(MarketMakerParametersColumns::QueueImportance, std::to_string(config._queueImportance)),
                Cell(MarketMakerParametersColumns::TransferPositionToRV, "PassPosToRV", toUInt(Color::TRANSPARENT)),
        });
        populateStateParameterCells(cells, internalId, _marketMakers[internalId]);
    }
}

void MarketMaker::populateStateParameterCells(::google::protobuf::RepeatedPtrField<::Python::proto::GridCell>* cells, uint32_t internalId, const SimpleMarketMaker& mm) const {
    guiThings.setCell(cells, internalId, MarketMakerParametersColumns::MidPoint, std::to_string(mm._midPoint));
    guiThings.setCell(cells, internalId, MarketMakerParametersColumns::InternalPosition, std::to_string(mm._internallyInitiatedPosition));
    guiThings.setCell(cells, internalId, MarketMakerParametersColumns::ExternalPosition, std::to_string(mm._externallyInitiatedPosition));
    auto& bidQuote = mm._quotes[BID];
    guiThings.setCell(cells, internalId, MarketMakerParametersColumns::BidInternalPrice, std::to_string(bidQuote._internalPrice));
    guiThings.setCell(cells, internalId, MarketMakerParametersColumns::BidInternalQty, std::to_string(bidQuote._internalSize + bidQuote._externalSize));
    guiThings.setCell(cells, internalId, MarketMakerParametersColumns::BidExternalPrice, std::to_string(bidQuote._externalPrice));
    guiThings.setCell(cells, internalId, MarketMakerParametersColumns::BidExternalQty, std::to_string(bidQuote._externalSize));
    auto& askQuote = mm._quotes[ASK];
    guiThings.setCell(cells, internalId, MarketMakerParametersColumns::AskInternalPrice, std::to_string(askQuote._internalPrice));
    guiThings.setCell(cells, internalId, MarketMakerParametersColumns::AskInternalQty, std::to_string(askQuote._internalSize + askQuote._externalSize));
    guiThings.setCell(cells, internalId, MarketMakerParametersColumns::AskExternalPrice, std::to_string(askQuote._externalPrice));
    guiThings.setCell(cells, internalId, MarketMakerParametersColumns::AskExternalQty, std::to_string(askQuote._externalSize));
    auto& bidHedge = mm._hedges[BID];
    guiThings.setCell(cells, internalId, MarketMakerParametersColumns::BidHedgeInternalPrice, std::to_string(bidHedge._internalPrice));
    guiThings.setCell(cells, internalId, MarketMakerParametersColumns::BidHedgeExternalPrice, std::to_string(bidHedge._externalPrice));
    guiThings.setCell(cells, internalId, MarketMakerParametersColumns::BidHedgeSize, std::to_string(bidHedge._sizeToQuote));
    auto& askHedge = mm._hedges[ASK];
    guiThings.setCell(cells, internalId, MarketMakerParametersColumns::AskHedgeInternalPrice, std::to_string(askHedge._internalPrice));
    guiThings.setCell(cells, internalId, MarketMakerParametersColumns::AskHedgeExternalPrice, std::to_string(askHedge._externalPrice));
    guiThings.setCell(cells, internalId, MarketMakerParametersColumns::AskHedgeSize, std::to_string(askHedge._sizeToQuote));
    _viperConfig._marketMakerConfigs[mm._contract._marketMakeIndex]._establishedLevel = mm._emaEstLevel;
    guiThings.setCell(cells, internalId, MarketMakerParametersColumns::EstablishedLevel, std::to_string(_viperConfig._marketMakerConfigs[mm._contract._marketMakeIndex]._establishedLevel));
}

void MarketMaker::sendStateToGUI(std::function<void(proto::GridUpdate&, WindowIds)>&& send, std::function<bool(WindowIds)>&& hasSubscription) const {
    if (hasSubscription(WindowIds::MarketMakerParameters)) {
        proto::GridUpdate message = getStateUpdate();
        send(message, WindowIds::MarketMakerParameters);
    }
}

proto::GridUpdate MarketMaker::getStateUpdate() const {
    proto::GridUpdate message;
    message.set_gridid(toUInt(WindowIds::MarketMakerParameters));
    message.set_updatetype(proto::GridUpdateType::CellsType);
    auto cells = message.mutable_cells();
    for (uint32_t contractIndex = 0; contractIndex < _viperConfig._allContracts.size(); ++contractIndex) {
        const auto& mm = _marketMakers[contractIndex];
        if (mm._contract._toMarketMake) {
            populateStateParameterCells(cells, contractIndex, mm);
        }
    }
    return message;
}

bool MarketMaker::processGridChange(GridUpdate& update) {
    if (update._update.updatetype() == proto::GridUpdateType::CellButtonType) {
        auto cellButton = update._update.cellbutton();
        auto internalId = cellButton.row();
        passPosToRV(internalId);
        return true;
    }

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

void MarketMaker::passPosToRV(uint32_t internalId) {
    auto& mm = _marketMakers[internalId];
    auto pos = mm._internallyInitiatedPosition + mm._externallyInitiatedPosition;
    ManualFill fill;
    fill._contract = internalId;
    fill._size = -pos;
    onManualFill(fill);
    mm.onEndOfFills(mm._orderExecutionToken, true);
    getStrategy(StrategyId::RelativeValue)->sendGenericMessage(ViperMessageType::TransferMMPosToRV, internalId, pos);
}