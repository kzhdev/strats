#include "MarketMaker.h"
#include <hf/ResourceManager.h>
#include "SimpleMarketMaker.h"
#include "OrderRequestManager.h"

using namespace Python;

static hf::ResourceRegistrator<MarketMaker> s_registrator("MarketMaker");

void MarketMaker::init(double frequency,
                       uint64_t internalHeartbeatPeriod,
                       std::function<bool(uint32_t)> pnlLimitsChecker) {
    ViperStrategyBase::init(frequency, internalHeartbeatPeriod, std::move(pnlLimitsChecker));
    auto contractSize = _viperConfig._allContracts.size();
    _marketMakers = new SimpleMarketMaker[contractSize];

    for (uint32_t i = 0; i < contractSize; ++i) {
        const ConfigContract& contract = _viperConfig._allContracts[i];
        auto& marketMaker = _marketMakers[i];
        if (contract._toMarketMake) {
            const MarketMakerConfig& mmConfig = _viperConfig._marketMakerConfigs[contract._marketMakeIndex];
            marketMaker.initialize(contract, mmConfig, this);
        }
    }
}

void MarketMaker::registerOrderClient() {
    for (size_t i = 0; i < _viperConfig._allContracts.size(); ++i) {
        auto& mm = _marketMakers[i];
        if (mm._contract._toMarketMake) {
            sendRegisterOrderClientRequest(mm._contract._internalId, {});
        }
    }
}

void MarketMaker::disableMarketMakers() {
    for (size_t i = 0; i < _viperConfig._allContracts.size(); ++i) {
        _marketMakers[i].disable(false, true);
    }
    static auto& orderRequestManager = OrderRequestManager::get();
    orderRequestManager.sendCancelRequest(StrategyId::MarketMaker);
}

void MarketMaker::disableMarketMaker(uint32_t internalId, bool disableGTC) {
    auto& mm = _marketMakers[internalId];
    mm.disable(true, false, disableGTC);
}

bool MarketMaker::pricesAndQuantitiesValid() const noexcept {
    bool retVal = true;
    for (size_t i = 0; i < _viperConfig._allContracts.size(); ++i) {
        auto& mm = _marketMakers[i];
        if (!mm._contract._toMarketMake) {
            continue;
        }
        auto check = [this](const md::top_t::level_t& book, uint32_t side, const ConfigContract& contract) -> bool {
            if(book.pr != hf::NULL_PRICE && ((book.q < 0) || (book.o < 0))) {
                LOG(0, "MM {C[i1]}. Failed pricesAndQuantity check, refusing to start. side={i2} px={i3} q={i4} o={i5}", contract._internalId, side, book.pr, book.q, book.o);
                std::string errorMessage = boost::str(boost::format("Quantity Check Failed.  Refusing to start. Contract=%1% side=%2% px=%3% q=%4% o=%5%")
                                                      % contract._name % side % book.pr % book.q % book.o);
                sendErrorMessage(errorMessage, ErrorType::Critical);
                logging::GetLogger().logError(errorMessage);
                return false;
            }
            return true;
        };

        retVal &= check(mm.topBook<BID>(), BID, mm._contract);
        retVal &= check(mm.topBook<ASK>(), ASK, mm._contract);
    }

    return retVal;
}

void MarketMaker::enableMarketMakers() {
    for (size_t i = 0; i < _viperConfig._allContracts.size(); ++i) {
        _marketMakers[i].start();
    }
}

void MarketMaker::onQuoteExternalChanged(SystemState quoteExt) {
    if (isEnabled()) {
        for (size_t i = 0; i < _viperConfig._allContracts.size(); ++i) {
            auto& mm = _marketMakers[i];
            if (mm._contract._toMarketMake) {
                mm.applyUpdates();
            }
        }
    }
}

void MarketMaker::onExchangeQuoteExternalChanged(const SharedSystemState::QuoteExtBitset& quoteExt) {
    if (isEnabled()) {
        for (uint8_t i = 1; i < static_cast<uint8_t>(hf::VENUE::ANY); ++i) {
            if (_oldExchangeQuoteExt[i] != quoteExt[i]) {
                for (size_t j = 0; j < _viperConfig._allContracts.size(); ++j) {
                    auto& mm = _marketMakers[j];
                    if ((static_cast<uint8_t>(mm._contract._venue) == i) & mm._contract._toMarketMake) {
                        mm.applyUpdates();
                    }
                }
            }
        }
    }
}




