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
    }

    void populateErrors(proto::GridDefinition* definition) {
    }

    void populateFills(proto::GridDefinition* definition) {
    }

    void populateMainControl(proto::GridDefinition* definition) {
    }

    void populateManualFillsEntry(proto::GridDefinition* definition) {
        definition->set_gridid(static_cast<uint32_t>(WindowIds::ManualFills));

        auto columnHeader = definition->mutable_columnheader();

        auto headerCell = columnHeader->Add();
        headerCell->set_id(static_cast<uint32_t>(ManualFillsColumns::Contract));
        headerCell->set_label("Contract");

        headerCell = columnHeader->Add();
        headerCell->set_id(static_cast<uint32_t>(ManualFillsColumns::Size));
        headerCell->set_label("Size");

        headerCell = columnHeader->Add();
        headerCell->set_id(static_cast<uint32_t>(ManualFillsColumns::Strategy));
        headerCell->set_label("Strategy");

        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t index = 0; index < 6; ++index) {
            headerCell = rowHeader->Add();
            headerCell->set_id(index);
            headerCell->set_label("Fill-" + index);
        }
    }

    void populateRiskMatrix(proto::GridDefinition* definition) {
        definition->set_gridid(static_cast<uint32_t>(WindowIds::RiskMatrix));

        auto columnHeader = definition->mutable_columnheader();
        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t index = 0; index < _config._numberOfOutrights; ++index) {
            ConfigContract& contract = _config._allContracts[index];
            auto headerCell = columnHeader->Add();
            headerCell->set_id(index);
            headerCell->set_label(contract._name);
        }

        for (uint32_t index = 0; index < _config._vectorConfigs.size(); ++index) {
            auto headerCell = rowHeader->Add();
            headerCell->set_id(index);
            headerCell->set_label("v" + index);
        }
    }

    void populatePricingMatrix(proto::GridDefinition* definition) {
        definition->set_gridid(static_cast<uint32_t>(WindowIds::PricingMatrix));

        auto columnHeader = definition->mutable_columnheader();
        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t index = 0; index < _config._numberOfOutrights; ++index) {
            ConfigContract& contract = _config._allContracts[index];
            auto headerCell = columnHeader->Add();
            headerCell->set_id(index);
            headerCell->set_label(contract._name);
        }

        for (uint32_t index = 0; index < _config._vectorConfigs.size(); ++index) {
            auto headerCell = rowHeader->Add();
            headerCell->set_id(index);
            headerCell->set_label("v" + index);
        }
    }

    void populatePricingConfig(proto::GridDefinition* definition) {
        definition->set_gridid(static_cast<uint32_t>(WindowIds::PricingConfig));

        auto columnHeader = definition->mutable_columnheader();

        auto headerCell = columnHeader->Add();
        headerCell->set_id(static_cast<uint32_t>(PricingConfigsColumns::Weight));
        headerCell->set_label("Weight");

        headerCell = columnHeader->Add();
        headerCell->set_id(static_cast<uint32_t>(PricingConfigsColumns::UseWeighted));
        headerCell->set_label("UseWeighted");

        headerCell = columnHeader->Add();
        headerCell->set_id(static_cast<uint32_t>(PricingConfigsColumns::EstablishedLevel));
        headerCell->set_label("EstablishedLevel");

        auto rowHeader = definition->mutable_columnheader();

        for (uint32_t contractIndex = 0; contractIndex < _config._pricingConfigs.size(); ++contractIndex) {
            PricingConfig& config = _config._pricingConfigs.at(contractIndex);
            uint32_t internalId = getInternalId(config._contract);
            headerCell = rowHeader->Add();
            headerCell->set_label(config._contract);
            headerCell->set_id(internalId);
        }
    }

    void populateLeanMatrix(proto::GridDefinition* definition) {
        definition->set_gridid(static_cast<uint32_t>(WindowIds::LeanMatrix));

        auto columnHeader = definition->mutable_columnheader();
        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t index = 0; index < _config._leanMatrix.size(); ++index) {
            VectorWithContract& config = _config._leanMatrix[index];
            uint32_t internalId = getInternalId(config._contract);

            auto headerCell = columnHeader->Add();
            headerCell->set_id(internalId);
            headerCell->set_label(config._contract);

            headerCell = rowHeader->Add();
            headerCell->set_id(internalId);
            headerCell->set_label(config._contract);
        }
    }

    void populateContractParameterRVColumns(proto::GridDefinition* definition) {
        definition->set_gridid(static_cast<uint32_t>(WindowIds::ContractParametersRv));

        auto columnHeader = definition->mutable_columnheader();

        auto headerCell = columnHeader->Add();
        headerCell->set_id(static_cast<uint32_t>(ContractParameterRVColumns::Position));
        headerCell->set_label("Position");

        headerCell = columnHeader->Add();
        headerCell->set_id(static_cast<uint32_t>(ContractParameterRVColumns::Lean));
        headerCell->set_label("Lean");

        headerCell = columnHeader->Add();
        headerCell->set_id(static_cast<uint32_t>(ContractParameterRVColumns::BidPrice));
        headerCell->set_label("BidPrice");

        headerCell = columnHeader->Add();
        headerCell->set_id(static_cast<uint32_t>(ContractParameterRVColumns::AskPrice));
        headerCell->set_label("AskPrice");

        headerCell = columnHeader->Add();
        headerCell->set_id(static_cast<uint32_t>(ContractParameterRVColumns::BidQty));
        headerCell->set_label("BidQty");

        headerCell = columnHeader->Add();
        headerCell->set_id(static_cast<uint32_t>(ContractParameterRVColumns::AskQty));
        headerCell->set_label("AskQty");

        headerCell = columnHeader->Add();
        headerCell->set_id(static_cast<uint32_t>(ContractParameterRVColumns::STF));
        headerCell->set_label("STF");

        headerCell = columnHeader->Add();
        headerCell->set_id(static_cast<uint32_t>(ContractParameterRVColumns::Mkt));
        headerCell->set_label("Mkt");

        headerCell = columnHeader->Add();
        headerCell->set_id(static_cast<uint32_t>(ContractParameterRVColumns::QuoteExternal));
        headerCell->set_label("QuoteExternal");

        headerCell = columnHeader->Add();
        headerCell->set_id(static_cast<uint32_t>(ContractParameterRVColumns::MaxHedgeBuffer));
        headerCell->set_label("MaxHedgeBuffer");

        headerCell = columnHeader->Add();
        headerCell->set_id(static_cast<uint32_t>(ContractParameterRVColumns::Commission));
        headerCell->set_label("Commission");

        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t index = 0; index < _config._relativeValueConfigs.size(); ++index) {
            RelativeValueConfig& config = _config._relativeValueConfigs[index];
            uint32_t internalId = getInternalId(config._contract);
            headerCell = rowHeader->Add();
            headerCell->set_label(config._contract);
            headerCell->set_id(internalId);
        }
    }

    uint32_t getInternalId(const std::string& name) {
        return 0;
    }

    void populateVectorParametersDefinition(proto::GridDefinition* definition) {
        definition->set_gridid(static_cast<uint32_t>(WindowIds::VectorParameters));

        auto columnHeader = definition->mutable_columnheader();

        auto headerCell = columnHeader->Add();
        headerCell->set_id(static_cast<uint32_t>(VectorParametersColumns::Mkt));
        headerCell->set_label("Market");

        headerCell = columnHeader->Add();
        headerCell->set_id(static_cast<uint32_t>(VectorParametersColumns::STF));
        headerCell->set_label("STF");

        headerCell = columnHeader->Add();
        headerCell->set_id(static_cast<uint32_t>(VectorParametersColumns::LTF));
        headerCell->set_label("LTF");

        headerCell = columnHeader->Add();
        headerCell->set_id(static_cast<uint32_t>(VectorParametersColumns::MaxPos));
        headerCell->set_label("MaxPos");

        headerCell = columnHeader->Add();
        headerCell->set_id(static_cast<uint32_t>(VectorParametersColumns::Range));
        headerCell->set_label("Range");

        headerCell = columnHeader->Add();
        headerCell->set_id(static_cast<uint32_t>(VectorParametersColumns::Width));
        headerCell->set_label("Width");

        headerCell = columnHeader->Add();
        headerCell->set_id(static_cast<uint32_t>(VectorParametersColumns::Step));
        headerCell->set_label("Step");

        headerCell = columnHeader->Add();
        headerCell->set_id(static_cast<uint32_t>(VectorParametersColumns::EMA));
        headerCell->set_label("EMA");

        headerCell = columnHeader->Add();
        headerCell->set_id(static_cast<uint32_t>(VectorParametersColumns::EMA_ON));
        headerCell->set_label("EMA_ON");

        headerCell = columnHeader->Add();
        headerCell->set_id(static_cast<uint32_t>(VectorParametersColumns::EMA_Time));
        headerCell->set_label("EMA_Time");

        headerCell = columnHeader->Add();
        headerCell->set_id(static_cast<uint32_t>(VectorParametersColumns::SetToMarket));
        headerCell->set_label("SetToMarket");

        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t vectorIndex = 0; vectorIndex < _config._vectorConfigs.size(); ++vectorIndex) {
            headerCell = rowHeader->Add();
            headerCell->set_label("v" + vectorIndex);
            headerCell->set_id(vectorIndex);
        }
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

