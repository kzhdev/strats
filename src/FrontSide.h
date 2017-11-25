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
                default:
                    break;
            }
        }
    }

    void onOtherRequest(ViperMessageType mt)  {
    }

    void onConfigChage(proto::ConfigChange& configChange) {
    }

private:
    ClientsAcceptor* _clientAcceptor;
    ViperQueue* _queueFromRelativeValue = nullptr;
    ViperQueue* _queueToRelativeValue = nullptr;
    ViperQueue* _queueFromMarketMaker = nullptr;
    ViperQueue* _queueToMarketMaker = nullptr;
};
};

}

#endif //VIPER_FRONT_SIDE_H

