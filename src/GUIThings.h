#ifndef VIPER_GUI_THINGS_H
#define VIPER_GUI_THINGS_H

#include "messages/omsproto.pb.h"
#include "ViperConfig.h"
#include "RelativeValue.h"

namespace Python {


class GUIThings {
public:

    void init(ViperConfig* config, std::unordered_map<std::string, uint32_t>* symbolToInterId) {
        _config = config;
        _symbolToInterId = symbolToInterId;
    }

    void populateFillMessage(GenericMessage& fill, proto::GridUpdate& message) {
        message.set_gridid(toUInt(WindowIds::FillsDisplay));
        auto cells = message.mutable_cells();
        auto& contract = _config->_allContracts[fill._number_1];
        setCell(cells, _fillNumber, FillsColumns::Contract, contract._name);
        setCell(cells, _fillNumber, FillsColumns::Side, (fill._number_2 == 0 ? "BID" : "ASK"));
        setCell(cells, _fillNumber, FillsColumns::Price, std::to_string(fill._number_3));
        setCell(cells, _fillNumber, FillsColumns::Size, std::to_string(fill._number_4));
        ++_fillNumber;
    }

    void populateLTFmessage(proto::GridUpdate& message, RelativeValue* relativeValue) {
        message.set_gridid(toUInt(WindowIds::VectorParameters));
        auto cells = message.mutable_cells();
        for (uint32_t vectorIndex = 0; vectorIndex < relativeValue->_numberOfVectors; ++vectorIndex) {
            const double ltf = relativeValue->_longTermFairsInVector.getValue(vectorIndex);
            setCell(cells, vectorIndex, VectorParametersColumns::LTF, std::to_string(ltf));
        }
    }

    void populateVectorPrices(proto::GridUpdate& message, RelativeValue* relativeValue) {
        message.set_gridid(toUInt(WindowIds::VectorParameters));
        auto cells = message.mutable_cells();
        for (uint32_t vectorIndex = 0; vectorIndex < relativeValue->_numberOfVectors; ++vectorIndex) {
            const double mkt = relativeValue->_newVectorMarketPrices.getValue(vectorIndex);
            setCell(cells, vectorIndex, VectorParametersColumns::Mkt, std::to_string(mkt));
        }
    }

    void populateContractPrices(proto::GridUpdate& message, RelativeValue* relativeValue) {
        message.set_gridid(toUInt(WindowIds::ContractParametersRv));
        auto cells = message.mutable_cells();
        for (uint32_t contractIndex = 0; relativeValue->_numberOfAllContracts; ++contractIndex) {
            RvContract& contract = relativeValue->_contracts[contractIndex];
            if (contract._traded) {
                const double mkt = relativeValue->_overdeterminedPrices.getValue(0, contractIndex);
                setCell(cells, contract._internalId, ContractParameterRVColumns::Mkt, std::to_string(mkt));
            }
        }
    }

    void populateContractQuotes(proto::GridUpdate& message, RelativeValue* relativeValue) {
        message.set_gridid(toUInt(WindowIds::ContractParametersRv));
        auto cells = message.mutable_cells();
        for (uint32_t contractIndex = 0; relativeValue->_numberOfAllContracts; ++contractIndex) {
            RvContract& contract = relativeValue->_contracts[contractIndex];
            if (contract._traded) {
                Quote& bid = contract._quotes[BID];
                setCell(cells, contract._internalId, ContractParameterRVColumns::BidPrice, std::to_string(bid._bestPrice));
                setCell(cells, contract._internalId, ContractParameterRVColumns::BidQty, std::to_string(bid._bestSize));
                RvContract& bidBestLean = relativeValue->_contracts[bid._bestLeanIndex];
                setCell(cells, contract._internalId, ContractParameterRVColumns::BidLean, bidBestLean._definition._name);

                Quote& ask = contract._quotes[ASK];
                setCell(cells, contract._internalId, ContractParameterRVColumns::AskPrice, std::to_string(ask._bestPrice));
                setCell(cells, contract._internalId, ContractParameterRVColumns::AskQty, std::to_string(ask._bestSize));
                RvContract& askBestLean = relativeValue->_contracts[ask._bestLeanIndex];
                setCell(cells, contract._internalId, ContractParameterRVColumns::AskLean, askBestLean._definition._name);
            }
        }
    }

