#include <hf/ResourceManager.h>
#include "RelativeValue.h"
#include <md/queue/MDQueueResource.h>
#include <hf/queue/DebugQueueResource.h>
#include <hf/queue/DebugQueueLogger.h>

namespace Python {

static hf::ResourceRegistrator<RelativeValue> s_registrator("RelativeValue");

void RelativeValue::onPostCreate(const libconfig::Setting& config) {
    if (config.exists("md-queues")) {
        const auto &queuesConfig = config["md-queues"];

        for (int i = 0; i < queuesConfig.getLength(); ++i) {
            const libconfig::Setting &queueConfig = queuesConfig[i];

            if (!hf::ConfigManager::getBool(queueConfig, "enabled", true)) continue;

            const auto name = hf::ConfigManager::getString(queueConfig, "name");

            auto *const ptr = dynamic_cast<md::queue::MDQueueResource *> (hf::ResourceManager::getResourceP(name));
            if (!ptr) {
                GetLogger().logErrorAndThrow(boost::str(boost::format("Cannot Find MDQueue by name '%1%'") % name));
            }

            if (!ptr->getPoolP()) {
                GetLogger().logErrorAndThrow(boost::str(boost::format("MDPool is not set for MDQueue with name '%1%'") % name));
            }

            addMarketDataQueue(ptr);

            _id = hf::ConfigManager::getLong(queueConfig, "id");
        }
    }

    {
        const auto queueName = hf::ConfigManager::getString(config, "queue_to_front");
        auto *const ptr = hf::ResourceManager::getResourceP(queueName);
        if (!ptr) {
            GetLogger().logErrorAndThrow(boost::str(boost::format("Cannot Find Queue Resource by name '%1%'") % queueName));
        }
        _outToFront = dynamic_cast<md::queue::MDQueueResource *> (ptr);
        if (!_outToFront) {
            GetLogger().logErrorAndThrow(boost::str(boost::format("Cannot Find Queue Resource by name '%1%'") % queueName));
        }
    }

    {
        const auto queueName = hf::ConfigManager::getString(config, "queue_from_front");
        auto *const ptr = hf::ResourceManager::getResourceP(queueName);
        if (!ptr) {
            GetLogger().logErrorAndThrow(boost::str(boost::format("Cannot Find Queue Resource by name '%1%'") % queueName));
        }
        _inFromFront = dynamic_cast<md::queue::MDQueueResource *> (ptr);
        if (!_inFromFront) {
            GetLogger().logErrorAndThrow(boost::str(boost::format("Cannot Find Queue Resource by name '%1%'") % queueName));
        }
    }

    {
        const auto queueName = hf::ConfigManager::getString(config, "queue_to_market_maker");
        auto *const ptr = hf::ResourceManager::getResourceP(queueName);
        if (!ptr) {
            GetLogger().logErrorAndThrow(boost::str(boost::format("Cannot Find Queue Resource by name '%1%'") % queueName));
        }
        _outToMM = dynamic_cast<md::queue::MDQueueResource *> (ptr);
        if (!_outToMM) {
            GetLogger().logErrorAndThrow(boost::str(boost::format("Cannot Find Queue Resource by name '%1%'") % queueName));
        }
    }

    {
        const auto queueName = hf::ConfigManager::getString(config, "queue_from_market_maker");
        auto *const ptr = hf::ResourceManager::getResourceP(queueName);
        if (!ptr) {
            GetLogger().logErrorAndThrow(boost::str(boost::format("Cannot Find Queue Resource by name '%1%'") % queueName));
        }
        _inFromMM = dynamic_cast<md::queue::MDQueueResource *> (ptr);
        if (!_inFromMM) {
            GetLogger().logErrorAndThrow(boost::str(boost::format("Cannot Find Queue Resource by name '%1%'") % queueName));
        }
    }

    _inFromMM->resetR();
    _inFromFront->resetR();
    _outToMM->resetW();
    _outToFront->resetW();

    const std::string loggerName = config["kdb_logger"];
    _kdbLogger = &(dynamic_cast<hf::DebugQueueLogger*> (hf::ResourceManager::getResourceP(loggerName)))->queue();
}

}
