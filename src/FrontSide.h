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
                case ViperMessageType::ConfigChangeAccepted: {
                    ConfigChangeReply& configChangeReply = reinterpret_cast<ConfigChangeReply &> (message._data[0]);
                    onMarketMakersConfigChangeAccepted(configChangeReply);
                    break;
                }
                case ViperMessageType::ConfigChangeRejected: {
                    ConfigChangeReply& configChangeReply = reinterpret_cast<ConfigChangeReply &> (message._data[0]);
                    onMarketMakersConfigChangeRejected(configChangeReply);
                    break;
                }
                default:
                    break;
            }
        }
    }

    void onGridUpdate(proto::GridUpdate& gridUpdate) {
    }

    void onMarketMakersConfigChangeAccepted(ConfigChangeReply& configChangeReply) {
        const proto::GridUpdate& change = _marketMakersConfigChanges.front();
        if (configChangeReply._id == change.id()) {
            _config.writeToFile();
            _marketMakersConfigChanges.pop();
        } else {
            logError("Inconsistent config ids");
        }
    }

    void onMarketMakersConfigChangeRejected(ConfigChangeReply& configChangeReply) {
        const proto::GridUpdate& change = _marketMakersConfigChanges.front();
        if (configChangeReply._id == change.id()) {
            notifyClientOfConfigReject(change.id());
            _marketMakersConfigChanges.pop();
        } else {
            logError("Inconsistent config ids");
        }
    }

    void notifyClientOfConfigReject(uint32_t id) {
    }

    void onOtherRequest(ViperMessageType mt)  {
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

    std::queue<proto::GridUpdate> _relativeValueConfigChanges;
    std::queue<proto::GridUpdate> _marketMakersConfigChanges;

    md::sym::SymbolsResource* _symbolsResource;

};

}

#endif //VIPER_FRONT_SIDE_H

