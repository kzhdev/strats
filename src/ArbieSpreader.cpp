#include "ArbieSpreader.h"
#include <hf/ResourceManager.h>
#include "MathStructures.h"

using namespace Python;

static hf::ResourceRegistrator<ArbieSpreader> s_registrator("ArbieSpreader");

/*static*/
const ArbieSpreader::ShortTermFairOffset ArbieSpreader::Contract::_zeroStfOffset;
ArbieSpreader* ArbieSpreader::_self = nullptr;

void ArbieSpreader::init(double frequency, uint64_t internalHeartbeatPeriod, std::function<bool(uint32_t)> pnlLimitsChecker) {
    ArbieSpreader::_self = this;
    ViperStrategyBase::init(frequency, internalHeartbeatPeriod, std::move(pnlLimitsChecker));
    auto numFactors = _viperConfig._vectorConfigs.size();
    _vectorsRange.resize(numFactors, 0.);
    _vectorsMaxPosition.resize(numFactors, 0.);
    Matrix pricingMatrix;
    _viperConfig.populatePricingMatrix(pricingMatrix);
    _riskMatrix = pricingMatrix.inverse().transpose();
    for (uint32_t index = 0; index < numFactors; ++index) {
        VectorConfig& vc = _viperConfig._vectorConfigs[index];
        _vectorsRange[index] = vc._range;
        _vectorsMaxPosition[index] = vc._maxPos;
    }
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
            _edgeMultiplier = boost::lexical_cast<double>(_viperConfig.getGlobal(AS_EDGE_MULTIPLIER));
            contract._edge = ToRhonePrice(config._edge * _edgeMultiplier);
            contract._quoteSize = config._quoteSize;
            contract._hedgeOffset = config._hedgeOffset;
            contract._maxTicksThroughMarket = config._maxTicksThroughMarket * configContract._tickSize;
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

                    if (pathContract->_legs.empty()) {
                        auto it = pathShortTermFares.find(pathContract->_internalId);
                        if (it == pathShortTermFares.end()) {
                            pathShortTermFares.try_emplace(pathContract->_internalId, pathContract->_internalId, -configPathContract._multiplier);
                        } else {
                            it->second._ratio -= configPathContract._multiplier;

                            if (it->second._ratio == 0) {
                                pathShortTermFares.erase(it);
                            }
                        }
                    } else {
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
                }

                if (configContract._legs.empty()) {
                    auto it = pathShortTermFares.find(configContract._internalId);
                    if (it == pathShortTermFares.end()) {
                        pathShortTermFares.try_emplace(configContract._internalId, configContract._internalId, 1);
                    } else {
                        it->second._ratio += 1;

                        if (it->second._ratio == 0) {
                            pathShortTermFares.erase(it);
                        }
                    }
                } else {
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
                for (auto& pathContract : bidPath._leanContracts) {
                    pathContract._contract->_affectedPaths[pathContract._side].emplace_back(&bidPath);
                }
                contract._paths[BID].emplace_back(&bidPath);
                _allPaths.emplace_back(ASK, path._name, id, &contract, pathContracts, path, stfOffset);
                auto& askPath = _allPaths.back();
                bidPath._otherSidePath = &askPath;
                askPath._otherSidePath = &bidPath;
                for (auto& pathContract : askPath._leanContracts) {
                    pathContract._contract->_affectedPaths[pathContract._side].emplace_back(&askPath);
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

                    calculateRVSTFMove(contract, *stfIter);
                }

                ++id;
            }
        }
    }
    logging::GetLogger().logInfo(boost::str(boost::format("AS. NumberOfPaths=%1%") % _allPaths.size()));
}

