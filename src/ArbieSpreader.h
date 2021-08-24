#pragma once

#include "ViperStrategyBase.h"
#include "OrderRequestManager.h"
#include "PerfMonitor.h"

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
        PriceQtyPair _priceSizeCache[2];

        PathContract(Contract* contract, int32_t multiplier) : _contract(contract), _multiplier(multiplier) {}

        bool shouldRecalculate(uint32_t side, hf::qty_t quoteSize) const noexcept {
            auto bestMarket = _contract->_bestMarket[side];
            auto bestSize = bestMarket->getAdjustedSize();
            return (bestMarket->getPrice() != _priceSizeCache[side]._price) |
                ((_priceSizeCache[side]._size > bestSize) & (bestSize < quoteSize)) |                   // need to quote less
                ((_priceSizeCache[side]._size < bestSize) & (_priceSizeCache[side]._size < quoteSize)); // can quote more
        }
    };

    template<uint32_t> friend Market* getBestMarket(ArbieSpreader::PathContract&);
    template<uint32_t> friend Market* getLeanMarkets(ArbieSpreader::PathContract&);

    struct Path {
        Contract* _contract;
        ArbieSpreaderPath* _config;
        Path* _otherSidePath = nullptr;
        const ShortTermFairOffset& _stfOffset;
        hf::price_t _rvSTFMove = 0;
        hf::price_t _commission;
        std::vector<PathContract> _leanContracts;
        PriceQtyPair _priceSize;
        PriceQtyPair _oldPriceSize;
        uint32_t _id;
        uint32_t _side;
        std::vector<Market*> _bestHedgeMarket;
        bool _dirty {false};
        bool _leanOnRV {false};
        std::string _name;

        template<uint32_t Side>
        inline bool calculate(bool force, uint32_t& pathCount);

        Path(uint32_t side, std::string name, uint32_t id, Contract* contract,
             const std::vector<PathContract>& pathContracts, ArbieSpreaderPath& pathConfig,
             const ShortTermFairOffset* stfOffset);

        bool marketDirty(Contract* contract, uint32_t side);
        void marketDirty(Contract* contract);

        bool canHedge(hf::qty_t size) const noexcept;
        void log() const noexcept;
    };

    template<uint32_t Side>
    struct PathSorter {
        static constexpr hf::price_t priceFactor = (Side == BID) ? 1 : -1;

        bool operator()(const ArbieSpreader::Path* a, const ArbieSpreader::Path* b) const {
            auto aValid = a->_priceSize._price != hf::NULL_PRICE;
            auto bValid = b->_priceSize._price != hf::NULL_PRICE;
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
        hf::price_t _rvSTFMove = 0;

        struct ShortTermFair {
            hf::price_t _fair = hf::NULL_PRICE;
            uint32_t _contractId = INVALID_INTERNAL_ID;
            int32_t _ratio = 0.0;

            ShortTermFair() = default;
            ShortTermFair(uint32_t id, int32_t r) : _contractId(id), _ratio(r) {}
        };

        std::vector<ShortTermFair> _shortTermFairs;
        std::unordered_set<Path*> _paths;

        ShortTermFairOffset() = default;
        ShortTermFairOffset(Contract* contract, std::string&& id, std::map<uint32_t, ShortTermFair>&& stfs)
                : _id(std::move(id)), _contract(contract), _stfOffset(hf::NULL_PRICE)
        {
            for (auto& kvp : stfs) {
                _shortTermFairs.emplace_back(kvp.second);
            }
        }

        void onSTFUpdate(uint32_t contractId, hf::price_t stf);
        bool calculateShortTermFairOffset();

        std::vector<ShortTermFair>::iterator findSTF(uint32_t contractId) noexcept {
            return std::find_if(_shortTermFairs.begin(), _shortTermFairs.end(),
                                [contractId] (const ShortTermFair& stf) { return stf._contractId == contractId; });
        }
        bool hasSTF(uint32_t contractId) const noexcept {
            return std::find_if(_shortTermFairs.begin(), _shortTermFairs.end(),
                                [contractId](const ShortTermFair& stf) { return stf._contractId == contractId; }) !=
                   _shortTermFairs.end();
        }

        void removeSTF(uint32_t contractId) {
            _shortTermFairs.erase(std::remove_if(_shortTermFairs.begin(), _shortTermFairs.end(),
                                                 [contractId](const ShortTermFair& stf) {
                                                     return stf._contractId == contractId;
                                                 }),
                                  _shortTermFairs.end());
        }
    };

    struct Contract {
        ConfigContract _def;
        Market _markets[2][2];
        Market* _bestMarket[2] = {&_markets[BID][0], &_markets[ASK][0]};
        std::vector<Path*> _paths[2];
        std::vector<Path*> _affectedPaths;  // The path used this contract
        static const ShortTermFairOffset _zeroStfOffset;
        hf::price_t _stf = hf::NULL_PRICE;
        hf::price_t _edge = 0;
        double _hedgeOffset = 0.;
        hf::price_t _maxTicksThroughMarket = 0;
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
                    || _externalSize != rhs._externalSize;
            }

            void reset() noexcept {
                _leanPrice = _internalPrice = _externalPrice = hf::NULL_PRICE;
                _internalSize = _externalSize = 0;
                _hedgePath = -1;
            }
        };
        Quote _quote[2];
        Quote _lastQuoteSent[2];
        hf::batch_id_t _lastMDbid {0};
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
                if (path->marketDirty(this, side)) {
                    ArbieSpreader::get()->pushUpdatedContract(path->_contract);
                }
            }
        }

        void markAffectedPathDirty() {
            for (auto* path : _affectedPaths) {
                path->marketDirty(this);
            }
        }

        template<uint32_t Side>
        bool updateMarket(MarketIds marketId, hf::price_t price, hf::qty_t qty, std::unordered_set<Market*>& affected_markets) {
            LOG(0, "AS {C[i1]}. MD update Side={i2} marketId={i3} {i4}@{d1}", name(), Side, toUInt(marketId), qty, ToDollars(price))
            Market* updated;
            bool changed = false;
            if (marketId == MarketIds::MARKET_ID_EXTERNAL) {
                updated = &_markets[Side][Market::Type::External];
                changed = updated->update(price, qty, affected_markets);
            } else {
                updated = &_markets[Side][Market::Type::Internal];
                updated->updateInternalMarket(price, qty);
                changed = true;
            }

            if (changed) {  // can quote more
                if (updateBestMarket<Side>(updated)) {
                    setMarketDirty(Side);
                    return true;
                }
            }
            return false;
        }

        void updatePaths(bool force = false) {
            uint32_t pathCount = 0;
            updatePaths<BID>(pathCount, force);
            updatePaths<ASK>(pathCount, force);
            ADD_PERF_VALUE(PerfId::AS_PathUpdateCountPerContractQuotes, pathCount);
        }

        template<uint32_t Side>
        inline void updatePaths(uint32_t& pathCount, bool force = false);

        template<uint32_t Side>
        bool updateBestMarket(Market* updated) {
            auto oldBest = _bestMarket[Side];
            if (_markets[Side][Market::Internal].getAdjustedSize() == 0) {
                _bestMarket[Side] = &_markets[Side][Market::External];
            } else if (_markets[Side][Market::External].getAdjustedSize() == 0) {
                _bestMarket[Side] = &_markets[Side][Market::Internal];
            } else if (IsPriceBetterOrEqual<Side>(_markets[Side][Market::Internal].getPrice(), _markets[Side][Market::External].getPrice())) {
                _bestMarket[Side] = &_markets[Side][Market::Internal];
            } else {
                _bestMarket[Side] =&_markets[Side][Market::External];
            }

            // Best market updated or best market changed
            auto changed =  (_bestMarket[Side] == updated || oldBest == updated);
            if (changed) {
                LOG(0, "AS {C[i1]}. {i2} BestMarket MarketId={i3} {i4}({i5})@{d1}", name(), Side, _bestMarket[Side]->getId(),
                    _bestMarket[Side]->getUnAdjustedSize(), _bestMarket[Side]->getAdjustedSize(), ToDollars(_bestMarket[Side]->getPrice()))
            }
            return changed;
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

    static ArbieSpreader* _self;
    std::vector<Contract> _contracts;
    std::vector<Path> _allPaths;
    OrderRequestManager& _orderRequestMgr;
    RingBufferQueue<Contract*> _updatedContracts;
    std::bitset<1024> _contractUpdatedFlag;
    uint64_t _updatedContractCursor {0};
    double _edgeMultiplier = 0.;
    uint32_t _hedgeId {0};
    std::vector<ReadyOrderRequests> _lastOrderRequests;
    std::unordered_map<uint32_t, Path*> _pathsParameters;
    std::unordered_set<Market*> _effected_markets;
    std::unordered_set<ShortTermFairOffset*> _updatedSTFOffsets;
    std::unordered_multimap<uint32_t, ShortTermFairOffset*> _mapShortTermFairOffset;
    Matrix _riskMatrix; // x,y - vectors - square
    std::vector<double> _vectorsRange;
    std::vector<double> _vectorsMaxPosition;

public:
    ArbieSpreader()
        : _orderRequestMgr(OrderRequestManager::get())
        , _updatedContracts(8192) {
    }

    ~ArbieSpreader() {
    }

    static ArbieSpreader* get() noexcept {
        return _self;
    }

    ResourceId id() const noexcept override { return ResourceId::ArbieSpreader; }
    StrategyId strategyId() const noexcept override { return StrategyId::ArbieSpreader; }

    void init(double frequency, uint64_t internalHeartbeatPeriod, std::function<bool(uint32_t)> pnlLimitsChecker) override;
    void registerOrderClient();

    bool isContractQuoteExternal(uint32_t contractId) const noexcept override;

    COLD void onManualFill(ManualFill &fill);
    COLD void onEndOfManualFills();
    bool onGlobalConfigsUpdate(GridUpdate& update);

    void onMDStatus(hf::VENUE venue, uint64_t extSeqNum, const md::MDStatusEvent &event, uint32_t internalId);

    // GUI update
    void populateGUIInitialization(::google::protobuf::RepeatedPtrField< ::Python::proto::GridDefinition >* definitions) override;
    void sendStateToGUI(std::function<void(proto::GridUpdate&)>&& sendingFunc) const override;

private:
    void calculateRVSTFMove(Contract& contract, ShortTermFairOffset& stfOffset, bool force = false);
    void initializeContracts();
    void runCycle(uint64_t now);
    // System State handling
    void handleHardShutdown() {}
    void handleCheckQuotes();
    void handleDisabled() {
        for (auto& contract : _contracts) {
            if (contract._usedInQuoting) {
                contract._quote[BID].reset();
                contract._quote[ASK].reset();
            }
        }
        _orderRequestMgr.sendCancelRequest(StrategyId::ArbieSpreader);
    }

    static bool isEdgeMultiplierWithinLimits(double value) noexcept {
        return (Compare(value, 1.0) >= 0) & (Compare(value, 10.0) <= 0);
    }

    bool canEnable() const noexcept;

    bool handleEnabled() {
        if (canEnable()) {
            updateQuotes(true);
            return isEnabled();
        }
        return false;
    }

    void handleLogin() {}
    void handleLogout() {}

    void cancelGtcAndDisable() {
        handleDisabled();
    }

    void onQuoteExternalChanged(SystemState quoteExt);
    void onExchangeQuoteExternalChanged(const SharedSystemState::QuoteExtBitset& quoteExt);

    // MD handling
    void onMDUpdate(const md::MDUpdateEvent &event, uint32_t internalId, hf::batch_id_t bid);
    void onMDTrade(const md::MDBatch& batch, const md::MDTradeEvent &event, uint32_t internalId);
    void handleUncommonMDEvent(const md::MDEvent &mde, const md::MDBatch& batch, uint32_t internalId);
    void onEndMDBatch(hf::batch_id_t bid);
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
    inline bool updateQuote(Contract& contract, bool force, bool canQuoteExternal, ReadyOrderRequests& lastRequest);

    template<uint32_t Side>
    inline bool checkMaxTicksThroughMarket(Contract& contract, Contract::Quote& quote);

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

    void handleFakDone(hf::VENUE venue, const md::MDFakDonenEvent& event, uint32_t internalId);
    void handleInternalHedgeDone(const InternalHedgeDone& message);
    void handleASInternalMatchingDone(uint32_t contractId);
    void markContractDirty(Contract& contract, Market* updatedMarket);
    void handleSTFUpdate(const STFUpdate& msg);

    template<uint32_t Side>
    inline void tryAddAggressiveQuoteAdjustment(Contract& contract, Path* path, const Contract::Quote& quote, bool quoteExternally);

    void pushUpdatedContract(Contract* contract);
    void onEconomicNumberEnded(EconomicNumber::Color color);
    void onShiftUpdate(uint32_t shift);
    void handleVectorParameterUpdate(const VectorParameterUpdate& msg);
};

