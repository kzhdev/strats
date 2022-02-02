#include "RelativeValue.h"
#include "GUIThings.h"

using namespace Python;

namespace {
GUIThings& guiThings = GUIThings::get();
uint32_t crossedColor(YesNoType ynt) {
    return ynt == YesNoType::Yes ? toUInt(Color::TRANSPARENT_RED) : ynt == YesNoType::No ? toUInt(
            Color::TRANSPARENT_GREEN) : toUInt(Color::TRANSPARENT);
}
}

void RelativeValue::populateGUIInitialization(::google::protobuf::RepeatedPtrField< ::Python::proto::GridDefinition >* definitions) {
    populateParametersGrid(definitions);
    populateVectorParameters(definitions);
    populatePricingMatrix(definitions);
    populateRiskMatrix(definitions);
    populateHedgeRatios(definitions);
    populateFactorHedged(definitions);
    populateQuoteSizes(definitions);
    populateCommissions(definitions);
}

void RelativeValue::populateParametersGrid(::google::protobuf::RepeatedPtrField< ::Python::proto::GridDefinition >* definitions) const {
    auto [definition, rowHeader, cells] = guiThings.addWindowWithSubmitButton(definitions, WindowIds::ContractParametersRv, "Relative Value Parameters", proto::GridType::Table);
    guiThings.addHeader(definition, {
            headerCell(ContractParameterRVColumns::BidLean, "BidLean", true, proto::DataType::enum_type, "contracts"),
            headerCell(ContractParameterRVColumns::AskLean, "AskLean", true, proto::DataType::enum_type, "contracts"),
            headerCell(ContractParameterRVColumns::BidLeanType, "BidLeanType", true, proto::DataType::enum_type, "LeanType"),
            headerCell(ContractParameterRVColumns::AskLeanType, "AskLeanType", true, proto::DataType::enum_type, "LeanType"),
            headerCell(ContractParameterRVColumns::BidInternalPrice, "BidIntPrice", true, proto::DataType::rhone_price),
            headerCell(ContractParameterRVColumns::AskInternalPrice, "AskIntPrice", true, proto::DataType::rhone_price),
            headerCell(ContractParameterRVColumns::BidExternalPrice, "BidExtPrice", true, proto::DataType::rhone_price),
            headerCell(ContractParameterRVColumns::AskExternalPrice, "AskExtPrice", true, proto::DataType::rhone_price),
            headerCell(ContractParameterRVColumns::BidCross, "BidCross", true, proto::DataType::enum_type, "YesNo"),
            headerCell(ContractParameterRVColumns::AskCross, "AskCross", true, proto::DataType::enum_type, "YesNo"),
            headerCell(ContractParameterRVColumns::BidInternalQty, "BidIntQty", true, proto::DataType::unsigned_type),
            headerCell(ContractParameterRVColumns::AskInternalQty, "AskIntQty", true, proto::DataType::unsigned_type),
            headerCell(ContractParameterRVColumns::BidExternalQty, "BidExtQty", true, proto::DataType::unsigned_type),
            headerCell(ContractParameterRVColumns::AskExternalQty, "AskExtQty", true, proto::DataType::unsigned_type),
            headerCell(ContractParameterRVColumns::STF, "STF", true, proto::DataType::rhone_price),
            headerCell(ContractParameterRVColumns::Mkt, "Mkt", true, proto::DataType::rhone_price),
            headerCell(ContractParameterRVColumns::QuoteExternal, "QuoteExternal", false, proto::DataType::bool_type),
            headerCell(ContractParameterRVColumns::HedgeTolerance, "HedgeTolerance", false, proto::DataType::double_type),
            headerCell(ContractParameterRVColumns::Width, "Width", false, proto::DataType::double_type),
            headerCell(ContractParameterRVColumns::PCTStfMove, "%stfMove", false, proto::DataType::double_type),
            headerCell(ContractParameterRVColumns::MaxTicksThroughMarket, "MaxTicksThroughMarket", false, proto::DataType::double_type),
            headerCell(ContractParameterRVColumns::Commission, "Commission", false, proto::DataType::double_type),
            headerCell(ContractParameterRVColumns::ExtraWidth, "ExtraWidth", true, proto::DataType::double_type),
            headerCell(ContractParameterRVColumns::StaticQuote, "StaticQuote", false, proto::DataType::bool_type),
            headerCell(ContractParameterRVColumns::QuoteSize, "QuoteSize", false, proto::DataType::unsigned_type),
    });

    bool checkForCross = (state() == SystemState::CheckQuotes);

    for (uint32_t index = 0; index < _viperConfig._relativeValueConfigs.size(); ++index) {
        RelativeValueConfig& config = _viperConfig._relativeValueConfigs[index];
        uint32_t internalId = _viperConfig.getInternalId(config._contract);
        guiThings.addRowHeaderCell(WindowIds::ContractParametersRv, rowHeader,internalId, config._contract);
        guiThings.addCells(cells, internalId, {
                Cell(ContractParameterRVColumns::QuoteExternal, std::to_string(config._quoteExternal)),
                Cell(ContractParameterRVColumns::HedgeTolerance, std::to_string(config._hedgeTolerance)),
                Cell(ContractParameterRVColumns::Width, std::to_string(config._width)),
                Cell(ContractParameterRVColumns::PCTStfMove, std::to_string(config._pctStfMove)),
                Cell(ContractParameterRVColumns::MaxTicksThroughMarket, std::to_string(config._maxTicksThroughMarket)),
                Cell(ContractParameterRVColumns::Commission, std::to_string(config._commission)),
                Cell(ContractParameterRVColumns::ExtraWidth, std::to_string(ToDollars(_extraContractWidths.getValue(internalId)))),
                Cell(ContractParameterRVColumns::StaticQuote, std::to_string(config._staticQuote)),
                Cell(ContractParameterRVColumns::QuoteSize, std::to_string(config._quoteSize)),
        });
        populateVectorParameterCells(cells, internalId, checkForCross);
    }
}

