#include "ArbieSpreader.h"
#include "GUIThings.h"

using namespace Python;

namespace {
GUIThings& guiThings = GUIThings::get();

inline uint32_t crossedColor(YesNoType ynt) {
    constexpr Color colors[] = {Color::TRANSPARENT, Color::TRANSPARENT_RED, Color::TRANSPARENT_GREEN};
    return toUInt(colors[toUInt(ynt)]);
}
}

void ArbieSpreader::populateGUIInitialization(::google::protobuf::RepeatedPtrField<::Python::proto::GridDefinition>* definitions) {
    auto [definition, rowHeader, cells] = guiThings.addWindowWithSubmitButton(definitions, WindowIds::ArbieSpreaderParameters, "Arbie Spreader Parameter", proto::GridType::Table);

    guiThings.addHeader(definition, {
        headerCell(ArbieSpreaderParameterColumns::QuoteExternal, "QuoteExternal", false, proto::DataType::bool_type),
        headerCell(ArbieSpreaderParameterColumns::Edge, "Edge", false, proto::DataType::double_type),
        headerCell(ArbieSpreaderParameterColumns::ActualEdge, "ActualEdge", true, proto::DataType::double_type),
        headerCell(ArbieSpreaderParameterColumns::QuoteSize, "QuoteSize", false, proto::DataType::unsigned_type),
        headerCell(ArbieSpreaderParameterColumns::STF, "STF", true, proto::DataType::rhone_price),
        headerCell(ArbieSpreaderParameterColumns::HedgeOffset, "hedgeOffset", false, proto::DataType::double_type),
        headerCell(ArbieSpreaderParameterColumns::MaxTicksThroughMarket, "MaxTicksThroughMarket", false, proto::DataType::double_type),
        headerCell(ArbieSpreaderParameterColumns::BidLeanPath, "BidLeanPath", true, proto::DataType::string_type),
        headerCell(ArbieSpreaderParameterColumns::AskLeanPath, "AskLeanPath", true, proto::DataType::string_type),
        headerCell(ArbieSpreaderParameterColumns::BidLeanPrice, "BidLeanPrice", true, proto::DataType::rhone_price),
        headerCell(ArbieSpreaderParameterColumns::AskLeanPrice, "AskLeanPrice", true, proto::DataType::rhone_price),
        headerCell(ArbieSpreaderParameterColumns::BidInternalPrice, "BidInternalPrice", true, proto::DataType::rhone_price),
        headerCell(ArbieSpreaderParameterColumns::BidExternalPrice, "BidExternalPrice", true, proto::DataType::rhone_price),
        headerCell(ArbieSpreaderParameterColumns::AskInternalPrice, "AskInternalPrice", true, proto::DataType::rhone_price),
        headerCell(ArbieSpreaderParameterColumns::AskExternalPrice, "AskExternalPrice", true, proto::DataType::rhone_price),
        headerCell(ArbieSpreaderParameterColumns::BidInternalSize, "BidInternalSize", true, proto::DataType::unsigned_type),
        headerCell(ArbieSpreaderParameterColumns::BidExternalSize, "BidExternalSize", true, proto::DataType::unsigned_type),
        headerCell(ArbieSpreaderParameterColumns::AskInternalSize, "AskInternalSize", true, proto::DataType::unsigned_type),
        headerCell(ArbieSpreaderParameterColumns::AskExternalSize, "AskExternalSize", true, proto::DataType::unsigned_type),
        headerCell(ArbieSpreaderParameterColumns::BidCross, "BidCross", true, proto::DataType::enum_type, "YesNo"),
        headerCell(ArbieSpreaderParameterColumns::AskCross, "AskCross", true, proto::DataType::enum_type, "YesNo"),
    });

    for (const auto& config : _viperConfig._arbieSpreaderConfigs) {
        uint32_t internalId = _viperConfig.getInternalId(config._contract);
        guiThings.addRowHeaderCell(WindowIds::ArbieSpreaderParameters, rowHeader, internalId, config._contract);
        guiThings.addRow(cells, internalId, {
            rowCell(ArbieSpreaderParameterColumns::QuoteExternal, std::to_string(config._quoteExternal)),
            rowCell(ArbieSpreaderParameterColumns::Edge, std::to_string(config._edge)),
            rowCell(ArbieSpreaderParameterColumns::ActualEdge, std::to_string(config._edge * _edgeMultiplier)),
            rowCell(ArbieSpreaderParameterColumns::QuoteSize, std::to_string(config._quoteSize)),
            rowCell(ArbieSpreaderParameterColumns::STF, std::to_string(hf::NULL_PRICE)),
            rowCell(ArbieSpreaderParameterColumns::HedgeOffset, std::to_string(config._hedgeOffset)),
            rowCell(ArbieSpreaderParameterColumns::MaxTicksThroughMarket, std::to_string(config._maxTicksThroughMarket)),
            rowCell(ArbieSpreaderParameterColumns::BidLeanPath, ""),
            rowCell(ArbieSpreaderParameterColumns::AskLeanPath, ""),
            rowCell(ArbieSpreaderParameterColumns::BidLeanPrice, std::to_string(hf::NULL_PRICE)),
            rowCell(ArbieSpreaderParameterColumns::AskLeanPrice, std::to_string(hf::NULL_PRICE)),
            rowCell(ArbieSpreaderParameterColumns::BidInternalPrice, std::to_string(hf::NULL_PRICE)),
            rowCell(ArbieSpreaderParameterColumns::BidExternalPrice, std::to_string(hf::NULL_PRICE)),
            rowCell(ArbieSpreaderParameterColumns::AskInternalPrice, std::to_string(hf::NULL_PRICE)),
            rowCell(ArbieSpreaderParameterColumns::AskExternalPrice, std::to_string(hf::NULL_PRICE)),
            rowCell(ArbieSpreaderParameterColumns::BidInternalSize, "0"),
            rowCell(ArbieSpreaderParameterColumns::BidExternalSize, "0"),
            rowCell(ArbieSpreaderParameterColumns::AskInternalSize, "0"),
            rowCell(ArbieSpreaderParameterColumns::AskExternalSize, "0"),
            rowCell(ArbieSpreaderParameterColumns::BidCross, std::to_string(toUInt(YesNoType::Unknown))),
            rowCell(ArbieSpreaderParameterColumns::AskCross, std::to_string(toUInt(YesNoType::Unknown))),
        });
    }

    auto [pathsDefinition, pathsRowHeader, pathsCells] = guiThings.addWindowWithSubmitButton(definitions, WindowIds::ArbieSpreaderPaths, "Arbie Spreader Paths", proto::GridType::Table);
    guiThings.addHeader(pathsDefinition, {
            headerCell(ArbieSpreaderPathsColumns::Commission, "Commission", false, proto::DataType::double_type),
            headerCell(ArbieSpreaderPathsColumns::Enabled, "Enabled", false, proto::DataType::bool_type),
            headerCell(ArbieSpreaderPathsColumns::STFOffset, "STFOffset", true, proto::DataType::rhone_price),
    });

    uint32_t index = 0;
    for (const auto& config : _viperConfig._arbieSpreaderConfigs) {
        uint32_t internalId = _viperConfig.getInternalId(config._contract);
        auto& contract = _contracts[internalId];
        if (contract._usedInQuoting) {
            for (auto* path : contract._paths[BID]) {
                _pathsParameters.emplace(index, path);
                guiThings.addRowHeaderCell(WindowIds::ArbieSpreaderPaths, pathsRowHeader, index,
                                           config._contract + " | " + path->_name);
                guiThings.addRow(pathsCells, index, {
                        rowCell(ArbieSpreaderPathsColumns::Commission, std::to_string(path->_config->_commission)),
                        rowCell(ArbieSpreaderPathsColumns::Enabled, std::to_string(path->_config->_enabled)),
                        rowCell(ArbieSpreaderPathsColumns::STFOffset, std::to_string(path->_stfOffset._stfOffset)),
                });
                ++index;
            }
        }
    }
}

