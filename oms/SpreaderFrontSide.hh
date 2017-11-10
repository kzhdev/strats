//
// Created by pkusch on 12/26/16.
//

#include <hf/ResourceManager.h>
#include <hf/ConfigManager.h>
#include <md/queue/MDQueueResource.h>
#include "SpreaderFrontSide.h"

namespace oms { namespace spreader  {

template class SpreaderFrontSide<strat::Context, SpreaderClientsAcceptor, md::sym::SymbolsResource>;

template <typename ContextT, typename ClientAcceptorT, typename SymbolsResourceT>
void SpreaderFrontSide<ContextT, ClientAcceptorT, SymbolsResourceT>::onCreate(const libconfig::Setting& config) {
    const auto name = hf::ConfigManager::getString(config, "symbols");
    m_symbolsResourceP = dynamic_cast<SymbolsResourceT *> (hf::ResourceManager::getResourceP(name));
    if (!m_symbolsResourceP) {
        LOG_FATAL_AND_THROW(boost::str(boost::format("Cannot Find Symbols Resource by name '%1%'") % name));
    }

    const auto host = hf::ConfigManager::getString(config, "host");
    const auto port = hf::ConfigManager::getLong(config, "port");

    _algoManager = new AlgoManager<ContextT>();
    _clientAcceptor = new ClientAcceptorT(host, port);
    _clientAcceptor->createMonitor();
    _clientAcceptor->setAlgoManager(_algoManager);
}

template <typename ContextT, typename ClientAcceptorT, typename SymbolsResourceT>
void SpreaderFrontSide<ContextT, ClientAcceptorT, SymbolsResourceT>::onPostCreate(const libconfig::Setting& config) {
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

            _algoManager->addMarketDataQueue(ptr);
        }
    }
}

template <typename ContextT, typename ClientAcceptorT, typename SymbolsResourceT>
void SpreaderFrontSide<ContextT, ClientAcceptorT, SymbolsResourceT>::onRun() {
    LOG_INFO(boost::str(boost::format("Starting Spreader Frontend.")));

    _outQueue->resetR();
    _clientAcceptor->getInQueue()->resetW();

    while (BOOST_LIKELY(!hf::Service::isExitAll())) {
        _clientAcceptor->runIteration();
        if (_spreaderType == SpreaderType::Master) {
            const bool good = _clientAcceptor->checkForMessagesFromSlave();
            if (!good) {
                _clientAcceptor->onSlaveConnectionError();
            }
        }
        processOutgoingMessages();
        _algoManager->poll();
    }
}

template <typename ContextT, typename ClientAcceptorT, typename SymbolsResourceT>
void SpreaderFrontSide<ContextT, ClientAcceptorT, SymbolsResourceT>::log(const std::string& s) {
    LOG_INFO(s);
}

}}