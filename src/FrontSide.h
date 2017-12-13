#ifndef VIPER_FRONT_SIDE_H
#define VIPER_FRONT_SIDE_H

#include <hf/Service.h>
#include <hf/ConfigManager.h>
#include <hf/ResourceManager.h>
#include "ClientAcceptor.h"
#include "ViperConfig.h"
#include "RelativeValue.h"
#include <queue>
#include <md/sym/SymbolsResource.h>

namespace Python {

class FrontSide : public virtual hf::Service {
public:
    FrontSide() {
    }

    void onCreate(const libconfig::Setting& config) override {
        const auto host = hf::ConfigManager::getString(config, "host");
        const auto port = hf::ConfigManager::getLong(config, "port");
        _clientAcceptor = new ClientsAcceptor(host, port);
    }

    void onPostCreate(const libconfig::Setting& config) override {
        const auto symbolsName = hf::ConfigManager::getString(config, "symbols");
        _symbolsResource = dynamic_cast<md::sym::SymbolsResource *> (hf::ResourceManager::getResourceP(symbolsName));
        if (!_symbolsResource) {
           throw std::runtime_error("could not get symbols resource");
        }

        const auto name = hf::ConfigManager::getString(config, "config_file");
        _config.initialize(name, *_symbolsResource);

        const std::string relativeValueName = config["RelativeValue"];
        auto relativeValue = dynamic_cast<RelativeValue*> (hf::ResourceManager::getResourceP(relativeValueName));

        const std::string marketMakersName = config["MarketMakers"];
        auto marketMakers = dynamic_cast<RelativeValue*> (hf::ResourceManager::getResourceP(marketMakersName));

        auto queueFromMMToFront = createQueue();
        auto queueFromFrontToMM = createQueue();
        auto queueFromRVToFront = createQueue();
        auto queueFromFrontToRV = createQueue();
        auto queueFromMMToRV = createQueue();
        auto queueFromRVToMM = createQueue();

        _inFromMM = queueFromMMToFront;
        _inFromRV = queueFromRVToFront;
        _outToMM = queueFromFrontToMM;
        _outToRV = queueFromFrontToRV;

        relativeValue->init(_config, queueFromFrontToRV, queueFromRVToFront, queueFromMMToRV, queueFromRVToMM);
        marketMakers->init(_config, queueFromFrontToMM, queueFromMMToFront, queueFromRVToMM, queueFromMMToRV);
    }

    ViperQueue* createQueue() {
        auto pool = new ViperPool();
        pool->create();
        auto queue = new ViperQueue();
        queue->create();
        queue->set_pool(pool);
        return queue;
    }

    void onRun() override {
        _inFromMM->resetR();
        _inFromRV->resetR();
        _outToMM->resetW();
        _outToRV->resetW();

        while (BOOST_LIKELY(!hf::Service::isExitAll())) {
            _clientAcceptor->runIteration();
            processOutgoingMessagesFromRelativeValue();
            processOutgoingMessagesFromMarketMaker();
        }
    }

    void processOutgoingMessagesFromRelativeValue() {
        while (true) {
            uint64_t idx = _inFromRV->getNext();

            if (BOOST_UNLIKELY(idx == 0))
                return;

            _inFromRV->freeNext();

            auto& message = _inFromRV->pool()->getMessage(idx);

            switch (message._type) {
                default:
                    break;
            }
        }
    }

    void processOutgoingMessagesFromMarketMaker() {
        while (true) {
            uint64_t idx = _inFromMM->getNext();

            if (BOOST_UNLIKELY(idx == 0))
                return;

            _inFromMM->freeNext();

            auto& message = _inFromMM->pool()->getMessage(idx);

            switch (message._type) {
                case ViperMessageType::GridUpdateAccepted: {
                    GridUpdateReply& reply = reinterpret_cast<GridUpdateReply &> (message._data[0]);
                    onGridUpdateAccepted(reply);
                    break;
                }
                case ViperMessageType::GridUpdateRejected: {
                    GridUpdateReply& reply = reinterpret_cast<GridUpdateReply &> (message._data[0]);
                    onGridUpdateRejected(reply);
                    break;
                }
                default:
                    break;
            }
        }
    }

