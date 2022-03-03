#include <hf/ResourceManager.h>
#include "RelativeValue.h"

namespace Python {

static hf::ResourceRegistrator<RelativeValue> s_registrator("RelativeValue");

uint32_t RelativeValue::anyContractCrossed() const noexcept {
    for (const auto& contract : _contracts) {
        const auto& bid = contract._quotes[BID];
        if (bid._externalPrice != hf::NULL_PRICE && contract.externalMarket<ASK>().getPrice() != hf::NULL_PRICE &&
            bid._externalPrice >= contract.externalMarket<ASK>().getPrice()) {
            return contract._internalId;
        }
        const auto& ask = contract._quotes[ASK];
        if (ask._externalPrice != hf::NULL_PRICE && contract.externalMarket<BID>().getPrice() != hf::NULL_PRICE &&
            ask._externalPrice <= contract.externalMarket<BID>().getPrice()) {
            return contract._internalId;
        }
    }
    return INVALID_INTERNAL_ID;
}

}
