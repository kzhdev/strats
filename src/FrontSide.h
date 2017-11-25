#ifndef VIPER_FRONT_SIDE_H
#define VIPER_FRONT_SIDE_H

#include <hf/Service.h>
#include <hf/ConfigManager.h>
#include "ClientAcceptor.h"

namespace Python {

class FrontSide : public virtual hf::Service {
public:
    FrontSide() {
    }

    void onCreate(const libconfig::Setting& config) {
        const auto host = hf::ConfigManager::getString(config, "host");
        const auto port = hf::ConfigManager::getLong(config, "port");
        _clientAcceptor = new ClientsAcceptor(host, port);
    }

    void onRun() override {
        _outQueue->resetR();
        _clientAcceptor->getInQueue()->resetW();

        while (BOOST_LIKELY(!hf::Service::isExitAll())) {
            _clientAcceptor->runIteration();
            processOutgoingMessages();
        }
    }

    void processOutgoingMessages() {
        while (true) {
            uint64_t idx = _outQueue->getNext();

            if (BOOST_UNLIKELY(idx == 0))
                return;

            _outQueue->freeNext();

            auto& message = _outQueue->pool()->getMessage(idx);

            switch (message._type) {
                default:
                    break;
            }
        }
    }

private:
    ClientsAcceptor* _clientAcceptor;
    ViperQueue* _outQueue = nullptr;
};

}

#endif //VIPER_FRONT_SIDE_H

