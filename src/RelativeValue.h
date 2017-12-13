#ifndef VIPER_RELATIVE_VALUE_H
#define VIPER_RELATIVE_VALUE_H

#include <boost/format.hpp>
#include <md/md.h>
#include <functional>
#include <limits>
#include <hf/Service.h>
#include <hf/ConfigManager.h>
#include <md/md_event.h>
#include <md/queue/MDQueue.h>
#include "Common.h"
#include "ViperConfig.h"
#include "MathStructures.h"
#include "ClockUtil.h"
#include "UpdateSelector.h"
#include <set>
#include "Logger.h"

namespace Python {

struct Lean {
    uint32_t _instrumentIndex;
    uint32_t _marketIndex;
    hf::price_t _price;
    hf::qty_t _size;
};

struct BidLeanSorter {
    bool operator()(Lean& a, Lean& b) const {
        if (a._size == 0)
            return false;
        if (b._size == 0)
            return true;
        return a._price > b._price;
    }
};

struct AskLeanSorter {
    bool operator()(Lean& a, Lean& b) const {
        if (a._size == 0)
            return false;
        if (b._size == 0)
            return true;
        return a._price < b._price;
    }
};

struct Leans {

    void add(uint32_t leanIndex, uint32_t marketIndex, hf::price_t price, hf::qty_t size) {
        Lean& lean = _leans[_leanIndex];
        lean._instrumentIndex = leanIndex;
        lean._marketIndex = marketIndex;
        lean._price = price;
        lean._size = size;
        ++_leanIndex;
    }

    void sortBids() {
        auto end = _leans.begin() + _leanIndex;
        std::sort(_leans.begin(), end, BidLeanSorter());
    }

    void sortAks() {
        auto end = _leans.begin() + _leanIndex;
        std::sort(_leans.begin(), end, AskLeanSorter());
    }

    uint32_t getNumberOfLeans() {
        return _leanIndex;
    }

    Lean* getLean_FOR_TESTING(uint32_t instrumentIndex) {
        for (auto& lean : _leans) {
            if (lean._instrumentIndex == instrumentIndex) {
                return &lean;
            }
        }
        return nullptr;
    }

    void clear() {
        _leanIndex = 0;
    }

    uint32_t _leanIndex = 0;
    std::array<Lean, 50> _leans;
};

struct ContractDefinition {

    struct Leg {
        int32_t _size = 0;
        uint32_t _index = 0;
    };

    hf::symbol_id_t _sid = 0;
    hf::VENUE _venue = hf::VENUE::UNKNOWN;
    hf::price_t _tickSize = 0;
    double _maxHedgeBuffer = 0;
    uint32_t _internalId;

    uint32_t _numberOfLegs = 0;
    Leg _legs[10];
};

struct Quote {
    Leans _leans;

    hf::price_t _bestPrice;
    hf::qty_t _bestSize;

    hf::price_t _externalPrice;
    hf::qty_t _externalSize;

    void createQuote(hf::price_t tickSize, uint32_t side, bool quoteExternal) {
        if (side == BID) {
            _leans.sortBids();
        } else {
            _leans.sortAks();
        }

        Lean& bestLean = _leans._leans[0];
        _bestPrice = bestLean._price;
        _bestSize = bestLean._size;

        hf::price_t roundedPrice = hf::NULL_PRICE;
        hf::qty_t size = 0;

        if (quoteExternal) {
            for (uint32_t leanIndex = 0; (leanIndex < 3) & (leanIndex < _leans.getNumberOfLeans()); ++leanIndex) {
                Lean& lean = _leans._leans[leanIndex];
                hf::price_t price = RoundToTickSize(lean._price, tickSize, side);
                if (roundedPrice == hf::NULL_PRICE) {
                    roundedPrice = price;
                    size = lean._size;
                } else if (roundedPrice == price) {
                    size += lean._size;
                } else {
                    break;
                }
            }

            _externalPrice = roundedPrice;
            _externalSize = size;
        }
    }
};

struct Contract {
    ContractDefinition _definition;
    Markets _internalMarkets[2];
    Market _externalMarket[2];
    Quote _quotes[2];

    void clearLeans() {
        _quotes[BID]._leans.clear();
        _quotes[ASK]._leans.clear();
    }
};

class RelativeValue : public virtual hf::Service {

public:

    RelativeValue()
    :
        _updateSelector(*this)
    {}

    void onStratBegin() {
    }

    void onStratEnd() {
    }

    void onPostCreate(const libconfig::Setting& config) override;

    void onRun() override {
        _inFromFront->resetR();
        _inFromMM->resetR();
        _outToMM->resetW();
        _outToFront->resetW();

        subscribeToMarketData();

        while (BOOST_LIKELY(!hf::Service::isExitAll())) {
            processMessagesFromFront();
            processMessagesFromMarketMaker();
            processMarketData();
        }
    }

    void subscribeToMarketData() {
        for (uint32_t internalId = 0; internalId < _numberOfAllContracts; ++internalId) {
            Contract& contract = _contracts[internalId];
            subscribeToMarketData(contract);
        }
    }

    void subscribeToMarketData(Contract& contract) {
        for (auto *const queuePtr : _marketDataQueues) {
            auto &si = queuePtr->getSI(contract._definition._sid);
            auto &vi = si.venues[unsigned(contract._definition._venue)];
            vi.level_num = 1;
            vi.level_qty = true;
            vi.trades = false;
        }
    }

    void processMessagesFromFront() {
        while (true) {
            uint64_t idx = _inFromFront->getNext();

            if (BOOST_UNLIKELY(idx == 0))
                return;

            _inFromFront->freeNext();
            auto& message = _inFromFront->pool()->getMessage(idx);

            switch (message._type) {
                case ViperMessageType::GridUpdate: {
                    GridUpdateMessage& gridChange = reinterpret_cast<GridUpdateMessage &> (message._data[0]);
                    bool accepted = processGridChange(*gridChange._gridUpdate);
                    sendGridUpdateReply(accepted, gridChange._gridUpdate->_gridId);
                    break;
                }
                case ViperMessageType::ON: {
                    turnOn();
                    break;
                }
                case ViperMessageType::OFF: {
                    turnOff();
                    break;
                }
                default:
                    break;
            }
        }
    }