void RelativeValue::populateVectorParameterCells(::google::protobuf::RepeatedPtrField< ::Python::proto::GridCell >* cells, uint32_t contractId, bool checkForCross) const {
    const RvContract& contract = _contracts[contractId];
    if (contract._traded) {
        const Quote& bid = contract._quotes[BID];
        if (bid._bestPrice != hf::NULL_PRICE) {
            guiThings.setCell(cells, contract._internalId, ContractParameterRVColumns::BidInternalPrice, std::to_string(bid._bestPrice));
            guiThings.setCell(cells, contract._internalId, ContractParameterRVColumns::BidInternalQty, std::to_string(bid._bestSize));
            guiThings.setCell(cells, contract._internalId, ContractParameterRVColumns::BidExternalPrice, std::to_string(bid._externalPrice));
            guiThings.setCell(cells, contract._internalId, ContractParameterRVColumns::BidExternalQty, std::to_string(bid._externalSize));
            const RvContract& bidBestLean = _contracts[bid._bestLeanIndex];
            guiThings.setCell(cells, contract._internalId, ContractParameterRVColumns::BidLean, std::to_string(bidBestLean._definition._internalId));
            LeanType leanType = LeanType::None;
            if (bid._leans.getNumberOfLeans() > 0) {
                const Lean& bidLean = bid._leans._leans[0];
                if (!bidLean._isExternal) {
                    leanType = static_cast<LeanType>(bidLean._marketIndex);
                } else {
                    leanType = LeanType::External;
                }
            }
            guiThings.setCell(cells, contract._internalId, ContractParameterRVColumns::BidLeanType, std::to_string(toUInt(leanType)));
            YesNoType bidCrossed = YesNoType::Unknown;
            if (checkForCross && bid._externalPrice != hf::NULL_PRICE && contract.externalMarket<ASK>().getPrice() != hf::NULL_PRICE) {
                bidCrossed = bid._externalPrice >= contract.externalMarket<ASK>().getPrice() ? YesNoType::Yes : YesNoType::No;
            }
            guiThings.setCell(cells, contract._internalId, ContractParameterRVColumns::BidCross, std::to_string(toUInt(bidCrossed)), crossedColor(bidCrossed));
        }
        else {
            guiThings.setCell(cells, contract._internalId, ContractParameterRVColumns::BidLean, GUIThings::NO_VALUE_ID);
            guiThings.setCell(cells, contract._internalId, ContractParameterRVColumns::BidLeanType, std::to_string(toUInt(LeanType::None)));
            guiThings.setCell(cells, contract._internalId, ContractParameterRVColumns::BidCross, std::to_string(toUInt(YesNoType::Unknown)));
            guiThings.setCell(cells, contract._internalId, ContractParameterRVColumns::BidInternalPrice, std::to_string(hf::NULL_PRICE));
            guiThings.setCell(cells, contract._internalId, ContractParameterRVColumns::BidInternalQty, "0");
            guiThings.setCell(cells, contract._internalId, ContractParameterRVColumns::BidExternalPrice, std::to_string(hf::NULL_PRICE));
            guiThings.setCell(cells, contract._internalId, ContractParameterRVColumns::BidExternalQty, "0");
        }
        const Quote& ask = contract._quotes[ASK];
        if (ask._bestPrice != hf::NULL_PRICE) {
            guiThings.setCell(cells, contract._internalId, ContractParameterRVColumns::AskInternalPrice, std::to_string(ask._bestPrice));
            guiThings.setCell(cells, contract._internalId, ContractParameterRVColumns::AskInternalQty, std::to_string(ask._bestSize));
            guiThings.setCell(cells, contract._internalId, ContractParameterRVColumns::AskExternalPrice, std::to_string(ask._externalPrice));
            guiThings.setCell(cells, contract._internalId, ContractParameterRVColumns::AskExternalQty, std::to_string(ask._externalSize));
            const RvContract& askBestLean = _contracts[ask._bestLeanIndex];
            guiThings.setCell(cells, contract._internalId, ContractParameterRVColumns::AskLean, std::to_string(askBestLean._definition._internalId));
            LeanType leanType = LeanType::None;
            if (ask._leans.getNumberOfLeans() > 0) {
                const Lean& askLean = ask._leans._leans[0];
                if (!askLean._isExternal) {
                    leanType = static_cast<LeanType>(askLean._marketIndex);
                } else {
                    leanType = LeanType::External;
                }
            }
            guiThings.setCell(cells, contract._internalId, ContractParameterRVColumns::AskLeanType, std::to_string(toUInt(leanType)));
            YesNoType askCrossed = YesNoType::Unknown;
            if (checkForCross && ask._externalPrice != hf::NULL_PRICE && contract.externalMarket<BID>().getPrice() != hf::NULL_PRICE) {
                askCrossed = ask._externalPrice <= contract.externalMarket<BID>().getPrice() ? YesNoType::Yes : YesNoType::No;
            }
            guiThings.setCell(cells, contract._internalId, ContractParameterRVColumns::AskCross, std::to_string(toUInt(askCrossed)), crossedColor(askCrossed));
        }
        else {
            guiThings.setCell(cells, contract._internalId, ContractParameterRVColumns::AskLean, GUIThings::NO_VALUE_ID);
            guiThings.setCell(cells, contract._internalId, ContractParameterRVColumns::AskLeanType, std::to_string(toUInt(LeanType::None)));
            guiThings.setCell(cells, contract._internalId, ContractParameterRVColumns::AskCross, std::to_string(toUInt(YesNoType::Unknown)));
            guiThings.setCell(cells, contract._internalId, ContractParameterRVColumns::AskInternalPrice, std::to_string(hf::NULL_PRICE));
            guiThings.setCell(cells, contract._internalId, ContractParameterRVColumns::AskInternalQty, "0");
            guiThings.setCell(cells, contract._internalId, ContractParameterRVColumns::AskExternalPrice, std::to_string(hf::NULL_PRICE));
            guiThings.setCell(cells, contract._internalId, ContractParameterRVColumns::AskExternalQty, "0");
        }

        {
            hf::price_t price = 0;
            if (contract._definition._legs.size() > 1) {
                double sum = 0;
                for (const auto& leg : contract._definition._legs) {
                    sum += _overdeterminedPrices.getValue(0, leg._index) * leg._ratio;
                }
                price = ToRhonePrice(sum);
            } else {
                price = ToRhonePrice(_overdeterminedPrices.getValue(0, contractId));
            }
            guiThings.setCell(cells, contract._internalId, ContractParameterRVColumns::Mkt, std::to_string(price));
        }
        {
            hf::price_t stf = _shortTermFairsForContracts.getRef(contractId)._fair;
            guiThings.setCell(cells, contract._internalId, ContractParameterRVColumns::STF, std::to_string(stf));
        }
    }
}

