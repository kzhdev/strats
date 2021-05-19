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
    _allPaths.reserve(65536);
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
            contract._pathPosition.resize(config._paths.size());
            contract._pathShortTermFairOffset.resize(config._paths.size());
            contract._pathShortTermFairs.resize(config._paths.size());
            for (auto& stfOffset : contract._pathShortTermFairOffset) {
                stfOffset = 0;
            }
            uint32_t id = 0;
            for (auto& path : config._paths) {
                logging::GetLogger().logInfo(boost::str(boost::format("AS %1%. Path %2% id=%3%") % configContract._name %
                    path._name % id));

                auto& pathShortTermFares = contract._pathShortTermFairs[id];
                std::vector<PathContract> pathContracts;
                for (const auto& configPathContract : path._contracts) {
                    auto* pathContract = _viperConfig.getContract(configPathContract._contract);
                    if (!pathContract) {
                        logging::GetLogger().logErrorAndThrow("Contract " + configPathContract._contract + " not found");
                    }
                    pathContracts.emplace_back(&_contracts[pathContract->_internalId], configPathContract._multiplier);

                    for (auto& leg : pathContract->_legs) {
                        auto it = pathShortTermFares.find(leg._index);
                        if (it == pathShortTermFares.end()) {
                            pathShortTermFares.emplace(leg._index, configPathContract._multiplier * leg._ratio);
                        } else {
                            it->second._ratio += configPathContract._multiplier * leg._ratio;

                            if (it->second._ratio == 0) {
                                pathShortTermFares.erase(it);
                            }
                        }
                    }
                }

                for (auto& leg : configContract._legs) {
                    auto it = pathShortTermFares.find(leg._index);
                    if (it == pathShortTermFares.end()) {
                        pathShortTermFares.emplace(leg._index, -1 * leg._ratio);
                    } else {
                        it->second._ratio += -1 * leg._ratio;

                        if (it->second._ratio == 0) {
                            pathShortTermFares.erase(it);
                        }
                    }
                }

                _allPaths.emplace_back(BID, path._name, id, &contract, pathContracts, path);
                auto& bidPath = _allPaths.back();
                for (auto& pathContract : pathContracts) {
                    pathContract._contract->_affectedPaths.emplace_back(&bidPath);
                }
                contract._paths[BID].emplace_back(&bidPath);
                _allPaths.emplace_back(ASK, path._name, id, &contract, pathContracts, path);
                auto& askPath = _allPaths.back();
                bidPath._otherSidePath = &askPath;
                askPath._otherSidePath = &bidPath;
                for (auto& pathContract : pathContracts) {
                    pathContract._contract->_affectedPaths.emplace_back(&askPath);
                }
                contract._paths[ASK].emplace_back(&askPath);

                if (pathShortTermFares.empty()) {
                    logging::GetLogger().logInfo(boost::str(
                            boost::format("AS %1%. Path %2% id=%3% ShortTermFares: 0") % configContract._name %
                            path._name % id));
                } else {
                    logging::GetLogger().logInfo(boost::str(
                            boost::format("AS %1%. Path %2% id=%3% ShortTermFares:") % configContract._name %
                            path._name % id));

                    for (auto& kvp : pathShortTermFares) {
                        logging::GetLogger().logInfo(boost::str(
                                boost::format("    %1% %2%") % kvp.second._ratio %
                                _viperConfig._allContracts[kvp.first]._name));
                        getStrategy(StrategyId::RelativeValue)->sendGenericMessage(ViperMessageType::SubscribeSTFUpdate, kvp.first);
                        _mapShortTermFairPath.emplace(kvp.first, &bidPath);
                    }
                    getStrategy(StrategyId::RelativeValue)->sendGenericMessage(ViperMessageType::SubscribeSTFUpdate, contract._def._internalId);
                }

                ++id;
            }
        }
    }
    logging::GetLogger().logInfo(boost::str(boost::format("AS. NumberOfPaths=%1%") % _allPaths.size()));
}

void ArbieSpreader::registerOrderClient() {
    for (const auto& contract : _contracts) {
        sendRegisterOrderClientRequest(contract._def._internalId, {StrategyId::MarketMaker, StrategyId::RelativeValue});
    }
}

