#include "ViperResource.h"
#include "ViperStrategyBase.h"
#include <hf/ResourceManager.h>
#include <hf/queue/DebugQueueLogger.h>
#include "ClockUtil.h"

using namespace Python;

namespace {
bool s_staticVarInitialized = false;
}

ViperConfig ViperResource::_viperConfig;
double ViperResource::_frequency = clockUtil::GetCpuFrequency();
SharedSystemState ViperResource::s_systemState;
RingBufferQueue<>* ViperResource::_frontQueue = nullptr;
RingBufferQueue<>* ViperResource::_executorQueue = nullptr;
std::vector<ViperStrategyBase*> ViperResource::_strategies;
hf::queue::LockFreeQueue<om::debug_t> ViperResource::_viperLogQueue;

void ViperResource::_onPostCreate(const libconfig::Setting &config) {
    if (!s_staticVarInitialized) {
        s_staticVarInitialized = true;

        auto viperResources = hf::ResourceManager::getResources([](const hf::Resource* resource) ->bool {
            return dynamic_cast<const ViperResource*>(resource) != nullptr;
        });

        if (viperResources.empty()) {
            logging::GetLogger().logErrorAndThrow("No Viper resource found");
        }
        _strategies.resize(toUInt(StrategyId::Execution));

        for (auto resource : viperResources) {
            auto viperResource = dynamic_cast<ViperResource*>(resource);
            if (!viperResource) {
                logging::GetLogger().logErrorAndThrow(boost::str(boost::format("%1% is not a Viper resource") % resource->getName()));
            }

            auto resourceId = viperResource->id();
            if (resourceId == ResourceId::FrontSide) {
                _frontQueue = &viperResource->_msgQueue;
            } else if (resourceId == ResourceId::Execution) {
                _executorQueue = &viperResource->_msgQueue;
            }

            auto strategy = dynamic_cast<ViperStrategyBase*>(viperResource);
            if (strategy) {
                _strategies[toUInt(strategy->strategyId())] = strategy;
            }
        }
    }

    if (config.exists("kdb_logger")) {
        const std::string loggerName = config["kdb_logger"];
        _kdbLogger = &(dynamic_cast<hf::DebugQueueLogger*> (hf::ResourceManager::getResourceP(loggerName)))->queue();
    }
}