void ArbieSpreader::sendStateToGUI(std::function<void(proto::GridUpdate&)>&& send) const {
    proto::GridUpdate message;
    message.set_gridid(toUInt(WindowIds::ArbieSpreaderParameters));
    message.set_updatetype(proto::GridUpdateType::CellsType);
    auto cells = message.mutable_cells();
    for (const auto& contract : _contracts) {
        if (!contract._usedInQuoting) {
            continue;
        }

        YesNoType bidCrossed = YesNoType::Unknown;
        YesNoType askCrossed = YesNoType::Unknown;
        if (state() == SystemState::CheckQuotes) {
            auto extBid = contract._markets[BID][Market::External].getPrice();
            auto extAsk = contract._markets[ASK][Market::External].getPrice();
            if (contract._quote[BID]._externalPrice != hf::NULL_PRICE && extAsk != hf::NULL_PRICE) {
                bidCrossed = contract._quote[BID]._externalPrice >= extAsk ? YesNoType::Yes : YesNoType::No;
            }
            if (contract._quote[ASK]._externalPrice != hf::NULL_PRICE && extBid != hf::NULL_PRICE) {
                askCrossed = contract._quote[ASK]._externalPrice <= extBid ? YesNoType::Yes : YesNoType::No;
            }
        }

        guiThings.addRow(cells, contract._def._internalId, {
                rowCell(ArbieSpreaderParameterColumns::ActualEdge, std::to_string(ToDollars(contract._edge))),
                rowCell(ArbieSpreaderParameterColumns::STF, std::to_string(contract._stf)),
                rowCell(ArbieSpreaderParameterColumns::BidLeanPath, contract._quote[BID]._hedgePath == -1 ? "" : contract._paths[BID][0]->_name),
                rowCell(ArbieSpreaderParameterColumns::AskLeanPath, contract._quote[ASK]._hedgePath == -1 ? "" : contract._paths[ASK][0]->_name),
                rowCell(ArbieSpreaderParameterColumns::BidLeanPrice, std::to_string(contract._quote[BID]._leanPrice)),
                rowCell(ArbieSpreaderParameterColumns::AskLeanPrice, std::to_string(contract._quote[ASK]._leanPrice)),
                rowCell(ArbieSpreaderParameterColumns::BidInternalPrice, std::to_string(contract._quote[BID]._internalPrice)),
                rowCell(ArbieSpreaderParameterColumns::BidExternalPrice, std::to_string(contract._quote[BID]._externalPrice)),
                rowCell(ArbieSpreaderParameterColumns::AskInternalPrice, std::to_string(contract._quote[ASK]._internalPrice)),
                rowCell(ArbieSpreaderParameterColumns::AskExternalPrice, std::to_string(contract._quote[ASK]._externalPrice)),
                rowCell(ArbieSpreaderParameterColumns::BidInternalSize, std::to_string(contract._quote[BID]._internalSize)),
                rowCell(ArbieSpreaderParameterColumns::BidExternalSize, std::to_string(contract._quote[BID]._externalSize)),
                rowCell(ArbieSpreaderParameterColumns::AskInternalSize, std::to_string(contract._quote[ASK]._internalSize)),
                rowCell(ArbieSpreaderParameterColumns::AskExternalSize, std::to_string(contract._quote[ASK]._externalSize)),
                rowCell(ArbieSpreaderParameterColumns::BidCross, std::to_string(toUInt(bidCrossed)), crossedColor(bidCrossed)),
                rowCell(ArbieSpreaderParameterColumns::AskCross, std::to_string(toUInt(askCrossed)), crossedColor(askCrossed)),
        });
    }
    send(message);

    proto::GridUpdate pathsUpdate;
    pathsUpdate.set_gridid(toUInt(WindowIds::ArbieSpreaderPaths));
    pathsUpdate.set_updatetype(proto::GridUpdateType::CellsType);
    auto pathCells = pathsUpdate.mutable_cells();
    for (auto& kvp : _pathsParameters) {
        auto path = kvp.second;
        if (BOOST_UNLIKELY(path->_stfOffset._stfOffset == hf::NULL_PRICE && getStrategy(StrategyId::RelativeValue)->isEnabled())) {
            logging::GetLogger().logWarning(boost::str(boost::format("AS %1%. Null stf. Path %2% id=%3% stf_id=%4%") % path->_contract->_def._name % path->_name % path->_id % path->_stfOffset._id));
        }
        guiThings.addRow(pathCells, kvp.first, {
            rowCell(ArbieSpreaderPathsColumns::STFOffset, std::to_string(path->_stfOffset._stfOffset)),
        });
    }
    send(pathsUpdate);
}

