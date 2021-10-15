#include "ViperResource.h"
#include "ViperStrategyBase.h"
#include <util/ResourceManager.h>
#include <hf/queue/DebugQueueLogger.h>
#include "ClockUtil.h"

using namespace Python;

class DummyStrategy : public ViperStrategyBaseImpl<DummyStrategy> {
    ResourceId _id;
    StrategyId _strategyId;
public:
    void setResourceId(ResourceId id) noexcept { _id = id; }
    ResourceId id() const noexcept override { return _id; }

    void setStrategyId(StrategyId id) noexcept { _strategyId = id; }
    StrategyId strategyId() const noexcept override { return _strategyId; }

    void populateGUIInitialization(::google::protobuf::RepeatedPtrField< ::Python::proto::GridDefinition >* definitions) override {}
    void sendStateToGUI(std::function<void(proto::GridUpdate&)>&& send) const override {}
    bool isContractQuoteExternal(uint32_t contractId) const noexcept override { return false; }

    void onManualFill(ManualFill&) {}
    void onEndOfManualFills() {}
    void registerOrderClient() {}
    void runCycle(uint64_t) {}
    void onQuoteExternalChanged(SystemState) {}
    void onExchangeQuoteExternalChanged(const SharedSystemState::QuoteExtBitset&) {}
    void handleViperMessage(ViperMessage&) {}
    void handleHardShutdown() {}
    void handleDisabled() {}
    bool handleEnabled() { return false; }
    void handleLogout() {}
    void handleLogin() {}
    void handleCheckQuotes() {}
    void cancelGtcAndDisable() {}
    void onMDTrade(const md::MDBatch&, const md::MDEvent&, uint32_t) {}
    void handleUncommonMDEvent(const md::MDEvent&, const md::MDBatch&, uint32_t) {}
    void onEndMDBatch(hf::batch_id_t) {}
    void onMDUpdate(const md::MDEvent&, uint32_t, hf::batch_id_t) {}
    void onMDStatus(hf::VENUE, uint64_t, const md::MDStatusEvent&, uint32_t) {}
};

namespace {
bool s_staticVarInitialized = false;

DummyStrategy* initDummyStrategies() noexcept {
    static DummyStrategy dummys[toUInt(StrategyId::MAX)];
    for (uint32_t i = 0; i < toUInt(StrategyId::MAX); ++i) {
        dummys[i].setResourceId(static_cast<ResourceId>(i));
        dummys[i].setStrategyId(static_cast<StrategyId>(i));
    }
    return dummys;
}
DummyStrategy* s_dummyStrategys = initDummyStrategies();
}

ViperConfig ViperResource::_viperConfig;
double ViperResource::_frequency = clockUtil::GetCpuFrequency();
SharedSystemState ViperResource::s_systemState;
std::vector<ViperResource*> ViperResource::_resources;
std::vector<ViperStrategyBase*> ViperResource::_strategies;

void ViperResource::_onPostCreate(const libconfig::Setting &config) {
    if (!s_staticVarInitialized) {
        s_staticVarInitialized = true;

        auto viperResources = rhone::util::ResourceManager::getResources([](const rhone::util::Resource* resource) ->bool {
            return dynamic_cast<const ViperResource*>(resource) != nullptr;
        });

        if (viperResources.empty()) {
            logging::GetLogger().logErrorAndThrow("No Viper resource found");
        }

        _resources.resize(toUInt(ResourceId::_Resource_MAX_));
        _strategies.resize(toUInt(StrategyId::MAX));

        for (auto resource : viperResources) {
            auto viperResource = dynamic_cast<ViperResource*>(resource);
            if (!viperResource) {
                logging::GetLogger().logErrorAndThrow(boost::str(boost::format("%1% is not a Viper resource") % resource->getName()));
            }

            auto resourceId = viperResource->id();
            assert(resourceId < ResourceId::_Resource_MAX_);
            _resources[toUInt(resourceId)] = viperResource;

            auto strategy = dynamic_cast<ViperStrategyBase*>(viperResource);
            if (strategy) {
                _strategies[toUInt(strategy->strategyId())] = strategy;
            }
        }

        for (uint32_t i = 0; i < toUInt(StrategyId::MAX); ++i) {
            if (BOOST_UNLIKELY(_strategies[i] == nullptr)) {
                logging::GetLogger().logWarning(boost::str(boost::format("Strategy %1% not exist") % strategyFullName((StrategyId)i)));
                _strategies[i] = &s_dummyStrategys[i];
            }
        }

        auto viperLogQueueList = rhone::util::ResourceManager::getResourcesByType("Python::ViperLoggerQueue");
        if (!viperLogQueueList || viperLogQueueList->empty()) {
            logging::GetLogger().logErrorAndThrow("ViperLoggerQueue not found");
        }
        auto viperLogQueue = reinterpret_cast<Python::ViperLoggerQueueResource*>(*viperLogQueueList->begin());
        viperLogQueue->clear();
        logging::setViperLogger(viperLogQueue);
    }

    if (config.exists("kdb_logger")) {
        const std::string loggerName = config["kdb_logger"];
        _kdbLogger = &(dynamic_cast<hf::DebugQueueLogger*> (rhone::util::ResourceManager::getResourceP(loggerName)))->queue();
    }
}