void RelativeValue::populateVectorParameters(::google::protobuf::RepeatedPtrField<::Python::proto::GridDefinition>* definitions) const {
    auto [definition, rowHeader, cells] =
        guiThings.addWindow(definitions, WindowIds::VectorParameters, "Factor Parameters", proto::GridType::Table);

    guiThings.addHeader(definition, {
            headerCell(VectorParametersColumns::Position, "Position", true, proto::DataType::double_type),
            headerCell(VectorParametersColumns::Mkt, "Mkt", true, proto::DataType::double_type),
            headerCell(VectorParametersColumns::STF, "STF", true, proto::DataType::double_type),
            headerCell(VectorParametersColumns::LTF, "LTF", false, proto::DataType::double_type),
            headerCell(VectorParametersColumns::MaxPos, "MaxPos", false, proto::DataType::signed_type),
            headerCell(VectorParametersColumns::Range, "Range", false, proto::DataType::double_type),
            headerCell(VectorParametersColumns::EMA_ON, "EmaOn", false, proto::DataType::bool_type),
            headerCell(VectorParametersColumns::EMA_Time, "EmaSeconds", false, proto::DataType::unsigned_type),
            headerCell(VectorParametersColumns::EMA_On_Time, "EmaOnTime", false, proto::DataType::enum_type, "ema_on_time"),
            headerCell(VectorParametersColumns::EMA_Off_Time, "EmaOffTime", false, proto::DataType::enum_type, "ema_off_time"),
            headerCell(VectorParametersColumns::SetToMarket, "SetToMarket", true, proto::DataType::bool_type),
            headerCell(VectorParametersColumns::Hedged, "Hedged", true, proto::DataType::bool_type),
            headerCell(VectorParametersColumns::STF_To_Market, "STFToMarket", false, proto::DataType::button),
            headerCell(VectorParametersColumns::LTF_Delta, "LTFDelta", false, proto::DataType::double_type),
            headerCell(VectorParametersColumns::Increment_LTF, "+LTFDelta", false, proto::DataType::value_increment_button, "", toUInt(VectorParametersColumns::LTF), toUInt(VectorParametersColumns::LTF_Delta)),
            headerCell(VectorParametersColumns::Decrement_LTF, "-LTFDelta", false, proto::DataType::value_decrement_button, "", toUInt(VectorParametersColumns::LTF), toUInt(VectorParametersColumns::LTF_Delta)),
    });

    for (uint32_t vectorIndex = 0; vectorIndex < _viperConfig._vectorConfigs.size(); ++vectorIndex) {
        VectorConfig& config = _viperConfig._vectorConfigs[vectorIndex];
        guiThings.addRowHeaderCell(WindowIds::VectorParameters, rowHeader, vectorIndex, config._vectorName);
        guiThings.addCells(cells, vectorIndex, {
                Cell(VectorParametersColumns::Position, std::to_string(_positionsInVector.getValue(vectorIndex))),
                Cell(VectorParametersColumns::Mkt, std::to_string(_currentVectorMarketPrices->getValue(vectorIndex))),
                Cell(VectorParametersColumns::STF, std::to_string(_shortTermFairsInVector.getValue(vectorIndex))),
                Cell(VectorParametersColumns::LTF, std::to_string(_longTermFairsInVector.getValue(vectorIndex))),
                Cell(VectorParametersColumns::MaxPos, std::to_string(config._maxPos)),
                Cell(VectorParametersColumns::Range, std::to_string(config._range)),
                Cell(VectorParametersColumns::EMA_ON, std::to_string(config._useEMA)),
                Cell(VectorParametersColumns::EMA_Time, std::to_string(config._EMALength)),
                Cell(VectorParametersColumns::EMA_On_Time, std::to_string(config._EMAOn)),
                Cell(VectorParametersColumns::EMA_Off_Time, std::to_string(config._EMAOff)),
                Cell(VectorParametersColumns::SetToMarket, std::to_string(config._setToMarket)),
                Cell(VectorParametersColumns::Hedged, std::to_string(config._hedged)),
                Cell(VectorParametersColumns::STF_To_Market, std::string(config._vectorName), toUInt(Color::TRANSPARENT)),
                Cell(VectorParametersColumns::LTF_Delta, std::to_string(config._ltfDelta)),
                Cell(VectorParametersColumns::Increment_LTF, "+", toUInt(Color::TRANSPARENT)),
                Cell(VectorParametersColumns::Decrement_LTF, "-", toUInt(Color::TRANSPARENT)),
        });
    }
    auto buttons = definition->mutable_buttons();
    guiThings.addButton(buttons, toUInt(STF_TO_MARKET), "STF to Market", proto::ButtonFrontBehavior_Nothing, proto::ButtonBackBehavior_SubmitId, GUIThings::RequiresConfirmation::NO);
    guiThings.addButton(buttons, toUInt(0), "Submit", proto::ButtonFrontBehavior_CommitValues, proto::ButtonBackBehavior_Send, GUIThings::RequiresConfirmation::NO);
}

