#include "FrontSide.h"
#include <util/ResourceManager.h>

namespace Python  {

static rhone::util::ResourceRegistrator<FrontSide> s_registrator_1(FrontSide::NAME);

void FrontSide::onCreate(const libconfig::Setting &config) {
    ViperResource::_onCreate(config);

    logging::GetLogger().logInfo(boost::str(boost::format("VIPER_VERSION %1%") % ViperVersion));
    logging::GetLogger().logInfo(boost::str(boost::format("VIPER_GITHASH: %1%") % ViperGitHash));
    logging::GetLogger().logInfo(boost::str(boost::format("RHONEPUBLIC_GITHASH: %1%") % RhonePublicGitHash));

    const auto host = hf::ConfigManager::getString(config, "host");
    const auto port = hf::ConfigManager::getLong(config, "port");
    _clientAcceptor = new ClientsAcceptor(host, port, this, _frequency);
#ifndef VIPER_BACKTEST
    _clientAcceptor->createMonitor();
#endif
    createGridUpdates();
}

bool FrontSide::validateLoggingConfig() {
    if(!_kdbLogger) {
        logging::GetLogger().logInfo("No KDB log queue configured for frontside");
        return true;
    }

    if(!_kdbLogger->eventMode()) {
        logging::GetLogger().logError("FS KDB Logger: Log queue not in event mode");
        return false;
    }

    const auto& symbolsName = _kdbLogger->symbolsResourceName();

    if(symbolsName == "") {
        logging::GetLogger().logError("FS KDB Logger: Log queue must have symbols set");
        return false;
    }

    const auto* symbolsResource = dynamic_cast<md::sym::SymbolsResource *>(rhone::util::ResourceManager::getResourceP(symbolsName));
    if(!symbolsResource) {
        logging::GetLogger().logError("FS KDB Logger: Log queue symbols did not correspond to any found symbols resource.  symbols=" + symbolsName);
        return false;
    }

    if(symbolsResource != _symbolsResource) {
        logging::GetLogger().logError("FS KDB Logger: KDB Logger symbols did not match Frontside symbols");
        return false;
    }

    //There really should be a better way to check auto-populate-timestamp
    for(rhone::util::Resource* resource : *rhone::util::ResourceManager::getResourcesByType("hf::DebugQueueLogger")) {
        auto* debugQueueLogger = dynamic_cast<hf::DebugQueueLogger *>(resource);
        if(&debugQueueLogger->queue() == _kdbLogger) {
            if(debugQueueLogger->getAutoPopulateTimestamp()) {
                logging::GetLogger().logError("FS KDB Logger: auto-populate-timestamp must be set to false in DebugQueueLogger");
                return false;
            }
        }
    }

    return true;
}

} // Python
