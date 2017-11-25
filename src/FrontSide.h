#ifndef VIPER_FRONT_SIDE_H
#define VIPER_FRONT_SIDE_H

#include <hf/Service.h>
#include <hf/ConfigManager.h>
#include <hf/ResourceManager.h>
#include "ClientAcceptor.h"
#include "ViperConfig.h"
#include "RelativeValue.h"
#include <queue>

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
        const auto name = hf::ConfigManager::getString(config, "config_file");
        _config.initialize(name);

        const std::string relativeValueName = config["RelativeValue"];
        auto relativeValue = dynamic_cast<RelativeValue*> (hf::ResourceManager::getResourceP(relativeValueName));
        relativeValue->onConfig(_config);

        const std::string marketMakersName = config["MarketMakers"];
        auto marketMakers = dynamic_cast<RelativeValue*> (hf::ResourceManager::getResourceP(marketMakersName));
        marketMakers->onConfig(_config);
    }

    void onRun() override {
        _queueFromMarketMaker->resetR();
        _queueFromRelativeValue->resetR();

        while (BOOST_LIKELY(!hf::Service::isExitAll())) {
            _clientAcceptor->runIteration();
            processOutgoingMessagesFromRelativeValue();
            processOutgoingMessagesFromMarketMaker();
        }
    }

    void processOutgoingMessagesFromRelativeValue() {
        while (true) {
            uint64_t idx = _queueFromRelativeValue->getNext();

            if (BOOST_UNLIKELY(idx == 0))
                return;

            _queueFromRelativeValue->freeNext();

            auto& message = _queueFromRelativeValue->pool()->getMessage(idx);

            switch (message._type) {
                default:
                    break;
            }
        }
    }

    void processOutgoingMessagesFromMarketMaker() {
        while (true) {
            uint64_t idx = _queueFromMarketMaker->getNext();

            if (BOOST_UNLIKELY(idx == 0))
                return;

            _queueFromMarketMaker->freeNext();

            auto& message = _queueFromMarketMaker->pool()->getMessage(idx);

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

    void onMarketMakersConfigChangeAccepted(ConfigChangeReply& configChangeReply) {
        const proto::MarketMakerConfigChange& change = _marketMakersConfigChanges.front();
        if (configChangeReply._id == change.id()) {
            _config.applyChange(change);
            _config.writeToFile();
            _marketMakersConfigChanges.pop();
        } else {
            logError("Inconsistent config ids");
        }
    }

    void onMarketMakersConfigChangeRejected(ConfigChangeReply& configChangeReply) {

    }

    void onOtherRequest(ViperMessageType mt)  {
    }

    void onRelativeValueConfigChange(proto::RelativeValueConfigChange& configChange) {
    }

    void onMarketMakersConfigChage(proto::MarketMakerConfigChange& configChange) {
    }

    void logError(const std::string& message) {
    }

private:
    ClientsAcceptor* _clientAcceptor;
    ViperQueue* _queueFromRelativeValue = nullptr;
    ViperQueue* _queueToRelativeValue = nullptr;
    ViperQueue* _queueFromMarketMaker = nullptr;
    ViperQueue* _queueToMarketMaker = nullptr;
    ViperConfig _config;
    std::queue<proto::RelativeValueConfigChange> _relativeValueConfigChanges;
    std::queue<proto::MarketMakerConfigChange> _marketMakersConfigChanges;
};

}

#endif //VIPER_FRONT_SIDE_H

