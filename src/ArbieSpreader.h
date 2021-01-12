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

    struct Path {
        struct Position {
            hf::qty_t _size = 0;
            hf::price_t _priceOffset = 0;
            double _posOffsetFactor = 0.0;
        };
        Contract* _contract;
        ArbieSpreaderPath* _config;
        Path* _otherSidePath = nullptr;
        hf::price_t _commission;
        std::vector<PathContract> _leanContracts;
        PriceQtyPair _priceSize;
        std::shared_ptr<Position> _position;
        uint32_t _id;
        uint32_t _side;
        std::vector<Market*> _bestHedgeMarket;
        bool _dirty {false};
        std::string _name;

        template<uint32_t Side>
        inline bool calculate(bool force);

        Path(uint32_t side, std::string name, uint32_t id, Contract* contract, const std::vector<PathContract>& pathContracts, ArbieSpreaderPath& pathConfig);
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

    struct Contract {
        ConfigContract _def;
        Market _markets[2][2];
        Market* _bestMarket[2] = {&_markets[BID][0], &_markets[ASK][0]};
        std::vector<Path*> _paths[2];
        std::vector<Path*> _affectedPaths;  // The path used this contract
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
        PriceQtyPair _hedge[2];
        uint32_t _hedgeId {0};
        bool _marketNeedToSort[2] {false, false};
        bool _forceUpdate[2] {false, false};
        bool _usedInQuoting = false;
        bool _usedInPath = false;
        bool _enabled = false;
        bool _canTrade = true;
        bool _quoteExternally = false;

        Contract() {
            init();
        }

        Contract(const ConfigContract& def) : _def(def) {
            init();
        }

        template<uint32_t Side>
        void updateMarket(MarketIds marketId, hf::price_t price, hf::qty_t qty) {
            TLOG(0, "AS {C[i1]}. MD update Side={i2} marketId={i3} {i4}@{d1}", name(), Side, marketId, qty, ToDollars(price))
            _markets[Side][marketId == MarketIds::MARKET_ID_EXTERNAL].update<Side>(price, qty);
            _marketNeedToSort[Side] = true;
            for (auto* path : _affectedPaths) {
                path->_dirty = true;
            }
        }

        void updatePaths() {
            updatePaths<BID>();
            updatePaths<ASK>();
        }

        template<uint32_t Side>
        void updatePaths(bool force = false) {
            bool sort = false;
            for (auto path : _paths[Side]) {
                sort |= path->calculate<Side>(force);
            }
            if (sort) {
                auto oldBest = *_paths[Side].begin();
                std::sort(_paths[Side].begin(), _paths[Side].end(), PathSorter<Side>());

#ifdef IS_DEV
                for (auto* path : _paths[Side]) {
                    LOG(0, "AS {C[i1]}. Path {i2} Side={i3} {i4}@{d1}", path->_contract->name(), path->_id, Side,
                        path->_priceSize._size, ToDollars(path->_priceSize._price))
                    for (auto& contract : path->_leanContracts) {
                        LOG(0, "LeanContract={C[i1]}", contract._contract->name())
                        for (auto& market : contract._contract->_markets[Side]) {
                            LOG(0, "Market={i1} Price={d1} Size={i2} FakAdjustedSize={i3}", market.getId(),
                                ToDollars(market.getPrice()), market.getUnAdjustedSize(), market.getFakAdjustedSize())
                        }
                    }
                }
#endif

                auto bestPath = *_paths[Side].begin();
                if (oldBest != bestPath) {
                    LOG(0, "AS {{C[i1]}. Lean on path {i2} {i3}@{d1}", bestPath->_contract->name(),
                        bestPath->_id, bestPath->_priceSize._size, ToDollars(bestPath->_priceSize._price))
                }
            }
        }

        template<uint32_t Side>
        Market* getBestMarket() {
            if (_marketNeedToSort[Side]) {
                _marketNeedToSort[Side] = false;
                if (_markets[Side][0].getPrice() == hf::NULL_PRICE) {
                    _bestMarket[Side] = &_markets[Side][1];
                } else if (_markets[Side][1].getPrice() == hf::NULL_PRICE) {
                    _bestMarket[Side] = &_markets[Side][0];
                } else if (IsPriceBetterOrEqual<Side>(_markets[Side][0].getPrice(), _markets[Side][1].getPrice())) {
                    _bestMarket[Side] = &_markets[Side][0];
                } else {
                    _bestMarket[Side] =&_markets[Side][1];
                }
            }
            return _bestMarket[Side];
        }

#ifdef IS_DEV
        std::string name() const noexcept {
            return _def._name;
        }
#else
        int32_t name() const noexcept {
            return _def._internalId;
        }
#endif

    private:
        ALWAYS_INLINE void init() {
            for (auto side : {BID, ASK}) {
//                uint32_t i = 0;
//                static_assert(MarketIds::__end__ == 4, "New market added. Need to change _sortedMarkets[side]");
//                _sortedMarkets[side].reserve(MarketIds::__end__);
//                for (auto& market : _markets[side]) {
//                    market.setId(i++);
//                    market.setContractId(_def._internalId);
//                    _sortedMarkets[side].emplace_back(&market);
//                }
                _markets[side][0].setId(0);
                _markets[side][1].setId(MarketIds::MARKET_ID_EXTERNAL);
            }
        }
    };

    std::vector<Contract> _contracts;
    std::vector<Path> _allPaths;
    OrderRequestManager& _orderRequestMgr;
    std::unordered_set<Contract*> _updatedContracts;
    std::vector<ReadyOrderRequests> _lastOrderRequests;
    int64_t _fillsTransactionNumber {0};
    bool _withinInternalFillsOfCycle {false};
    bool _updateQuotes {false};
    std::unordered_map<uint32_t, Path*> _pathsParameters;

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
    bool canSentQuote() const noexcept {
        return isEnabled() && !_withinInternalFillsOfCycle;
    }
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

    void handleEndOfInternalFills(int64_t tranNumber) {
        LOG(0, "AS End of internal fills transaction {i1}", tranNumber)
        _withinInternalFillsOfCycle = false;
        _updateQuotes = true;
    }

    template<uint32_t Side>
    inline bool updateHedge(Contract& contract, hf::qty_t qty);

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
};