template<uint32_t Side>
inline bool ArbieSpreader::updateHedge(Contract& contract, hf::qty_t qty, const std::function<void()>& sendFillToFront) {
    SCOPED_PERF_POINT(PerfId::AS_UpdateHedge);
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

    auto size = positionSize<Side>(qty);
    auto pBest = contract._paths[Side][0];
    if (!pBest->canHedge(size)) {
        uint32_t pathCount = 0;
        contract.template updatePaths<Side>(pathCount);
        ADD_PERF_VALUE(PerfId::AS_PathUpdateCountPerContractHedge, pathCount);
        pBest = contract._paths[Side][0];
    }

    if (BOOST_LIKELY(pBest->_priceSize.valid())) {
        sendFillToFront();
        auto hedgeId = ++_hedgeId;
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
                                                message._timestamp = now;
                                                message._forceOverFill = false;
                                                message._rvStf = hf::NULL_PRICE;
                                            });

            auto dataSize = (pBest->_leanContracts.size() + 1) * sizeof(FillMessage);
            auto* rv = getStrategy(StrategyId::RelativeValue);
            rv->template sendMessage<ASFillsToRV>(
                    ViperMessageType::TransferPositionToRV,
                    [this, &contract, qty, now, pBest, otherSide](ASFillsToRV& message) {
                        auto quoteAdjustedFillPrice = contract._stf + pBest->_stfOffset._rvSTFMove * qty;
                        message._numFills = pBest->_leanContracts.size() + 1;
                        FillMessage* fill = message._fills;
                        fill->_contractId = contract._def._internalId;
                        fill->_strategy = StrategyId::RelativeValue;
                        fill->_price = quoteAdjustedFillPrice;
                        fill->_qty = qty;
                        fill->_side = Side;
                        fill->_type = FillType::Internal;
                        fill->_source = FillSource::AsToRv;
                        fill->_checkLimit = false;
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
inline bool ArbieSpreader::Path::calculate(bool force, uint32_t& pathCount) {
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
            // STF hasn't received from RV yet or reset when RV disabled
            if (BOOST_UNLIKELY(_priceSize.valid())) {
                _priceSize.reset();
                return true;
            }
            return false;
        }

        SCOPED_PERF_POINT(PerfId::AS_Path_Calculation);

#ifdef ENABLE_PERF
        static uint64_t total = 0;
        ++total;
        ++pathCount;
#endif

        _oldPriceSize = _priceSize;
        _priceSize._price = 0;
        _priceSize._size = std::numeric_limits<hf::qty_t>::max();
        _leanOnRV = false;
        for (uint32_t i = 0; i < _leanContracts.size(); ++i) {
            auto& pathContract = _leanContracts[i];
            if (BOOST_UNLIKELY(!pathContract._contract->_canTrade)) {
                _priceSize.reset();
                break;
            }

            auto bestHedgeMarket = getBestMarket<Side>(pathContract);
            _bestHedgeMarket[i] = bestHedgeMarket;
            _leanOnRV |= bestHedgeMarket->getId() == MarketIds::Market_ID_RV_QUOTE;
            auto marketPrice = bestHedgeMarket->getPrice();
            // apply commission to external market
            if (bestHedgeMarket->getId() == MarketIds::MARKET_ID_EXTERNAL && marketPrice != hf::NULL_PRICE) {
                // apply commission to external market
                marketPrice = applyPriceOffset<Side>(marketPrice, -pathContract._contract->_extCommission);
            }

            if (marketPrice == hf::NULL_PRICE) {
                _priceSize.reset();
                break;
            }

            auto size = bestHedgeMarket->getAdjustedSize();
            if (size == 0) {
                if (_contract->_markets[1 - _side][Market::External].getASQuoteAdjustment() == 0) {
                    _priceSize.reset();
                    break;
                }

                // if 0 size was due to the aggressive AS quoting, exclude the FAK Pre Adjustment
                size = bestHedgeMarket->getAdjustedSizeExcludeFAKPreAdj();
                if (size == 0) {
                    // still get 0 size, other contract must have adjusted the hedge contract too, switch to
                    // other path.
                    _priceSize.reset();
                    break;
                }
            }

#ifdef IS_DEV
            DLOG(0, "AS {C[i1]}. Update Path {i2} Side={i3} lean={C[i4]} marketId={i5} side={i6} {i7}({i8})@{d1}({d2})", _contract->name(),
                 _id, _side, pathContract._contract->name(), bestHedgeMarket->getId(), bestHedgeMarket->getSide(),
                 bestHedgeMarket->getUnAdjustedSize(), bestHedgeMarket->getAdjustedSize(),
                 ToDollars(bestHedgeMarket->getPrice()), ToDollars(marketPrice))
#endif

            pathContract._priceSizeCache[bestHedgeMarket->getSide()]._price = marketPrice;
            pathContract._priceSizeCache[bestHedgeMarket->getSide()]._size = size;
            _priceSize._price += pathContract._multiplier * marketPrice;
            _priceSize._size = std::min<hf::qty_t>(_priceSize._size, size);
        }

        if (_priceSize.valid()) {
            _priceSize._price += _stfOffset._stfOffset;
            _priceSize._price = applyPriceOffset<Side>(_priceSize._price, -_commission);
        }

        if ((_oldPriceSize._price != _priceSize._price) ||
            ((_oldPriceSize._size > _priceSize._size) & (_priceSize._size < _contract->_quoteSize)) ||      // need to quote less
            ((_oldPriceSize._size < _priceSize._size) & (_oldPriceSize._size < _contract->_quoteSize))) {   // can quote more
            LOG(0, "AS {C[i1]}. Path {i2} Side={i3} {i4}@{d1}", _contract->name(), _id, _side, _priceSize._size, ToDollars(_priceSize._price))
            return true;
        }
#ifdef ENABLE_PERF
        else {
            static uint64_t count = 0;
            ADD_PERF_PERCENTAGE(PerfId::AS_SamePathCalcResult, ++count, total);
        }
#endif
    }
    return false;
}