void RelativeValue::populatePricingMatrix(::google::protobuf::RepeatedPtrField<::Python::proto::GridDefinition>* definitions) const {
    auto [definition, rowHeader, cells] = guiThings.addWindow(definitions, WindowIds::PricingMatrix, "Pricing Matrix", proto::GridType::Matrix);
    for (uint32_t index = 0; index < _viperConfig._pricingOutrights.size(); ++index) {
        ConfigContract& contract = _viperConfig._allContracts[index];
        guiThings.addColumnHeaderCell(WindowIds::PricingMatrix, definition->mutable_columnheader(), index, contract._name, true, proto::DataType::double_type);
    }

    for (uint32_t index = 0; index < _viperConfig._vectorConfigs.size(); ++index) {
        VectorConfig& config = _viperConfig._vectorConfigs[index];
        guiThings.addRowHeaderCell(WindowIds::PricingMatrix, rowHeader, index, config._vectorName);
    }

    for (uint32_t row = 0; row < _pricingMatrix._numberOfRows; ++row) {
        for (uint32_t col = 0; col < _pricingMatrix._numberOfColumns; ++col) {
            guiThings.setCell(cells, row, col, std::to_string(_pricingMatrix.getValue(row, col)));
        }
    }
}

void RelativeValue::populateRiskMatrix(::google::protobuf::RepeatedPtrField<::Python::proto::GridDefinition>* definitions) const {
    auto [definition, rowHeader, cells] = guiThings.addWindow(definitions, WindowIds::RiskMatrix, "Risk Matrix", proto::GridType::Matrix);

    for (uint32_t index = 0; index < _viperConfig._pricingOutrights.size(); ++index) {
        ConfigContract& contract = _viperConfig._allContracts[index];
        guiThings.addColumnHeaderCell(WindowIds::RiskMatrix, definition->mutable_columnheader(), index, contract._name, true, proto::DataType::double_type);
    }

    for (uint32_t index = 0; index < _viperConfig._vectorConfigs.size(); ++index) {
        VectorConfig& config = _viperConfig._vectorConfigs[index];
        guiThings.addRowHeaderCell(WindowIds::RiskMatrix, rowHeader, index, config._vectorName);
    }

    for (uint32_t row = 0; row < _riskMatrix._numberOfRows; ++row) {
        for (uint32_t col = 0; col < _riskMatrix._numberOfColumns; ++col) {
            guiThings.setCell(cells, row, col, std::to_string(_riskMatrix.getValue(row, col)));
        }
    }
}