void ArbieSpreader::runCycle(uint64_t now) {
    bool hasUpdate = false;
    auto iter = _updatedContracts.begin();
    while (iter != _updatedContracts.end()) {
#ifdef IS_DEV
        if (!hasUpdate) {
            DLOG(0, "AS. Begin update quotes {i1}", 0)
        }
#endif
        hasUpdate = true;
        Contract* contract = *iter;
        if (contract->_usedInQuoting) {
            if (BOOST_LIKELY(_quotesUpdatedContracts.find(contract) == _quotesUpdatedContracts.end())) {
                updateQuotes(*contract);
                _quotesUpdatedContracts.emplace(contract);
            } else {
                logging::GetLogger().logWarning(boost::str(boost::format("AS %1%. Quote update triggered again") % contract->_def._name));
                _updateContractsInNextRound.emplace(contract);
            }
        }
        iter = _updatedContracts.erase(iter);
    }
    _updatedContracts.swap(_updateContractsInNextRound);
    _quotesUpdatedContracts.clear();

    if (hasUpdate) {
        const uint64_t latency = clockUtil::GetCycles() - now;
        ILOG(0, "AS. CalculateQuotes Latency {i1}", latency)
    }
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
    for (auto* path : contract._affectedPaths) {
        if (path->_dirty) {
            TLOG(0, "AS {C[i1]}. trigger {C[i2]} recalculate, path={i3} side={i3}", contract.name(),
                 path->_contract->name(), path->_id, path->_side)
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
             pBestBid->_id, ToDollars(quote._leanPrice), ToDollars(quote._internalPrice), pBestBid->_position._size,
             ToDollars(pBestBid->_position._priceOffset))
        quote._internalPrice -= pBestBid->_position._priceOffset;
        quote._internalSize = std::min<hf::qty_t>(std::min<hf::qty_t>(pBestBid->_priceSize._size, contract._quoteSize), (pBestBid->_config->_maxPos - pBestBid->_position._size));
        quote._hedgePath = pBestBid->_id;
        if (canQuoteExternal) {
            quote._externalPrice = RoundToTickSize(quote._internalPrice, contract._def._tickSize, BID);
            quote._externalSize = contract.getExternalQuoteSize<BID>(quote);
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
                    canQuoteExternal, pBestBid->_leanOnRV, 0);
            LOG(0, "AS {[i1]}. {i2} Update quote. Internal: {i3}@{d1} External: {i4}@{d2}. LeanPath={i5} LeanOnRV={i6}", contract.name(),
                BID, quote._internalSize, ToDollars(quote._internalPrice), quote._externalSize,
                ToDollars(quote._externalPrice), quote._hedgePath, pBestBid->_leanOnRV)
            contract._lastQuoteSent[BID] = quote;
            contract._forceUpdate[BID] = false;
            tryAddAggressiveQuoteAdjustment<BID>(contract, pBestBid, quote, canQuoteExternal);
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
                    canQuoteExternal, false,0);
            LOG(0, "AS {[i1]}. {i2} Update quote. Internal: {i3}@{d1} External: {i4}@{d2}. LeanPath={i5} LeanOnRV=0", contract.name(),
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
             pBestAsk->_id, ToDollars(quote._leanPrice), ToDollars(quote._internalPrice), pBestAsk->_position._size,
             ToDollars(pBestAsk->_position._priceOffset))
        quote._internalPrice -= pBestAsk->_position._priceOffset;
        quote._internalSize = std::min<hf::qty_t>(std::min<hf::qty_t>(pBestAsk->_priceSize._size, contract._quoteSize), (pBestAsk->_config->_maxPos + pBestAsk->_position._size));
        quote._hedgePath = pBestAsk->_id;
        if (canQuoteExternal) {
            quote._externalPrice = RoundToTickSize(quote._internalPrice, contract._def._tickSize, ASK);
            quote._externalSize = contract.getExternalQuoteSize<ASK>(quote);
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
                    canQuoteExternal, pBestAsk->_leanOnRV, 0);
            LOG(0, "AS {[i1]}. {i2} Update quote. Internal: {i3}@{d1} External: {i4}@{d2}. LeanPath={i5} LeanOnRV={i6}", contract.name(),
                ASK, quote._internalSize, ToDollars(quote._internalPrice), quote._externalSize,
                ToDollars(quote._externalPrice), quote._hedgePath, pBestAsk->_leanOnRV)
            contract._lastQuoteSent[ASK] = quote;
            contract._forceUpdate[ASK] = false;
            tryAddAggressiveQuoteAdjustment<ASK>(contract, pBestAsk, quote, canQuoteExternal);
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
                    canQuoteExternal, false,0);
            LOG(0, "AS {[i1]}. {i2} Update quote. Internal: {i3}@{d1} External: {i4}@{d2}. LeanPath={i5} LeanOnRV=0", contract.name(),
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