    void sendGridUpdateReply(bool accepted, uint32_t id) {
        auto &outMessage = _outToFront->pool()->getNextMessage();
        outMessage._type = accepted ? ViperMessageType::GridUpdateAccepted : ViperMessageType::GridUpdateRejected;
        GridUpdateReply& reply = reinterpret_cast<GridUpdateReply &> (outMessage._data[0]);
        reply._gridId = id;
        const auto outIdx = _outToFront->pool()->append(sizeof(GridUpdateMessage) + sizeof(ViperMessageType));
        mb();
        _outToFront->appendNew(outIdx);
    }

    void processMessagesFromMarketMaker() {
        while (true) {
            uint64_t idx = _inFromMM->getNext();

            if (BOOST_UNLIKELY(idx == 0))
                return;

            _inFromFront->freeNext();
            auto& message = _inFromMM->pool()->getMessage(idx);

            switch (message._type) {
                case ViperMessageType::MarketUpdate: {
                    MarketUpdate& marketUpdate = reinterpret_cast<MarketUpdate &> (message._data[0]);
                    processMarketUpdate(marketUpdate);
                    break;
                }
                default:
                    break;
            }
        }
    }

    bool processGridChange(GridUpdate& update) {
        return _updateSelector.processUpdate(update);
    }

    bool validateRvVectorParametersUpdate(GridUpdate& update) {
        return true;
    }

    bool onRvVectorParametersUpdate(GridUpdate& update) {
        bool valid = validateRvVectorParametersUpdate(update);

        if (!valid) {
            return false;
        }

        for (int i = 0; i < update._update.cells_size(); ++i) {
            const proto::GridCell& gridCell = update._update.cells(i);
            VectorParametersColumns columnType = static_cast<VectorParametersColumns>(gridCell.col());
            switch (columnType) {
                case VectorParametersColumns::Step: {
                    double value = atof(gridCell.value().c_str());
                    _stepSize.setValue(gridCell.row(), value);
                    break;
                }
                case VectorParametersColumns::Range: {
                    double value = atof(gridCell.value().c_str());
                    _vectorsRange.setValue(gridCell.row(), value);
                    break;
                }
                case VectorParametersColumns::MaxPos: {
                    double value = atof(gridCell.value().c_str());
                    _vectorsMaxPosition.setValue(gridCell.row(), value);
                    break;
                }
                case VectorParametersColumns::Width: {
                    double value = atof(gridCell.value().c_str());
                    _vectorsWidth.setValue(gridCell.row(), value);
                    break;
                }
                case VectorParametersColumns::EMA_ON: {
                    bool value = 1 == atoi(gridCell.value().c_str());
                    _useEMAInVector.setValue(gridCell.row(), value);
                    break;
                }
                case VectorParametersColumns::EMA_Time: {
                    uint32_t value = atoi(gridCell.value().c_str());
                    uint64_t periodInCycles = calculatePeriodInCycles(value);
                    _emaPeriodInVector.setValue(gridCell.row(), value);
                    break;
                }
                case VectorParametersColumns::LTF: {
                    double value = atof(gridCell.value().c_str());
                    _longTermFairsInVector.setValue(gridCell.row(), value);
                    break;
                }
                case VectorParametersColumns::SetToMarket: {
                    bool value = 1 == atoi(gridCell.value().c_str());
                    _setToMarket.setValue(gridCell.row(), value);
                    break;
                }
                default:
                    break;
            }
        }

        return true;
    }

    uint64_t calculatePeriodInCycles(uint32_t seconds) {
        return 0;
    }

    bool onRvContractParametersUpdate(GridUpdate& update) {
        bool valid = validateRvContractParametersUpdate(update);

        if (!valid) {
            return false;
        }

        for (int i = 0; i < update._update.cells_size(); ++i) {
            const proto::GridCell& gridCell = update._update.cells(i);
            ContractParameterRVColumns columnType = static_cast<ContractParameterRVColumns>(gridCell.col());

            switch (columnType) {
                case ContractParameterRVColumns::QuoteExternal: {
                    bool value = 1 == atoi(gridCell.value().c_str());
                    _quoteExternal.setValue(gridCell.row(), value);
                    break;
                }
                default:
                    break;
            }
        }

        return true;
    }

    bool validateRvContractParametersUpdate(GridUpdate& update) {
        return true;
    }

    bool onRvLeanMatrixUpdate(GridUpdate& update) {
        bool valid = validateRvLeanMatrixUpdate(update);

        if (!valid) {
            return false;
        }

        for (int i = 0; i < update._update.cells_size(); ++i) {
            const proto::GridCell& gridCell = update._update.cells(i);
            bool value = 1 == atoi(gridCell.value().c_str());
            _leanMatrix.setValue(gridCell.row(), gridCell.col(), value);
        }

        return true;
    }

    bool validateRvLeanMatrixUpdate(GridUpdate& update) {
        return true;
    }

    bool onRvPricingConfigUpdate(GridUpdate& update) {
        bool valid = validateRvPricingConfigUpdate(update);

        if (!valid) {
            return false;
        }

        for (int i = 0; i < update._update.cells_size(); ++i) {
            const proto::GridCell& gridCell = update._update.cells(i);

            PricingConfigsColumns columnType = static_cast<PricingConfigsColumns>(gridCell.col());

            switch (columnType) {
                case PricingConfigsColumns::UseWeighted: {
                    bool value = 1 == atoi(gridCell.value().c_str());
                    _useWeightedMid.setValue(gridCell.row(), value);
                    break;
                }
                case PricingConfigsColumns::Weight: {
                    double value = atof(gridCell.value().c_str());
                    _weights.setValue(gridCell.row(), value);
                    break;
                }
                default:
                    break;
            }
        }

        return true;
    }

    bool validateRvPricingConfigUpdate(GridUpdate& update) {
        return true;
    }