void RelativeValue::populateHedgeRatios(::google::protobuf::RepeatedPtrField<::Python::proto::GridDefinition>* definitions) const {
    auto [definition, rowHeader, cells] = guiThings.addWindow(definitions, WindowIds::HedgeRatios, "Hedge Ratios", proto::GridType::Matrix);

    for (uint32_t index = _viperConfig._indexOfFirstTradedContract; index < _viperConfig._allContracts.size(); ++index) {
        ConfigContract& contract = _viperConfig._allContracts[index];
        guiThings.addColumnHeaderCell(WindowIds::HedgeRatios, definition->mutable_columnheader(), index, contract._name, true, proto::DataType::double_type);
        guiThings.addRowHeaderCell(WindowIds::HedgeRatios, rowHeader, index, contract._name);

        for (uint32_t col = _viperConfig._indexOfFirstTradedContract; col < _viperConfig._allContracts.size(); ++col) {
            guiThings.setCell(cells, index, col, std::to_string(_hedgeRatios.getValue(index, col)));
        }
    }
}

void RelativeValue::populateFactorHedged(::google::protobuf::RepeatedPtrField<::Python::proto::GridDefinition>* definitions) const {
    auto [definition, rowHeader, cells] = guiThings.addWindow(definitions, WindowIds::FactorHedged, "Factor hedged", proto::GridType::Matrix);

    for (uint32_t index = _viperConfig._indexOfFirstTradedContract; index < _viperConfig._allContracts.size(); ++index) {
        ConfigContract& contract = _viperConfig._allContracts[index];
        guiThings.addColumnHeaderCell(WindowIds::FactorHedged, definition->mutable_columnheader(), index, contract._name, true, proto::DataType::enum_type, "factors");
        guiThings.addRowHeaderCell(WindowIds::FactorHedged, rowHeader, index, contract._name);
        for (uint32_t col = _viperConfig._indexOfFirstTradedContract; col < _viperConfig._allContracts.size(); ++col) {
            guiThings.setCell(cells, index, col, std::to_string(_factorHedgedIndexes.getValue(index, col)));
        }
    }
}

