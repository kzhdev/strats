#include <hf/ResourceManager.h>
#include <hf/log.h>
#include "RelativeValue.h"
#include <md/queue/MDQueueResource.h>

LOG_DECLARE();

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
                LOG_ERROR_AND_THROW(boost::str(boost::format("Cannot Find MDQueue by name '%1%'") % name));
            }

            if (!ptr->getPoolP()) {
                LOG_ERROR_AND_THROW(boost::str(boost::format("MDPool is not set for MDQueue with name '%1%'") % name));
            }

            addMarketDataQueue(ptr);
        }
    }
}

void RelativeValue::logError(const std::string& message) {
    LOG_ERROR(message);
}

void RelativeValue::logInfo(const std::string& message) {
    LOG_INFO(message);
}

}