    bool onManualFills(GridUpdate& update) {
        for (int i = 0; i < update._update.cells_size(); ++i) {
            const proto::GridCell& gridCell = update._update.cells(i);
            ManualFillsEntryColumns columnType = static_cast<ManualFillsEntryColumns>(gridCell.col());
            ManualFill& fill = _manualFills._manualFills[gridCell.row()];

            switch (columnType) {
                case ManualFillsEntryColumns::Contract: {
                    fill._contract = atoi(gridCell.value().c_str());
                    break;
                }
                case ManualFillsEntryColumns::Size: {
                    fill._size = atoi(gridCell.value().c_str());
                    break;
                }
                case ManualFillsEntryColumns::Strategy: {
                    fill._size = atoi(gridCell.value().c_str());
                    break;
                }
            }
        }

        bool valid = _manualFills.validate();

        if (!valid) {
            return false;
        }

        for (auto& fill : _manualFills._manualFills) {
            if (fill._strategyId == _strategyId) {
                Contract& contract = _contracts[fill._contract];
                uint32_t size = std::abs(fill._size);
                uint32_t side = fill._size > 0 ? BID : ASK;
                addPosition(contract, size, side);
            }
        }

        calculateShortTermFairs();
        calculateQuotes();

        return true;
    }

    void addMarketDataQueue(md::queue::MDQueue* q) {
        _marketDataQueues.emplace_back(q);
    }

    void processMarketData() {
        for (auto *const qp : _marketDataQueues) {
            const auto idx = qp->getNext();

            if (BOOST_UNLIKELY(!idx))
                continue;

            qp->freeNext();

            auto &batch = qp->getPoolP()->getBatch(idx);
            onMDBatch(batch);

            {
                qp->poolBytesRead(batch.size);
                uint64_t usage;
                if (BOOST_UNLIKELY(qp->poolThresholdExceeded(usage))) {
                    onPoolThresholdExceeded(qp->getQueueID(), usage);
                }
                if (BOOST_UNLIKELY(qp->queueThresholdExceeded(usage))) {
                    onQueueThresholdExceeded(qp->getQueueID(), usage);
                }
            }
        }
    }

    void onPoolThresholdExceeded(const std::string& queueName, uint64_t usage) {
    }

    void onQueueThresholdExceeded(const std::string& queueName, uint64_t usage) {
    }

    void logError(const std::string& message);
    void logInfo(const std::string& message);

    void onMDBatch(const md::MDBatch &batch)  {
        const auto *data = batch.data;
        for (auto i = batch.count; i; --i) {
            const auto &mde = reinterpret_cast<const md::MDEvent &> (*data);
            data += mde.size;
            switch (mde.type) {
                case md::MDEvent::TYPE::UPDATE: {
                    const auto &event = static_cast<const md::MDUpdateEvent &> (mde);
                    onMDUpdate(batch.venue, event);
                    break;
                }
                case md::MDEvent::TYPE::TRADE: {
                    const auto &event = static_cast<const md::MDTradeEvent &> (mde);
                    onMDTrade(batch.venue, event);
                    break;
                }
                case md::MDEvent::TYPE::GAP: {
                    const auto &event = static_cast<const md::MDGapEvent &> (mde);
                    onMDGap(batch.venue, event);
                    break;
                }
                case md::MDEvent::TYPE::STATUS: {
                    const auto &event = static_cast<const md::MDStatusEvent &> (mde);
                    onMDStatus(batch.venue, event);
                    break;
                }
                default:
                    break;
            }
        }
        onEndOfBatch();
    }

    void onMDTrade(hf::VENUE venue, const md::MDTradeEvent &event) {
    }

    void onMDStatus(hf::VENUE venue, const md::MDStatusEvent &event) {
    }

    void onMDGap(hf::VENUE venue, const md::MDGapEvent &event) {
    }

    void onMDUpdate(hf::VENUE venue, const md::MDUpdateEvent &event) {
        auto it = _sidToInternalId.find(event.sid);
        if (it != _sidToInternalId.end()) {
            const uint32_t internalId = it->second;
            Contract& contract = _contracts[internalId];

            const bool bidChanged = event.buy.updatedQtyNum == 1;
            if (bidChanged) {
                Market& bid = contract._externalMarket[BID];
                const md::top_t::level_t& level = event.buy.top.l[0];
                bid._price = level.pr;
                bid._size = level.q;
            }

            const bool askChanged = event.sell.updatedQtyNum == 1;
            if (askChanged) {
                Market& bid = contract._externalMarket[ASK];
                const md::top_t::level_t& level = event.buy.top.l[0];
                bid._price = level.pr;
                bid._size = level.q;
            }

            _hadBookUpdates = true;
        } else {
            logError(boost::str(boost::format("Received book update for unrecognized SID='%1%'") % event.sid));
        }
    }

    void onEndOfBatch() {
        if (_hadBookUpdates) {
            for (uint32_t id : _updatedContracts) {
                Contract& contract = _contracts[id];
                updateMidPoint(contract._externalMarket[BID], contract._externalMarket[ASK], id);
            }
            calculateOverdeterminedPrices();
            calculateVectorMarketPrices();
            calculateLongTermFairsInVector();
            calculateShortTermFairs();
            calculateQuotes();
            _hadBookUpdates = false;
            _updatedContracts.clear();
        }
    }

    void turnOn() {
        if (!_isOn) {
            calculateShortTermFairs();
            calculateQuotes();
        }
    }

    void turnOff() {
        if (_isOn) {
            shutdownSystem();
        }
    }

    void shutdownSystem() {
    }

    void processMarketUpdate(MarketUpdate& update) {
        Contract& contract = _contracts[update._instrumentIndex];
        Market& market = contract._internalMarkets[update._side].getMarket(update._marketId);
        market._price = update._price;
        market._size = update._size;
        calculateQuotes();
    }

    void updateMidPoint(Market& bid, Market& offer, uint32_t instumentIndex) {
        const bool useWightedMid = _useWeightedMid.getValue(instumentIndex);
        double midpoint;
        if (useWightedMid) {
            midpoint = calculateWeightedMidpoint(bid, offer);
        } else {
            midpoint = calculateSimpleAverage(bid, offer);
        }
        _midPoints.setValue(0, instumentIndex, midpoint);
    }

    void calculateOverdeterminedPrices() {
        for (uint32_t outrightIndex = 0; outrightIndex < _numberOfOutrights; ++outrightIndex) {
            double sum = 0;
            for (uint32_t allIndex = 0; allIndex < _numberOfAllContracts; ++allIndex) {
                sum += _ODM.getValue(outrightIndex, allIndex) * _midPoints.getValue(0, allIndex);
            }
            _overdeterminedPrices.setValue(0, outrightIndex, sum);
        }
    }