void RelativeValue::populateQuoteSizes(::google::protobuf::RepeatedPtrField<::Python::proto::GridDefinition>* definitions) const {
    auto [definition, rowHeader, cells] = guiThings.addWindow(definitions, WindowIds::QuoteSizes, "Quote sizes", proto::GridType::Matrix);
    for (uint32_t index = _viperConfig._indexOfFirstTradedContract; index < _viperConfig._allContracts.size(); ++index) {
        ConfigContract& contract = _viperConfig._allContracts[index];
        guiThings.addColumnHeaderCell(WindowIds::QuoteSizes, definition->mutable_columnheader(), index, contract._name, true, proto::DataType::double_type);
        guiThings.addRowHeaderCell(WindowIds::QuoteSizes, rowHeader, index, contract._name);
        for (uint32_t col = _viperConfig._indexOfFirstTradedContract; col < _viperConfig._allContracts.size(); ++col) {
            guiThings.setCell(cells, index, col, std::to_string(_quoteSizes.getValue(index, col)));
        }
    }
}

void RelativeValue::populateCommissions(::google::protobuf::RepeatedPtrField<::Python::proto::GridDefinition>* definitions) const {
    auto [definition, rowHeader, cells] = guiThings.addWindow(definitions, WindowIds::Commissions, "Commissions", proto::GridType::Matrix);

    for (uint32_t index = _viperConfig._indexOfFirstTradedContract; index < _viperConfig._allContracts.size(); ++index) {
        ConfigContract& contract = _viperConfig._allContracts[index];
        guiThings.addColumnHeaderCell(WindowIds::Commissions, definition->mutable_columnheader(), index, contract._name, true, proto::DataType::double_type);
        guiThings.addRowHeaderCell(WindowIds::Commissions, rowHeader, index, contract._name);
        for (uint32_t col = _viperConfig._indexOfFirstTradedContract; col < _viperConfig._allContracts.size(); ++col) {
            guiThings.setCell(cells, index, col, std::to_string(ToDollars(_commissions.getValue(index, col))));
        }
    }
}

proto::GridUpdate RelativeValue::getQuoteSizeUpdate() const {
    proto::GridUpdate message;
    message.set_gridid(toUInt(WindowIds::QuoteSizes));
    message.set_updatetype(proto::GridUpdateType::CellsType);
    auto cells = message.mutable_cells();
    for (uint32_t row = _viperConfig._indexOfFirstTradedContract; row < _viperConfig._allContracts.size(); ++row) {
        for (uint32_t col = _viperConfig._indexOfFirstTradedContract; col < _viperConfig._allContracts.size(); ++col) {
            guiThings.setCell(cells, row, col, std::to_string(_quoteSizes.getValue(row, col)));
        }
    }
    return message;
}

proto::GridUpdate RelativeValue::getHedgeRatiosUpdate() const {
    proto::GridUpdate message;
    message.set_gridid(toUInt(WindowIds::HedgeRatios));
    message.set_updatetype(proto::GridUpdateType::CellsType);
    auto cells = message.mutable_cells();
    for (uint32_t row = _viperConfig._indexOfFirstTradedContract; row < _viperConfig._allContracts.size(); ++row) {
        for (uint32_t col = _viperConfig._indexOfFirstTradedContract; col < _viperConfig._allContracts.size(); ++col) {
            guiThings.setCell(cells, row, col, std::to_string(_hedgeRatios.getValue(row, col)));
        }
    }
    return message;
}

proto::GridUpdate RelativeValue::getFactorHedgedUpdate() const {
    proto::GridUpdate message;
    message.set_gridid(toUInt(WindowIds::FactorHedged));
    message.set_updatetype(proto::GridUpdateType::CellsType);
    auto cells = message.mutable_cells();
    for (uint32_t row = _viperConfig._indexOfFirstTradedContract; row < _viperConfig._allContracts.size(); ++row) {
        for (uint32_t col = _viperConfig._indexOfFirstTradedContract; col < _viperConfig._allContracts.size(); ++col) {
            guiThings.setCell(cells, row, col, std::to_string(_factorHedgedIndexes.getValue(row, col)));
        }
    }
    return message;
}

