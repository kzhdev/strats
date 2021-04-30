#include "ArbieSpreader.h"
#include <hf/ResourceManager.h>

using namespace Python;

static hf::ResourceRegistrator<ArbieSpreader> s_registrator("ArbieSpreader");

void ArbieSpreader::init(double frequency, uint64_t internalHeartbeatPeriod, std::function<bool(uint32_t)> pnlLimitsChecker) {
    ViperStrategyBase::init(frequency, internalHeartbeatPeriod, std::move(pnlLimitsChecker));
    initializeContracts();
}

void ArbieSpreader::initializeContracts() {
    const auto& asConfig = _viperConfig._arbieSpreaderConfigs;
    _contracts.resize(_viperConfig._allContracts.size());
    _lastOrderRequests.resize(_viperConfig._allContracts.size());
    _allPaths.reserve(256);
    const auto& pricingConfig = _viperConfig._pricingConfigs;
    for (uint32_t contractIndex = 0; contractIndex < _viperConfig._allContracts.size(); ++contractIndex) {
        ConfigContract& configContract = _viperConfig._allContracts[contractIndex];
        Contract& contract = _contracts[contractIndex];
        contract._def = configContract;
        contract.initMarkets();
        contract._hedgeOffsetPrice = 0;
        contract._extCommission = ToRhonePrice(pricingConfig[contractIndex]._asExtCommission);
        auto iter = std::find_if(_viperConfig._arbieSpreaderConfigs.begin(), _viperConfig._arbieSpreaderConfigs.end(), [&contract](const ArbieSpreaderConfig& config) {
            return config._contract == contract._def._name;
        });
        if (iter != asConfig.end()) {
            contract._usedInQuoting = true;
            auto& config = *iter;
            contract._quoteExternally = config._quoteExternal;
            contract._edge = ToRhonePrice(config._edge);
            contract._quoteSize = config._quoteSize;
            contract._hedgeOffset = config._hedgeOffset;
            contract._hedgeOffsetPrice = config._hedgeOffset * configContract._tickSize;

            uint32_t id = 0;
            for (auto& path : config._paths) {
                logging::GetLogger().logInfo(boost::str(boost::format("AS %1%. Path %2% id=%3%") % configContract._name %
                    path._name % id));

                std::vector<PathContract> pathContracts;
                for (const auto& configPathContract : path._contracts) {
                    auto* pathContract = _viperConfig.getContract(configPathContract._contract);
                    if (!pathContract) {
                        logging::GetLogger().logErrorAndThrow("Contract " + configPathContract._contract + " not found");
                    }
                    pathContracts.emplace_back(&_contracts[pathContract->_internalId], configPathContract._multiplier);
                }
                _allPaths.emplace_back(BID, path._name, id, &contract, pathContracts, path);
                auto& bidPath = _allPaths.back();
                for (auto& pathContract : pathContracts) {
                    pathContract._contract->_affectedPaths.emplace_back(&bidPath);
                }
                contract._paths[BID].emplace_back(&bidPath);
                _allPaths.emplace_back(ASK, path._name, id, &contract, pathContracts, path);
                auto& askPath = _allPaths.back();
                askPath._position = bidPath._position;
                bidPath._otherSidePath = &askPath;
                askPath._otherSidePath = &bidPath;
                for (auto& pathContract : pathContracts) {
                    pathContract._contract->_affectedPaths.emplace_back(&askPath);
                }
                contract._paths[ASK].emplace_back(&askPath);
                ++id;
            }
        }
    }
}

void ArbieSpreader::registerOrderClient() {
    for (const auto& contract : _contracts) {
        sendRegisterOrderClientRequest(contract._def._internalId, {StrategyId::MarketMaker, StrategyId::RelativeValue});
    }
}

void ArbieSpreader::runCycle(uint64_t now) {
#ifdef IS_DEV
    MAYBE_UNUSED bool hasUpdate = false;
    uint64_t startTimeStamp = clockUtil::GetCycles();
#endif
    auto iter = _updatedContracts.begin();
    while (iter != _updatedContracts.end()) {
#ifdef IS_DEV
        if (!hasUpdate) {
            DLOG(0, "AS. Begin update quotes {i1}", 0)
        }
        hasUpdate = true;
#endif
        Contract* contract = *iter;
        if (contract->_usedInQuoting) {
            updateQuotes(*contract);
        }
        iter = _updatedContracts.erase(iter);
    }
#ifdef IS_DEV
    if (hasUpdate) {
        const uint64_t latency = clockUtil::GetCycles() - startTimeStamp;
        DLOG(0, "AS. CalculateQuotes Latency {i1}", latency)
    }
#endif
}

bool ArbieSpreader::isContractQuoteExternal(uint32_t contractId) const noexcept {
    return _contracts[contractId]._quoteExternally;
}

void ArbieSpreader::onQuoteExternalChanged(SystemState quoteExt) {
    updateQuotes(true);
}

void ArbieSpreader::onExchangeQuoteExternalChanged(const SharedSystemState::QuoteExtBitset& quoteExt) {
    updateQuotes(true);
}

