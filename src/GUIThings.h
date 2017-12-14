#ifndef VIPER_GUI_THINGS_H
#define VIPER_GUI_THINGS_H

#include "messages/omsproto.pb.h"
#include "ViperConfig.h"

namespace Python {


class GUIThings {
public:

    void init(ViperConfig* config, std::unordered_map<std::string, uint32_t>* symbolToInterId) {
        _config = config;
        _symbolToInterId = symbolToInterId;
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

        addHeaderCell(columnHeader, MainControllAction::Enabled, "Enabled");
        addHeaderCell(columnHeader, MainControllAction::Quoting, "Quoting");

        auto rowHeader = definition->mutable_rowheader();

        addHeaderCell(rowHeader, StrategyId::MarketMaker, "MarketMaker");
        addHeaderCell(rowHeader, StrategyId::RelativeValue, "RelativeValue");

        auto cells = definition->mutable_cells();

        for (uint32_t index = 0; index < 2; ++index) {
            setCell(cells, index, MainControllAction::Enabled, "");
            setCell(cells, index, MainControllAction::Quoting, "");
        }
    }

    template <typename HeaderT, typename EnumValue>
    void addHeaderCell(HeaderT* header, EnumValue id, const std::string& label) {
        auto headerCell = header->Add();
        headerCell->set_id(static_cast<uint32_t>(id));
        headerCell->set_label("Contract");
    }

    void populateManualFillsEntry(proto::GridDefinition* definition) {
        definition->set_gridid(static_cast<uint32_t>(WindowIds::ManualFillsEntry));

        auto columnHeader = definition->mutable_columnheader();

        addHeaderCell(columnHeader, ManualFillsEntryColumns::Contract, "Contract");
        addHeaderCell(columnHeader, ManualFillsEntryColumns::Size, "Size");
        addHeaderCell(columnHeader, ManualFillsEntryColumns::Strategy, "Strategy");

        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t index = 0; index < 6; ++index) {
            addHeaderCell(rowHeader, index, "Fill-" + index);
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
            addHeaderCell(rowHeader, index, "v" + index);
        }

        auto cells = definition->mutable_cells();

        for (uint32_t row = 0; row < riskMatrix._numberOfRows; ++row) {
            for (uint32_t col = 0; riskMatrix._numberOfColumns; ++col) {
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
            addHeaderCell(rowHeader, index, "v" + index);
        }

        auto cells = definition->mutable_cells();

        for (uint32_t row = 0; row < pricingMatrix._numberOfRows; ++row) {
            for (uint32_t col = 0; pricingMatrix._numberOfColumns; ++col) {
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
        addHeaderCell(columnHeader, ContractParameterRVColumns::Lean, "Lean");
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
            setCell(cells, index, ContractParameterRVColumns::Lean, "");
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
        addHeaderCell(columnHeader, VectorParametersColumns::EMA, "EMA");
        addHeaderCell(columnHeader, VectorParametersColumns::EMA_ON, "EMA_ON");
        addHeaderCell(columnHeader, VectorParametersColumns::EMA_Time, "EMA_Time");
        addHeaderCell(columnHeader, VectorParametersColumns::SetToMarket, "SetToMarket");

        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t vectorIndex = 0; vectorIndex < _config->_vectorConfigs.size(); ++vectorIndex) {
            addHeaderCell(rowHeader, vectorIndex, "v" + vectorIndex);
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
            setCell(cells, vectorIndex, VectorParametersColumns::EMA, "");
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
};

};

#endif //VIPER_VIPER_CONFIG_H
