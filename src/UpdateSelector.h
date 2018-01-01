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

    bool processUpdate(GridUpdate& update) {
        switch (static_cast<WindowIds>(update._gridId)) {
            case WindowIds::VectorParameters: {
                return _client.onRvVectorParametersUpdate(update);
            }
            case WindowIds::ContractParametersRv: {
                return _client.onRvContractParametersUpdate(update);
            }
            case WindowIds::LeanMatrix: {
                return _client.onRvLeanMatrixUpdate(update);
            }
            case WindowIds::PricingConfig: {
                return _client.onRvPricingConfigUpdate(update);
            }
            case WindowIds::ManualFillsEntry: {
                return _client.onManualFills(update);
            }
            case WindowIds::MarketMakerParameters: {
                return _client.onMarketMakerParametersUpdate(update);
            }
            default:
                break;
        }
        return true;
    }

    ClientT& _client;
};

}

#endif //VIPER_UPDATE_SELECTOR_H