void ArbieSpreader::markContractDirty(Contract& contract, uint32_t side) {
    contract.setMarketDirty(side);
    addUpdatedContract(contract);
}

void ArbieSpreader::addUpdatedContract(Contract& contract) {
//    if (contract._usedInQuoting) {
//        _updatedContracts.emplace(&contract);
//    }
    for (auto* path : contract._affectedPaths) {
        if (path->_dirty) {
            TLOG(0, "AS {C[i1]}. trigger {C[i2]} recalculate, path={i3}", contract.name(), path->_contract->name(), path->_id)
            _updatedContracts.emplace(path->_contract);
        }
    }
}

void ArbieSpreader::updateQuotes(bool force) {
    for (auto& contract : _contracts) {
        if (contract._usedInQuoting) {
            updateQuotes(contract, force);
        }
    }
}

bool ArbieSpreader::updateQuotes(Contract& contract, bool force) {
    contract.updatePaths();

    if (!isEnabled()) {
        return false;
    }

    auto& lastRequest = _lastOrderRequests[contract._def._internalId];
    auto pBestBid = contract._paths[BID][0];
    auto pBestAsk = contract._paths[ASK][0];
    bool quoteUpdated = false;
    bool canQuoteExternal = canQuoteExternally() && canExchangeQuoteExternally(contract._def._venue) && contract._quoteExternally;
    if (pBestBid->_priceSize.valid()) {
        auto& quote = contract._quote[BID];
        quote._leanPrice = pBestBid->_priceSize._price + pBestBid->_commission;
        quote._internalPrice = pBestBid->_priceSize._price - contract._edge;
        DLOG(0, "AS {C[i1]}. Path {i2} BID Adjust internal price. leanPrice={d1} internalPrice={d2} position={i3} posPriceAdjustment={d3}", contract.name(),
             pBestBid->_id, ToDollars(quote._leanPrice), ToDollars(quote._internalPrice), pBestBid->_position->_size,
             ToDollars(pBestBid->_position->_priceOffset))
        quote._internalPrice -= pBestBid->_position->_priceOffset;
        quote._internalSize = std::min<hf::qty_t>(std::min<hf::qty_t>(pBestBid->_priceSize._size, contract._quoteSize), (pBestBid->_config->_maxPos - pBestBid->_position->_size));
        quote._hedgePath = pBestBid->_id;
        if (canQuoteExternal) {
            quote._externalPrice = RoundToTickSize(quote._internalPrice, contract._def._tickSize, BID);
            quote._externalSize = quote._internalSize;
        } else {
            quote._externalPrice = hf::NULL_PRICE;
            quote._externalSize = 0;
        }

        if (contract._lastQuoteSent[BID] != quote || contract._forceUpdate[BID] || force) {
            quoteUpdated = true;
            lastRequest._quote[BID] = _orderRequestMgr.sendQuoteRequest(
                    contract._def._internalId,
                    orderId(StrategyId::ArbieSpreader, Order::Type::Quoting), BID,
                    quote._internalPrice, quote._internalSize, quote._externalPrice, quote._externalSize,
                    canQuoteExternal, 0);
            LOG(0, "AS {[i1]}. {i2} Update quote. Internal: {i3}@{d1} External: {i4}@{d2}. LeanPath={i5}", contract.name(),
                BID, quote._internalSize, ToDollars(quote._internalPrice), quote._externalSize,
                ToDollars(quote._externalPrice), quote._hedgePath)
            contract._lastQuoteSent[BID] = quote;
            contract._forceUpdate[BID] = false;
            if (quote._externalSize) {
                std::unordered_set<Market*> affected_markets;
                if (contract.addAggressiveQuoteAdjustment<BID>(pBestBid, quote, canQuoteExternal, affected_markets)) {
                    markContractDirty(contract, BID);
                    for (auto market : affected_markets) {
                        markContractDirty(_contracts[market->getContractId()], market->getSide());
                    }
                }
            }
        }
    } else {
        auto& quote = contract._quote[BID];
        if (quote._hedgePath != -1) {
            quote.reset();
            quoteUpdated = true;
            lastRequest._quote[BID] = _orderRequestMgr.sendQuoteRequest(
                    contract._def._internalId,
                    orderId(StrategyId::ArbieSpreader, Order::Type::Quoting), BID,
                    quote._internalPrice, quote._internalSize, quote._externalPrice, quote._externalSize,
                    canQuoteExternal, 0);
            LOG(0, "AS {[i1]}. {i2} Update quote. Internal: {i3}@{d1} External: {i4}@{d2}. LeanPath={i5}", contract.name(),
                BID, quote._internalSize, ToDollars(quote._internalPrice), quote._externalSize,
                ToDollars(quote._externalPrice), quote._hedgePath)
            contract._lastQuoteSent[BID] = quote;
            contract._forceUpdate[BID] = false;
        }
    }

    if (pBestAsk->_priceSize.valid()) {
        auto& quote = contract._quote[ASK];
        quote._leanPrice = pBestAsk->_priceSize._price - pBestAsk->_commission;
        quote._internalPrice = pBestAsk->_priceSize._price + contract._edge;
        DLOG(0, "AS {C[i1]}. Path {i2} ASK Adjust internal price. leanPrice={d1} internalPrice={d2} position={i3} posPriceAdjustment={d3}", contract.name(),
             pBestAsk->_id, ToDollars(quote._leanPrice), ToDollars(quote._internalPrice), pBestAsk->_position->_size,
             ToDollars(pBestAsk->_position->_priceOffset))
        quote._internalPrice -= pBestAsk->_position->_priceOffset;
        quote._internalSize = std::min<hf::qty_t>(std::min<hf::qty_t>(pBestAsk->_priceSize._size, contract._quoteSize), (pBestAsk->_config->_maxPos + pBestAsk->_position->_size));
        quote._hedgePath = pBestAsk->_id;
        if (canQuoteExternal) {
            quote._externalPrice = RoundToTickSize(quote._internalPrice, contract._def._tickSize, ASK);
            quote._externalSize = quote._internalSize;
        } else {
            quote._externalPrice = hf::NULL_PRICE;
            quote._externalSize = 0;
        }
        if (contract._lastQuoteSent[ASK] != quote || contract._forceUpdate[ASK] || force) {
            quoteUpdated = true;
            lastRequest._quote[ASK] = _orderRequestMgr.sendQuoteRequest(
                    contract._def._internalId,
                    orderId(StrategyId::ArbieSpreader, Order::Type::Quoting), ASK,
                    quote._internalPrice, quote._internalSize, quote._externalPrice, quote._externalSize,
                    canQuoteExternal, 0);
            LOG(0, "AS {[i1]}. {i2} Update quote. Internal: {i3}@{d1} External: {i4}@{d2}. LeanPath={i5}", contract.name(),
                ASK, quote._internalSize, ToDollars(quote._internalPrice), quote._externalSize,
                ToDollars(quote._externalPrice), quote._hedgePath)
            contract._lastQuoteSent[ASK] = quote;
            contract._forceUpdate[ASK] = false;
            if (quote._externalSize) {
                std::unordered_set<Market*> affected_markets;
                if (contract.addAggressiveQuoteAdjustment<ASK>(pBestAsk, quote, canQuoteExternal, affected_markets)) {
                    markContractDirty(contract, ASK);
                    for (auto market : affected_markets) {
                        markContractDirty(_contracts[market->getContractId()], market->getSide());
                    }
                }
            }
        }
    } else {
        auto& quote = contract._quote[ASK];
        if (quote._hedgePath != -1) {
            quote.reset();
            quoteUpdated = true;
            lastRequest._quote[ASK] = _orderRequestMgr.sendQuoteRequest(
                    contract._def._internalId,
                    orderId(StrategyId::ArbieSpreader, Order::Type::Quoting), ASK,
                    quote._internalPrice, quote._internalSize, quote._externalPrice, quote._externalSize,
                    canQuoteExternal, 0);
            LOG(0, "AS {[i1]}. {i2} Update quote. Internal: {i3}@{d1} External: {i4}@{d2}. LeanPath={i5}", contract.name(),
                ASK, quote._internalSize, ToDollars(quote._internalPrice), quote._externalSize,
                ToDollars(quote._externalPrice), quote._hedgePath)
            contract._lastQuoteSent[ASK] = quote;
            contract._forceUpdate[ASK] = false;
        }
    }

    if (quoteUpdated) {
        _orderRequestMgr.notifyOrderRequest(contract._def._internalId, StrategyId::ArbieSpreader, lastRequest);

    }
    return quoteUpdated;
}

void ArbieSpreader::sendHedgeOrder(uint32_t internalId,
                                   uint32_t hedgeContractId,
                                   uint32_t side,
                                   hf::price_t internalPrice,
                                   hf::price_t externalPrice,
                                   hf::qty_t size,
                                   bool external,
                                   uint32_t hedgeId,
                                   uint32_t marketId,
                                   uint32_t pathId) {
    LOG(0, "AS {C[i1]}. Send Hedge to {C[i2]}. Side={i3} InternalPrice={d1} ExternalPrice={d2} Size={i4} isExternal={i5} path={i6} hedgeId={i7} marketId={i8}", getContract(internalId),
        getContract(hedgeContractId), side, ToDollars(internalPrice), ToDollars(externalPrice), size, external, pathId,
        hedgeId, marketId)
    _lastOrderRequests[hedgeContractId]._hedge[side] = _orderRequestMgr.sendHedgeRequest(
            hedgeContractId, orderId(StrategyId::ArbieSpreader, Order::Type::Hedging), side, internalPrice,
            externalPrice, size, external, false, 0, 0, marketId, hedgeId);
    _orderRequestMgr.notifyOrderRequest(hedgeContractId, StrategyId::ArbieSpreader, _lastOrderRequests[hedgeContractId]);
}