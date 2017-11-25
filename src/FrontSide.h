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
        _outQueueFromMarketMaker->resetR();
        _outQueueFromRelativeValue->resetR();
        _clientAcceptor->getInQueue()->resetW();

        while (BOOST_LIKELY(!hf::Service::isExitAll())) {
            _clientAcceptor->runIteration();
            processOutgoingMessagesFromRelativeValue();
            processOutgoingMessagesFromMarketMaker();
        }
    }

    void processOutgoingMessagesFromRelativeValue() {
        while (true) {
            uint64_t idx = _outQueueFromRelativeValue->getNext();

            if (BOOST_UNLIKELY(idx == 0))
                return;

            _outQueueFromRelativeValue->freeNext();

            auto& message = _outQueueFromRelativeValue->pool()->getMessage(idx);

            switch (message._type) {
                default:
                    break;
            }
        }
    }

    void processOutgoingMessagesFromMarketMaker() {
        while (true) {
            uint64_t idx = _outQueueFromMarketMaker->getNext();

            if (BOOST_UNLIKELY(idx == 0))
                return;

            _outQueueFromMarketMaker->freeNext();

            auto& message = _outQueueFromMarketMaker->pool()->getMessage(idx);

            switch (message._type) {
                default:
                    break;
            }
        }
    }

private:
    ClientsAcceptor* _clientAcceptor;
    ViperQueue* _outQueueFromRelativeValue = nullptr;
    ViperQueue* _outQueueFromMarketMaker = nullptr;
};

}

#endif //VIPER_FRONT_SIDE_H

