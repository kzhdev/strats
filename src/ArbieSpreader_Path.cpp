#include "ArbieSpreader.h"

using namespace Python;

ArbieSpreader::Path::Path(uint32_t side, std::string name, uint32_t id, Contract* contract,
                          const std::vector<PathContract>& pathContracts, ArbieSpreaderPath& pathConfig,
                          const ShortTermFairOffset* stfOffset, uint32_t numAllContracts)
    : _contract(contract)
    , _config(&pathConfig)
    , _stfOffset(*stfOffset)
    , _commission(ToRhonePrice(pathConfig._commission))
    , _leanContracts(pathContracts)
    , _id(id)
    , _side(side)
    , _name(std::move(name)) {
    _multipliers.resize(numAllContracts, 0);
    for (uint32_t i = 0; i < _leanContracts.size(); ++i) {
        auto& pathContract = _leanContracts[i];
        pathContract._side = pathContract._multiplier > 0 ? _side : (1 - _side);
        pathContract._contract->_usedInPath = true;
        _multipliers[pathContract._internalId] = pathContract._multiplier;
    }
}

void ArbieSpreader::Path::log() const noexcept {
    LOG(0, "AS {C[i1]}. {i2} Path {i3} stfOffset={d1}", _contract->name(), _side, _id, _stfOffset._stfOffset)
    for (auto const& pathContract : _leanContracts) {
        Market* market = pathContract.bestMarket();
        LOG(0, "{C[i1]}. MarketId={i2} Side={i3} multiplier={i4} {i5}({i6})@{d1} extCommission={d2}", pathContract._contract->name(),
            market->getId(), market->getSide(), pathContract._multiplier, market->getUnAdjustedSize(),
            market->getAdjustedSize(), ToDollars(market->getPrice()), ToDollars(pathContract._contract->_extCommission))
    }
}

bool ArbieSpreader::Path::canHedge(hf::qty_t size) const noexcept {
    for (const auto& pathContract : _leanContracts) {
        auto market = pathContract.bestMarket();
        auto side = market->getSide();
        auto price = market->getPrice();
        if (price == hf::NULL_PRICE || market->getId() == MarketIds::__end__ ||
        IsBetter(pathContract._contract->_bestCache[side], price, side) ||
        market->getAdjustedSize() < size) {
            return false;
        }
    }
    return true;
}