proto::GridUpdate RelativeValue::getExtraContractWidth_PctStfMove_MaxTicksThroughMarket_Update() const {
    proto::GridUpdate message;
    message.set_gridid(toUInt(WindowIds::ContractParametersRv));
    message.set_updatetype(proto::GridUpdateType::CellsType);
    auto cells = message.mutable_cells();
    for (uint32_t contractIndex = 0; contractIndex < _numberOfAllContracts; ++contractIndex) {
        const RvContract& contract = _contracts[contractIndex];
        if (contract._traded) {
            guiThings.setCell(cells, contract._internalId, ContractParameterRVColumns::ExtraWidth, std::to_string(ToDollars(_extraContractWidths.getValue(contract._internalId))));
            guiThings.setCell(cells, contract._internalId, ContractParameterRVColumns::PCTStfMove, std::to_string(_pctStfMoves.getValue(contractIndex)));
            guiThings.setCell(cells, contract._internalId, ContractParameterRVColumns::MaxTicksThroughMarket, std::to_string(_maxTicksThroughMarket.getValue(contractIndex) / contract._definition._tickSize));
        }
    }
    return message;
}

proto::GridUpdate RelativeValue::getCommissionUpdate() const {
    proto::GridUpdate message;
    message.set_gridid(toUInt(WindowIds::Commissions));
    message.set_updatetype(proto::GridUpdateType::CellsType);
    auto cells = message.mutable_cells();
    for (uint32_t row = _viperConfig._indexOfFirstTradedContract; row < _viperConfig._allContracts.size(); ++row) {
        for (uint32_t col = _viperConfig._indexOfFirstTradedContract; col < _viperConfig._allContracts.size(); ++col) {
            guiThings.setCell(cells, row, col, std::to_string(ToDollars(_commissions.getValue(row, col))));
        }
    }
    return message;
}

void RelativeValue::sendStateToGUI(std::function<void(proto::GridUpdate&, WindowIds)>&& send, std::function<bool(WindowIds)>&& hasSubscription) const {
    if (hasSubscription(WindowIds::ContractParametersRv)) {
        proto::GridUpdate message = getParametersUpdate();
        send(message, WindowIds::ContractParametersRv);
    }

    if (hasSubscription(WindowIds::VectorParameters)) {
        proto::GridUpdate message = getVectorParametersUpdate();
        send(message, WindowIds::VectorParameters);
    }
}

proto::GridUpdate RelativeValue::getParametersUpdate() const {
    proto::GridUpdate message;
    bool checkForCross = (state() == SystemState::CheckQuotes);
    message.set_gridid(toUInt(WindowIds::ContractParametersRv));
    message.set_updatetype(proto::GridUpdateType::CellsType);
    auto cells = message.mutable_cells();
    for (uint32_t contractIndex = 0; contractIndex < _numberOfAllContracts; ++contractIndex) {
        populateVectorParameterCells(cells, contractIndex, checkForCross);
    }
    return message;
}

proto::GridUpdate RelativeValue::getVectorParametersUpdate() const {
    proto::GridUpdate message;
    message.set_gridid(toUInt(WindowIds::VectorParameters));
    message.set_updatetype(proto::GridUpdateType::CellsType);
    auto cells = message.mutable_cells();
    for (uint32_t vectorIndex = 0; vectorIndex < _numberOfVectors; ++vectorIndex) {
        const double ltf = _longTermFairsInVector.getValue(vectorIndex);
        guiThings.setCell(cells, vectorIndex, VectorParametersColumns::LTF, std::to_string(ltf));
        const double mkt = _currentVectorMarketPrices->getValue(vectorIndex);
        guiThings.setCell(cells, vectorIndex, VectorParametersColumns::Mkt, std::to_string(mkt));
        double stf = _shortTermFairsInVector.getValue(vectorIndex);
        guiThings.setCell(cells, vectorIndex, VectorParametersColumns::STF, std::to_string(stf));
        double position = _positionsInVector.getValue(vectorIndex);
        guiThings.setCell(cells, vectorIndex, VectorParametersColumns::Position, std::to_string(position));
    }
    return message;
}