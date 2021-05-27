#pragma once

#include "ViperStrategyBase.h"
#include "OrderRequestManager.h"

namespace Python {
template<uint32_t>
inline hf::qty_t positionSize(hf::qty_t size);

template<>
inline hf::qty_t positionSize<BID>(hf::qty_t size) { return size; }

template<>
inline hf::qty_t positionSize<ASK>(hf::qty_t size) { return -size; }

class ArbieSpreader : public ViperStrategyBaseImpl<ArbieSpreader> {
    friend class ViperStrategyBaseImpl<ArbieSpreader>;
    friend class MarketDataConsumer<ArbieSpreader>;
    struct Contract;
    struct ShortTermFairOffset;

    struct PriceQtyPair {
        hf::price_t _price {hf::NULL_PRICE};
        hf::qty_t _size {0};

        PriceQtyPair() = default;

        PriceQtyPair(const PriceQtyPair& rhs) noexcept
            : _price(rhs._price), _size(rhs._size) {
        }

        PriceQtyPair& operator=(const PriceQtyPair& rhs) noexcept {
            memcpy(this, &rhs, sizeof(PriceQtyPair));
            return *this;
        }

        bool operator!=(const PriceQtyPair& rhs) const noexcept {
            return _price != rhs._price || _size != rhs._size;
        }

        bool operator==(const PriceQtyPair& rhs) const noexcept {
            return !operator!=(rhs);
        }

        bool valid() const noexcept {
            return _price != hf::NULL_PRICE;
        }

        void reset() noexcept {
            _price = hf::NULL_PRICE;
            _size = 0 ;
        }
    };

    struct PathContract {
        Contract* _contract;
        int32_t _multiplier;

        PathContract(Contract* contract, int32_t multiplier) : _contract(contract), _multiplier(multiplier) {}
    };

    template<uint32_t> friend Market* getBestMarket(ArbieSpreader::PathContract&);
    template<uint32_t> friend Market* getLeanMarkets(ArbieSpreader::PathContract&);

    struct Path {
        Contract* _contract;
        ArbieSpreaderPath* _config;
        Path* _otherSidePath = nullptr;
        const ShortTermFairOffset& _stfOffset;
        hf::price_t _commission;
        std::vector<PathContract> _leanContracts;
        PriceQtyPair _priceSize;
        uint32_t _id;
        uint32_t _side;
        std::vector<Market*> _bestHedgeMarket;
        bool _dirty {false};
        bool _leanOnRV {false};
        std::string _name;

        template<uint32_t Side>
        inline bool calculate(bool force);

        Path(uint32_t side, std::string name, uint32_t id, Contract* contract,
             const std::vector<PathContract>& pathContracts, ArbieSpreaderPath& pathConfig,
             const ShortTermFairOffset* stfOffset);

        void marketDirty(Contract* contract, uint32_t side);
        void marketDirty(Contract* contract);
    };

    template<uint32_t Side>
    struct PathSorter {
        static constexpr hf::price_t priceFactor = (Side == BID) ? 1 : -1;

        bool operator()(const ArbieSpreader::Path* a, const ArbieSpreader::Path* b) const {
            auto aValid = a->_priceSize._size >= 1;
            auto bValid = b->_priceSize._size >= 1;
            // 1. if one is valid and other is not, consider the valid better.
            // 2. if both same validity
            //    a. If side is BID, bigger price is best
            //    b. If side is ASK, lower price is best (hence priceFactor == -1)
            auto aPair = std::make_pair(aValid, a->_priceSize._price * priceFactor);
            auto bPair = std::make_pair(bValid, b->_priceSize._price * priceFactor);
            if (aPair == bPair) {
                return a->_priceSize._size > b->_priceSize._size;
            }
            return aPair > bPair;  // pair ordering is lexicographical
        }
    };

    struct ShortTermFairOffset {
        std::string _id;
        Contract* _contract = nullptr;
        hf::price_t _stfOffset = 0;

        struct ShortTermFair {
            hf::price_t _fair = hf::NULL_PRICE;
            int32_t _ratio = 0.0;

            ShortTermFair() = default;
            ShortTermFair(int32_t r) : _ratio(r) {}
        };

