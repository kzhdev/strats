#ifndef VIPER_UPDATE_SELECTOR_H
#define VIPER_UPDATE_SELECTOR_H

#include <set>
#include <cstdint>
#include "messages/omsproto.pb.h"
#include "Common.h"

namespace Python {

enum GridIds {RvVectorParameters = 1, RvContractParameters = 2, RvLeanMatrix = 3, RvPricingConfig = 4, RvPricingMatrix = 5, RvRiskMatrix = 6, ManualFills = 7};

template <typename ClientT>
class UpdateSelector {

public:
    UpdateSelector(ClientT& client)
    :
        _client(client)
    {}

    void processUpdate(GridUpdate& update) {
        switch (update._gridId) {
            case GridIds::RvVectorParameters: {
                _client.onRvVectorParametersUpdate(update);
                break;
            }
            case GridIds::RvContractParameters: {
                _client.onRvContractParametersUpdate(update);
                break;
            }
            case GridIds::RvLeanMatrix: {
                _client.onRvLeanMatrixUpdate(update);
                break;
            }
            case GridIds::RvPricingConfig: {
                _client.onRvPricingConfigUpdate(update);
                break;
            }
            case GridIds::ManualFills: {
                _client.onManualFills(update);
                break;
            }
            default:
                break;
        }
    }

    ClientT& _client;
};

}

#endif //VIPER_UPDATE_SELECTOR_H