    void onGridUpdate(proto::GridUpdate& update) {
        auto it = _gridUpdates.find(update.gridid());
        if (it != _gridUpdates.end()) {
            GridUpdate& gridUpdate = it->second;
            if (gridUpdate._ready) {
                gridUpdate._update = update;
                gridUpdate._ready = false;
                const bool isForRv = gridUpdate._strategyId == StrategyId::RelativeValue;
                if (isForRv) {
                    sendGridUpdateToRv(gridUpdate);
                } else {
                    sendGridUpdateToMM(gridUpdate);
                }
            } else {
                logError("received grid update but another already in flight");
            }
        } else {
            logError("received grid update for unknown grid " + update.gridid());
        }
    }

    void sendGridUpdateToRv(GridUpdate& update) {
        auto &outMessage = _outToRV->pool()->getNextMessage();
        outMessage._type = ViperMessageType::GridUpdate;

        GridUpdateMessage& message = reinterpret_cast<GridUpdateMessage &> (outMessage._data[0]);
        message._gridUpdate = &update;

        const auto outIdx = _outToRV->pool()->append(sizeof(GridUpdateMessage) + sizeof(ViperMessageType));
        mb();
        _outToRV->appendNew(outIdx);
    }

    void sendGridUpdateToMM(GridUpdate& update) {
        auto &outMessage = _outToMM->pool()->getNextMessage();
        outMessage._type = ViperMessageType::GridUpdate;

        GridUpdateMessage& reply = reinterpret_cast<GridUpdateMessage &> (outMessage._data[0]);
        reply._gridUpdate = &update;

        const auto outIdx = _outToMM->pool()->append(sizeof(GridUpdateMessage) + sizeof(ViperMessageType));
        mb();
        _outToMM->appendNew(outIdx);
    }

    void onGridUpdateAccepted(GridUpdateReply& reply) {
        auto it = _gridUpdates.find(reply._gridId);
        if (it != _gridUpdates.end()) {
            GridUpdate& gu = it->second;
            _config.applyGridUpdate(gu);
            gu.clear();

        } else {
            logError("Accepted grid update which cannot be found");
        }
    }

    void onGridUpdateRejected(GridUpdateReply& reply) {
        auto it = _gridUpdates.find(reply._gridId);
        if (it != _gridUpdates.end()) {
            GridUpdate& gu = it->second;
            gu.clear();
        } else {
            logError("Rejected grid update which cannot be found");
        }
    }

    void onOtherRequest(ViperMessageType mt)  {
        switch (mt) {
            case ViperMessageType::Login: {
                sendGUIInitialization();
                break;
            }
        }
    }

    void sendGUIInitialization() {
        proto::GUIInitialization message;
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
        populatePricingMatrix(definition);

        definition = definitions->Add();
        populateRiskMatrix(definition);

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
    }

    void populateErrors(proto::GridDefinition* definition) {
        definition->set_gridid(static_cast<uint32_t>(WindowIds::Errors));

        auto columnHeader = definition->mutable_columnheader();

        addHeaderCell(columnHeader, ErrorsColumns::Time, "Time");
        addHeaderCell(columnHeader, ErrorsColumns::Message, "Message");

        auto rowHeader = definition->mutable_rowheader();

        addHeaderCell(rowHeader, 1, "ICE");
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

        addHeaderCell(columnHeader, MainControllColumns::Enabled, "Enabled");
        addHeaderCell(columnHeader, MainControllColumns::Quoting, "Quoting");

        auto rowHeader = definition->mutable_rowheader();

        addHeaderCell(rowHeader, StrategyId::MarketMaker, "MarketMaker");
        addHeaderCell(rowHeader, StrategyId::RelativeValue, "RelativeValue");
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
    }

