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

        bool hedgeSent = false;
        auto sendFillToFront = [this, &fill, size, price, &contract]() {
            sendMessageToFront<AcceptedManualFillMessage>(ViperMessageType::AcceptedManualFill, [&](AcceptedManualFillMessage& msg) {
                msg._contractId = fill._contract;
                msg._isInternal = false;
                msg._size = size;
                msg._price = price;
                msg._stf = hf::NULL_PRICE;
            });
        };
        if (side == BID) {
            hedgeSent |= updateHedge<BID>(contract, price, size, sendFillToFront, false);
        } else {
            hedgeSent |= updateHedge<ASK>(contract, price, size, sendFillToFront, false);
        }

        if (!hedgeSent) {
            LOG(0, "AS {C[i1]}. got manual filled. Unable to hedge. Sending fill to MM. Side={i2} Price={d1} Qty={i3}", contract.name(),
                side, ToDollars(price), size)

            getStrategy(StrategyId::MarketMaker)->sendMessage<FillMessage>(
                    ViperMessageType::Fill,
                    [&fill, price, size, side](FillMessage& message) {
                        message._contractId = fill._contract;
                        message._price = price;
                        message._qty = size;
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

            auto sendFillToFront = [this, &fill, &contract]() {
                sendMessageToFront<FillMessage>(
                        ViperMessageType::Fill,
                        [&](FillMessage& message) {
                            memcpy(&message, &fill, sizeof(FillMessage));
                            message._rvStf = hf::NULL_PRICE;
                        });
            };

            if (fill._side == BID) {
                hedgeSent |= updateHedge<BID>(contract, fill._price, fill._qty, sendFillToFront, fill._type == FillType::Internal);
            } else {
                hedgeSent |= updateHedge<ASK>(contract, fill._price, fill._qty, sendFillToFront, fill._type == FillType::Internal);
            }

            if (!hedgeSent) {
                LOG(0, "AS {C[i1]}. got filled. Unable to hedge. Sending fill to MarketMaker. Side={i2} Price={d1} Qty={i3}", getContract(fill._contractId),
                    fill._side, ToDollars(fill._price), fill._qty)

                getStrategy(StrategyId::MarketMaker)->sendMessage<FillMessage>(
                        ViperMessageType::Fill,
                        [&](FillMessage& message) {
                            memcpy(&message, &fill, sizeof(FillMessage));
                            message._source = FillSource::ASUnableToHedge;
                            message._forceOverFill = true;
                        });
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