        std::map<uint32_t, ShortTermFair> _shortTermFairs;
        std::unordered_set<Path*> _paths;

        ShortTermFairOffset() = default;
        ShortTermFairOffset(Contract* contract, std::string&& id, std::map<uint32_t, ShortTermFair>&& stfs)
                : _id(std::move(id)), _contract(contract), _stfOffset(hf::NULL_PRICE), _shortTermFairs(std::move(stfs))
        {}

        void onSTFUpdate(uint32_t contractId, hf::price_t stf);
        void calculateShortTermFairOffset(std::unordered_set<Contract*>& updatedContracts);
    };

    struct Contract {
        ConfigContract _def;
        Market _markets[2][2];
        Market* _bestMarket[2] = {&_markets[BID][0], &_markets[ASK][0]};
        std::vector<Path*> _paths[2];
        std::vector<Path*> _affectedPaths;  // The path used this contract
        static const ShortTermFairOffset _zeroStfOffset;
        hf::price_t _stf = 0;
        hf::price_t _edge = 0;
        double _hedgeOffset = 0;
        hf::price_t _extCommission = 0.;
        hf::price_t _hedgeOffsetPrice = 0;
        hf::qty_t _quoteSize = 0;

        struct Quote {
            hf::price_t _leanPrice = hf::NULL_PRICE;
            hf::price_t _internalPrice = hf::NULL_PRICE;
            hf::price_t _externalPrice = hf::NULL_PRICE;
            hf::qty_t _internalSize = 0;
            hf::qty_t _externalSize = 0;
            int32_t _hedgePath = -1;

            Quote& operator=(const Quote& rhs) noexcept {
                memcpy(this, &rhs, sizeof(Quote));
                return *this;
            }

            bool operator==(const Quote& rhs) const noexcept {
                return !(operator!=(rhs));
            }

            bool operator!=(const Quote& rhs) const noexcept {
                return _internalPrice != rhs._internalPrice
                    || _internalSize != rhs._internalSize
                    || _externalPrice != rhs._externalPrice
                    || _externalSize != rhs._externalSize
                    || _hedgePath != rhs._hedgePath;
            }

            void reset() noexcept {
                _leanPrice = _internalPrice = _externalPrice = hf::NULL_PRICE;
                _internalSize = _externalSize = 0;
                _hedgePath = -1;
            }
        };
        Quote _quote[2];
        Quote _lastQuoteSent[2];
        uint32_t _hedgeId {0};
        bool _forceUpdate[2] {false, false};
        bool _usedInQuoting = false;
        bool _usedInPath = false;
        bool _enabled = false;
        bool _canTrade = true;
        bool _quoteExternally = false;

        std::vector<ShortTermFairOffset> _shortTermFairOffsets;

        Contract() {
        }

        Contract(const ConfigContract& def) : _def(def) {
            initMarkets();
        }

        void setMarketDirty(uint32_t side) {
            for (auto* path : _affectedPaths) {
                path->marketDirty(this, side);
            }
        }

        void markAffectedPathDirty() {
            for (auto* path : _affectedPaths) {
                path->marketDirty(this);
            }
        }

        template<uint32_t Side>
        bool updateMarket(MarketIds marketId, hf::price_t price, hf::qty_t qty, std::unordered_set<Market*>& affected_markets) {
            LOG(0, "AS {C[i1]}. MD update Side={i2} marketId={i3} {i4}@{d1}", name(), Side, marketId, qty, ToDollars(price))
            Market* updated;
            if (marketId == MarketIds::MARKET_ID_EXTERNAL) {
                _markets[Side][Market::Type::External].update(price, qty, affected_markets);
                updated = &_markets[Side][Market::Type::External];
            } else {
                _markets[Side][Market::Type::Internal].updateInternalMarket(price, qty);
                updated = &_markets[Side][Market::Type::Internal];
            }
            if (updateBestMarket<Side>(updated)) {
                setMarketDirty(Side);
                return true;
            }
            return false;
        }

        void updatePaths() {
            updatePaths<BID>();
            updatePaths<ASK>();
        }

        template<uint32_t Side>
        inline void updatePaths(bool force = false);