    void calculateVectorMarketPrices() {
        for (uint32_t vectorIndex = 0; vectorIndex < _numberOfVectors; ++vectorIndex) {
            double sum = 0;
            for (uint32_t outrightIndex = 0; outrightIndex < _numberOfAllContracts; ++outrightIndex) {
                sum += _riskMatrix.getValue(vectorIndex, outrightIndex) * _overdeterminedPrices.getValue(0, outrightIndex);
            }
            _newVectorMarketPrices.setValue(vectorIndex, sum);
        }
    }

    void calculateLongTermFairsInVector() {
        const uint64_t now = clockUtil::GetCycles();
        const uint64_t timeInCycles = now - _lastEmaTime;

        for (uint32_t vectorIndex = 0; vectorIndex < _numberOfVectors; ++vectorIndex) {
            const bool useEma = _useEMAInVector.getValue(vectorIndex);
            const bool useSetToMarket = _setToMarket.getValue(vectorIndex);
            if (useEma) {
                const double oldMarketPrice = _oldVectorMarketPrices.getValue(vectorIndex);
                const uint64_t period = _emaPeriodInVector.getValue(vectorIndex);
                const double alpha = static_cast<double>(now) / period;
                const double mu = std::exp(-alpha);
                const double nu = (1 - mu) / alpha;
                const double lastEma = _longTermFairsInVector.getValue(vectorIndex);
                const double newEma = mu * lastEma * (nu - mu) * +(1 - nu) * _newVectorMarketPrices.getValue(vectorIndex);
                _longTermFairsInVector.setValue(vectorIndex, newEma);
            } else if (useSetToMarket) {
                const double marketPrice = _newVectorMarketPrices.getValue(vectorIndex);
                _longTermFairsInVector.setValue(vectorIndex, marketPrice);
            }
        }

        _oldVectorMarketPrices = _newVectorMarketPrices;
    }

    double calculateWeightedMidpoint(Market& bid, Market& offer) {
        return (offer._price * bid._size + bid._price * offer._size) / (bid._size + offer._size);
    }

    double calculateSimpleAverage(Market& bid, Market& offer) {
        return (bid._price + offer._price) / 2;
    }

    void init(ViperConfig& viperConfig, ViperQueue* inFromFront, ViperQueue* outToFront, ViperQueue* inFromMM, ViperQueue* outToMM) {
        initializeModel(viperConfig);
        _inFromFront = inFromFront;
        _outToFront = outToFront;
        _inFromMM = inFromMM;
        _outToMM = outToMM;
    }

    void initializeModel(ViperConfig& config) {
        initializeContracts(config);
        populatePricingMatrix(config);
        populateVectors(config);
        _riskMatrix = _pricingMatrix.inverse().transpose();
        _fareMoveForOneUnitPosition = _vectorsRange.divide(_vectorsMaxPosition);
        initializeStructures(config);
        calculateHedgeRatiosAndWidth();
        calculateOverdeterminedMatrix(config);
    }

    void calculateOverdeterminedMatrix(ViperConfig& config) {
        Matrix A;
        std::vector<VectorWithContract>& pricingDefinitions = config._pricingDefinitions;
        A.initialize(_numberOfAllContracts, _numberOfOutrights);
        for (uint32_t row = 0; row < _numberOfAllContracts; ++row) {
            for (uint32_t column = 0; column < _numberOfOutrights; ++column) {
                A.setValue(row, column, pricingDefinitions[row]._values[column]);
            }
        }

        Matrix W;
        std::vector<PricingConfig>& pricingConfigs = config._pricingConfigs;
        W.initialize(_numberOfAllContracts, _numberOfAllContracts);
        for (uint32_t row = 0; row < _numberOfAllContracts; ++row) {
            for (uint32_t column = 0; column < _numberOfOutrights; ++column) {
                double value = 0;
                if (row == column) {
                    value = pricingConfigs[column]._weight;
                }
                W.setValue(row, column, value);
            }
        }

        Matrix At = A.transpose();
        Matrix Wt = W.transpose();

        _ODM = At.multiply(Wt).multiply(W).multiply(A).inverse().multiply(At).multiply(Wt).multiply(W);
    }

    void populatePricingMatrix(ViperConfig& config) {
         config.populatePricingMatrix(_pricingMatrix);
    }

    void populateVectors(ViperConfig& config) {
        const uint32_t size = config._vectorConfigs.size();
        _vectorsRange.init(size);
        _vectorsMaxPosition.init(size);
        _vectorsWidth.init(size);
        _stepSize.init(size);
        _fareMoveForOneUnitPosition.init(size);
        _oldVectorMarketPrices.init(size);
        _newVectorMarketPrices.init(size);
        _useEMAInVector.init(size);
        _setToMarket.init(size);
        _emaPeriodInVector.init(size);
        _longTermFairsInVector.init(size);
        _positionsInVector.init(size);

        for (uint32_t index = 0; index < size; ++index) {
            VectorConfig& vc = config._vectorConfigs[index];
            _vectorsRange.setValue(index, vc._range);
            _vectorsMaxPosition.setValue(index, vc._maxPos);
            _vectorsWidth.setValue(index, vc._width);
            _setToMarket.setValue(index, vc._setToMarket);
            _useEMAInVector.setValue(index, vc._useEMA);
            _emaPeriodInVector.setValue(index, vc._EMALength);
            _stepSize.setValue(index, vc._step);
            _longTermFairsInVector.setValue(index, vc._longTermFair);
            _positionsInVector.setValue(index, vc._position);
        }
    }