    void populateVectorStf(proto::GridUpdate& vectorMessage, RelativeValue* relativeValue) {
        vectorMessage.set_gridid(toUInt(WindowIds::VectorParameters));
        auto cells = vectorMessage.mutable_cells();
        for (uint32_t vectorIndex = 0; vectorIndex < relativeValue->_numberOfVectors; ++vectorIndex) {
            double stf = relativeValue->_shortTermFairsInVector.getValue(vectorIndex);
            setCell(cells, vectorIndex, VectorParametersColumns::STF, std::to_string(stf));
        }
    }

    void populateContractStf(proto::GridUpdate& message, RelativeValue* relativeValue) {
        message.set_gridid(toUInt(WindowIds::ContractParametersRv));
        auto cells = message.mutable_cells();
        for (uint32_t contractIndex = 0; relativeValue->_numberOfAllContracts; ++contractIndex) {
            RvContract& contract = relativeValue->_contracts[contractIndex];
            if (contract._traded) {
                double stf = relativeValue->_shortTermFairsForContracts.getValue(contractIndex);
                setCell(cells, contract._internalId, ContractParameterRVColumns::STF, std::to_string(stf));
            }
        }
    }

    void populateContractPosition(uint32_t contractIndex, proto::GridUpdate& message, RelativeValue* relativeValue) {
        message.set_gridid(toUInt(WindowIds::ContractParametersRv));
        auto cells = message.mutable_cells();
        ContractDefinition& def = relativeValue->_contracts[contractIndex]._definition;
        if (def._numberOfLegs > 1) {
            for (uint32_t li = 0; li < def._numberOfLegs; ++li) {
                ContractDefinition::Leg& leg = def._legs[li];
                double position = relativeValue->_contractPositions.getValue(leg._index);
                setCell(cells, leg._index, ContractParameterRVColumns::Position, std::to_string(position));
            }
        } else {
            double position = relativeValue->_contractPositions.getValue(contractIndex);
            setCell(cells, contractIndex, ContractParameterRVColumns::Position, std::to_string(position));
        }
    }

    void populateVectorPosition(proto::GridUpdate& message, RelativeValue* relativeValue) {
        message.set_gridid(toUInt(WindowIds::VectorParameters));
        auto cells = message.mutable_cells();
        for (uint32_t vectorIndex = 0; vectorIndex < relativeValue->_numberOfVectors; ++vectorIndex) {
            double position = relativeValue->_positionsInVector.getValue(vectorIndex);
            setCell(cells, vectorIndex, VectorParametersColumns::Position, std::to_string(position));
        }

    }

    void populateGUIInitialization(proto::GUIInitialization& message, Matrix& pricingMatrix, Matrix& riskMatrix) {
        auto definitions = message.mutable_definitions();

        proto::GridDefinition* definition = definitions->Add();
        populateVectorParametersDefinition(definition);

        definition = definitions->Add();
        populateContractParameterRVColumns(definition);

        definition = definitions->Add();
        populateLeanMatrix(definition);

        definition = definitions->Add();
        populatePricingConfig(definition);

        definition = definitions->Add();
        populatePricingMatrix(definition, pricingMatrix);

        definition = definitions->Add();
        populateRiskMatrix(definition, riskMatrix);

        definition = definitions->Add();
        populateMainControl(definition);

        definition = definitions->Add();
        populateManualFillsEntry(definition);

        definition = definitions->Add();
        populateFills(definition);

        definition = definitions->Add();
        populateErrors(definition);

        definition = definitions->Add();
        populateGatewayStatuses(definition);

        definition = definitions->Add();
        populateMarketMakerParameters(definition);
    }