        template<uint32_t Side>
        bool updateBestMarket(Market* updated) {
            auto oldBest = _bestMarket[Side];
            if (_markets[Side][Market::Internal].getPrice() == hf::NULL_PRICE) {
                _bestMarket[Side] = &_markets[Side][Market::External];
            } else if (_markets[Side][Market::External].getPrice() == hf::NULL_PRICE) {
                _bestMarket[Side] = &_markets[Side][Market::Internal];
            } else if (_markets[Side][Market::Internal].getAdjustedSize() == 0) {
                _bestMarket[Side] = &_markets[Side][Market::External];
            } else if (_markets[Side][Market::External].getAdjustedSize() == 0) {
                _bestMarket[Side] = &_markets[Side][Market::Internal];
            } else if (IsPriceBetterOrEqual<Side>(_markets[Side][Market::Internal].getPrice(), _markets[Side][Market::External].getPrice())) {
                _bestMarket[Side] = &_markets[Side][Market::Internal];
            } else {
                _bestMarket[Side] =&_markets[Side][Market::External];
            }

            // Best market updated or best market changed
            return (_bestMarket[Side] == updated || oldBest == updated);
        }

        template<uint32_t Side>
        Market* getBestMarket() const noexcept {
            return _bestMarket[Side];
        }

        template<uint32_t Side>
        inline bool addAggressiveQuoteAdjustment(Path* path, const Quote& quote, bool quoteExternally, std::unordered_set<Market*>& affected_contract);

        template<uint32_t Side>
        inline bool isAggressiveQuote(const Market& otherSideMarket, hf::price_t quote_price) const noexcept;

        template<uint32_t Side>
        inline hf::qty_t getExternalQuoteSize(const Quote& quote) const noexcept;

#ifdef IS_DEV
        std::string name() const noexcept {
            return _def._name;
        }
#else
        int32_t name() const noexcept {
            return _def._internalId;
        }
#endif

        ALWAYS_INLINE void initMarkets() {
            for (auto side : {BID, ASK}) {
                _markets[side][Market::Internal].init(StrategyId::ArbieSpreader, side, MarketIds::__end__, _def._internalId);
                _markets[side][Market::External].init(StrategyId::ArbieSpreader, side, MarketIds::MARKET_ID_EXTERNAL, _def._internalId);
            }
        }
    };

    std::vector<Contract> _contracts;
    std::vector<Path> _allPaths;
    OrderRequestManager& _orderRequestMgr;
    std::unordered_set<Contract*> _updatedContracts;
    std::unordered_set<Contract*> _quotesUpdatedContracts;
    std::unordered_set<Contract*> _updateContractsInNextRound;
    std::vector<ReadyOrderRequests> _lastOrderRequests;
    std::unordered_map<uint32_t, Path*> _pathsParameters;
    std::unordered_set<Market*> _effected_markets;
    std::unordered_set<ShortTermFairOffset*> _updatedSTFOffsets;
    std::unordered_multimap<uint32_t, ShortTermFairOffset*> _mapShortTermFairOffset;

public:
    ArbieSpreader() : _orderRequestMgr(OrderRequestManager::get()) {
    }

    ResourceId id() const noexcept override { return ResourceId::ArbieSpreader; }
    StrategyId strategyId() const noexcept override { return StrategyId::ArbieSpreader; }

    void init(double frequency, uint64_t internalHeartbeatPeriod, std::function<bool(uint32_t)> pnlLimitsChecker) override;
    void registerOrderClient();

    bool isContractQuoteExternal(uint32_t contractId) const noexcept override;

    COLD void onManualFill(ManualFill &fill);
    COLD void onEndOfManualFills();
    bool onGlobalConfigsUpdate(GridUpdate& update) { return true; }

    void onMDStatus(hf::VENUE venue, uint64_t extSeqNum, const md::MDStatusEvent &event, uint32_t internalId);

    // GUI update
    void populateGUIInitialization(::google::protobuf::RepeatedPtrField< ::Python::proto::GridDefinition >* definitions) override;
    void sendStateToGUI(std::function<void(proto::GridUpdate&)>&& sendingFunc) const override;

private:
    void initializeContracts();
    void runCycle(uint64_t now);
    // System State handling
    void handleHardShutdown() {}
    void handleCheckQuotes() {}
    void handleDisabled() {
        _orderRequestMgr.sendCancelRequest(StrategyId::ArbieSpreader);
    }