void ArbieSpreader::calculateRVSTFMove(Contract& quoteContract, ShortTermFairOffset& stfOffset, bool force) {
    if (stfOffset._rvSTFMove && !force) {
        // already calculated
        return;
    }

    auto numVectors = _viperConfig._vectorConfigs.size();
    std::vector<double> factorNetPos;
    factorNetPos.resize(numVectors, 0.);
    Matrix factorSTFMove(numVectors, 1);

    auto calculateFectorsNetPos = [this, &factorNetPos](const ConfigContract& contract, int32_t radio, double qtyMultiplier) {
        for (uint32_t i = 0; i < factorNetPos.size(); ++i) {
            factorNetPos[i] += radio * _riskMatrix.getValue(i, contract._internalId) * qtyMultiplier;
//#ifdev IS_DEV
//            LOG(0, "{C[i1]} riskMatrix[{i2}][{i3}]={d1} qtyMultipler={d2} radio={i4} net_pos={d3}", contract._name, i, contract._internalId, _riskMatrix.getValue(i, contract._internalId), qtyMultiplier, radio, factorNetPos[i])
//#endif
        }
    };

    for (auto& stf : stfOffset._shortTermFairs) {
        auto& contract = _viperConfig._allContracts[stf._contractId];
        auto qtyMultiplier = _viperConfig._pricingConfigs[contract._internalId]._quantityMultiplier;
        if (!contract._legs.empty()) {
            for (auto& leg : contract._legs) {
                auto& legContract = _viperConfig._allContracts[leg._index];
                calculateFectorsNetPos(legContract, stf._ratio * leg._ratio, qtyMultiplier);
            }
        } else {
            calculateFectorsNetPos(contract, stf._ratio, qtyMultiplier);
        }
    }

    for (size_t i = 0; i < numVectors; ++i) {
        factorSTFMove.setValue(i, 0, -factorNetPos[i] * _vectorsRange[i] / _vectorsMaxPosition[i]);
//#ifdef IS_DEV
//        LOG(0, "Factor {i1} NetPos={d1}. range={d2} maxPos={d3} Factor STF move for 1 unit path: {d4}", i, factorNetPos[i], _vectorsRange[i], _vectorsMaxPosition[i], factorSTFMove.getValue(i, 0))
//#endif
    }

    auto numContracts = _contracts.size();
    auto contractSTFMoveMatrix = _riskMatrix.transpose().multiply(factorSTFMove);
    assert(contractSTFMoveMatrix._numberOfColumns == 1);
    std::vector<double> contractSTFMove(numContracts, 0.);
    for (uint32_t i = 0; i < numContracts; ++i) {
        auto& contract = _viperConfig._allContracts[i];
        if (contract._legs.empty()) {
            contractSTFMove[i] = contractSTFMoveMatrix.getValue(contract._internalId, 0) / _viperConfig._pricingConfigs[contract._internalId]._pricingMultiplier;
        } else {
            for (auto& leg : contract._legs) {
                contractSTFMove[i] += contractSTFMove[leg._index] * leg._ratio;
            }
        }
//#ifdef IS_DEV
//        LOG(0, "{C[i1]} stf move {d1}", contract._name, contractSTFMove[i])
//#endif
    }

#ifdef IS_DEV
    std::string path;
#endif
    double stfMove = 0.;
    for (auto& stf : stfOffset._shortTermFairs) {
#ifdef IS_DEV
        auto& contract = _viperConfig._allContracts[stf._contractId];
        path.append(std::to_string(stf._ratio)).append("*").append(contract._name);
#endif
        stfMove += stf._ratio * contractSTFMove[stf._contractId];
    }
    stfOffset._rvSTFMove = ToRhonePrice(stfMove / 2);

#ifdef IS_DEV
    LOG(0, "{C[i1]} rv stf move for path {i2} = {d1}", quoteContract.name(), path, stfMove)
#else
    LOG(0, "{C[i1]} rv stf move for path {i2} = {d1}", quoteContract.name(), (*stfOffset._paths.begin())->_id, stfMove)
#endif
}

void ArbieSpreader::registerOrderClient() {
    for (const auto& contract : _contracts) {
        sendRegisterOrderClientRequest(contract._def._internalId, {StrategyId::MarketMaker, StrategyId::RelativeValue});
    }
}

void ArbieSpreader::runCycle(uint64_t now) {
#ifdef ENABLE_PERF
    auto begin = clockUtil::getCpuTicksStart();
#endif
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
#ifdef ENABLE_PERF
        auto end = clockUtil::getCpuTicksEnd();
        PerfMonitor::get()->addPerfPoint(PerfId::AS_RunCycle, PerfType::Begin, begin);
        PerfMonitor::get()->addPerfPoint(PerfId::AS_RunCycle, PerfType::End, end);
        ADD_PERF_VALUE(PerfId::AS_UpdateContractCount, nUpdate);
#endif
        const uint64_t latency = clockUtil::GetCycles() - now;
        ILOG(0, "AS. CalculateQuotes Latency {i1}. {i2} updated contract(s). size={i3}", latency, nUpdate, size)
    }
}

bool ArbieSpreader::canEnable() const noexcept {
    if (!isEdgeMultiplierWithinLimits(_edgeMultiplier)) {
        sendErrorMessage(boost::str(boost::format("AS width multiplier not within limits. Value must be > 1 and < 10 Value=%1%") % _edgeMultiplier), ErrorType::Critical);
        return false;
    }
    return true;
}

void ArbieSpreader::handleCheckQuotes() {
    LOG(0, "AS check quotes {i1}", 0);
    updateQuotes(false);
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
    SCOPED_PERF_POINT(PerfId::AS_UpdateQuotes);
    contract.updatePaths(force);

    if (BOOST_UNLIKELY(!isEnabled() && (state() != SystemState::CheckQuotes))) {
        return false;
    }

    bool canQuoteExternal = canQuoteExternally() && canExchangeQuoteExternally(contract._def._venue) && contract._quoteExternally;
    auto& lastRequest = _lastOrderRequests[contract._def._internalId];
    bool quoteUpdated = updateQuote<BID>(contract, force, canQuoteExternal, lastRequest);
    quoteUpdated |= updateQuote<ASK>(contract, force, canQuoteExternal, lastRequest);

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