    void initializeStructures(ViperConfig& config) {
        _hedgeRatios.initialize(_numberOfAllContracts, _numberOfAllContracts);
        _widths.initialize(_numberOfAllContracts, _numberOfAllContracts);
        _quoteSizes.initialize(_numberOfAllContracts, _numberOfAllContracts);
        _maxQuoteSizes.init(_numberOfAllContracts);
        _numberOfVectors = _riskMatrix._numberOfColumns;
        _shortTermFairsInVector.init(_numberOfVectors);
        _possibleHedgeRatios.initialize(_numberOfVectors, 3);
        _hedgeVectorRiskExposures.initialize(_numberOfVectors, _numberOfVectors);
        _hedgeVectorRiskExposuresSums.init(_numberOfVectors);
        _widthWorkingMatrix.initialize(_numberOfOutrights, 3);
        _widthWorkingVector.init(_numberOfVectors);
        _commissions.initialize(_numberOfAllContracts, _numberOfAllContracts);
        _shortTermFairsForContracts.init(_numberOfAllContracts);
        initializeFromRelativeValue(config);
        initializeLeanMatrix(config);
    }

    void initializeLeanMatrix(ViperConfig& config) {
        _leanMatrix.initialize(_numberOfAllContracts, _numberOfAllContracts);
        for (uint32_t row = 0; row < _numberOfAllContracts; ++row) {
            for (uint32_t col = 0; col < _numberOfAllContracts; ++col) {
                bool included = config._leanMatrix[row]._values[col] == 1;
                _leanMatrix.setValue(row, col, included);
            }
        }
    }

    void initializeFromRelativeValue(ViperConfig& config) {
        _commissionsInput.init(_numberOfAllContracts);
        _contractPositions.init(_numberOfAllContracts);
        _quoteExternal.init(_numberOfAllContracts);
        for (uint32_t contractIndex = 0; contractIndex < _numberOfAllContracts; ++contractIndex) {
            RelativeValueConfig& rvConfig = config._relativeValueConfigs[contractIndex];
            _commissionsInput.setValue(contractIndex, rvConfig._commision);
            _contractPositions.setValue(contractIndex, rvConfig._position);
            _quoteExternal.setValue(contractIndex, rvConfig._quoteExternal);
        }
    }

    void initializeContracts(ViperConfig& config) {
        for (uint32_t contractIndex = 0; contractIndex < config._allContracts.size(); ++contractIndex) {
            ConfigContract& configContract = config._allContracts[contractIndex];
            Contract contract;
            ContractDefinition& contractDefinition = contract._definition;
            contractDefinition._internalId = contractIndex;
            contractDefinition._sid = configContract._sid;
            contractDefinition._tickSize = configContract._tickSize;
            contractDefinition._venue = configContract._venue;
            for (uint32_t legIndex = 0; legIndex < configContract._legs.size(); ++legIndex) {
                ContractDefinition::Leg& leg = contractDefinition._legs[legIndex];
                leg._index = configContract._legs[legIndex]._index;
                leg._size = configContract._legs[legIndex]._ratio;
            }
            contractDefinition._numberOfLegs = configContract._legs.size();
            _contracts.push_back(contract);
        }
        _numberOfOutrights = config._numberOfOutrights;
        _numberOfAllContracts = config._allContracts.size();
    }

    void calculateHedgeRatiosAndWidth() {
        for (uint32_t rowIndex = 0; rowIndex < _hedgeRatios._numberOfRows; ++rowIndex) {
            for (uint32_t columnIndex = 0; columnIndex < _hedgeRatios._numberOfColumns; ++columnIndex) {
                double hedgeRatio = 0;
                if (rowIndex != columnIndex) {
                    hedgeRatio = calculateHedgeRatio(rowIndex, columnIndex);
                } else {
                    calculateVectorRiskExposureForContracts(rowIndex, columnIndex);
                }
                _hedgeRatios.setValue(rowIndex, columnIndex, hedgeRatio);
                const double width = calculateWidth(rowIndex, columnIndex, hedgeRatio);
                _widths.setValue(rowIndex, columnIndex, ToRhonePrice(width));
                const double commission = calculateCommission(rowIndex, columnIndex, hedgeRatio);
                _commissions.setValue(rowIndex, columnIndex, ToRhonePrice(commission));
                const double quoteSize = calculateQuoteSize(rowIndex, columnIndex, hedgeRatio);
                _quoteSizes.setValue(rowIndex, columnIndex, quoteSize);
            }
        }
        calculateMaxQuoteSizes();
    }

    void calculateMaxQuoteSizes() {
        for (uint32_t row = 0; row < _numberOfAllContracts; ++row) {
            double maxQuoteSize = std::numeric_limits<double>::min();
            for (uint32_t col = 0; col < _numberOfAllContracts; ++col) {
                const double quoteSize = _quoteSizes.getValue(row, col);
                maxQuoteSize = std::max(maxQuoteSize, quoteSize);
            }
            hf::qty_t finalSize = static_cast<hf::qty_t>(maxQuoteSize);
            _maxQuoteSizes.setValue(row, finalSize);
        }
    }

    enum PossibleHedgeRatiosColumns {ContractOneRisk = 0, ContractTwoRisk = 1, PossibleHedgeRatio = 2};

    void calculateVectorRiskExposureForContracts(uint32_t contractOneIndex, uint32_t contractTwoIndex) {
        ContractDefinition& contractOneDefinition = _contracts[contractOneIndex]._definition;
        ContractDefinition& contractTwoDefinition = _contracts[contractTwoIndex]._definition;
        calculateVectorRiskExposureForContract(PossibleHedgeRatiosColumns::ContractOneRisk, contractOneDefinition);
        calculateVectorRiskExposureForContract(PossibleHedgeRatiosColumns::ContractTwoRisk, contractTwoDefinition);
    }

    double calculateHedgeRatio(uint32_t contractOneIndex, uint32_t contractTwoIndex) {
        calculateVectorRiskExposureForContracts(contractOneIndex, contractTwoIndex);
        calculatePossibleHedgeRatios();
        calculateVectorRisks();
        sumUpVectorRisks();
        return pickBestHedge();
    }

    void calculateVectorRiskExposureForContract(uint32_t tableColumn, ContractDefinition& contractDefinition) {
        for (uint32_t vectorIndex = 0; vectorIndex < _numberOfVectors; ++vectorIndex) {
            double exposure = 0;
            if (contractDefinition._numberOfLegs > 1) {
                for (uint32_t legIndex = 0; legIndex < contractDefinition._numberOfLegs; ++legIndex) {
                    ContractDefinition::Leg& leg = contractDefinition._legs[legIndex];
                    double vectorRik = _riskMatrix.getValue(vectorIndex, leg._index);
                    exposure += vectorRik * leg._size;
                }
            } else {
                exposure = _riskMatrix.getValue(vectorIndex, contractDefinition._internalId);
            }
            _possibleHedgeRatios.setValue(vectorIndex, tableColumn, exposure);
        }
    }