    void populateMarketMakerParameters(proto::GridDefinition* definition) {
        definition->set_gridid(static_cast<uint32_t>(WindowIds::MarketMakerParameters));

        auto columnHeader = definition->mutable_columnheader();

        addHeaderCell(columnHeader, MarketMakerParametersColumns::QuoteExternal, "QuoteExternal");
        addHeaderCell(columnHeader, MarketMakerParametersColumns::QuoteSize, "QuoteSize");
        addHeaderCell(columnHeader, MarketMakerParametersColumns::Width, "Width");
        addHeaderCell(columnHeader, MarketMakerParametersColumns::JoinSize, "JoinSize");
        addHeaderCell(columnHeader, MarketMakerParametersColumns::LeaveSize, "LeaveSize");
        addHeaderCell(columnHeader, MarketMakerParametersColumns::PayUpQty, "PayUpQty");
        addHeaderCell(columnHeader, MarketMakerParametersColumns::PayUpOrders, "PayUpOrders");
        addHeaderCell(columnHeader, MarketMakerParametersColumns::PayUpMultiple, "PayUpMultiple");
        addHeaderCell(columnHeader, MarketMakerParametersColumns::MaxPosInterrnal, "MaxPosInternal");
        addHeaderCell(columnHeader, MarketMakerParametersColumns::MaxPosExternal, "MaxPosExternal");
        addHeaderCell(columnHeader, MarketMakerParametersColumns::Commission, "Commission");

        auto rowHeader = definition->mutable_columnheader();

        for (uint32_t contractIndex = 0; contractIndex < _config->_marketMakerConfigs.size(); ++contractIndex) {
            MarketMakerConfig& config = _config->_marketMakerConfigs.at(contractIndex);
            uint32_t internalId = getInternalId(config._contract);
            addHeaderCell(rowHeader,internalId, config._contract);
        }

        auto cells = definition->mutable_cells();

        for (uint32_t contractIndex = 0; contractIndex < _config->_marketMakerConfigs.size(); ++contractIndex) {
            MarketMakerConfig& config = _config->_marketMakerConfigs.at(contractIndex);
            setCell(cells, contractIndex, MarketMakerParametersColumns::QuoteExternal, std::to_string(config._quoteExternal));
            setCell(cells, contractIndex, MarketMakerParametersColumns::QuoteSize, std::to_string(config._quoteSize));
            setCell(cells, contractIndex, MarketMakerParametersColumns::Width, std::to_string(config._width));
            setCell(cells, contractIndex, MarketMakerParametersColumns::JoinSize, std::to_string(config._joinSize));
            setCell(cells, contractIndex, MarketMakerParametersColumns::LeaveSize, std::to_string(config._leaveSize));
            setCell(cells, contractIndex, MarketMakerParametersColumns::PayUpQty, std::to_string(config._payUpQty));
            setCell(cells, contractIndex, MarketMakerParametersColumns::PayUpOrders, std::to_string(config._payUpOrders));
            setCell(cells, contractIndex, MarketMakerParametersColumns::PayUpMultiple, std::to_string(config._payUpMultiple));
            setCell(cells, contractIndex, MarketMakerParametersColumns::MaxPosInterrnal, std::to_string(config._maxPosInternal));
            setCell(cells, contractIndex, MarketMakerParametersColumns::MaxPosExternal, std::to_string(config._maxPosExternal));
            setCell(cells, contractIndex, MarketMakerParametersColumns::Commission, std::to_string(config._commission));
        }
    }

    void populateGatewayStatuses(proto::GridDefinition* definition) {
        definition->set_gridid(static_cast<uint32_t>(WindowIds::GatewayStatuses));

        auto columnHeader = definition->mutable_columnheader();

        addHeaderCell(columnHeader, GatewayStatusColumns::Exchange, "Exchange");
        addHeaderCell(columnHeader, GatewayStatusColumns::Status, "Status");

        auto rowHeader = definition->mutable_rowheader();

        addHeaderCell(rowHeader, 1, "");

        auto cells = definition->mutable_cells();

        setCell(cells, 0, GatewayStatusColumns::Exchange, "ICE");
        setCell(cells, 0, GatewayStatusColumns::Status, "");
    }

    void populateErrors(proto::GridDefinition* definition) {
        definition->set_gridid(static_cast<uint32_t>(WindowIds::Errors));

        auto columnHeader = definition->mutable_columnheader();

        addHeaderCell(columnHeader, ErrorsColumns::Time, "Time");
        addHeaderCell(columnHeader, ErrorsColumns::Message, "Message");
    }

    void populateFills(proto::GridDefinition* definition) {
        definition->set_gridid(static_cast<uint32_t>(WindowIds::FillsDisplay));

        auto columnHeader = definition->mutable_columnheader();

        addHeaderCell(columnHeader, FillsColumns::Contract, "Contract");
        addHeaderCell(columnHeader, FillsColumns::Side, "Side");
        addHeaderCell(columnHeader, FillsColumns::Price, "Price");
        addHeaderCell(columnHeader, FillsColumns::Size, "Size");
    }