    bool handleEnabled() {
        updateQuotes(true);
        return true;
    }

    void handleLogin() {}
    void handleLogout() {}

    void cancelGtcAndDisable() {
        handleDisabled();
    }

    void onQuoteExternalChanged(SystemState quoteExt);
    void onExchangeQuoteExternalChanged(const SharedSystemState::QuoteExtBitset& quoteExt);

    // MD handling
    void onMDUpdate(const md::MDUpdateEvent &event, uint32_t internalId);
    void onMDTrade(hf::VENUE venue, const md::MDTradeEvent &event);
    void handleUncommonMDEvent(const md::MDEvent &mde, const md::MDBatch& batch);
    void onEndMDBatch() {}
    void handleICEMDStatus(const md::MDStatusEvent& event, Contract& contract);
    void handle_CME_B3_MDStatus(const md::MDStatusEvent& event, Contract& contract);
    void handleEUREXMDStatus(const md::MDStatusEvent& event, Contract& contract);

    void onMarketDataUpdate(Contract& contract,
                            bool bidChanged,
                            hf::price_t bidPrice,
                            hf::qty_t bidSize,
                            bool askChanged,
                            hf::price_t askPrice,
                            hf::qty_t askSize);

    void processInternalMarketUpdate(const MarketUpdate& marketUpdate);
    void onMarketDataChanged();

    // Event handling
    void handleViperMessage(const ViperMessage& msg);
    void doDisable() {}

    void handleFill(const FillMessage& fill);

    void updateQuotes(bool force = false);
    bool updateQuotes(Contract& contract, bool force = false);

    template<uint32_t Side>
    inline bool updateHedge(Contract& contract, hf::qty_t qty, const std::function<void()>& sendFillToFront);

    void sendHedgeOrder(uint32_t internalId,
                        uint32_t hedgeContractId,
                        uint32_t side,
                        hf::price_t internalPrice,
                        hf::price_t externalPrice,
                        hf::qty_t size,
                        bool external,
                        uint32_t hedgeId,
                        uint32_t marketId,
                        uint32_t pathId);

    void processGridChange(GridUpdate& update);

    void handleHedgeMarketDataAdjustmentAck(const HedgeMarketDataAdjustmentAck& hedgeAck);

    void handleFakDone(hf::VENUE venue, const md::MDFakDonenEvent& event);
    void handleInternalHedgeDone(const InternalHedgeDone& message);
    void handleASInternalMatchingDone(uint32_t contractId);
    void handleASQuoteAdjustmentACK(uint32_t contractId, uint32_t side, hf::qty_t matchedSize);
    void markContractDirty(Contract& contract, Market* updatedMarket);
    void addUpdatedContract(Contract& contract);
    void handleSTFUpdate(const STFUpdate& msg);