    void calculatePossibleHedgeRatios() {
        for (uint32_t vectorIndex = 0; vectorIndex < _numberOfVectors; ++vectorIndex) {
            const double hedgeValue = _possibleHedgeRatios.getValue(vectorIndex, PossibleHedgeRatiosColumns::ContractTwoRisk);
            double hedgeRatio = 0;
            if (!IsZero(hedgeValue)) {
                hedgeRatio = _possibleHedgeRatios.getValue(vectorIndex, PossibleHedgeRatiosColumns::ContractOneRisk) / hedgeValue;
            }
            _possibleHedgeRatios.setValue(vectorIndex, PossibleHedgeRatiosColumns::PossibleHedgeRatio, hedgeRatio);
        }
    }

    void calculateVectorRisks() {
        for (uint32_t vectorIndexRow = 0; vectorIndexRow < _numberOfVectors; ++vectorIndexRow) {
            for (uint32_t vectorIndexColumn = 0; vectorIndexColumn < _numberOfVectors; ++vectorIndexColumn) {
                double vectorRisk = _possibleHedgeRatios.getValue(vectorIndexRow, PossibleHedgeRatiosColumns::ContractOneRisk)
                                    - _possibleHedgeRatios.getValue(vectorIndexRow, PossibleHedgeRatiosColumns::ContractTwoRisk)
                                      * _possibleHedgeRatios.getValue(vectorIndexColumn, PossibleHedgeRatiosColumns::PossibleHedgeRatio);
                _hedgeVectorRiskExposures.setValue(vectorIndexRow, vectorIndexColumn, vectorRisk);
            }
        }
    }

    void sumUpVectorRisks() {
        for (uint32_t vectorIndex = 0; vectorIndex < _numberOfVectors; ++vectorIndex) {
            double sum = _hedgeVectorRiskExposures.multiplyAbsColumn(vectorIndex, _fareMoveForOneUnitPosition);
            _hedgeVectorRiskExposuresSums.setValue(vectorIndex, sum);
        }
    }

    double pickBestHedge() {
        uint32_t minIndex = 0;
        double min = _hedgeVectorRiskExposuresSums.getValue(0);
        for (uint32_t vectorIndex = 0; vectorIndex < _numberOfVectors; ++vectorIndex) {
            double sum = _hedgeVectorRiskExposuresSums.getValue(vectorIndex);
            if (sum < min) {
                min = sum;
                minIndex = vectorIndex;
            }
        }
        return _possibleHedgeRatios.getValue(minIndex, 2);
    }

    double calculateCommission(uint32_t contractOneIndex, uint32_t contractTwoIndex, double hedge) {
        const double commissionOne = _commissionsInput.getValue(contractOneIndex);
        const double commissionTwo = _commissionsInput.getValue(contractTwoIndex);
        const double normalizedValue = findNormalizer();
        return (commissionOne + std::abs(hedge) * commissionTwo) / normalizedValue;
    }

    enum WidthWorkingMatrixColumns {ContractOneRatio = 0, ContractTwoRatio = 1, Total = 2, Normalized = 3};

    double calculateWidth(uint32_t contractOneIndex, uint32_t contractTwoIndex, double hedge) {
        ContractDefinition& contractDefinitionOne = _contracts[contractOneIndex]._definition;
        setHedgeRatioForWidth(contractDefinitionOne, 1, WidthWorkingMatrixColumns::ContractOneRatio);
        ContractDefinition& contractDefinitionTwo = _contracts[contractTwoIndex]._definition;
        setHedgeRatioForWidth(contractDefinitionTwo, hedge, WidthWorkingMatrixColumns::ContractTwoRatio);
        sumUpRatiosForWidth();
        normalizeRatiosForWidth();
        return calculateFinalWidth();
    }

    void setHedgeRatioForWidth(ContractDefinition& contractDefinition, double multiplier, uint32_t column) {
        for (uint32_t outrightIndex = 0; outrightIndex < _numberOfOutrights; ++outrightIndex) {
            double value = 0;
            if (contractDefinition._numberOfLegs > 1) {
                for (uint32_t legIndex = 0; legIndex < contractDefinition._numberOfLegs; ++legIndex) {
                    ContractDefinition::Leg& leg = contractDefinition._legs[legIndex];
                    if (leg._index == outrightIndex) {
                        value = leg._size;
                    }
                }
            } else {
                if (contractDefinition._internalId == outrightIndex) {
                    value = 1;
                }
            }
            _widthWorkingMatrix.setValue(outrightIndex, column, value * multiplier);
        }
    }

    void sumUpRatiosForWidth() {
        for (uint32_t outrightIndex = 0; outrightIndex < _numberOfOutrights; ++outrightIndex) {
            double value = _widthWorkingMatrix.getValue(outrightIndex, WidthWorkingMatrixColumns::ContractOneRatio) - _widthWorkingMatrix.getValue(outrightIndex, WidthWorkingMatrixColumns::ContractTwoRatio);
            _widthWorkingMatrix.setValue(outrightIndex, WidthWorkingMatrixColumns::Total, value);
        }
    }

    double findNormalizer() {
        double min = std::numeric_limits<double>::max();
        for (uint32_t outrightIndex = 0; outrightIndex < _numberOfOutrights; ++outrightIndex) {
            double value = _widthWorkingMatrix.getValue(outrightIndex, WidthWorkingMatrixColumns::Total);
            if (!IsZero(value)) {
                double absValue = std::abs(value);
                if (absValue < min) {
                    min = absValue;
                }
            }
        }
        return min;
    }

    void normalizeRatiosForWidth() {
        double min = findNormalizer();
        for (uint32_t outrightIndex = 0; outrightIndex < _numberOfOutrights; ++outrightIndex) {
            double value = _widthWorkingMatrix.getValue(outrightIndex, WidthWorkingMatrixColumns::Total);
            value /= min;
            _widthWorkingMatrix.setValue(outrightIndex, WidthWorkingMatrixColumns::Normalized, value);
        }
    }

