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
    double worsenPayupCondPercent = boost::lexical_cast<double>(_viperConfig.getGlobal(MM_WORSEN_PAYUP_COND));

    for (uint32_t i = 0; i < contractSize; ++i) {
        const ConfigContract& contract = _viperConfig._allContracts[i];
        auto& marketMaker = _marketMakers[i];
        marketMaker._inFillProcess.store(0, std::memory_order_relaxed);
        marketMaker._finishedFillProcess = 0;
        if (contract._toMarketMake) {
            const MarketMakerConfig& mmConfig = _viperConfig._marketMakerConfigs[contract._marketMakeIndex];
            marketMaker.initialize(contract, mmConfig, this, worsenPayupCondPercent);
        }
    }
}

void MarketMaker::registerOrderClient() {
    for (size_t i = 0; i < _viperConfig._allContracts.size(); ++i) {
        auto& mm = _marketMakers[i];
        if (mm._contract._toMarketMake) {
            sendRegisterOrderClientRequest(mm._contract._internalId, {});
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

void MarketMaker::disableMarketMaker(uint32_t internalId) {
    auto& mm = _marketMakers[internalId];
    mm.disable(true);
}

bool MarketMaker::pricesAndQuantitiesValid() const noexcept {
    bool retVal = true;
    for (size_t i = 0; i < _viperConfig._allContracts.size(); ++i) {
        auto& mm = _marketMakers[i];
        if (!mm._contract._toMarketMake) {
            continue;
        }
        auto check = [this](const Book& book, uint32_t side, const ConfigContract& contract) -> bool {
            if(book._price != hf::NULL_PRICE && ((book._otherQuantity < 0) || (book._otherOrderCount < 0))) {
                LOG(0, "MM {C[i1]}. Failed pricesAndQuantity check, refusing to start. side={i2} px={i3} q={i4} o={i5}", contract._internalId, side, book._price, book._otherQuantity, book._price);
                std::string errorMessage = boost::str(boost::format("Quantity Check Failed.  Refusing to start. Contract=%1% side=%2% px=%3% q=%4% o=%5%")
                                                      % contract._name % side % book._price % book._otherQuantity % book._otherOrderCount);
                sendErrorMessage(errorMessage, ErrorType::Critical);
                logging::GetLogger().logError(errorMessage);
                return false;
            }
            return true;
        };

        retVal &= check(mm._books[BID], BID, mm._contract);
        retVal &= check(mm._books[ASK], ASK, mm._contract);
    }

    return retVal;
}

void MarketMaker::enableMarketMakers() {
    for (size_t i = 0; i < _viperConfig._allContracts.size(); ++i) {
        _marketMakers[i].start();
    }
}

void MarketMaker::onQuoteExternalChanged(const SharedSystemState::QuoteExtBitset& quoteExt) {
    LOG(0, "MM. QuoteExternal Switch Changed. {i1} -> {i2}", _oldQuoteExt.to_ulong(), quoteExt.to_ulong())
    if (isEnabled()) {
        for (uint8_t i = 1; i < static_cast<uint8_t>(hf::VENUE::TEST); ++i) {
            if (_oldQuoteExt[i] != quoteExt[i]) {
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