    template<uint32_t Side>
    inline void tryAddAggressiveQuoteAdjustment(Contract& contract, Path* path, const Contract::Quote& quote, bool quoteExternally);
};

template<uint32_t Side>
inline bool ArbieSpreader::updateHedge(Contract& contract, hf::qty_t qty, const std::function<void()>& sendFillToFront) {
    auto otherSide = GetOtherSide<Side>();
    if (contract._markets[otherSide][Market::External].getASQuoteAdjustment()) {
        // the aggressor quote filled?
        if (contract._markets[otherSide][Market::External].undoAsQuoteAdjustment(std::min<hf::qty_t>(qty, contract._markets[otherSide][Market::External].getASQuoteAdjustment()), _effected_markets)) {
            markContractDirty(contract, &contract._markets[otherSide][Market::External]);
        }
        for (auto market : _effected_markets) {
            markContractDirty(_contracts[market->getContractId()], market);
        }
        _effected_markets.clear();
    }
    contract.template updatePaths<Side>();
    auto size = positionSize<Side>(qty);
    auto pBest = contract._paths[Side][0];
    if (BOOST_LIKELY(pBest->_priceSize.valid())) {
        sendFillToFront();
        auto hedgeId = ++contract._hedgeId;
        LOG(0, "AS {C[i1]}. Hedge on Path {i2} {i3}@{d1}.", contract.name(), pBest->_id, pBest->_priceSize._size, ToDollars(pBest->_priceSize._price))
        for (uint32_t i = 0; i < pBest->_leanContracts.size(); ++i) {
            auto& hedgeContract = pBest->_leanContracts[i];
            auto* hedgeMarket = pBest->_bestHedgeMarket[i];
            if (hedgeContract._multiplier > 0) {
                auto internalHedgePrice = applyPriceOffset<1 - Side>(hedgeMarket->getPrice(), hedgeContract._contract->_hedgeOffsetPrice);
                auto externalHedgePrice = hedgeMarket->getPrice();
                sendHedgeOrder(contract._def._internalId, hedgeContract._contract->_def._internalId, otherSide,
                               internalHedgePrice, externalHedgePrice, qty, hedgeMarket->getId() == MarketIds::MARKET_ID_EXTERNAL,
                               hedgeId, hedgeMarket->getId(), pBest->_id);
            } else {
                auto internalHedgePrice = applyPriceOffset<Side>(hedgeMarket->getPrice(), hedgeContract._contract->_hedgeOffsetPrice);
                auto externalHedgePrice = hedgeMarket->getPrice();
                sendHedgeOrder(contract._def._internalId, hedgeContract._contract->_def._internalId, Side,
                               internalHedgePrice, externalHedgePrice, qty, hedgeMarket->getId() == MarketIds::MARKET_ID_EXTERNAL,
                               hedgeId, hedgeMarket->getId(), pBest->_id);
            }
        }

        if (!pBest->_stfOffset._id.empty()) {
            // This is a switch path, forward the fill to RV
            auto now = hf::util::TimeUtils::clock_gettime();
            sendMessageToFront<FillMessage>(ViperMessageType::Fill,
                                            [&contract, otherSide, qty, now](FillMessage& message) {
                                                message._contractId = contract._def._internalId;
                                                message._strategy = StrategyId::ArbieSpreader;
                                                message._price = contract._stf;
                                                message._qty = qty;
                                                message._side = otherSide;
                                                message._type = FillType::Internal;
                                                message._source = FillSource::AsToRv;
                                                message._checkLimit = false;
                                                message._internalAllocation = 0;
                                                message._externalAllocation = 0;
                                                message._timestamp = now;
                                                message._forceOverFill = false;
                                                message._rvStf = hf::NULL_PRICE;
                                            });

            auto dataSize = (pBest->_leanContracts.size() + 1) * sizeof(FillMessage);
            auto* rv = getStrategy(StrategyId::RelativeValue);
            rv->template sendMessage<ASFillsToRV>(
                    ViperMessageType::TransferPositionToRV,
                    [this, &contract, qty, now, pBest, otherSide](ASFillsToRV& message) {
                        message._numFills = pBest->_leanContracts.size() + 1;
                        FillMessage* fill = message._fills;
                        fill->_contractId = contract._def._internalId;
                        fill->_strategy = StrategyId::RelativeValue;
                        fill->_price = contract._stf;
                        fill->_qty = qty;
                        fill->_side = Side;
                        fill->_type = FillType::Internal;
                        fill->_source = FillSource::AsToRv;
                        fill->_checkLimit = false;
                        fill->_internalAllocation = 0;
                        fill->_externalAllocation = 0;
                        fill->_timestamp = now;
                        fill->_forceOverFill = false;
                        fill->_rvStf = hf::NULL_PRICE;

                        for (const auto& hedgeContract : pBest->_leanContracts) {
                            sendMessageToFront<FillMessage>(ViperMessageType::Fill,
                                                            [&hedgeContract, otherSide, qty, now](FillMessage& message) {
                                                                message._contractId = hedgeContract._contract->_def._internalId;
                                                                message._strategy = StrategyId::ArbieSpreader;
                                                                message._price = hedgeContract._contract->_stf;
                                                                message._qty = qty;
                                                                message._side = hedgeContract._multiplier > 0 ? Side
                                                                                                              : otherSide;
                                                                message._type = FillType::Internal;
                                                                message._source = FillSource::AsToRv;
                                                                message._checkLimit = false;
                                                                message._internalAllocation = 0;
                                                                message._externalAllocation = 0;
                                                                message._timestamp = now;
                                                                message._forceOverFill = false;
                                                                message._rvStf = hf::NULL_PRICE;
                                                            });

                            ++fill;
                            fill->_contractId = hedgeContract._contract->_def._internalId;
                            fill->_strategy = StrategyId::RelativeValue;
                            fill->_price = hedgeContract._contract->_stf;
                            fill->_qty = qty;
                            fill->_side = hedgeContract._multiplier > 0 ? otherSide : Side;
                            fill->_type = FillType::Internal;
                            fill->_source = FillSource::AsToRv;
                            fill->_checkLimit = false;
                            fill->_internalAllocation = 0;
                            fill->_externalAllocation = 0;
                            fill->_timestamp = now;
                            fill->_forceOverFill = false;
                            fill->_rvStf = hf::NULL_PRICE;
                        }
                    }, dataSize);
        }

        // Adjust FAK in separate loop so that the hedges are send out ASAP.
        for (uint32_t i = 0; i < pBest->_leanContracts.size(); ++i) {
            auto& hedgeContract = pBest->_leanContracts[i];
            auto* hedgeMarket = pBest->_bestHedgeMarket[i];
            hedgeMarket->addFAKAdjustment(hedgeId, qty);
            assert(hedgeMarket->getSide() == (1 - Side) || (hedgeContract._multiplier > 0));
            markContractDirty(*hedgeContract._contract, hedgeMarket);
        }
        return true;
    }
    LOG(0, "AS {C[i1]}. Unable to hedge. No valid path.", contract.name())
    sendErrorMessage(boost::str(boost::format("AS %1%. Unable to hedge. No valid path.") % contract._def._name), ErrorType::Critical);
    return false;
}

template<uint32_t Side>
inline Market* getBestMarket(ArbieSpreader::PathContract& pathContract);

template<>
inline Market* getBestMarket<BID>(ArbieSpreader::PathContract& pathContract) {
    return pathContract._multiplier > 0 ? pathContract._contract->getBestMarket<BID>() : pathContract._contract->getBestMarket<ASK>();
}

template<>
inline Market* getBestMarket<ASK>(ArbieSpreader::PathContract& pathContract) {
    return pathContract._multiplier > 0 ? pathContract._contract->getBestMarket<ASK>() : pathContract._contract->getBestMarket<BID>();
}

template<uint32_t Side>
inline Market* getLeanMarkets(ArbieSpreader::PathContract& pathContract);

template<>
inline Market* getLeanMarkets<BID>(ArbieSpreader::PathContract& pathContract) {
    return pathContract._multiplier > 0 ? pathContract._contract->_markets[BID] : pathContract._contract->_markets[ASK];
}

template<>
inline Market* getLeanMarkets<ASK>(ArbieSpreader::PathContract& pathContract) {
    return pathContract._multiplier > 0 ? pathContract._contract->_markets[ASK] : pathContract._contract->_markets[BID];
}

template<uint32_t Side>
inline bool positionMaxedOut(hf::qty_t position, hf::qty_t maxPos) noexcept;

template<>
inline bool positionMaxedOut<BID>(hf::qty_t position, hf::qty_t maxPos) noexcept {
    return position >= maxPos;
}

template<>
inline bool positionMaxedOut<ASK>(hf::qty_t position, hf::qty_t maxPos) noexcept {
    return (-position) >= maxPos;
}

template<uint32_t Side>
inline bool ArbieSpreader::Path::calculate(bool force) {
    if (_dirty | force) {
        _dirty = false;

        if (!_config->_enabled) {
            if (_priceSize.valid()) {
                // just changed from Enabled to Disabled
                _priceSize.reset();
                return true;
            }
            // already disabled
            return false;
        }

        if (BOOST_UNLIKELY(!_stfOffset._id.empty() && _stfOffset._stfOffset == hf::NULL_PRICE)) {
            // STF hasn't received from RV yet
            if (BOOST_UNLIKELY(_priceSize.valid())) {
                _priceSize.reset();
                return true;
            }
            return false;
        }

        auto oldPriceSize = _priceSize;
        _priceSize._price = 0;
        _priceSize._size = std::numeric_limits<hf::qty_t>::max();
        _leanOnRV = false;
        for (uint32_t i = 0; i < _leanContracts.size(); ++i) {
            auto& pathContract = _leanContracts[i];
            if (BOOST_UNLIKELY(!pathContract._contract->_canTrade)) {
                _priceSize.reset();
                break;
            }

            _bestHedgeMarket[i] = getBestMarket<Side>(pathContract);
            _leanOnRV |= _bestHedgeMarket[i]->getId() == MarketIds::Market_ID_RV_QUOTE;
            auto marketPrice = _bestHedgeMarket[i]->getPrice();
            // apply commission to external market
            if (_bestHedgeMarket[i]->getId() == MarketIds::MARKET_ID_EXTERNAL && marketPrice != hf::NULL_PRICE) {
                // apply commission to external market
                marketPrice = applyPriceOffset<Side>(marketPrice, -pathContract._contract->_extCommission);
            }
//            DLOG(0, "AS {C[i1]}. Update Path {i2} Side={i3} lean={C[i4]} marketId={i5} side={i6} {i7}({i8})@{d1}({d2})", _contract->name(),
//                 _id, _side, pathContract._contract->name(), _bestHedgeMarket[i]->getId(),
//                 _bestHedgeMarket[i]->getSide(), _bestHedgeMarket[i]->getUnAdjustedSize(),
//                 _bestHedgeMarket[i]->getAdjustedSize(), ToDollars(_bestHedgeMarket[i]->getPrice()),
//                 ToDollars(marketPrice))

            if (marketPrice == hf::NULL_PRICE) {
                _priceSize.reset();
                break;
            }

            auto size = _bestHedgeMarket[i]->getAdjustedSize();
            if (size == 0) {
                if (_contract->_markets[1 - _side][Market::External].getASQuoteAdjustment() == 0) {
                    _priceSize.reset();
                    break;
                }

                // if 0 size was due to the aggressive AS quoting, exclude the FAK Pre Adjustment
                size = _bestHedgeMarket[i]->getAdjustedSizeExcludeFAKPreAdj();
                if (size == 0) {
                    // still get 0 size, other contract must have adjusted the hedge contract too, switch to
                    // other path.
                    _priceSize.reset();
                    break;
                }
            }

            _priceSize._price += pathContract._multiplier * marketPrice;
            _priceSize._size = std::min<hf::qty_t>(_priceSize._size, size);
        }

        if (_priceSize.valid()) {
            _priceSize._price += _stfOffset._stfOffset;
            _priceSize._price = applyPriceOffset<Side>(_priceSize._price, -_commission);
        }

        if ((oldPriceSize._price != _priceSize._price) ||
            ((oldPriceSize._size > _priceSize._size) & (_priceSize._size < _contract->_quoteSize)) ||       // need to quote less
            ((oldPriceSize._size < _priceSize._size) & (oldPriceSize._size < _contract->_quoteSize))) {     // can quote more
            LOG(0, "AS {C[i1]}. Path {i2} Side={i3} {i4}@{d1}", _contract->name(), _id, _side, _priceSize._size, ToDollars(_priceSize._price))
            return true;
        }
    }
    return false;
}

template<uint32_t Side>
inline void ArbieSpreader::Contract::updatePaths(bool force) {
    bool sort = false;
    for (auto path : _paths[Side]) {
        sort |= path->calculate<Side>(force);
    }
    if (sort) {
        auto oldBest = _paths[Side][0];
        std::sort(_paths[Side].begin(), _paths[Side].end(), PathSorter<Side>());

//#ifdef IS_DEV
//        for (auto* path : _paths[Side]) {
//            LOG(0, " - AS {C[i1]}. Path {i2} Side={i3} {i4}@{d1}", path->_contract->name(), path->_id, Side,
//                path->_priceSize._size, ToDollars(path->_priceSize._price))
//            for (auto& contract : path->_leanContracts) {
//                LOG(0, " - LeanContract={C[i1]}", contract._contract->name())
//                auto markets = getLeanMarkets<Side>(contract);
//                LOG(0, " - Market {C[i1]} Id={i2} Side={i3} Price={d1} Size={i4} AdjustedSize={i5}", markets[Market::Internal].getContractId(),
//                    markets[Market::Internal].getId(), markets[Market::Internal].getSide(),
//                    ToDollars(markets[Market::Internal].getPrice()), markets[Market::Internal].getUnAdjustedSize(),
//                    markets[Market::Internal].getAdjustedSize())
//                LOG(0, " - Market {C[i1]} Id={i2} Side={i3} Price={d1} Size={i4} AdjustedSize={i5}", markets[Market::Internal].getContractId(),
//                    markets[Market::External].getId(), markets[Market::Internal].getSide(),
//                    ToDollars(markets[Market::External].getPrice()), markets[Market::External].getUnAdjustedSize(),
//                    markets[Market::External].getAdjustedSize())
//            }
//        }
//#endif

        auto bestPath = _paths[Side][0];
        if (oldBest != bestPath) {
            LOG(0, "AS {{C[i1]}. Lean on path {i2} {i3}@{d1}", bestPath->_contract->name(),
                bestPath->_id, bestPath->_priceSize._size, ToDollars(bestPath->_priceSize._price))
        }
    }
}

template<uint32_t Side>
inline void ArbieSpreader::tryAddAggressiveQuoteAdjustment(Contract& contract, Path* path, const Contract::Quote& quote, bool quoteExternally) {
    if (quote._externalSize) {
        if (contract.addAggressiveQuoteAdjustment<Side>(path, quote, quoteExternally, _effected_markets)) {
            addUpdatedContract(contract);
            for (auto market : _effected_markets) {
                markContractDirty(_contracts[market->getContractId()], market);
            }
            _effected_markets.clear();
        }
    }
}

template<uint32_t Side>
inline bool ArbieSpreader::Contract::isAggressiveQuote(const Market& otherSideMarket, hf::price_t quote_price) const noexcept {
    assert(quote_price != hf::NULL_PRICE);
    return otherSideMarket.getPrice() != hf::NULL_PRICE && IsPriceBetterOrEqual<Side>(quote_price, otherSideMarket.getPrice());
}

template<uint32_t Side>
inline hf::qty_t ArbieSpreader::Contract::getExternalQuoteSize(const Quote& quote) const noexcept {
    auto& otherSideMarket = _markets[1 - Side][Market::Type::External];
    if (isAggressiveQuote<Side>(otherSideMarket, quote._externalPrice)) {
        return std::min<hf::qty_t>(otherSideMarket.getAdjustedSize(), quote._internalSize);
    }
    return quote._internalSize;
}

template<uint32_t Side>
inline bool ArbieSpreader::Contract::addAggressiveQuoteAdjustment(Path* path, const Quote& quote, bool quoteExternally, std::unordered_set<Market*>& effected_contract) {
    constexpr uint32_t otherSide = GetOtherSide<Side>();
    auto addAggQuoteAdjustment = [this, path, &effected_contract](Market& otherSideMarket, hf::price_t quote_price, hf::qty_t quote_size) -> bool {
        if (isAggressiveQuote<Side>(otherSideMarket, quote_price)) {
            otherSideMarket.addAsQuoteAdjustment(quote_size, effected_contract);
            if (updateBestMarket<otherSide>(&otherSideMarket)) {
                setMarketDirty(otherSide);
            }
            for (auto hedgeMarket : path->_bestHedgeMarket) {
                otherSideMarket.addFAKPreAdjustment(hedgeMarket);
                effected_contract.emplace(hedgeMarket);
            }
            return true;
        }
        return false;
    };

    bool adjusted = addAggQuoteAdjustment(_markets[otherSide][Market::Type::Internal], quote._internalPrice, quote._internalSize);
    if (quoteExternally) {
        adjusted |= addAggQuoteAdjustment(_markets[otherSide][Market::Type::External], quote._externalPrice, quote._externalSize);
    }
    return adjusted;
}

}