    double calculateFinalWidth() {
        for (uint32_t vectorIndex = 0; vectorIndex < _numberOfVectors; ++vectorIndex) {
            double value = 0;
            for (uint32_t contractIndex = 0; contractIndex < _numberOfOutrights; ++contractIndex) {
                value += _widthWorkingMatrix.getValue(contractIndex, WidthWorkingMatrixColumns::Normalized) * _riskMatrix.getValue(vectorIndex, contractIndex);
            }
            _widthWorkingVector.setValue(vectorIndex, value);
        }
        double finalValue = 0;
        for (uint32_t vectorIndex = 0; vectorIndex < _numberOfVectors; ++vectorIndex) {
            finalValue += std::abs(_widthWorkingVector.getValue(vectorIndex)) * _vectorsWidth.getValue(vectorIndex);
        }
        return finalValue;
    }

    double calculateQuoteSize(uint32_t contractOneIndex, uint32_t contractTwoIndex, double hedgeRatio) {
        double minQuoteSize = std::numeric_limits<double>::max();
        for (uint32_t vectorIndex = 0; vectorIndex < _numberOfVectors; ++vectorIndex) {
            const double riskExposure = _possibleHedgeRatios.getValue(vectorIndex, PossibleHedgeRatiosColumns::ContractOneRisk)
                                        - _possibleHedgeRatios.getValue(vectorIndex, PossibleHedgeRatiosColumns::ContractTwoRisk) * hedgeRatio;
            if (!IsZero(riskExposure)) {
                const double fairMove = _fareMoveForOneUnitPosition.getValue(vectorIndex);
                const double stepSize = _stepSize.getValue(vectorIndex);
                const double oneLotMove = std::abs(riskExposure) * fairMove;
                const double quoteSize = stepSize / oneLotMove;
                if (quoteSize < minQuoteSize) {
                    minQuoteSize = quoteSize;
                }
            }
        }
        return minQuoteSize;
    }

    void onFill(uint32_t contractIndex, hf::qty_t size, uint32_t side) {
        Contract& contract = _contracts[contractIndex];
        sendHedges(contract, size, side);
        addPosition(contract, size, side);
        calculateShortTermFairs();
        calculateQuotes();
    }

    void sendHedges(Contract& contract, hf::qty_t size, uint32_t side) {
        Quote& quote = contract._quotes[side];
        hf::qty_t needToHedge = size;
        uint32_t leanIndex = 0;

    }

    void addPosition(Contract& contract, hf::qty_t size, uint32_t side) {
        const hf::qty_t positionChange = side == BID ? size : -size;
        ContractDefinition& contractDefinition = contract._definition;
        for (ContractDefinition::Leg& leg : contractDefinition._legs) {
            const hf::qty_t legPositionChange = positionChange * leg._size;
            _contractPositions.add(leg._index, legPositionChange);
            for (uint32_t vectorIndex = 0; vectorIndex < _riskMatrix._numberOfColumns; ++vectorIndex) {
                const double changeInVector = _riskMatrix.getValue(leg._index, vectorIndex) * legPositionChange;
                _positionsInVector.add(vectorIndex, changeInVector);
            }
        }
    }

    void calculateShortTermFairs() {
        for (uint32_t vectorIndex = 0; vectorIndex < _numberOfVectors; ++vectorIndex) {
            const double fair = _longTermFairsInVector.getValue(vectorIndex) - _positionsInVector.getValue(vectorIndex) * _fareMoveForOneUnitPosition.getValue(vectorIndex);
            _shortTermFairsInVector.setValue(vectorIndex, fair);
        }
        for (uint32_t contractIndex = 0; contractIndex < _numberOfOutrights; ++contractIndex) {
            double shortTermFair = 0;
            for (uint32_t vectorIndex = 0; vectorIndex < _numberOfVectors; ++vectorIndex) {
                const double value = _shortTermFairsInVector.getValue(vectorIndex) * _riskMatrix.getValue(vectorIndex, contractIndex);
                shortTermFair += value;
            }
            const hf::price_t rhonePrice = ToRhonePrice(shortTermFair);
            _shortTermFairsForContracts.setValue(contractIndex, rhonePrice);
        }
        for (uint32_t spreadIndex = _numberOfOutrights; spreadIndex < _numberOfAllContracts; ++spreadIndex) {
            ContractDefinition& contractDefinition = _contracts[spreadIndex]._definition;
            hf::price_t shortTermFair = 0;
            for (ContractDefinition::Leg& leg : contractDefinition._legs) {
                hf::price_t value = _shortTermFairsForContracts.getValue(leg._index) * leg._size;
                shortTermFair += value;
            }
            const hf::price_t rhonePrice = shortTermFair;
            _shortTermFairsForContracts.setValue(spreadIndex, rhonePrice);
        }
    }

    void calculateQuotes() {
        for (uint32_t instrumentIndex = 0; instrumentIndex < _numberOfAllContracts; ++instrumentIndex) {
            Contract& contract = _contracts[instrumentIndex];
            for (uint32_t leanIndex = 0; leanIndex < _numberOfAllContracts; ++leanIndex) {
                const bool canLeanOn = _leanMatrix.getValue(instrumentIndex, leanIndex);
                if (canLeanOn) {
                    const hf::price_t fair = _shortTermFairsForContracts.getValue(instrumentIndex);
                    const hf::price_t width = _widths.getValue(instrumentIndex, leanIndex);
                    const hf::price_t leanFair = _shortTermFairsForContracts.getValue(leanIndex);
                    const hf::qty_t maxQuoteSize = static_cast<hf::qty_t>(_quoteSizes.getValue(contract._definition._internalId, leanIndex));

                    Contract& leanContract = _contracts[leanIndex];

                    Markets& leanBids = leanContract._internalMarkets[BID];
                    for (uint32_t leanBidIndex = 0; leanBidIndex < leanBids._numberOfMarkets; ++leanBidIndex) {
                        Market& market = leanBids._markets[leanBidIndex];
                        calculateBidQuote(market, contract, leanIndex, leanFair, fair, width, maxQuoteSize);
                    }
                    Market& externalBid = leanContract._externalMarket[BID];
                    calculateBidQuote(externalBid, contract, leanIndex, leanFair, fair, width, maxQuoteSize);

                    Markets& leanOffers = leanContract._internalMarkets[ASK];
                    for (uint32_t leanBidIndex = 0; leanBidIndex < leanBids._numberOfMarkets; ++leanBidIndex) {
                        Market& market = leanBids._markets[leanBidIndex];
                        calculateAskQuote(market, contract, leanIndex, leanFair, fair, width, maxQuoteSize);
                    }
                    Market& externalAsk = leanContract._externalMarket[ASK];
                    calculateAskQuote(externalAsk, contract, leanIndex, leanFair, fair, width, maxQuoteSize);
                }
            }

            const bool quoteExternal = _quoteExternal.getValue(instrumentIndex);

            Quote& bidQuote =  contract._quotes[BID];
            bidQuote.createQuote(contract._definition._tickSize, BID, quoteExternal);

            Quote& askQuote = contract._quotes[ASK];
            askQuote.createQuote(contract._definition._tickSize, ASK, quoteExternal);
        }
    }

