#include "ArbieSpreader.h"
#include <hf/ResourceManager.h>

using namespace Python;

static hf::ResourceRegistrator<ArbieSpreader> s_registrator("ArbieSpreader");

/*static*/
const ArbieSpreader::ShortTermFairOffset ArbieSpreader::Contract::_zeroStfOffset;

void ArbieSpreader::init(double frequency, uint64_t internalHeartbeatPeriod, std::function<bool(uint32_t)> pnlLimitsChecker) {
    ViperStrategyBase::init(frequency, internalHeartbeatPeriod, std::move(pnlLimitsChecker));
    initializeContracts();
}

void ArbieSpreader::initializeContracts() {
    const auto& asConfig = _viperConfig._arbieSpreaderConfigs;
    auto numContracts = _viperConfig._allContracts.size();
    _contracts.resize(numContracts);
    _lastOrderRequests.resize(numContracts);
    _effected_markets.reserve(numContracts);
    _allPaths.reserve(65536);
    if (_viperConfig._allContracts.size() > _contractUpdatedFlag.size()) {
        logging::GetLogger().logErrorAndThrow(boost::str(boost::format("Number of contracts > %1%") % _contractUpdatedFlag.size()));
    }
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
            contract._shortTermFairOffsets.reserve(config._paths.size());
            uint32_t id = 0;
            for (auto& path : config._paths) {
                logging::GetLogger().logInfo(boost::str(boost::format("AS %1%. Path %2% id=%3%") % configContract._name %
                    path._name % id));

                std::map<uint32_t, ShortTermFairOffset::ShortTermFair> pathShortTermFares;
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
                            pathShortTermFares.try_emplace(leg._index, leg._index, -configPathContract._multiplier * leg._ratio);
                        } else {
                            it->second._ratio -= configPathContract._multiplier * leg._ratio;

                            if (it->second._ratio == 0) {
                                pathShortTermFares.erase(it);
                            }
                        }
                    }
                }

                for (auto& leg : configContract._legs) {
                    auto it = pathShortTermFares.find(leg._index);
                    if (it == pathShortTermFares.end()) {
                        pathShortTermFares.try_emplace(leg._index, leg._index, leg._ratio);
                    } else {
                        it->second._ratio += leg._ratio;

                        if (it->second._ratio == 0) {
                            pathShortTermFares.erase(it);
                        }
                    }
                }

                auto stfOffset = const_cast<ShortTermFairOffset*>(&Contract::_zeroStfOffset);
                auto stfIter = contract._shortTermFairOffsets.end();
                if (!pathShortTermFares.empty()) {
                    std::string stfId;
                    for (const auto& kvp : pathShortTermFares) {
                        stfId.append(std::to_string(kvp.second._ratio)).append("*").append(std::to_string(kvp.first));
                    }

                    stfIter = std::find_if(contract._shortTermFairOffsets.begin(),
                                             contract._shortTermFairOffsets.end(),
                                             [&stfId](const ShortTermFairOffset& offset) {
                                                 return offset._id == stfId;
                                             });
                    if (stfIter == contract._shortTermFairOffsets.end()) {
                        contract._shortTermFairOffsets.emplace_back(&contract, std::move(stfId), std::move(pathShortTermFares));
                        stfIter = std::prev(contract._shortTermFairOffsets.end());
                        stfOffset = &contract._shortTermFairOffsets.back();
                    } else {
                        stfOffset = &(*stfIter);
                    };
                }

                _allPaths.emplace_back(BID, path._name, id, &contract, pathContracts, path, stfOffset);
                auto& bidPath = _allPaths.back();
                for (auto& pathContract : pathContracts) {
                    pathContract._contract->_affectedPaths.emplace_back(&bidPath);
                }
                contract._paths[BID].emplace_back(&bidPath);
                _allPaths.emplace_back(ASK, path._name, id, &contract, pathContracts, path, stfOffset);
                auto& askPath = _allPaths.back();
                bidPath._otherSidePath = &askPath;
                askPath._otherSidePath = &bidPath;
                for (auto& pathContract : pathContracts) {
                    pathContract._contract->_affectedPaths.emplace_back(&askPath);
                }
                contract._paths[ASK].emplace_back(&askPath);

                if (stfIter == contract._shortTermFairOffsets.end()) {
                    logging::GetLogger().logInfo(boost::str(
                            boost::format("AS %1%. Path %2% id=%3% ShortTermFares: 0") % configContract._name %
                            path._name % id));
                } else {
                    logging::GetLogger().logInfo(boost::str(
                            boost::format("AS %1%. Path %2% id=%3% ShortTermFares: %4%") % configContract._name %
                            path._name % id % stfIter->_id));

                    stfIter->_paths.emplace(&bidPath);
                    stfIter->_paths.emplace(&askPath);

                    auto tryReplaceSTFsWithSpreadSTF = [stfOffset](const ConfigContract& contract) {
                        int ratio = 0;
                        for (const auto& leg : contract._legs) {
                            auto iter = stfOffset->findSTF(leg._index);
                            if (iter == stfOffset->_shortTermFairs.end()) {
                                return;
                            }

                            if (ratio == 0) {
                                ratio = iter->_ratio / leg._ratio;
                            } else if ((iter->_ratio / leg._ratio) != ratio) {
                                logging::GetLogger().logInfo(boost::str(
                                        boost::format("AS %1%. STF leg ratio does not match. leg._ratio=%2% stf._ratio=%3%") % _viperConfig._allContracts[leg._index]._name %
                                        leg._ratio % iter->_ratio));
                                return;
                            }
                        }
                        // Can replace with spread stf
                        for (const auto& leg : contract._legs) {
                            stfOffset->removeSTF(leg._index);
                        }
                        if (!stfOffset->hasSTF(contract._internalId)) {
                            stfOffset->_shortTermFairs.emplace_back(contract._internalId, ratio);
                        }
                    };

                    tryReplaceSTFsWithSpreadSTF(configContract);
                    for (const auto& contr : _viperConfig._allContracts) {
                        if (contr._legs.empty() || contr._internalId == configContract._internalId) {
                            continue;
                        }
                        tryReplaceSTFsWithSpreadSTF(contr);
                    }

                    for (auto& stf : stfOffset->_shortTermFairs) {
                        logging::GetLogger().logInfo(boost::str(
                                boost::format("    %1% %2%") % stf._ratio %
                                _viperConfig._allContracts[stf._contractId]._name));
                        getStrategy(StrategyId::RelativeValue)->sendGenericMessage(ViperMessageType::SubscribeSTFUpdate, stf._contractId);
                        _mapShortTermFairOffset.emplace(stf._contractId, &(*stfIter));
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
    uint32_t nUpdate = 0;
    auto last = _updatedContracts.end();
    auto size = last - _updatedContractCursor;
    _contractUpdatedFlag.reset();
    while (_updatedContractCursor < last) {
        auto contract = _updatedContracts[_updatedContractCursor++];
        if (contract->_usedInQuoting && !_contractUpdatedFlag.test(contract->_def._internalId)) {
#ifdef IS_DEV
            const uint64_t start = clockUtil::GetCycles();
#endif
            updateQuotes(*contract);
            _contractUpdatedFlag.set(contract->_def._internalId);
#ifdef IS_DEV
            ILOG(0, "AS {C[i1]}. updateQuotes latency {i2}", contract->name(), (clockUtil::GetCycles() - start))
#endif
            ++nUpdate;
        }
    }

    if (nUpdate) {
        const uint64_t latency = clockUtil::GetCycles() - now;
        ILOG(0, "AS. CalculateQuotes Latency {i1}. {i2} updated contract(s). size={i3}", latency, nUpdate, size)
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

void ArbieSpreader::addUpdatedContract(Contract& contract) {
    for (auto* path : contract._affectedPaths) {
        if (path->_dirty) {
            pushUpdatedContract(path->_contract);
        }
    }
}

void ArbieSpreader::pushUpdatedContract(Contract* contract) {
    auto index = _updatedContracts.reserve();
    _updatedContracts[index] = contract;
    _updatedContracts.publish(index);
}

void ArbieSpreader::markContractDirty(Contract& contract, Market* updatedMarket) {
    bool bestMarketChanged= false;
    if (updatedMarket->getSide() == BID) {
        bestMarketChanged = contract.updateBestMarket<BID>(updatedMarket);
    } else {
        bestMarketChanged = contract.updateBestMarket<ASK>(updatedMarket);
    }
    if (bestMarketChanged) {
        contract.setMarketDirty(updatedMarket->getSide());
        addUpdatedContract(contract);
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
    auto* pBestBid = contract._paths[BID][0];
    auto* pBestAsk = contract._paths[ASK][0];
    bool quoteUpdated = false;
    bool canQuoteExternal = canQuoteExternally() && canExchangeQuoteExternally(contract._def._venue) && contract._quoteExternally;
    if (pBestBid->_priceSize.valid()) {
        auto& quote = contract._quote[BID];
        quote._leanPrice = pBestBid->_priceSize._price + pBestBid->_commission;
        quote._internalPrice = pBestBid->_priceSize._price - contract._edge;
        quote._internalSize = std::min<hf::qty_t>(pBestBid->_priceSize._size, contract._quoteSize);
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
        quote._internalSize = std::min<hf::qty_t>(pBestAsk->_priceSize._size, contract._quoteSize);
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