void ArbieSpreader::processGridChange(GridUpdate& update) {
    std::vector<Contract*> updatedContracts;
    bool edgeMultiplierChanged = false;
    auto gridId = update._update.gridid();
    if (gridId == toUInt(WindowIds::ArbieSpreaderParameters)) {
        for (proto::GridCell& cell : *update._update.mutable_cells()) {
            const uint32_t internalId = cell.row();
            auto& contract = _contracts[internalId];
            auto columnType = static_cast<ArbieSpreaderParameterColumns>(cell.col());
            switch (columnType) {
                case ArbieSpreaderParameterColumns::QuoteExternal: {
                    auto value = boost::lexical_cast<bool>(cell.value());
                    LOG(0, "AS {C[i1]}. QuoteExternal changed. {i2}->{i3}", contract.name(), contract._quoteExternally, value)
                    contract._quoteExternally = value;
                    updatedContracts.emplace_back(&contract);
                    break;
                }
                case ArbieSpreaderParameterColumns::Edge: {
                    auto value = boost::lexical_cast<double>(cell.value());
                    LOG(0, "AS {C[i1]}. Edge changed. {d1}->{d2}", contract.name(), ToDollars(contract._edge), value)
                    contract._edge = ToRhonePrice(value * _edgeMultiplier);
                    updatedContracts.emplace_back(&contract);
                    break;
                }
                case ArbieSpreaderParameterColumns::QuoteSize: {
                    auto value = boost::lexical_cast<hf::qty_t>(cell.value());
                    LOG(0, "AS {C[i1]}. QuoteSize changed. {i2}->{i3}", contract.name(), contract._quoteSize, value)
                    contract._quoteSize = value;
                    updatedContracts.emplace_back(&contract);
                    break;
                }
                case ArbieSpreaderParameterColumns::HedgeOffset: {
                    auto value = boost::lexical_cast<double>(cell.value());
                    if (value <= 0.0) {
                        LOG(0, "AS {C[i1]}. HedgeOffset must be > 0. Set Value back to {d1}", contract.name(), contract._hedgeOffset)
                        sendErrorMessage("AS HedgeOffset Parameter must > 0.", ErrorType::Normal);
                        cell.set_value(std::to_string(contract._hedgeOffset));
                    } else {
                        LOG(0, "AS {C[i1]}. HedgeOffset changed. {d1}->{d2}", contract.name(), ToDollars(contract._hedgeOffset), value)
                        contract._hedgeOffset = value;
                        contract._hedgeOffsetPrice = value * contract._def._tickSize;
                        updatedContracts.emplace_back(&contract);
                    }
                    break;
                }
                case ArbieSpreaderParameterColumns::MaxTicksThroughMarket: {
                    try {
                        auto value = boost::lexical_cast<double>(cell.value());
                        if (value < 0.0) {
                            double oldValue = (double)contract._maxTicksThroughMarket / contract._def._tickSize;
                            LOG(0, "AS {C[i1]}. MaxTicksThroughMarket must be >= 0. Set Value back to {d1}", contract.name(), oldValue)
                            sendErrorMessage(boost::str(boost::format("AS MaxTicksThroughMarket must be >= 0.  Contract=%1% Value=%2%") % contract._def._name % cell.value()), ErrorType::Critical);
                            cell.set_value(std::to_string(oldValue));
                        } else {
                            LOG(0, "AS {C[i1]}. MaxTicksThroughMarket changed. {d1}->{d2}", contract.name(), (contract._maxTicksThroughMarket / contract._def._tickSize), value)
                            contract._maxTicksThroughMarket = value * contract._def._tickSize;
                            updatedContracts.emplace_back(&contract);
                        }
                    }
                    catch(boost::bad_lexical_cast& blc) {
                        sendErrorMessage(boost::str(boost::format("maxTicksThroughmarket not parsable (too big?).  Contract=%1% Value=%2%") % contract._def._name % cell.value()), ErrorType::Critical);
                        cell.set_value(std::to_string((double)contract._maxTicksThroughMarket / contract._def._tickSize));
                    }
                    break;
                }
                default:
                    break;
            }
        }
    } else if (gridId == toUInt(WindowIds::ArbieSpreaderPaths)) {
        for (proto::GridCell& cell : *update._update.mutable_cells()) {
            auto pathIndex = cell.row();
            auto iter = _pathsParameters.find(pathIndex);
            if (iter != _pathsParameters.end()) {
                auto* path = iter->second;
                auto& contract = *path->_contract;
                auto columnType = static_cast<ArbieSpreaderPathsColumns>(cell.col());
                switch (columnType) {
                    case ArbieSpreaderPathsColumns::Commission: {
                        auto value = boost::lexical_cast<double>(cell.value());
                        LOG(0, "AS {C[i1]}. Path {i2} commission changed. {i3}->{i4}", contract.name(), path->_id, path->_config->_commission, value)
                        path->_config->_commission = value;
                        path->_commission = path->_otherSidePath->_commission = ToRhonePrice(value);
                        path->_otherSidePath->_dirty = true;
                        path->_dirty = true;
                        updatedContracts.emplace_back(&contract);
                        break;
                    }
                    case ArbieSpreaderPathsColumns::Enabled: {
                        auto value = boost::lexical_cast<bool>(cell.value());
                        LOG(0, "AS {C[i1]}. Path {i2} enabled changed. {i3}->{i4}", contract.name(), path->_id,
                            path->_config->_enabled, value)
                        if (path->_config->_enabled != value) {
                            path->_config->_enabled = value;
                            path->_dirty = true;
                            path->_otherSidePath->_dirty = true;
                        }
                        updatedContracts.emplace_back(&contract);
                        break;
                    }
                    default:
                        break;
                }
            }
        }
    } else if (gridId == toUInt(WindowIds::PricingConfig)) {
        for (int i = 0; i < update._update.cells_size(); ++i) {
            const proto::GridCell& gridCell = update._update.cells(i);
            auto columnType = static_cast<PricingConfigsColumns>(gridCell.col());
            if (columnType == PricingConfigsColumns::ASExtCommission) {
                Contract& contract = _contracts[gridCell.row()];
                auto value = ToRhonePrice(boost::lexical_cast<double>(gridCell.value()));
                LOG(0, "AS {C[i1]}. External Commission changed. {d1}->{d2}", contract.name(),
                    ToDollars(contract._extCommission), value)

                if (contract._extCommission != value) {
                    contract._extCommission = ToRhonePrice(value);
                    for (auto path : contract._affectedPaths) {
                        path->_dirty = true;
                        updatedContracts.emplace_back(path->_contract);
                    }
                }
            }
        }
    } else if (gridId == toUInt(WindowIds::ManualFillsEntry)) {
        onManualFills(update);
    } else if (gridId == toUInt(WindowIds::GlobalConfigs)) {
        for (int i = 0; i < update._update.cells_size(); ++i) {
            proto::GridCell& gridCell = *update._update.mutable_cells(i);
            auto& config = _viperConfig._globalConfigs[gridCell.row()];
            if (config._key == AS_EDGE_MULTIPLIER) {
                auto shift = static_cast<Shifts>(gridCell.col() - 1);
                try {
                    double value = boost::lexical_cast<double>(gridCell.value());
                    if (!isEdgeMultiplierWithinLimits(value)) {
                        LOG(0, "AS could not change edge multiplier. Value must be > 1 and < 10 Value={d1}", value)
                        sendErrorMessage(boost::str(boost::format(
                                "AS could not change edge multiplier. Value must be > 1 and < 10 Value=%1%") %
                                                    value), ErrorType::Critical);
                        gridCell.set_value(config._shifts[toUInt(shift)]);
                    } else {
                        if (_viperConfig._currentShift == shift) {
                            _edgeMultiplier = value;
                            edgeMultiplierChanged = true;
                            LOG(0, "AS EdgeMultiplier changed. Value={d1}", _edgeMultiplier)
                        }
                    }
                }
                catch (const boost::bad_lexical_cast&) {
                    LOG(0, "AS. Invalid edge multiplier value {i1}", 0)
                    gridCell.set_value(config._shifts[toUInt(shift)]);
                    sendErrorMessage(boost::str(boost::format("Invalid AS edge multiplier value %1%") % gridCell.value()), ErrorType::Critical);
                }
            }
        }
    }

    if (edgeMultiplierChanged) {
        for (auto& asConfig : _viperConfig._arbieSpreaderConfigs) {
            auto configContract = _viperConfig.getContract(asConfig._contract);
            if (configContract) {
                auto& contract = _contracts[configContract->_internalId];
                contract._edge = ToRhonePrice(asConfig._edge * _edgeMultiplier);
                updateQuotes(contract, true);
            } else {
                assert(false);
            }
        }
    } else {
        for (auto contract : updatedContracts) {
            updateQuotes(*contract, true);
        }
    }
}