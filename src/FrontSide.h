#ifndef VIPER_FRONT_SIDE_H
#define VIPER_FRONT_SIDE_H

#include <hf/Service.h>
#include <hf/ConfigManager.h>
#include <hf/ResourceManager.h>
#include "ClientAcceptor.h"
#include "ViperConfig.h"
#include "RelativeValue.h"
#include "MarketMakers.h"
#include <queue>
#include <md/sym/SymbolsResource.h>
#include "GUIThings.h"
#include "CommonHelper.h"

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

        populateSymbolToInterId();

        _guiThings.init(&_config, &_symbolToInterId);

        const std::string relativeValueName = config["RelativeValue"];
        _relativeValue = dynamic_cast<RelativeValue*> (hf::ResourceManager::getResourceP(relativeValueName));

        const std::string marketMakersName = config["MarketMakers"];
        _marketMakers = dynamic_cast<MarketMakersT*> (hf::ResourceManager::getResourceP(marketMakersName));

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

        _relativeValue->init(_config, queueFromFrontToRV, queueFromRVToFront, queueFromMMToRV, queueFromRVToMM);
        _marketMakers->init(_config, queueFromFrontToMM, queueFromMMToFront, queueFromRVToMM, queueFromMMToRV);
    }

    void populateSymbolToInterId() {
        for (auto i = 0; i < _config._allContracts.size(); ++i) {
            auto& contract = _config._allContracts[i];
            _symbolToInterId.insert({contract._name, i});
        }
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
                case ViperMessageType::SwitchChangeReply: {
                    GenericMessage& reply = reinterpret_cast<GenericMessage &> (message._data[0]);
                    _clientAcceptor->sendSwitchChangeReply(toUInt(StrategyId::RelativeValue), toUInt(reply._number_1), std::to_string(reply._number_2), toUInt(WindowIds::MainControl), true);
                    break;
                }
                case ViperMessageType::PositionUpdate: {
                    GenericMessage& reply = reinterpret_cast<GenericMessage &> (message._data[0]);
                    const uint32_t contractIndex = reply._number_1;
                    sendPositionsToGUI(contractIndex);
                    break;
                }
                case ViperMessageType::STFUpdate: {
                    sendStfToGui();
                    break;
                }
                case ViperMessageType::QuotesUpdate: {
                    sendQuotesToGUI();
                    break;
                }
                case ViperMessageType::OverdeterminedPricesUpdate: {
                    sendOverdeterminedPricesUpdate();
                    break;
                }
                case ViperMessageType::LTFUpdate: {
                    sendLtfToGui();
                    break;
                }
                default:
                    break;
            }
        }
    }

    void sendLtfToGui() {
        proto::GridUpdate message;
        message.set_gridid(toUInt(WindowIds::VectorParameters));

        auto cells = message.mutable_cells();

        for (uint32_t vectorIndex = 0; vectorIndex < _relativeValue->_numberOfVectors; ++vectorIndex) {
            const double ltf = _relativeValue->_longTermFairsInVector.getValue(vectorIndex);
            GUIThings::setCell(cells, vectorIndex, VectorParametersColumns::LTF, std::to_string(ltf));
        }

        _clientAcceptor->sendGridUpdate(message);
    }

    void sendOverdeterminedPricesUpdate() {
        proto::GridUpdate message;
        message.set_gridid(toUInt(WindowIds::VectorParameters));

        auto cells = message.mutable_cells();

        for (uint32_t vectorIndex = 0; vectorIndex < _relativeValue->_numberOfVectors; ++vectorIndex) {
            const double mkt = _relativeValue->_newVectorMarketPrices.getValue(vectorIndex);
            GUIThings::setCell(cells, vectorIndex, VectorParametersColumns::Mkt, std::to_string(mkt));
        }

        _clientAcceptor->sendGridUpdate(message);

        proto::GridUpdate contractMessage;
        contractMessage.set_gridid(toUInt(WindowIds::ContractParametersRv));

        cells = message.mutable_cells();

        for (uint32_t contractIndex = 0; _relativeValue->_numberOfAllContracts; ++contractIndex) {
            RvContract& contract = _relativeValue->_contracts[contractIndex];
            if (contract._traded) {
                const double mkt = _relativeValue->_overdeterminedPrices.getValue(0, contractIndex);
                GUIThings::setCell(cells, contract._internalId, ContractParameterRVColumns::Mkt, std::to_string(mkt));
            }
        }

        _clientAcceptor->sendGridUpdate(contractMessage);
    }

    void sendQuotesToGUI() {
        proto::GridUpdate message;
        message.set_gridid(toUInt(WindowIds::ContractParametersRv));

        auto cells = message.mutable_cells();

        for (uint32_t contractIndex = 0; _relativeValue->_numberOfAllContracts; ++contractIndex) {
            RvContract& contract = _relativeValue->_contracts[contractIndex];
            if (contract._traded) {
                Quote& bid = contract._quotes[BID];
                GUIThings::setCell(cells, contract._internalId, ContractParameterRVColumns::BidPrice, std::to_string(bid._bestPrice));
                GUIThings::setCell(cells, contract._internalId, ContractParameterRVColumns::BidQty, std::to_string(bid._bestSize));
                RvContract& bidBestLean = _relativeValue->_contracts[bid._bestLeanIndex];
                GUIThings::setCell(cells, contract._internalId, ContractParameterRVColumns::BidLean, bidBestLean._definition._name);

                Quote& ask = contract._quotes[ASK];
                GUIThings::setCell(cells, contract._internalId, ContractParameterRVColumns::AskPrice, std::to_string(ask._bestPrice));
                GUIThings::setCell(cells, contract._internalId, ContractParameterRVColumns::AskQty, std::to_string(ask._bestSize));
                RvContract& askBestLean = _relativeValue->_contracts[ask._bestLeanIndex];
                GUIThings::setCell(cells, contract._internalId, ContractParameterRVColumns::AskLean, askBestLean._definition._name);
            }
        }

        _clientAcceptor->sendGridUpdate(message);
    }

    void sendStfToGui() {
        proto::GridUpdate vectorMessage;
        vectorMessage.set_gridid(toUInt(WindowIds::VectorParameters));

        auto cells = vectorMessage.mutable_cells();

        for (uint32_t vectorIndex = 0; vectorIndex < _relativeValue->_numberOfVectors; ++vectorIndex) {
            double stf = _relativeValue->_shortTermFairsInVector.getValue(vectorIndex);
            GUIThings::setCell(cells, vectorIndex, VectorParametersColumns::STF, std::to_string(stf));
        }

        _clientAcceptor->sendGridUpdate(vectorMessage);

        proto::GridUpdate message;
        message.set_gridid(toUInt(WindowIds::ContractParametersRv));

        cells = message.mutable_cells();

        for (uint32_t contractIndex = 0; _relativeValue->_numberOfAllContracts; ++contractIndex) {
            RvContract& contract = _relativeValue->_contracts[contractIndex];
            if (contract._traded) {
                double stf = _relativeValue->_shortTermFairsForContracts.getValue(contractIndex);
                GUIThings::setCell(cells, contract._internalId, ContractParameterRVColumns::STF, std::to_string(stf));
            }
        }
    }

    void sendPositionsToGUI(uint32_t contractIndex) {
        proto::GridUpdate message;
        message.set_gridid(toUInt(WindowIds::ContractParametersRv));

        auto cells = message.mutable_cells();

        ContractDefinition& def = _relativeValue->_contracts[contractIndex]._definition;
        if (def._numberOfLegs > 1) {
            for (uint32_t li = 0; li < def._numberOfLegs; ++li) {
                ContractDefinition::Leg& leg = def._legs[li];
                double position = _relativeValue->_contractPositions.getValue(leg._index);
                GUIThings::setCell(cells, leg._index, ContractParameterRVColumns::Position, std::to_string(position));
            }
        } else {
            double position = _relativeValue->_contractPositions.getValue(contractIndex);
            GUIThings::setCell(cells, contractIndex, ContractParameterRVColumns::Position, std::to_string(position));
        }

        _clientAcceptor->sendGridUpdate(message);

        proto::GridUpdate vectorMessage;
        vectorMessage.set_gridid(toUInt(WindowIds::VectorParameters));

        cells = vectorMessage.mutable_cells();

        for (uint32_t vectorIndex = 0; vectorIndex < _relativeValue->_numberOfVectors; ++vectorIndex) {
            double position = _relativeValue->_positionsInVector.getValue(vectorIndex);
            GUIThings::setCell(cells, vectorIndex, VectorParametersColumns::Position, std::to_string(position));
        }

        _clientAcceptor->sendGridUpdate(vectorMessage);
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
                _logger.logError("received grid update but another already in flight");
            }
        } else {
            _logger.logError("received grid update for unknown grid " + update.gridid());
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

    void sendGenericMessageToRV(ViperMessageType type, int32_t action, int32_t value) {
        auto &outMessage = _outToRV->pool()->getNextMessage();
        outMessage._type = type;

        GenericMessage& message = reinterpret_cast<GenericMessage &> (outMessage._data[0]);
        message._number_1 = action;
        message._number_2 = value;

        const auto outIdx = _outToRV->pool()->append(sizeof(ViperMessageType) + sizeof(GenericMessage));
        mb();
        _outToRV->appendNew(outIdx);
    }

    void sendGenericMessageToMM(ViperMessageType type, int32_t action, int32_t value) {
        auto &outMessage = _outToMM->pool()->getNextMessage();
        outMessage._type = type;

        GenericMessage& message = reinterpret_cast<GenericMessage &> (outMessage._data[0]);
        message._number_1 = action;
        message._number_2 = value;

        const auto outIdx = _outToMM->pool()->append(sizeof(ViperMessageType) + sizeof(GenericMessage));
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
            _logger.logError("Accepted grid update which cannot be found");
        }
    }

    void onGridUpdateRejected(GridUpdateReply& reply) {
        auto it = _gridUpdates.find(reply._gridId);
        if (it != _gridUpdates.end()) {
            GridUpdate& gu = it->second;
            gu.clear();
        } else {
            _logger.logError("Rejected grid update which cannot be found");
        }
    }

    void onOtherRequest(ViperMessageType mt)  {
        switch (mt) {
            case ViperMessageType::Login: {
                proto::GUIInitialization message;
                _guiThings.populateGUIInitialization(message, _relativeValue->_pricingMatrix, _relativeValue->_riskMatrix);
                _clientAcceptor->sendGUIInitialization(message);
                break;
            }
            default:
                break;
        }
    }

    void onGuiInitializationAccepted() {
        _guiInitialized = true;
    }

    void onGuiInitializationRejected() {
        _logger.logErrorAndThrow("GUI initialization failed;");
    }

    void onSystemStateChange(proto::SystemState ns) {
        SystemState newState = toHf(ns);
        if (newState != _systemState) {
            _logger.logError("New State: " + (int) newState);
            _relativeValue->_newSystemState = newState;
            _marketMakers->_newSystemState = newState;
        }
    }

    void onSwitchChange(proto::SwitchChange& change) {
        auto& cell = change.cell();

        MainControlAction action = static_cast<MainControlAction>(cell.col());

        if (action != MainControlAction::Enabled && action != MainControlAction::Quoting) {
            _clientAcceptor->sendSwitchChangeReply(cell.row(), cell.col(), cell.value(), toUInt(WindowIds::MainControl), false);
            return;
        }

        int32_t value = atoi(cell.value().c_str());

        StrategyId strategyId = static_cast<StrategyId>(cell.row());

        switch (strategyId) {
            case StrategyId::MarketMaker: {
                sendGenericMessageToRV(ViperMessageType::SwitchChange, cell.col(), value);
                break;
            }
            case StrategyId::RelativeValue: {
                sendGenericMessageToRV(ViperMessageType::SwitchChange, cell.col(), value);
                break;
            }
            default: {
                _clientAcceptor->sendSwitchChangeReply(cell.row(), cell.col(), cell.value(), toUInt(WindowIds::MainControl), false);
                break;
            }
        }
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

    std::unordered_map<std::string, uint32_t> _symbolToInterId;

    RelativeValue* _relativeValue;
    MarketMakersT* _marketMakers;

    GUIThings _guiThings;
    bool _guiInitialized = false;

    SystemState _systemState = SystemState::Disabled;

    Logger _logger;

};

}

#endif //VIPER_FRONT_SIDE_H

