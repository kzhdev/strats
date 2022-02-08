#include "ArbieSpreader.h"

using namespace Python;

COLD void ArbieSpreader::onManualFill(ManualFill &fill) {
    auto &contract = _contracts[fill._contract];
    uint32_t side = fill._size > 0 ? BID : ASK;
    uint32_t size = std::abs(fill._size);

    if (!isEnabled()) {
        LOG(0, "AS {C[i1]}. ManualFill not accepted. AS not enabled. Side={i2} Qty={i3}", contract.name(), side, size)
        sendErrorMessage(boost::str(boost::format("AS ManualFill not accepted. AS not enabled. Contract=%1% Side=%2% Qty=%3%") %
                                    contract._def._name % side % size), ErrorType::Critical);
        return;
    }

    if (contract._usedInQuoting) {
        contract._forceUpdate[side] = true;
        pushUpdatedContract(&contract);

        auto& bidMarket = contract._markets[BID][Market::External];
        auto& askMarket = contract._markets[ASK][Market::External];

        hf::price_t price;
        auto [bidPrice, bidQty, askPrice, askQty] = std::make_tuple(bidMarket.getPrice(), bidMarket.getUnAdjustedSize(),
                                                                    askMarket.getPrice(), askMarket.getUnAdjustedSize());
        if(bidPrice != hf::NULL_PRICE && bidQty > 0 && askPrice != hf::NULL_PRICE && askQty > 0) {
            price = (bidPrice * askQty + askPrice * bidQty) / (bidQty + askQty);
        }
        else {
            price = GetPriceForManualFill(side, bidPrice, bidQty, askPrice, askQty);
        }

        hf::qty_t unhedgedSize;
        _fillsToGUI.clear();
        if (side == BID) {
            unhedgedSize = updateHedge<BID>(contract, price, size, _fillsToGUI, false);
        } else {
            unhedgedSize = updateHedge<ASK>(contract, price, size, _fillsToGUI, false);
        }

        if (unhedgedSize != size) {
            sendMessageToFront<AcceptedManualFillMessage>(ViperMessageType::AcceptedManualFill, [&](AcceptedManualFillMessage& msg) {
                msg._contractId = fill._contract;
                msg._isInternal = false;
                msg._size = size - unhedgedSize;
                msg._price = price;
                msg._stf = hf::NULL_PRICE;
            });

            for (auto& fillMsg : _fillsToGUI) {
                sendMessageToFront<FillMessage>(ViperMessageType::Fill, [&](FillMessage& msg) {
                    memcpy(&msg, &fillMsg, sizeof(FillMessage));
                });
            }
        }

        if (unhedgedSize) {
            LOG(0, "AS {C[i1]}. got manual filled. Side={i2} Price={d1} Qty={i3} UnhedgedSize={i4}. Send unhedgeSize to IME", contract.name(),
                side, ToDollars(price), size, unhedgedSize)

            getStrategy(StrategyId::MarketMaker)->sendMessage<FillMessage>(
                    ViperMessageType::Fill,
                    [&fill, price, unhedgedSize, side](FillMessage& message) {
                        message._contractId = fill._contract;
                        message._price = price;
                        message._qty = unhedgedSize;
                        message._side = side;
                        message._strategy = StrategyId::ArbieSpreader;
                        message._source = FillSource::ASUnableToHedge;
                        message._forceOverFill = true;
                    });
        }
    } else {
        LOG(0, "AS {C[i1]}. ManualFill not accepted. Not a quoting contract. Side={i2} Qty={i3}", contract.name(), side, size)
        sendErrorMessage(boost::str(boost::format("ManualFill not accepted. Not a quoting contract. Contract=%1% Side=%2% Qty=%3%") %
            contract._def._name % side % size), ErrorType::Critical);
    }
}

COLD void ArbieSpreader::onEndOfManualFills() {}

