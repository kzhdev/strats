#include "ArbieSpreader.h"

using namespace Python;

ArbieSpreader::Path::Path(uint32_t side, std::string name, uint32_t id, Contract* contract,
                          const std::vector<PathContract>& pathContracts, ArbieSpreaderPath& pathConfig,
                          const ShortTermFairOffset* stfOffset)
    : _contract(contract)
    , _config(&pathConfig)
    , _stfOffset(*stfOffset)
    , _commission(ToRhonePrice(pathConfig._commission))
    , _leanContracts(pathContracts)
    , _id(id)
    , _side(side)
    , _name(std::move(name)) {
    _bestHedgeMarket.resize(pathContracts.size());
    for (uint32_t i = 0; i < _leanContracts.size(); ++i) {
        auto& pathContract = _leanContracts[i];
        pathContract._contract->_usedInPath = true;
        if (side == BID) {
            _bestHedgeMarket[i] = getBestMarket<BID>(pathContract);
        } else {
            _bestHedgeMarket[i] = getBestMarket<ASK>(pathContract);
        }
    }
}

void ArbieSpreader::Path::marketDirty(Contract* contract, uint32_t side) {
    if (_config->_enabled) {
        for (auto& leanContract : _leanContracts) {
            if (leanContract._contract == contract) {
                if (leanContract._multiplier > 0) {
                    if (_side == side) {
                        _dirty = true;
                    }
                } else if (_side == (1- side)) {
                    _dirty = true;
                }
                break;
            }
        }
    }
}

void ArbieSpreader::Path::marketDirty(Contract* contract) {
    if (_config->_enabled) {
        for (auto& leanContract : _leanContracts) {
            if (leanContract._contract == contract) {
                _dirty = true;
                break;
            }
        }
    }
}

void ArbieSpreader::Path::log() const noexcept {
    LOG(0, "AS {C[i1]}. {i2} Path {i3} stfOffset={d1}", _contract->name(), _side, _id, _stfOffset._stfOffset)
    for (uint32_t i = 0; i < _leanContracts.size(); ++i) {
        auto& pathContract = _leanContracts[i];
        auto& market = *_bestHedgeMarket[i];
        LOG(0, "{C[i1]}. MarketId={i2} Side={i3} multiplier={i4} {i5}({i6})@{d1} extCommission={d2}", pathContract._contract->name(),
            market.getId(), market.getSide(), pathContract._multiplier, market.getUnAdjustedSize(),
            market.getAdjustedSize(), ToDollars(market.getPrice()), ToDollars(pathContract._contract->_extCommission))
    }
}