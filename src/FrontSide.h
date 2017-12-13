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

    void sendCommandToRV(ViperMessageType type) {
        auto &outMessage = _outToRV->pool()->getNextMessage();
        outMessage._type = type;
        const auto outIdx = _outToRV->pool()->append(sizeof(ViperMessageType));
        mb();
        _outToRV->appendNew(outIdx);
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
        _clientAcceptor->sendSystemChangeAccepted();
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