template<uint32_t Side>
inline void ArbieSpreader::Contract::updatePaths(uint32_t& pathCount, bool force) {
    SCOPED_PERF_POINT(PerfId::AS_UpdatePaths);
#ifdef ENABLE_PERF
    static uint64_t total = 0;
    ++total;
#endif
    bool sort = false;
    for (auto path : _paths[Side]) {
        sort |= path->calculate<Side>(force, pathCount);
    }
    if (sort) {
        auto oldBest = _paths[Side][0];
        std::sort(_paths[Side].begin(), _paths[Side].end(), PathSorter<Side>());

#ifdef IS_DEV
        for (auto* path : _paths[Side]) {
            TLOG(0, " - AS {C[i1]}. Path {i2} Side={i3} {i4}@{d1}", path->_contract->name(), path->_id, Side,
                path->_priceSize._size, ToDollars(path->_priceSize._price))
            for (auto& contract : path->_leanContracts) {
                TLOG(0, " - LeanContract={C[i1]}", contract._contract->name())
                auto markets = getLeanMarkets<Side>(contract);
                TLOG(0, " - Market {C[i1]} Id={i2} Side={i3} Price={d1} Size={i4} AdjustedSize={i5}", markets[Market::Internal].getContractId(),
                    markets[Market::Internal].getId(), markets[Market::Internal].getSide(),
                    ToDollars(markets[Market::Internal].getPrice()), markets[Market::Internal].getUnAdjustedSize(),
                    markets[Market::Internal].getAdjustedSize())
                TLOG(0, " - Market {C[i1]} Id={i2} Side={i3} Price={d1} Size={i4} AdjustedSize={i5}", markets[Market::Internal].getContractId(),
                    markets[Market::External].getId(), markets[Market::Internal].getSide(),
                    ToDollars(markets[Market::External].getPrice()), markets[Market::External].getUnAdjustedSize(),
                    markets[Market::External].getAdjustedSize())
            }
        }
#endif

        auto bestPath = _paths[Side][0];
        if (oldBest != bestPath) {
            LOG(0, "AS {C[i1]}. {i2} Lean on path {i3} {i4}@{d1}", bestPath->_contract->name(),
                Side, bestPath->_id, bestPath->_priceSize._size, ToDollars(bestPath->_priceSize._price))
        }
#ifdef ENABLE_PERF
        else  {
            static uint64_t count = 0;
            ADD_PERF_PERCENTAGE(PerfId::AS_BestPathUnChangedAfterRecalc, ++count, total);
        }
#endif
    }
}

