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