    void populateMainControl(proto::GridDefinition* definition) {
        definition->set_gridid(static_cast<uint32_t>(WindowIds::MainControl));

        auto columnHeader = definition->mutable_columnheader();

        addHeaderCell(columnHeader, MainControlAction::Enabled, "Enabled");
        addHeaderCell(columnHeader, MainControlAction::Quoting, "Quoting");

        auto rowHeader = definition->mutable_rowheader();

        addHeaderCell(rowHeader, StrategyId::MarketMaker, "MarketMaker");
        addHeaderCell(rowHeader, StrategyId::RelativeValue, "RelativeValue");

        auto cells = definition->mutable_cells();

        for (uint32_t index = 0; index < 2; ++index) {
            setCell(cells, index, MainControlAction::Enabled, std::to_string(proto::SystemState::SystemStateDisabled));
            setCell(cells, index, MainControlAction::Quoting, std::to_string(proto::SystemState::SystemStateDisabled));
        }
    }

    template <typename HeaderT, typename EnumValue>
    void addHeaderCell(HeaderT* header, EnumValue id, const std::string& label) {
        auto headerCell = header->Add();
        headerCell->set_id(static_cast<uint32_t>(id));
        headerCell->set_label(label);
    }

    void populateManualFillsEntry(proto::GridDefinition* definition) {
        definition->set_gridid(static_cast<uint32_t>(WindowIds::ManualFillsEntry));

        auto columnHeader = definition->mutable_columnheader();

        addHeaderCell(columnHeader, ManualFillsEntryColumns::Contract, "Contract");
        addHeaderCell(columnHeader, ManualFillsEntryColumns::Size, "Size");
        addHeaderCell(columnHeader, ManualFillsEntryColumns::Strategy, "Strategy");

        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t index = 0; index < 6; ++index) {
            std::string name;
            name.append(std::to_string(index)).append(". ");
            addHeaderCell(rowHeader, index, name);
        }

        auto cells = definition->mutable_cells();

        for (uint32_t index = 0; index < 6; ++index) {
            setCell(cells, index, ManualFillsEntryColumns::Contract, "");
            setCell(cells, index, ManualFillsEntryColumns::Size, "");
            setCell(cells, index, ManualFillsEntryColumns::Strategy, "");
        }
    }

    void populateRiskMatrix(proto::GridDefinition* definition, Matrix& riskMatrix) {
        definition->set_gridid(static_cast<uint32_t>(WindowIds::RiskMatrix));

        auto columnHeader = definition->mutable_columnheader();
        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t index = 0; index < _config->_numberOfOutrights; ++index) {
            ConfigContract& contract = _config->_allContracts[index];
            addHeaderCell(columnHeader, index, contract._name);
        }

        for (uint32_t index = 0; index < _config->_vectorConfigs.size(); ++index) {
            std::string name;
            name.append("V").append(std::to_string(index));
            addHeaderCell(rowHeader, index, name);
        }

        auto cells = definition->mutable_cells();