void ArbieSpreader::handleFill(const FillMessage& fill) {
    auto& contract = _contracts[fill._contractId];
    if (contract._usedInQuoting) {
        bool hedgeSent = false;
        if (isEnabled()) {
            contract._forceUpdate[fill._side] = true;
            pushUpdatedContract(&contract);

            LOG(0, "AS {C[i1]}. got filled. Side={i2} Price={d1} Qty={i3} Source={i4} Type={i5}", contract.name(),
                fill._side, ToDollars(fill._price), fill._qty, toUInt(fill._source), toUInt(fill._type))

            hf::qty_t unhedgedSize;
            _fillsToGUI.clear();
            if (fill._side == BID) {
                unhedgedSize = updateHedge<BID>(contract, fill._price, fill._qty, _fillsToGUI, fill._type == FillType::Internal);
            } else {
                unhedgedSize = updateHedge<ASK>(contract, fill._price, fill._qty, _fillsToGUI, fill._type == FillType::Internal);
            }

            if (unhedgedSize != fill._qty) {
                if (contract._banInitiator[fill._side] != Contract::BanInitiator::Aggressor) {
                    auto now = clockUtil::GetCycles();
                    auto banExpireTime = now + _contractBanPeriod;
                    contract._banExpiresAt[fill._side] = banExpireTime;
                    contract._banInitiator[fill._side] = Contract::BanInitiator::Fill;
                    _bannedContracts.emplace(&contract);
                    LOG(0, "AS {C[i1]}. {i2} Initiate temporary bans until {i3}", contract.name(), fill._side, banExpireTime)
                    for (auto* p : contract._affectedPaths[fill._side]) {
                        p->_priceSize.reset();
                        p->_banExpiresAt = banExpireTime;
                        p->_resort = true;
                        ArbieSpreader::get()->_bannedPaths.emplace(p);
                        LOG(0, "AS {C[i1]}. {i2} Path {i3} banned temporarily until {i4}", p->_contract->name(),
                            p->_side, p->_id, p->_banExpiresAt)
                    }
                }

                sendMessageToFront<FillMessage>(ViperMessageType::Fill, [&](FillMessage& message) {
                    memcpy(&message, &fill, sizeof(FillMessage));
                    message._rvStf = hf::NULL_PRICE;
                    message._qty = fill._qty - unhedgedSize;
                });

                for (auto& fillMsg : _fillsToGUI) {
                    sendMessageToFront<FillMessage>(ViperMessageType::Fill, [&](FillMessage& msg) {
                        memcpy(&msg, &fillMsg, sizeof(FillMessage));
                    });
                }
            }

            if (unhedgedSize) {
                LOG(0, "AS {C[i1]}. got filled. Side={i2} Price={d1} Qty={i3} UnhedgedQty={i4}", getContract(fill._contractId),
                    fill._side, ToDollars(fill._price), fill._qty, unhedgedSize)

                getStrategy(StrategyId::MarketMaker)->sendMessage<FillMessage>(
                        ViperMessageType::Fill,
                        [&](FillMessage& message) {
                            memcpy(&message, &fill, sizeof(FillMessage));
                            message._source = FillSource::ASUnableToHedge;
                            message._qty = unhedgedSize;
                            message._forceOverFill = true;
                        });

                if (logging::loggingLevel() >= logging::LOG_LEVEL::DEBUG) {
                    for (auto path : contract._paths[fill._side]) {
                        LOG(0, "AS {C[i1]}. Path {i2} {i3}@{d1}", contract.name(), path->_id, path->_priceSize._size,
                            ToDollars(path->_priceSize._price))
                        for (auto& pathContract : path->_leanContracts) {
                            auto bestMarket = pathContract.bestMarket();
                            LOG(0, "    - {C[i1]}. {i2} {i3}({i4})@{d1}", pathContract._contract->name(), pathContract._side,
                                bestMarket->getUnAdjustedSize(), bestMarket->getAdjustedSize(), ToDollars(bestMarket->getPrice()))
                        }
                    }
                }
            }
        } else {
            LOG(0, "AS {C[i1]}. got filled. But disabled. Sending fill to MarketMaker. Side={i2} Price={d1} Qty={i3}", getContract(fill._contractId),
                fill._side, ToDollars(fill._price), fill._qty)
            getStrategy(StrategyId::MarketMaker)->sendMessage<FillMessage>(
                    ViperMessageType::Fill,
                    [&](FillMessage& message) {
                        memcpy(&message, &fill, sizeof(FillMessage));
                        message._source = FillSource::AsDisable;
                        message._forceOverFill = true;
                    });
        }
    } else {
        LOG(0, "AS {C[i1]}. got filled. Not quoting a contract. Sending fill to MarketMaker. Side={i2} Price={d1} Qty={i3}", getContract(fill._contractId),
            fill._side, ToDollars(fill._price), fill._qty);
        getStrategy(StrategyId::MarketMaker)->sendMessage<FillMessage>(
                ViperMessageType::Fill,
                [&](FillMessage& message) {
                    memcpy(&message, &fill, sizeof(FillMessage));
                    message._source = FillSource::ASUnableToHedge;
                    message._forceOverFill = true;
                });
    }
}