template<uint32_t Side>
inline void ArbieSpreader::tryAddAggressiveQuoteAdjustment(Contract& contract, Path* path, const Contract::Quote& quote, bool quoteExternally) {
    if (quote._externalSize) {
        if (contract.addAggressiveQuoteAdjustment<Side>(path, quote, quoteExternally, _effected_markets)) {
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
            for (auto hedgeMarket : path->_bestHedgeMarket) {
                otherSideMarket.addFAKPreAdjustment(hedgeMarket);
                effected_contract.emplace(hedgeMarket);
            }
            if (updateBestMarket<otherSide>(&otherSideMarket)) {
                setMarketDirty(otherSide);
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

template<uint32_t Side>
inline bool ArbieSpreader::updateQuote(Contract& contract, bool force, bool canQuoteExternal, ReadyOrderRequests& lastRequest) {
    bool quoteUpdated = false;
    auto* pBestPath = contract._paths[Side][0];
    auto& quote = contract._quote[Side];
    if (pBestPath->_priceSize.valid()) {
        quote._leanPrice = applyPriceOffset<Side>(pBestPath->_priceSize._price, pBestPath->_commission);
        quote._internalPrice = applyPriceOffset<Side>(pBestPath->_priceSize._price, -contract._edge);
        quote._internalSize = std::min<hf::qty_t>(pBestPath->_priceSize._size, contract._quoteSize);
        quote._hedgePath = pBestPath->_id;
        if (canQuoteExternal) {
            quote._externalPrice = (quote._internalPrice != hf::NULL_PRICE) ? RoundToTickSize(quote._internalPrice, contract._def._tickSize, Side) : hf::NULL_PRICE;
            quote._externalSize = contract.getExternalQuoteSize<Side>(quote);
        } else {
            quote._externalPrice = hf::NULL_PRICE;
            quote._externalSize = 0;
        }

        if (isEnabled()) {
            if (!checkMaxTicksThroughMarket<Side>(contract, quote)) {
                LOG(0, "AS {C[i1]}. MaxTicksThroughMarketCheckFailed. Disable AS and Disable AS External Quoting. Side={i2} Internal: {i3}@{d1} External: {i4}@{d2}. LeanPath={i5} LeanPrice={d3} LeanOnRV={i6} OtherSideBestPrice={d4}", contract.name(),
                    Side, quote._internalSize, ToDollars(quote._internalPrice), quote._externalSize,
                    ToDollars(quote._externalPrice), quote._hedgePath, ToDollars(quote._leanPrice), pBestPath->_leanOnRV,
                    ToDollars(contract.template getBestMarket<GetOtherSide<Side>()>()->getPrice()))
                pBestPath->log();
                sendErrorMessage(boost::str(boost::format("Disabling AS and AS QuoteExternal. Reason=AS MaxTicksThroughMarketCheckFailed. OffendingContract=%1% Side=%2% calculatedQuote=%3% otherSideBest=%4%")
                    % contract._def._name % Side % ToDollars(quote._externalPrice)
                    % ToDollars(contract.template getBestMarket<GetOtherSide<Side>()>()->getPrice())), ErrorType::Critical);
                disable();
                handleDisabled();
                disableQuoteExternal();
                return false;
            }

            if (contract._lastQuoteSent[Side] != quote || contract._forceUpdate[Side] || force) {
                quoteUpdated = true;
                lastRequest._quote[Side] = _orderRequestMgr.sendQuoteRequest(
                        contract._def._internalId, orderId(StrategyId::ArbieSpreader, Order::Type::Quoting),
                        Side, quote._internalPrice, quote._internalSize, quote._externalPrice, quote._externalSize,
                        canQuoteExternal, pBestPath->_leanOnRV, 0);
                LOG(0, "AS {C[i1]}. {i2} Update quote. Internal: {i3}@{d1} External: {i4}@{d2}. LeanPath={i5} LeanPrice={d3} LeanOnRV={i6}", contract.name(),
                    Side, quote._internalSize, ToDollars(quote._internalPrice), quote._externalSize,
                    ToDollars(quote._externalPrice), quote._hedgePath, ToDollars(quote._leanPrice), pBestPath->_leanOnRV)
                contract._lastQuoteSent[Side] = quote;
                contract._forceUpdate[Side] = false;
                tryAddAggressiveQuoteAdjustment<Side>(contract, pBestPath, quote, canQuoteExternal);
            }
        }
    } else {
        if (quote._hedgePath != -1) {
            quote.reset();
            if (isEnabled()) {
                quoteUpdated = true;
                lastRequest._quote[Side] = _orderRequestMgr.sendQuoteRequest(
                        contract._def._internalId, orderId(StrategyId::ArbieSpreader, Order::Type::Quoting),
                        Side, quote._internalPrice, quote._internalSize, quote._externalPrice, quote._externalSize,
                        canQuoteExternal, false,0);
                LOG(0, "AS {C[i1]}. {i2} Update quote. Internal: {i3}@{d1} External: {i4}@{d2}. LeanPath={i5} LeanOnRV=0", contract.name(),
                    Side, quote._internalSize, ToDollars(quote._internalPrice), quote._externalSize,
                    ToDollars(quote._externalPrice), quote._hedgePath)
                contract._lastQuoteSent[Side] = quote;
                contract._forceUpdate[Side] = false;
            }
        }
    }
    return quoteUpdated;
}

template<uint32_t Side>
inline bool ArbieSpreader::checkMaxTicksThroughMarket(Contract& contract, Contract::Quote& quote) {
    auto otherSideBestMarket = contract.template getBestMarket<GetOtherSide<Side>()>();
    if (quote._externalPrice == hf::NULL_PRICE ||  otherSideBestMarket->getPrice() == hf::NULL_PRICE) {
        return true;
    }
    return calculateTicksThroughMarket<Side>(quote._externalPrice, otherSideBestMarket->getPrice()) < contract._maxTicksThroughMarket;
}

}