    void populateRiskMatrix(proto::GridDefinition* definition) {
        definition->set_gridid(static_cast<uint32_t>(WindowIds::RiskMatrix));

        auto columnHeader = definition->mutable_columnheader();
        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t index = 0; index < _config._numberOfOutrights; ++index) {
            ConfigContract& contract = _config._allContracts[index];
            addHeaderCell(columnHeader, index, contract._name);
        }

        for (uint32_t index = 0; index < _config._vectorConfigs.size(); ++index) {
            addHeaderCell(rowHeader, index, "v" + index);
        }
    }

    void populatePricingMatrix(proto::GridDefinition* definition) {
        definition->set_gridid(static_cast<uint32_t>(WindowIds::PricingMatrix));

        auto columnHeader = definition->mutable_columnheader();
        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t index = 0; index < _config._numberOfOutrights; ++index) {
            ConfigContract& contract = _config._allContracts[index];
            addHeaderCell(columnHeader, index, contract._name);
        }

        for (uint32_t index = 0; index < _config._vectorConfigs.size(); ++index) {
            addHeaderCell(rowHeader, index, "v" + index);
        }
    }

    void populatePricingConfig(proto::GridDefinition* definition) {
        definition->set_gridid(static_cast<uint32_t>(WindowIds::PricingConfig));

        auto columnHeader = definition->mutable_columnheader();

        addHeaderCell(columnHeader, PricingConfigsColumns::Weight, "Weight");
        addHeaderCell(columnHeader, PricingConfigsColumns::UseWeighted, "UseWeighted");
        addHeaderCell(columnHeader, PricingConfigsColumns::EstablishedLevel, "EstablishedLevel");

        auto rowHeader = definition->mutable_columnheader();

        for (uint32_t contractIndex = 0; contractIndex < _config._pricingConfigs.size(); ++contractIndex) {
            PricingConfig& config = _config._pricingConfigs.at(contractIndex);
            uint32_t internalId = getInternalId(config._contract);
            addHeaderCell(rowHeader,internalId, config._contract);
        }
    }

    void populateLeanMatrix(proto::GridDefinition* definition) {
        definition->set_gridid(static_cast<uint32_t>(WindowIds::LeanMatrix));

        auto columnHeader = definition->mutable_columnheader();
        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t index = 0; index < _config._leanMatrix.size(); ++index) {
            VectorWithContract& config = _config._leanMatrix[index];
            uint32_t internalId = getInternalId(config._contract);
            addHeaderCell(columnHeader,internalId, config._contract);
            addHeaderCell(rowHeader,internalId, config._contract);
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

        for (uint32_t index = 0; index < _config._relativeValueConfigs.size(); ++index) {
            RelativeValueConfig& config = _config._relativeValueConfigs[index];
            uint32_t internalId = getInternalId(config._contract);
            addHeaderCell(rowHeader, internalId, config._contract);
        }
    }

    uint32_t getInternalId(const std::string& name) {
        return 0;
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

        for (uint32_t vectorIndex = 0; vectorIndex < _config._vectorConfigs.size(); ++vectorIndex) {
            addHeaderCell(rowHeader, vectorIndex, "v" + vectorIndex);
        }

        auto cells = definition->mutable_cells();

        for (uint32_t vectorIndex = 0; vectorIndex < _config._vectorConfigs.size(); ++vectorIndex) {
            VectorConfig& config = _config._vectorConfigs[vectorIndex];
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
    void setCell(CellsT cells, uint32_t row, ColT col, const std::string& value) {
        auto cell = cells->Add();
        cell->set_row(row);
        cell->set_col(static_cast<uint32_t>(col));
        cell->set_value(value);
    }

    void logError(const std::string& message) {
    }

private:
    ClientsAcceptor* _clientAcceptor;

    ViperQueue* _inFromRV;
    ViperQueue* _inFromMM;
    ViperQueue* _outToRV;
    ViperQueue* _outToMM;

    ViperConfig _config;
    std::unordered_map<uint32_t, GridUpdate> _gridUpdates;

    md::sym::SymbolsResource* _symbolsResource;

};

}

#endif //VIPER_FRONT_SIDE_H

