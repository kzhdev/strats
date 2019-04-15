#include "FrontSide.h"
#include <hf/ResourceManager.h>


namespace Python  {

static hf::ResourceRegistrator<FrontSide> s_registrator_1("FrontSide");

void FrontSide::onCreate(const libconfig::Setting &config) {

    logging::GetLogger().logInfo(boost::str(boost::format("VIPER_VERSION %1%") % ViperVersion));
    logging::GetLogger().logInfo(boost::str(boost::format("VIPER_GITHASH: %1%") % ViperGitHash));
    logging::GetLogger().logInfo(boost::str(boost::format("RHONEPUBLIC_GITHASH: %1%") % RhonePublicGitHash));

    const auto host = hf::ConfigManager::getString(config, "host");
    const auto port = hf::ConfigManager::getLong(config, "port");
    _frequency = clockUtil::GetCpuFrequency();
    _clientAcceptor = new ClientsAcceptor(host, port, this, _frequency);
    _clientAcceptor->createMonitor();
    createGridUpdates();
}

} // Python