    void calculateBidQuote(Market& market, Contract& contract, uint32_t leanIndex, hf::price_t leanFair, hf::price_t fair, hf::price_t width, hf::qty_t maxQuoteSize) {
        if (market._price != hf::NULL_PRICE) {
            const hf::price_t bidHedgingCost = leanFair - market._price;
            const double hedgeRatio = _hedgeRatios.getValue(contract._definition._internalId, leanIndex);
            const hf::price_t commission = _commissions.getValue(contract._definition._internalId, leanIndex);
            const hf::price_t bidPrice = fair - width - static_cast<hf::price_t>(bidHedgingCost * hedgeRatio) - commission;
            const hf::qty_t size = calculateSize(hedgeRatio, market._size, maxQuoteSize);
            contract._quotes[BID]._leans.add(leanIndex, market._id, bidPrice, size);
        }
    }

    void calculateAskQuote(Market& market, Contract& contract, uint32_t leanIndex, hf::price_t leanFair, hf::price_t fair, hf::price_t width, hf::qty_t maxQuoteSize) {
        if (market._price != hf::NULL_PRICE) {
            const hf::price_t askHedgingCost = market._price - leanFair;
            const double hedgeRatio = _hedgeRatios.getValue(contract._definition._internalId, leanIndex);
            const hf::price_t askPrice = fair + width + static_cast<hf::price_t>(askHedgingCost * hedgeRatio) + _commissions.getValue(contract._definition._internalId, leanIndex);
            const hf::qty_t size = calculateSize(hedgeRatio, market._size, maxQuoteSize);
            contract._quotes[ASK]._leans.add(leanIndex, market._id, askPrice, size);
        }
    }

    hf::qty_t calculateSize(double hedgeRatio, hf::qty_t marketSize, hf::qty_t maxQuoteSize) {
        hf::qty_t size = maxQuoteSize;
        if (!IsZero(hedgeRatio)) {
            const hf::qty_t possibleSize = static_cast<hf::qty_t>(marketSize / hedgeRatio);
            size = std::max(size, possibleSize);
        }
        return size;
    }

    void updateQuote(hf::symbol_id_t sid, hf::price_t price, hf::qty_t size) {
    }

    Matrix _pricingMatrix; // x,y - vectors - square
    Matrix _riskMatrix; // x,y - vectors - square
    uint32_t _numberOfVectors;

    Vector<double> _vectorsRange;
    Vector<double> _vectorsMaxPosition;
    Vector<double> _vectorsWidth;
    Vector<double> _stepSize;
    Vector<double> _fareMoveForOneUnitPosition;
    Vector<bool> _useWeightedMid;
    Vector<double> _weights;

    SimpleMatrix<double> _hedgeRatios; // x,y - outrights, spreads - square
    SimpleMatrix<hf::price_t> _widths; // x,y - outrights, spreads - square
    Matrix _quoteSizes; // x,y - outrights, spreads - square
    Vector<hf::qty_t> _maxQuoteSizes;

    std::vector<Contract> _contracts; // outrights, spreads
    uint32_t _numberOfOutrights;
    uint32_t _numberOfAllContracts;

    Vector<double> _contractPositions; // outrights
    Vector<double> _positionsInVector; // vectors

    SimpleMatrix<bool> _leanMatrix;
    Vector<bool> _quoteExternal;
    SimpleMatrix<hf::price_t> _commissions; // row, col - all contracts, all contracts
    Vector<double> _commissionsInput;

    Vector<hf::price_t> _shortTermFairsForContracts;
    Vector<double> _shortTermFairsInVector;
    Vector<double> _longTermFairsInVector;

    Matrix _ODM;
    Matrix _midPoints;
    Matrix _overdeterminedPrices;
    Vector<double> _oldVectorMarketPrices;
    Vector<double> _newVectorMarketPrices;
    Vector<bool> _useEMAInVector;
    Vector<bool> _setToMarket;
    Vector<uint64_t> _emaPeriodInVector;
    bool _hasOldVectorPrices;
    uint64_t _lastEmaTime;

    ViperQueue* _inFromFront;
    ViperQueue* _inFromMM;
    ViperQueue* _outToFront;
    ViperQueue* _outToMM;

    bool _isOn = false;

    std::vector<md::queue::MDQueue *> _marketDataQueues;
    std::unordered_map<hf::symbol_id_t, uint32_t> _sidToInternalId;
    bool _hadBookUpdates;
    std::set<uint32_t> _updatedContracts;

    UpdateSelector<RelativeValue> _updateSelector;

    uint32_t _strategyId;

    Logger _logger;

    // ********** TEMP working structures TEMP *********** //
    Matrix _possibleHedgeRatios; // row, col - vectors, 3
    Matrix _hedgeVectorRiskExposures; // row, col - vectors, vectors
    Vector<double> _hedgeVectorRiskExposuresSums; // vectors
    Matrix _widthWorkingMatrix; // row, col - outrights, 3
    Vector<double> _widthWorkingVector; // vectors
    ManualFillsCollection _manualFills;
    // *************************************************** //
};

}

#endif //VIPER_RELATIVE_VALUE_H