template<uint32_t Side>
inline bool ArbieSpreader::updateHedge(Contract& contract, hf::qty_t qty) {
    auto hedgeId = ++contract._hedgeId;
    contract.template updatePaths<Side>(true);
    auto size = positionSize<Side>(qty);
    auto pBest = contract._paths[Side][0];
    auto& pos = pBest->_position;
    pos->_size += size;
    pos->_priceOffset = ToRhonePrice(pos->_size * pos->_posOffsetFactor);
    if (std::abs(pos->_size) >= pBest->_config->_maxPos) {
        LOG(0, "AS {C[i1]}. Path {i2} Side={i3} invalidated due to reached maxPos. position={i4}", contract.name(),
            pBest->_id, (pBest->_position->_size < 0), pBest->_position->_size);
        sendErrorMessage(boost::str(boost::format("AS %1%. Path %2% %3% side invalidated due to reached maxPos. position=%4%") % contract._def._name % pBest->_name % (pBest->_position->_size > 0 ? "BID" : "ASK") % pBest->_position->_size), ErrorType::Critical);
        // set dirty so the path will be invalidated on next update
        pBest->_dirty = true;
    }
    if (BOOST_LIKELY(pBest->_priceSize.valid())) {
        for (uint32_t i = 0; i < pBest->_leanContracts.size(); ++i) {
            auto& hedgeContract = pBest->_leanContracts[i];
            auto* hedgeMarket = pBest->_bestHedgeMarket[i];
            if (hedgeContract._multiplier > 0) {
                auto internalHedgePrice = applyPriceOffset<1 - Side>(hedgeMarket->getPrice(), hedgeContract._contract->_hedgeOffsetPrice);
                auto externalHedgePrice = hedgeMarket->getPrice();
                sendHedgeOrder(contract._def._internalId, hedgeContract._contract->_def._internalId, 1 - Side,
                               internalHedgePrice, externalHedgePrice, qty, hedgeMarket->getId() == MarketIds::MARKET_ID_EXTERNAL,
                               hedgeId, hedgeMarket->getId(), pBest->_id);
                hedgeMarket->addAdjustment(hedgeId, qty);
            } else {
                auto internalHedgePrice = applyPriceOffset<Side>(hedgeMarket->getPrice(), hedgeContract._contract->_hedgeOffsetPrice);
                auto externalHedgePrice = hedgeMarket->getPrice();
                sendHedgeOrder(contract._def._internalId, hedgeContract._contract->_def._internalId, Side,
                               internalHedgePrice, externalHedgePrice, qty, hedgeMarket->getId() == MarketIds::MARKET_ID_EXTERNAL,
                               hedgeId, hedgeMarket->getId(), pBest->_id);
                hedgeMarket->addAdjustment(hedgeId, qty);
            }
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

        auto oldPriceSize = _priceSize;
        if (!positionMaxedOut<Side>(_position->_size, _config->_maxPos)) {
            _priceSize._price = 0;
            _priceSize._size = std::numeric_limits<hf::qty_t>::max();
            for (uint32_t i = 0; i < _leanContracts.size(); ++i) {
                auto& pathContract = _leanContracts[i];
                _bestHedgeMarket[i] = getBestMarket<Side>(pathContract);
                auto marketPrice = _bestHedgeMarket[i]->getPrice();
                // apply commission to external market
                if (_bestHedgeMarket[i]->getId() == MarketIds::MARKET_ID_EXTERNAL && marketPrice != hf::NULL_PRICE) {
                    // apply commission to external market
                    marketPrice = applyPriceOffset<Side>(marketPrice, -pathContract._contract->_extCommission);
                }
                DLOG(0, "AS {C[i1]}. Update Path {i2} Side={i3} lean={C[i4]} marketId={i5} {i6}@{d1}({d2})", _contract->name(),
                     _id, _side, pathContract._contract->name(), _bestHedgeMarket[i]->getId(),
                     _bestHedgeMarket[i]->getFakAdjustedSize(), ToDollars(_bestHedgeMarket[i]->getPrice()),
                     ToDollars(marketPrice))

                if (marketPrice == hf::NULL_PRICE) {
                    _priceSize.reset();
                    break;
                }

                _priceSize._price += pathContract._multiplier * marketPrice;
                _priceSize._size = std::min<hf::qty_t>(_priceSize._size, _bestHedgeMarket[i]->getFakAdjustedSize());
            }

            if (_priceSize.valid()) {
                _priceSize._price = applyPriceOffset<Side>(_priceSize._price, -_commission);
            }

        } else {
            _priceSize.reset();
        }

        if (oldPriceSize != _priceSize) {
            LOG(0, "AS {C[i1]}. Path {i2} Side={i3} {i4}@{d1}", _contract->name(), _id, _side, _priceSize._size, ToDollars(_priceSize._price))
            return true;
        }
    }
    return false;
}

}