        for (uint32_t row = 0; row < riskMatrix._numberOfRows; ++row) {
            for (uint32_t col = 0; col < riskMatrix._numberOfColumns; ++col) {
                setCell(cells, row, col, std::to_string(riskMatrix.getValue(row, col)));
            }
        }
    }

    void populatePricingMatrix(proto::GridDefinition* definition, Matrix& pricingMatrix) {
        definition->set_gridid(static_cast<uint32_t>(WindowIds::PricingMatrix));

        auto columnHeader = definition->mutable_columnheader();
        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t index = 0; index < _config->_numberOfOutrights; ++index) {
            ConfigContract& contract = _config->_allContracts[index];
            addHeaderCell(columnHeader, index, contract._name);
        }

        for (uint32_t index = 0; index < _config->_vectorConfigs.size(); ++index) {
            std::string name;
            name.append("V").append(std::to_string(index));
            addHeaderCell(rowHeader, index, name);
        }

        auto cells = definition->mutable_cells();

        for (uint32_t row = 0; row < pricingMatrix._numberOfRows; ++row) {
            for (uint32_t col = 0; col < pricingMatrix._numberOfColumns; ++col) {
                setCell(cells, row, col, std::to_string(pricingMatrix.getValue(row, col)));
            }
        }
    }

    void populatePricingConfig(proto::GridDefinition* definition) {
        definition->set_gridid(static_cast<uint32_t>(WindowIds::PricingConfig));

        auto columnHeader = definition->mutable_columnheader();

        addHeaderCell(columnHeader, PricingConfigsColumns::Weight, "Weight");
        addHeaderCell(columnHeader, PricingConfigsColumns::UseWeighted, "UseWeighted");
        addHeaderCell(columnHeader, PricingConfigsColumns::EstablishedLevel, "EstablishedLevel");

        auto rowHeader = definition->mutable_columnheader();

        for (uint32_t contractIndex = 0; contractIndex < _config->_pricingConfigs.size(); ++contractIndex) {
            PricingConfig& config = _config->_pricingConfigs.at(contractIndex);
            uint32_t internalId = getInternalId(config._contract);
            addHeaderCell(rowHeader,internalId, config._contract);
        }

        auto cells = definition->mutable_cells();

        for (uint32_t contractIndex = 0; contractIndex < _config->_pricingConfigs.size(); ++contractIndex) {
            PricingConfig& config = _config->_pricingConfigs.at(contractIndex);
            setCell(cells, contractIndex, PricingConfigsColumns::Weight, std::to_string(config._weight));
            setCell(cells, contractIndex, PricingConfigsColumns::UseWeighted, std::to_string(config._useWeightedMid));
            setCell(cells, contractIndex, PricingConfigsColumns::EstablishedLevel, std::to_string(config._establishedLevel));
        }
    }

    void populateLeanMatrix(proto::GridDefinition* definition) {
        definition->set_gridid(static_cast<uint32_t>(WindowIds::LeanMatrix));

        auto columnHeader = definition->mutable_columnheader();
        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t index = 0; index < _config->_leanMatrix.size(); ++index) {
            VectorWithContract& config = _config->_leanMatrix[index];
            uint32_t internalId = getInternalId(config._contract);
            addHeaderCell(columnHeader, internalId, config._contract);
            addHeaderCell(rowHeader, internalId, config._contract);
        }

        auto cells = definition->mutable_cells();

        for (uint32_t row = 0; row < _config->_leanMatrix.size(); ++row) {
            for (uint32_t col = 0; col < _config->_leanMatrix.size(); ++col) {
                int value = _config->_leanMatrix[row]._values[col];
                setCell(cells, row, col, std::to_string(value));
            }
        }
    }

    void populateContractParameterRVColumns(proto::GridDefinition* definition) {
        definition->set_gridid(static_cast<uint32_t>(WindowIds::ContractParametersRv));

        auto columnHeader = definition->mutable_columnheader();

        addHeaderCell(columnHeader, ContractParameterRVColumns::Position, "Position");
        addHeaderCell(columnHeader, ContractParameterRVColumns::BidLean, "BidLean");
        addHeaderCell(columnHeader, ContractParameterRVColumns::AskLean, "AskLean");
        addHeaderCell(columnHeader, ContractParameterRVColumns::BidPrice, "BidPrice");
        addHeaderCell(columnHeader, ContractParameterRVColumns::AskPrice, "AskPrice");
        addHeaderCell(columnHeader, ContractParameterRVColumns::BidQty, "BidQty");
        addHeaderCell(columnHeader, ContractParameterRVColumns::AskQty, "AskQty");
        addHeaderCell(columnHeader, ContractParameterRVColumns::STF, "STF");
        addHeaderCell(columnHeader, ContractParameterRVColumns::Mkt, "Mkt");
        addHeaderCell(columnHeader, ContractParameterRVColumns::QuoteExternal, "QuoteExternal");
        addHeaderCell(columnHeader, ContractParameterRVColumns::MaxHedgeBuffer, "MaxHedgeBuffer");
        addHeaderCell(columnHeader, ContractParameterRVColumns::Commission, "Commission");

        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t index = 0; index < _config->_relativeValueConfigs.size(); ++index) {
            RelativeValueConfig& config = _config->_relativeValueConfigs[index];
            uint32_t internalId = getInternalId(config._contract);
            addHeaderCell(rowHeader, internalId, config._contract);
        }

        auto cells = definition->mutable_cells();

        for (uint32_t index = 0; index < _config->_relativeValueConfigs.size(); ++index) {
            RelativeValueConfig& config = _config->_relativeValueConfigs[index];
            setCell(cells, index, ContractParameterRVColumns::Position, std::to_string(config._position));
            setCell(cells, index, ContractParameterRVColumns::BidLean, "");
            setCell(cells, index, ContractParameterRVColumns::AskLean, "");
            setCell(cells, index, ContractParameterRVColumns::BidPrice, "");
            setCell(cells, index, ContractParameterRVColumns::AskPrice, "");
            setCell(cells, index, ContractParameterRVColumns::BidQty, "");
            setCell(cells, index, ContractParameterRVColumns::AskQty, "");
            setCell(cells, index, ContractParameterRVColumns::STF, "");
            setCell(cells, index, ContractParameterRVColumns::Mkt, "");
            setCell(cells, index, ContractParameterRVColumns::QuoteExternal, std::to_string(config._quoteExternal));
            setCell(cells, index, ContractParameterRVColumns::MaxHedgeBuffer, std::to_string(config._MinLeanBuffer));
            setCell(cells, index, ContractParameterRVColumns::Commission, std::to_string(config._commision));
        }
    }

    uint32_t getInternalId(const std::string& name) {
        auto it = _symbolToInterId->find(name);
        if (it != _symbolToInterId->end()) {
            return it->second;
        }
        throw std::runtime_error("could not map symbol name to internal id. " + name);
    }

    void populateVectorParametersDefinition(proto::GridDefinition* definition) {
        definition->set_gridid(static_cast<uint32_t>(WindowIds::VectorParameters));

        auto columnHeader = definition->mutable_columnheader();

        addHeaderCell(columnHeader, VectorParametersColumns::Position, "Position");
        addHeaderCell(columnHeader, VectorParametersColumns::Mkt, "Mkt");
        addHeaderCell(columnHeader, VectorParametersColumns::STF, "STF");
        addHeaderCell(columnHeader, VectorParametersColumns::LTF, "LTF");
        addHeaderCell(columnHeader, VectorParametersColumns::MaxPos, "MaxPos");
        addHeaderCell(columnHeader, VectorParametersColumns::Range, "Range");
        addHeaderCell(columnHeader, VectorParametersColumns::Width, "Width");
        addHeaderCell(columnHeader, VectorParametersColumns::Step, "Step");
        addHeaderCell(columnHeader, VectorParametersColumns::EMA_ON, "EMA_ON");
        addHeaderCell(columnHeader, VectorParametersColumns::EMA_Time, "EMA_Time");
        addHeaderCell(columnHeader, VectorParametersColumns::SetToMarket, "SetToMarket");

        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t vectorIndex = 0; vectorIndex < _config->_vectorConfigs.size(); ++vectorIndex) {
            std::string name;
            name.append("V").append(std::to_string(vectorIndex));
            addHeaderCell(rowHeader, vectorIndex, name);
        }

        auto cells = definition->mutable_cells();

        for (uint32_t vectorIndex = 0; vectorIndex < _config->_vectorConfigs.size(); ++vectorIndex) {
            VectorConfig& config = _config->_vectorConfigs[vectorIndex];
            setCell(cells, vectorIndex, VectorParametersColumns::Position, "");
            setCell(cells, vectorIndex, VectorParametersColumns::Mkt, "");
            setCell(cells, vectorIndex, VectorParametersColumns::STF, "");
            setCell(cells, vectorIndex, VectorParametersColumns::LTF, std::to_string(config._longTermFair));
            setCell(cells, vectorIndex, VectorParametersColumns::MaxPos, std::to_string(config._maxPos));
            setCell(cells, vectorIndex, VectorParametersColumns::Range, std::to_string(config._range));
            setCell(cells, vectorIndex, VectorParametersColumns::Width, std::to_string(config._width));
            setCell(cells, vectorIndex, VectorParametersColumns::Step, std::to_string(config._step));
            setCell(cells, vectorIndex, VectorParametersColumns::EMA_ON, std::to_string(config._useEMA));
            setCell(cells, vectorIndex, VectorParametersColumns::EMA_Time, std::to_string(config._EMALength));
            setCell(cells, vectorIndex, VectorParametersColumns::SetToMarket, std::to_string(config._setToMarket));
        }
    }

    template <typename CellsT, typename ColT>
    static void setCell(CellsT cells, uint32_t row, ColT col, const std::string& value) {
        auto cell = cells->Add();
        cell->set_row(row);
        cell->set_col(static_cast<uint32_t>(col));
        cell->set_value(value);
    }

private:
    ViperConfig* _config;
    std::unordered_map<std::string, uint32_t>* _symbolToInterId;
    uint32_t _fillNumber = 0;
};

};

#endif //VIPER_VIPER_CONFIG_H
