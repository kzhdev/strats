#pragma once

#include "ViperStrategyBase.h"
#include "OrderRequestManager.h"
#include "PerfMonitor.h"
#include <boost/dynamic_bitset.hpp>

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

    struct PathContract {
        Contract* _contract;
        int32_t _multiplier;
        uint32_t _internalId;
        uint32_t _side;
        uint32_t _index;

        PathContract(Contract* contract, int32_t multiplier, uint32_t internalId, uint32_t index)
            : _contract(contract), _multiplier(multiplier), _internalId(internalId), _index(index) {}

        Market* bestMarket() const noexcept {
            return _contract->_bestMarket[_side];
        }
    };

    template<uint32_t> friend Market* getBestMarket(const ArbieSpreader::PathContract&);
    template<uint32_t> friend Market* getLeanMarkets(const ArbieSpreader::PathContract&);

    struct Path {
    private:
        friend class ShortTermFairOffset;
        hf::price_t _price = hf::NULL_PRICE;
        hf::qty_t _size = 0;
        hf::price_t _commission;

    public:
        Contract* _contract;
        ArbieSpreaderPath* _config;
        Path* _otherSidePath = nullptr;
        const ShortTermFairOffset& _stfOffset;
        hf::price_t _rvSTFMove = 0;
        std::vector<PathContract> _leanContracts;
        std::vector<PathContract*> _contractToPathContract;
        boost::dynamic_bitset<> _pricesInvalid;

        uint64_t _banExpiresAt = 0;
        uint32_t _id;
        uint32_t _side;
        Contract* _sizeDetermineContract = nullptr;
        bool _dirty {false};
        bool _recalc {false};
        bool _resort {false};
        bool _leanOnRV {false};
        std::string _name;

        enum InvalidField : uint8_t {
            STFOffset,
            AdjustedSize,
            CanTrade,
            __InvalidField_End__,
        };

        std::bitset<InvalidField::__InvalidField_End__> _invalidFlags;

        bool isValid() const noexcept {
            return _invalidFlags.none() && _config->_enabled && _pricesInvalid.none();
        }

        bool isBanned() const noexcept {
            return _banExpiresAt;
        }

        bool isEnabled() const noexcept {
            return _config->_enabled & _contract->_enabled;
        }

        hf::price_t price() const noexcept {
            return isValid() ? _price : hf::NULL_PRICE;
        }

        hf::qty_t size() const noexcept {
            return isValid() ? _size : 0;
        }

        hf::price_t commission() const noexcept {
            return _commission;
        }

        void reset() noexcept {
            _price = hf::NULL_PRICE;
            _size = 0;
            _resort = true;
        }

        template<uint32_t Side>
        inline bool calculate(bool force, uint32_t& pathCount);

        Path(uint32_t side, std::string name, uint32_t id, Contract* contract,
             const std::vector<PathContract>& pathContracts, ArbieSpreaderPath& pathConfig,
             const ShortTermFairOffset* stfOffset, uint32_t numAllContracts);

        template<uint32_t Side>
        void onBestMarketDataUpdate(Contract* contract, hf::price_t oldPrice, hf::price_t newPirce, hf::qty_t size);

        template<uint32_t Side>
        void onBestMarketAdjusted(Market* market);

        bool marketDirty(Contract* contract);

        void log() const noexcept;

        bool updateCommission(hf::price_t value) noexcept {
            hf::price_t oldPrice = _price;
            _price += applyPriceOffset(_price, _commission - value, _side);
            LOG(0, "AS {C[i1]}. {i2} Path {i3} commission changed {i4}->{i5}. price {d1}->{d2}", _contract->name(),
                _side, _id, _commission, value, ToDollars(oldPrice), ToDollars(_price))
            _commission = value;
            if ((IsBetter(_price, oldPrice, _side) && (this != _contract->_paths[_side][0])) || // Other path's price gets better
                (IsBetter(oldPrice, _price, _side) && this == _contract->_paths[_side][0])) {   // Best path's price is worsened
                _resort = true;
                return true;
            }
            return false;
        }
    };

    template<uint32_t Side>
    struct PathSorter {
        static constexpr hf::price_t priceFactor = (Side == BID) ? 1 : -1;

        bool operator()(const ArbieSpreader::Path* a, const ArbieSpreader::Path* b) const {
            auto aPrice = a->price();
            auto bPrice = b->price();
            auto aValid = aPrice != hf::NULL_PRICE;
            auto bValid = bPrice != hf::NULL_PRICE;
            // 1. if one is valid and other is not, consider the valid better.
            // 2. if both same validity
            //    a. If side is BID, bigger price is best
            //    b. If side is ASK, lower price is best (hence priceFactor == -1)
            auto aPair = std::make_pair(aValid, aPrice * priceFactor);
            auto bPair = std::make_pair(bValid, bPrice * priceFactor);
            if (aPair == bPair) {
                return a->size() > b->size();
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
                : _id(std::move(id)), _contract(contract)
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
        std::vector<Path*> _affectedPaths[2];  // The path used this contract
        static const ShortTermFairOffset _zeroStfOffset;
        hf::price_t _stf = hf::NULL_PRICE;
        hf::price_t _edge = 0;
        double _hedgeOffset = 0.;
        hf::price_t _maxTicksThroughMarket = 0;
        hf::price_t _2TimesMaxTicksThroughMarket = 0;
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
        uint64_t _banExpiresAt[2] {0, 0};
        hf::batch_id_t _lastMDbid {0};

        enum BanInitiator : uint8_t {
            None,
            Aggressor,
            Fill,
        };
        BanInitiator _banInitiator[2] { BanInitiator::None, BanInitiator::None };

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
            for (auto* path : _affectedPaths[side]) {
                if (path->_dirty | path->_recalc | path->_resort) {
                    ArbieSpreader::get()->pushUpdatedContract(path->_contract);
                }
            }
        }

        void setCanTrade(bool canTrade) {
            _canTrade = canTrade;
            for (auto& affectedPathBySide : _affectedPaths) {
                for (auto* path : affectedPathBySide) {
                    bool pathWasValid = path->isValid();
                    path->_invalidFlags.set(Path::InvalidField::CanTrade, !_canTrade);
                    if (!_canTrade && pathWasValid) {
                        path->_resort = true;
                    }
                }
            }
            ArbieSpreader::get()->pushUpdatedContract(this);
        }

        template<uint32_t Side>
        hf::price_t getExtCommissionAdjustedBestPrice() const noexcept {
            auto price = _bestMarket[Side]->getPrice();
            if (_bestMarket[Side]->getId() == MarketIds::MARKET_ID_EXTERNAL) {
                return (price != hf::NULL_PRICE) ? applyPriceOffset<Side>(price, -_extCommission) : hf::NULL_PRICE;
            }
            return price;
        }

        template<uint32_t Side>
        bool updateMarket(MarketIds marketId, hf::price_t price, hf::qty_t qty, std::unordered_set<Market*>& affected_markets) {
            LOG(0, "AS {C[i1]}. MD update Side={i2} marketId={i3} {i4}@{d1}", name(), Side, toUInt(marketId), qty, ToDollars(price))
            Market* updated;
            bool changed = false;
            hf::price_t oldBestPrice = getExtCommissionAdjustedBestPrice<Side>();
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
                    updateAffectedPathsBestMarket<Side>(oldBestPrice);
                    return true;
                }

            }
            return false;
        }

        void updatePaths(bool force = false) {
            uint32_t pathCount = 0;

            if (_banInitiator[BID] == Contract::BanInitiator::None) {
                updatePaths<BID>(pathCount, force);
            }

            if (_banInitiator[ASK] == Contract::BanInitiator::None) {
                updatePaths<ASK>(pathCount, force);
            }
            ADD_PERF_VALUE(PerfId::AS_PathUpdateCountPerContractQuotes, pathCount);
        }

        template<uint32_t Side>
        inline void updatePaths(uint32_t& pathCount, bool force = false);

        template<uint32_t Side>
        inline Path* getQuotingPath() const noexcept;

        template<uint32_t Side>
        bool updateBestMarket(Market* updated) {
            auto oldBest = _bestMarket[Side];
            hf::qty_t internalSize = _markets[Side][Market::Internal].getAdjustedSize();
            hf::qty_t externalSize;
            hf::price_t internalPrice, externalPrice;
            if (internalSize == 0) {
                _bestMarket[Side] = &_markets[Side][Market::External];
            } else if ((externalSize = _markets[Side][Market::External].getAdjustedSize()) == 0) {
                _bestMarket[Side] = &_markets[Side][Market::Internal];
            } else if (IsBetterPrice<Side>((internalPrice = _markets[Side][Market::Internal].getPrice()), (externalPrice = _markets[Side][Market::External].getPrice()))) {
                _bestMarket[Side] = &_markets[Side][Market::Internal];
            } else if (internalPrice == externalPrice) {
                if (internalSize <= externalSize) {
                    _bestMarket[Side] = &_markets[Side][Market::Internal];
                } else {
                    _bestMarket[Side] =&_markets[Side][Market::External];
                }
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

        template<uint32_t Side>
        inline void updateAffectedPathsBestMarket(hf::price_t oldBestPrice, uint64_t banEnd = 0, Path* banException = nullptr);
    };

    static ArbieSpreader* _self;
    std::vector<Contract> _contracts;
    std::vector<Path> _allPaths;
    OrderRequestManager& _orderRequestMgr;
    RingBufferQueue<Contract*> _updatedContracts;
    std::bitset<1024> _contractUpdatedFlag;
    uint64_t _updatedContractCursor {0};
    uint64_t _contractBanPeriod {0};
    double _edgeMultiplier = 0.;
    uint32_t _hedgeId {0};
    std::vector<ReadyOrderRequests> _lastOrderRequests;
    std::unordered_map<uint32_t, Path*> _pathsParameters;
    std::unordered_set<Market*> _affected_markets;
    std::unordered_set<ShortTermFairOffset*> _updatedSTFOffsets;
    std::unordered_multimap<uint32_t, ShortTermFairOffset*> _mapShortTermFairOffset;
    std::unordered_set<Contract*> _bannedContracts;
    std::unordered_set<Path*> _bannedPaths;
    Matrix _riskMatrix; // x,y - vectors - square
    std::vector<double> _vectorsRange;
    std::vector<double> _vectorsMaxPosition;
    std::vector<FillMessage> _fillsToGUI;

public:
    ArbieSpreader()
        : _orderRequestMgr(OrderRequestManager::get())
        , _updatedContracts(8192) {
        _fillsToGUI.reserve(8);
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

    void onMDStatus(hf::VENUE venue, uint64_t extSeqNum, const md::MDStatusEvent &event, uint32_t internalId, hf::batch_id_t bid);

    // GUI update
    void populateGUIInitialization(::google::protobuf::RepeatedPtrField< ::Python::proto::GridDefinition >* definitions) override;
    void sendStateToGUI(std::function<void(proto::GridUpdate&, WindowIds)>&& sendingFunc, std::function<bool(WindowIds)>&& hasSubscription) const override;

private:
    void populateParameterCells(::google::protobuf::RepeatedPtrField< ::Python::proto::GridCell >* cells, const Contract& contract) const;
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
    inline hf::qty_t updateHedge(Contract& contract, hf::price_t fillPrice, hf::qty_t qty, std::vector<FillMessage>& fillsToGUI, bool isInternal);

    template<uint32_t Side>
    inline bool hedge(Contract& contract, Path* path, hf::price_t fillPrice, hf::qty_t qty, std::vector<FillMessage>& fillsToGUI, bool isInternal);

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

    void handleFakDone(uint32_t side, uint32_t internalId, om::strategy_order_id_t::value_type orderId);
    void handleInternalHedgeDone(const InternalHedgeDone& message);
    void handleASInternalMatchingDone(uint32_t contractId, hf::qty_t bidFills, hf::qty_t askFills);
    void markContractDirty(Contract& contract, Market* updatedMarket, uint64_t banEnd = 0, Path* banException = nullptr);
    void handleSTFUpdate(const STFUpdate& msg);

    template<uint32_t Side>
    inline void tryAddAggressiveQuoteAdjustment(Contract& contract, Path* path, const Contract::Quote& quote, bool quoteExternally);

    void pushUpdatedContract(Contract* contract);
    void onEconomicNumberEnded(EconomicNumber::Color color);
    void onShiftUpdate(uint32_t shift);
    void handleVectorParameterUpdate(const VectorParameterUpdate& msg);
    void checkBans(uint64_t now);
};

template<uint32_t Side>
inline Market* getLeanMarkets(const ArbieSpreader::PathContract& pathContract);

template<>
inline Market* getLeanMarkets<BID>(const ArbieSpreader::PathContract& pathContract) {
    return pathContract._multiplier > 0 ? pathContract._contract->_markets[BID] : pathContract._contract->_markets[ASK];
}

template<>
inline Market* getLeanMarkets<ASK>(const ArbieSpreader::PathContract& pathContract) {
    return pathContract._multiplier > 0 ? pathContract._contract->_markets[ASK] : pathContract._contract->_markets[BID];
}

template<uint32_t Side>
inline hf::qty_t ArbieSpreader::updateHedge(Contract& contract, hf::price_t fillPrice, hf::qty_t qty, std::vector<FillMessage>& fillsToGUI, bool isInternal) {
    SCOPED_PERF_POINT(PerfId::AS_UpdateHedge);
    constexpr auto otherSide = GetOtherSide<Side>();
    if (isInternal) {
        if (contract._markets[Side][Market::Internal].undoAsQuoteAdjustment(_affected_markets)) {
            markContractDirty(contract, &contract._markets[Side][Market::Internal]);
        }

        // If AS internally crossed, side AS Quote will be unadjusted when ASK fill received
        // this could cause bid hedge switch path. We will try to undo AS quote on both side
        if (contract._markets[Side][Market::External].getASQuoteAdjustment() &&
            contract._markets[Side][Market::External].undoAsQuoteAdjustment(std::min<hf::qty_t>(qty, contract._markets[Side][Market::External].getASQuoteAdjustment()), _affected_markets)) {
            markContractDirty(contract, &contract._markets[Side][Market::External]);
        }
    }
    if (contract._markets[otherSide][Market::External].getASQuoteAdjustment() &&
        contract._markets[otherSide][Market::External].undoAsQuoteAdjustment(std::min<hf::qty_t>(qty, contract._markets[otherSide][Market::External].getASQuoteAdjustment()), _affected_markets)) {
        markContractDirty(contract, &contract._markets[otherSide][Market::External]);
    }

    for (auto market : _affected_markets) {
        markContractDirty(_contracts[market->getContractId()], market);
    }
    _affected_markets.clear();

    Path* pBest = contract._paths[Side][0];
    hf::qty_t remainingSize = qty;
    hf::price_t price = pBest->price();
    hf::qty_t size = pBest->size();
    if (price != hf::NULL_PRICE && size && !pBest->_recalc) {
        auto hedgeSize = std::min<hf::qty_t>(size, qty);
        if (!hedge<Side>(contract, pBest, fillPrice, hedgeSize, fillsToGUI, isInternal)) {
            return remainingSize;
        }
        remainingSize -= hedgeSize;
    }

    if (remainingSize) {
        uint32_t pathCount = 0;
        contract.template updatePaths<Side>(pathCount);

        for (auto path : contract._paths[Side]) {
            size = path->size();
            if (path->price() == hf::NULL_PRICE || !size) {
                break;
            }

            auto hedgeSize = std::min<hf::qty_t>(size, remainingSize);
            if (!hedge<Side>(contract, path,fillPrice, hedgeSize, fillsToGUI, isInternal)) {
                return remainingSize;
            }
            remainingSize -= hedgeSize;

            if (!remainingSize) {
                // fully hedged
                break;
            }
        }
    }

    return remainingSize;
}

template<uint32_t Side>
inline bool ArbieSpreader::hedge(Contract& contract, Path* path, hf::price_t fillPrice, hf::qty_t qty, std::vector<FillMessage>& fillsToGUI, bool isInternal) {
    auto price = path->price();
    auto size = path->size();
    if (calculateTicksThroughMarket<Side>(fillPrice, price) >= contract._2TimesMaxTicksThroughMarket) {
        LOG(0, "AS {C[i1]}. MaxTicksThroughMarketCheckFailed in updateHedge. Disable AS. Side={i2} fillPrice={d1} hedgePath: {i3} {i4}@{d2}", contract.name(),
            Side, ToDollars(fillPrice), path->_id, size, ToDollars(price))
        path->log();
        sendErrorMessage(boost::str(boost::format("Disabling AS and AS QuoteExternal. Reason=MaxTicksThroughMarketCheckFailed in updateHedge. OffendingContract=%1% Side=%2% fillPrice=%3% pathPrice=%4%")
                                    % contract._def._name % Side % ToDollars(fillPrice)
                                    % ToDollars(price)), ErrorType::Critical);
        disable();
        handleDisabled();
        return false;
    }

    constexpr auto otherSide = GetOtherSide<Side>();
    auto hedgeId = ++_hedgeId;
    LOG(0, "AS {C[i1]}. Hedge on Path {i2} {i3}@{d1}. IsSwitchPath={i4} invalidFlags={i5} allPricesValid={i6} banned={i7}", contract.name(),
        path->_id, size, ToDollars(price), !path->_stfOffset._id.empty(), path->_invalidFlags.to_ulong(),
        path->_pricesInvalid.none(), path->isBanned())
    for (uint32_t i = 0; i < path->_leanContracts.size(); ++i) {
        auto& hedgeContract = path->_leanContracts[i];
        auto* hedgeMarket = hedgeContract.bestMarket();
        if (hedgeContract._multiplier > 0) {
            auto internalHedgePrice = applyPriceOffset<otherSide>(hedgeMarket->getPrice(), hedgeContract._contract->_hedgeOffsetPrice);
            auto externalHedgePrice = hedgeMarket->getPrice();
            sendHedgeOrder(contract._def._internalId, hedgeContract._internalId, otherSide,
                           internalHedgePrice, externalHedgePrice, qty, hedgeMarket->getId() == MarketIds::MARKET_ID_EXTERNAL,
                           hedgeId, hedgeMarket->getId(), path->_id);
        } else {
            auto internalHedgePrice = applyPriceOffset<Side>(hedgeMarket->getPrice(), hedgeContract._contract->_hedgeOffsetPrice);
            auto externalHedgePrice = hedgeMarket->getPrice();
            sendHedgeOrder(contract._def._internalId, hedgeContract._internalId, Side,
                           internalHedgePrice, externalHedgePrice, qty, hedgeMarket->getId() == MarketIds::MARKET_ID_EXTERNAL,
                           hedgeId, hedgeMarket->getId(), path->_id);
        }
    }

    if (!path->_stfOffset._id.empty()) {
        // This is a switch path, forward the fill to RV
        auto now = hf::util::TimeUtils::clock_gettime();
        FillMessage message;
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
        fillsToGUI.template emplace_back(message);

        auto dataSize = (path->_leanContracts.size() + 1) * sizeof(FillMessage);
        auto* rv = getStrategy(StrategyId::RelativeValue);
        rv->template sendMessage<ASFillsToRV>(
                ViperMessageType::TransferPositionToRV,
                [this, &contract, qty, now, path, otherSide, &fillsToGUI](ASFillsToRV& message) {
                    auto quoteAdjustedFillPrice = contract._stf + path->_stfOffset._rvSTFMove * qty;
                    message._numFills = path->_leanContracts.size() + 1;
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

                    for (const auto& hedgeContract : path->_leanContracts) {
                        FillMessage msg;
                        msg._contractId = hedgeContract._internalId;
                        msg._strategy = StrategyId::ArbieSpreader;
                        msg._price = hedgeContract._contract->_stf;
                        msg._qty = qty;
                        msg._side = hedgeContract._multiplier > 0 ? Side : otherSide;
                        msg._type = FillType::Internal;
                        msg._source = FillSource::AsToRv;
                        msg._checkLimit = false;
                        msg._timestamp = now;
                        msg._forceOverFill = false;
                        msg._rvStf = hf::NULL_PRICE;
                        fillsToGUI.emplace_back(msg);

                        ++fill;
                        fill->_contractId = hedgeContract._internalId;
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

    // Adjust FAK in separate loop so that the hedges are sent out ASAP.
    for (const auto& hedgeContract : path->_leanContracts) {
        auto* hedgeMarket = hedgeContract.bestMarket();
        hedgeMarket->addFAKAdjustment(hedgeId, qty);
        assert(hedgeMarket->getSide() == (1 - Side) || (hedgeContract._multiplier > 0));
        markContractDirty(*hedgeContract._contract, hedgeMarket);
    }
    return true;
}


template<uint32_t Side>
inline bool ArbieSpreader::Path::calculate(bool force, uint32_t& pathCount) {
    if (_recalc | force) {
        _recalc = false;

        if (!isEnabled()) {
            if (_price != hf::NULL_PRICE) {
                // just changed from Enabled to Disabled
                reset();
                return true;
            }
            // already disabled
            return false;
        }

        bool wasValid = isValid();

        SCOPED_PERF_POINT(PerfId::AS_Path_Calculation);

#ifdef ENABLE_PERF
        static uint64_t total = 0;
        ++total;
        ++pathCount;
#endif

        auto oldPrice = _price;
        auto oldSize = _size;
        _price = 0;
        _size = _contract->_quoteSize;
        _sizeDetermineContract = nullptr;
        _leanOnRV = false;
        for (uint32_t i = 0; i < _leanContracts.size(); ++i) {
            auto& pathContract = _leanContracts[i];
            auto bestHedgeMarket = pathContract.bestMarket();
            _leanOnRV |= bestHedgeMarket->getId() == MarketIds::Market_ID_RV_QUOTE;
            auto marketPrice = bestHedgeMarket->getPrice();

            // apply commission to external market
            if (bestHedgeMarket->getId() == MarketIds::MARKET_ID_EXTERNAL && marketPrice != hf::NULL_PRICE) {
                // apply commission to external market
                marketPrice = applyPriceOffset(marketPrice, -pathContract._contract->_extCommission, bestHedgeMarket->getSide());
            }

            _pricesInvalid.set(pathContract._index, marketPrice == hf::NULL_PRICE);
            if (BOOST_UNLIKELY(marketPrice == hf::NULL_PRICE)) {
                marketPrice = 0;
            }

            auto size = bestHedgeMarket->getAdjustedSize();
            if (!size && (_contract->_markets[1 - _side][Market::External].getASQuoteAdjustment())) {
                // if 0 size was due to the aggressive AS quoting, exclude the FAK Pre Adjustment
                size = bestHedgeMarket->getAdjustedSizeExcludeFAKPreAdj();
            }

#ifdef IS_DEV
            DLOG(0, "AS {C[i1]}. Update Path {i2} Side={i3} lean={C[i4]} marketId={i5} side={i6} {i7}({i8})@{d1}({d2}) invalidFlags={i9} allPricesValid={i10}", _contract->name(),
                 _id, _side, pathContract._contract->name(), bestHedgeMarket->getId(), bestHedgeMarket->getSide(),
                 bestHedgeMarket->getUnAdjustedSize(), bestHedgeMarket->getAdjustedSize(),
                 ToDollars(bestHedgeMarket->getPrice()), ToDollars(marketPrice), _invalidFlags.to_ulong(),
                 _pricesInvalid.none())
#endif

            _price += pathContract._multiplier * marketPrice;

            if (!_size || _size > size) {
                _sizeDetermineContract = size ? pathContract._contract : nullptr;
                _size = std::min(size, _contract->_quoteSize);
            }
        }

        _invalidFlags.set(InvalidField::AdjustedSize, !_size);

        _price += _stfOffset._stfOffset;
        _price = applyPriceOffset<Side>(_price, -_commission);

        if (wasValid && ((oldPrice != _price) ||
            ((oldSize > _size) & (_size < _contract->_quoteSize)) ||      // need to quote less
            ((oldSize < _size) & (oldSize < _contract->_quoteSize)))) {   // can quote more
            LOG(0, "AS {C[i1]}. Path {i2} Side={i3} {i4}@{d1} invalidFlags={i5} allPriceValid={i6}", _contract->name(),
                _id, _side, _size, ToDollars(_price), _invalidFlags.to_ulong(), _pricesInvalid.none())
#ifdef IS_DEV
            if (_leanContracts.size() == 1 && _price != hf::NULL_PRICE && _leanContracts[0].bestMarket()->getPrice() != hf::NULL_PRICE &&
                IsBetterPrice<Side>(_price, _leanContracts[0].bestMarket()->getPrice())) {
                LOG(0, "!!!! AS {C[i1]}. {i2} Path {i3} price better than best market {i4} stfOffset={i5}", _contract->name(),
                    Side, _id, _leanContracts[0].bestMarket()->getPrice(), _stfOffset._stfOffset)
            }
#endif
            return true;
        }
#ifdef ENABLE_PERF
        else {
            static uint64_t count = 0;
            ADD_PERF_PERCENTAGE(PerfId::AS_SamePathCalcResult, ++count, total);
        }
#endif
    }
#ifdef IS_DEV
    if (isValid() && _leanContracts.size() == 1 && _price != hf::NULL_PRICE && IsBetterPrice<Side>(_price, _leanContracts[0].bestMarket()->getPrice())) {
        LOG(0, "!!!! AS {C[i1]}. {i2} Path {i3} {i4}@{i5} price better than best market {i6} stfOffset={i7}", _contract->name(),
            Side, _id, _size, _price, _leanContracts[0].bestMarket()->getPrice(), _stfOffset._stfOffset)
    }
#endif
    return false;
}

template<uint32_t Side>
void ArbieSpreader::Path::onBestMarketDataUpdate(Contract* contract, hf::price_t oldPrice, hf::price_t newPrice,
                                                 hf::qty_t size) {
    if (_recalc || !isEnabled()) {
        // need to re-calc anyway
        return;
    }

    auto pathContract = _contractToPathContract[contract->_def._internalId];
    assert(pathContract);

    if (_price == hf::NULL_PRICE) {
        if (newPrice != hf::NULL_PRICE && size != 0) {
            // need to recalculate
            _pricesInvalid.reset(pathContract->_index);
            _recalc = true;
        }
        return;
    }

    bool wasValid = isValid();

    if (newPrice == hf::NULL_PRICE) {
        _pricesInvalid.set(pathContract->_index);
        _resort |= wasValid;
        newPrice = 0;
    } else if (_pricesInvalid.test(pathContract->_index)) {
        _pricesInvalid.reset(pathContract->_index);
        _resort |= _pricesInvalid.none();
    }

    if (oldPrice == hf::NULL_PRICE) {
        oldPrice = 0;
    }

    hf::price_t priceDelta = newPrice - oldPrice;
    hf::price_t delta = (pathContract->_multiplier * priceDelta);
    TLOG(0, "AS {C[i1]}. {i2} Path {i3} Market {C[i4]} Side={i5} Apply delta {i6}. {i7}@{i8}->{i9} _sizeDetermineContract={i10}", _contract->name(),
         _side, _id, contract->name(), Side, delta, _size, _price, (_price + delta),
         (_sizeDetermineContract ? _sizeDetermineContract->_def._internalId : -1))
    _price += delta;
    _dirty |= priceDelta;
    if (IsBetterPrice<Side>(newPrice, oldPrice)) {
        // If best path price getting better, we don't need to re-sort
        _resort |= wasValid && (this != _contract->_paths[_side][0]);
    } else {
        // If best path price getting worse, we need to re-sort.
        // If other path price getting worse, we don't need to re-sort
        _resort |= wasValid && (this == _contract->_paths[_side][0]) && priceDelta;
    }

    if (newPrice != hf::NULL_PRICE) {
        if (size == 0 && _contract->_markets[1 - _side][Market::External].getASQuoteAdjustment()) {
            // if 0 size was due to the aggressive AS quoting, exclude the FAK Pre Adjustment
            auto bestMarket = contract->_bestMarket[Side];
            size = bestMarket->getAdjustedSizeExcludeFAKPreAdj();
        }

        _invalidFlags.set(InvalidField::AdjustedSize, !size);
        if (size == 0) {
            _resort |= wasValid;
        }
    }

    if (!_size || size < _size ) {
        _size = std::min<hf::qty_t>(size, _contract->_quoteSize);
        _sizeDetermineContract = size ? contract : nullptr;
        _resort = size ? true : wasValid;
    } else if ((size > _size) && (_size < _contract->_quoteSize) &&
               (!_sizeDetermineContract || contract == _sizeDetermineContract)) {
        _size = std::min<hf::qty_t>(size, _contract->_quoteSize);
        for (auto& pathContract : _leanContracts) {
            auto sz = pathContract.bestMarket()->getAdjustedSize();
            if (sz < _size) {
                _size = sz;
                _sizeDetermineContract = sz ? pathContract._contract : nullptr;
            }
        }

        _resort = true;
    }
}

template<uint32_t Side>
inline void ArbieSpreader::Contract::updatePaths(uint32_t& pathCount, bool force) {
    SCOPED_PERF_POINT(PerfId::AS_UpdatePaths);
    bool sort = false;
    for (auto path : _paths[Side]) {
        if (path->_recalc | force) {
            sort |= path->calculate<Side>(force, pathCount);
        }
        sort |= path->_resort;
        path->_resort = false;
        path->_dirty = false;
    }
    if (sort) {
        auto oldBest = _paths[Side][0];
        std::sort(_paths[Side].begin(), _paths[Side].end(), PathSorter<Side>());

#ifdef IS_DEV
        if (BOOST_UNLIKELY(logging::loggingLevel() > logging::LOG_LEVEL::TRACE)) {
            for (auto* path: _paths[Side]) {
                TLOG(0, " - AS {C[i1]}. Path {i2} Side={i3} {i4}@{d1} invalidFlags={i5} allPricesValid={i6}", path->_contract->name(),
                     path->_id, Side, path->size(), ToDollars(path->price()), path->_invalidFlags.to_ulong(),
                     path->_pricesInvalid.none())
                for (auto& contract: path->_leanContracts) {
                    TLOG(0, " - LeanContract={C[i1]}", contract._contract->name())
                    auto markets = getLeanMarkets<Side>(contract);
                    TLOG(0, " - Market {C[i1]} Id={i2} Side={i3} Price={d1} Size={i4} AdjustedSize={i5}",
                         markets[Market::Internal].getContractId(),
                         markets[Market::Internal].getId(), markets[Market::Internal].getSide(),
                         ToDollars(markets[Market::Internal].getPrice()), markets[Market::Internal].getUnAdjustedSize(),
                         markets[Market::Internal].getAdjustedSize())
                    TLOG(0, " - Market {C[i1]} Id={i2} Side={i3} Price={d1} Size={i4} AdjustedSize={i5}",
                         markets[Market::Internal].getContractId(),
                         markets[Market::External].getId(), markets[Market::Internal].getSide(),
                         ToDollars(markets[Market::External].getPrice()), markets[Market::External].getUnAdjustedSize(),
                         markets[Market::External].getAdjustedSize())
                }
            }
        }
#endif
    }
}

template<uint32_t Side>
inline ArbieSpreader::Path* ArbieSpreader::Contract::getQuotingPath() const noexcept {
    for (auto path : _paths[Side]) {
        if (!path->isValid()) {
            break;
        }

        if (path->isBanned()) {
#ifdef IS_DEV
            LOG(0, "AS {C[i1]}. {i2} Skip banned for quoting. Path {i3} {i4}@{d1} invalidFlags={i5} allPriceValid={i6}", name(),
                Side, path->_id, path->size(), ToDollars(path->price()), path->_invalidFlags.to_ulong(),
                path->_pricesInvalid.none())
#endif
            continue;
        }

        return path;
    }
    return nullptr;
}

template<uint32_t Side>
inline void ArbieSpreader::tryAddAggressiveQuoteAdjustment(Contract& contract, Path* path, const Contract::Quote& quote,
                                                           bool quoteExternally) {
    if (quote._externalSize) {
        if (contract.addAggressiveQuoteAdjustment<Side>(path, quote, quoteExternally, _affected_markets)) {
            auto otherSide = GetOtherSide<Side>();
            auto now = clockUtil::GetCycles();
            auto banExpireTime = now + _contractBanPeriod;
            contract._banExpiresAt[Side] = banExpireTime;
            contract._banInitiator[Side] = Contract::BanInitiator::Aggressor;
            assert(banExpireTime);
            _bannedContracts.emplace(&contract);
            LOG(0, "AS {C[i1]}. {i2} Initiate temporary bans until {i3}. ASAggressiveQuote", contract.name(), Side, banExpireTime)
            for (auto* p : contract._affectedPaths[Side]) {
                if (p != path) {
                    p->_banExpiresAt = banExpireTime;
                    p->_resort = true;
                    ArbieSpreader::get()->_bannedPaths.emplace(p);
                    LOG(0, "AS {C[i1]}. {i2} Path {i3} banned temporarily until {i4}", p->_contract->name(),
                        p->_side, p->_id, p->_banExpiresAt)
                }
            }

            for (auto market : _affected_markets) {
                markContractDirty(_contracts[market->getContractId()], market, banExpireTime, path);
            }
            _affected_markets.clear();
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
inline bool ArbieSpreader::Contract::addAggressiveQuoteAdjustment(Path* path, const Quote& quote, bool quoteExternally, std::unordered_set<Market*>& affected_contract) {
    constexpr uint32_t otherSide = GetOtherSide<Side>();
    auto addAggQuoteAdjustment = [this, path, &affected_contract](Market& otherSideMarket, hf::price_t quote_price, hf::qty_t quote_size) -> bool {
        assert(otherSideMarket.getId() != MarketIds::__end__);
        if (isAggressiveQuote<Side>(otherSideMarket, quote_price)) {
            auto oldBestPrice = getExtCommissionAdjustedBestPrice<otherSide>();
            otherSideMarket.addAsQuoteAdjustment(quote_size, affected_contract);
            // don't need to ban old affected contracts
            for (auto iter = affected_contract.begin(); iter != affected_contract.end(); ) {
                auto market = *iter;
                if (!path->_contractToPathContract[market->getContractId()]) {
                    // contract doesn't need to be banned
                    LOG(0, "AS {C[i1]}. Path {i2} Side={i3} not banned", path->_contract->name(), path->_id, path->_side)
                    ArbieSpreader::get()->markContractDirty(*this, market);
                    iter = affected_contract.erase(iter);
                } else {
                    ++iter;
                }
            }
            for (const auto& pathContract : path->_leanContracts) {
                auto hedgeMarket = pathContract.bestMarket();
                if (hedgeMarket->getId() != MarketIds::__end__) {
                    otherSideMarket.addFAKPreAdjustment(hedgeMarket);
                    affected_contract.emplace(hedgeMarket);
                }
            }
            if (updateBestMarket<otherSide>(&otherSideMarket)) {
                updateAffectedPathsBestMarket<otherSide>(oldBestPrice);
            }
            return true;
        }
        return false;
    };

    bool adjusted = false;
    if (_markets[otherSide][Market::Type::Internal].getId() != MarketIds::__end__) {
        adjusted = addAggQuoteAdjustment(_markets[otherSide][Market::Type::Internal], quote._internalPrice, quote._internalSize);
    }
    if (quoteExternally) {
        adjusted |= addAggQuoteAdjustment(_markets[otherSide][Market::Type::External], quote._externalPrice, quote._externalSize);
    }
    return adjusted;
}

template<uint32_t Side>
inline bool ArbieSpreader::updateQuote(Contract& contract, bool force, bool canQuoteExternal, ReadyOrderRequests& lastRequest) {
    bool quoteUpdated = false;
    auto* pBestPath = contract.template getQuotingPath<Side>();
    auto& quote = contract._quote[Side];
    hf::price_t price;
    if (pBestPath && ((price = pBestPath->price()) != hf::NULL_PRICE && (!contract._banExpiresAt[Side] || contract._banInitiator[Side] == Contract::BanInitiator::Aggressor))) {
        quote._leanPrice = applyPriceOffset<Side>(price, pBestPath->commission());
        quote._internalPrice = applyPriceOffset<Side>(price, -contract._edge);
        quote._internalSize = pBestPath->size();
        if (quote._hedgePath != static_cast<int32_t>(pBestPath->_id)) {
            LOG(0, "AS {C[i1]}. {i2} Lean on path {i3} {i4}@{d1} invalidFlag={i5} allPriceValid={i6}", pBestPath->_contract->name(), Side, pBestPath->_id,
                quote._internalSize, ToDollars(price), pBestPath->_invalidFlags.to_ulong(), pBestPath->_pricesInvalid.none())
        }
        assert(quote._internalSize);
        quote._hedgePath = pBestPath->_id;
        if (canQuoteExternal) {
            quote._externalPrice = (quote._internalPrice != hf::NULL_PRICE) ? RoundToTickSize(quote._internalPrice, contract._def._tickSize, Side) : hf::NULL_PRICE;
            quote._externalSize = contract.getExternalQuoteSize<Side>(quote);
        } else {
            quote._externalPrice = hf::NULL_PRICE;
            quote._externalSize = 0;
        }

        if (isEnabled() && contract._banInitiator[Side] == Contract::BanInitiator::None) {
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

template<uint32_t Side>
inline void ArbieSpreader::Contract::updateAffectedPathsBestMarket(hf::price_t oldBestPrice, uint64_t banEnd,
                                                                   Path* banException) {
    auto newBestPrice = getExtCommissionAdjustedBestPrice<Side>();
    auto size = getBestMarket<Side>()->getAdjustedSize();
    for (auto* path : _affectedPaths[Side]) {
        path->onBestMarketDataUpdate<Side>(this, oldBestPrice, newBestPrice, size);
        TLOG(0, "AS {C[i1]}. {i2} Path {i3} {C[i4]} best market updated. price={d1} oldPrice={d2} path {i5}@{d3} dirty={i6} recalc={i7} resort={i8} invalidFlags={i9} allPriceValid={i10} banned={i11}", path->_contract->name(),
             path->_side, path->_id, name(), ToDollars(newBestPrice), ToDollars(oldBestPrice),
             path->size(), ToDollars(path->price()), path->_dirty, path->_recalc, path->_resort,
             path->_invalidFlags.to_ulong(), path->_pricesInvalid.none(), path->isBanned())
#ifdef IS_DEV
        if (path->_contract == this && newBestPrice != hf::NULL_PRICE && path->price() != hf::NULL_PRICE &&
            IsBetterPrice<Side>(path->price(), newBestPrice)) {
            LOG(0, "!!!! AS {C[i1]}. Path price better than best market", name())
        }
#endif

        if (banEnd && path != banException) {
            if (path != banException) {
                path->_banExpiresAt = banEnd;
                path->_resort = true;
                ArbieSpreader::get()->_bannedPaths.emplace(path);
                LOG(0, "AS {C[i1]}. {i2} Path {i3} banned temporarily until {i4}", path->_contract->name(),
                    path->_side, path->_id, path->_banExpiresAt)
            }
        }
    }

    setMarketDirty(Side);
}

}