#ifndef VIPER_RELATIVE_VALUE_H
#define VIPER_RELATIVE_VALUE_H

#include <md/md.h>
#include <functional>
#include <limits>
#include <hf/Service.h>
#include <hf/ConfigManager.h>
#include "Common.h"
#include "ViperConfig.h"
#include "MathStructures.h"

namespace Python {

struct Market {
    double _price;
    hf::qty_t _size;
    uint32_t _id;
};

struct Markets {

    void update(uint32_t id, hf::price_t price, hf::qty_t size) {
    }

    Market _markets[5];
    uint32_t _numberOfMarkets;
};

struct Lean {
    uint32_t _instrumentIndex;
    uint32_t _marketIndex;
    double _price;
    hf::qty_t _size;
};

struct LeantOn {
    uint32_t _instrumentIndex;
    uint32_t _marketIndex;
};

struct Leans {
    Lean _leans[10];

    void add(uint32_t leanIndex, uint32_t marketIndex, double price, hf::qty_t size) {
    }

    void sort() {
    }

    uint32_t getNumberOfLeans() {
        return 0;
    }

    Lean& getLean(uint32_t index) {
        return _leans[index];
    }
};


struct ContractDefinition {

    struct Leg {
        int32_t _size;
        uint32_t _index;
    };

    hf::symbol_id_t _sid;
    hf::price_t _tickSize;

    uint32_t _numberOfLegs;
    Leg _legs[10];
};

struct Quote {
    Leans _leans;

    hf::price_t _quotedPrice;
    hf::qty_t _quotedSize;

    bool createQuote(hf::price_t tickSize) {
        _leans.sort();

        hf::price_t roundedPrice = hf::NULL_PRICE;
        hf::qty_t size = 0;
        for (uint32_t leanIndex = 0; (leanIndex < 3) & (leanIndex < _leans.getNumberOfLeans()); ++leanIndex) {
            Lean& lean = _leans.getLean(leanIndex);
            hf::price_t price = roundedPrice = RoundToTickSize(lean._price, tickSize, BID);
            if (roundedPrice == hf::NULL_PRICE) {
                roundedPrice = price;
                size = lean._size;
            } else if (roundedPrice == price) {
                size += lean._size;
            }
        }

        if ((_quotedPrice != roundedPrice) | (_quotedSize != size)) {
            _quotedPrice = roundedPrice;
            _quotedSize = size;
            return true;
        }

        return false;
    }
};

struct Contract {
    ContractDefinition _definition;
    Markets _markets[2];
    Quote _quotes[2];
    LeantOn _leantOn[2];
};

class RelativeValue : public virtual hf::Service {

public:

    RelativeValue() {
    }

    void onStratBegin() {
    }

    void onStratEnd() {
    }

    void onPostCreate(const libconfig::Setting& config) override {
    }

    void onRun() override {
        while (BOOST_LIKELY(!hf::Service::isExitAll())) {
            processIncomingMessages();
        }
    }

    void processIncomingMessages() {
        while (true) {
            uint64_t idx = _inQueue->getNext();

            if (BOOST_UNLIKELY(idx == 0))
                return;

            _inQueue->freeNext();
            auto& message = _inQueue->pool()->getMessage(idx);

            switch (message._type) {
                case ViperMessageType::ConfigChange: {
                    ConfigChange& configChange = reinterpret_cast<ConfigChange &> (message._data[0]);
                    processConfigChange(configChange);
                    break;
                }
                default:
                    break;
            }
        }
    }

    void processConfigChange(ConfigChange& configChange) {
    }

    void onConfig(ViperConfig& viperConfig) {
        initializeModel(viperConfig);
    }

    void init(ViperQueue* inQueue, ViperQueue* outQueue) {
        _inQueue = inQueue;
        _outQueue = outQueue;
    }

    void initializeModel(ViperConfig& config) {
        populatePricingMatrix(config);
        populateVectors(config);
        _riskMatrix = _pricingMatrix.inverse();
        _fareMoveForOneUnitPosition = _vectorsRange.divide(_vectorsMaxPosition);
        initializeSructures(config);
        calculateHedgeRatiosAndWidth();
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
        for (uint32_t index = 0; index < size; ++index) {
            ViperConfig::VectorConfig& vc = config._vectorConfigs[index];
            _vectorsRange.setValue(index, vc._range);
            _vectorsMaxPosition.setValue(index, vc._maxPos);
            _vectorsWidth.setValue(index, vc._width);
        }
    }

    void initializeSructures(ViperConfig& config) {
        uint32_t numberOfContracts = config._allContracts.size();
        _hedgeRatios.initialize(numberOfContracts, numberOfContracts);
        _widths.initialize(numberOfContracts, numberOfContracts);
        _quoteSizes.initialize(numberOfContracts, numberOfContracts);
        _numberOfOutrights = config._numberOfOutrights;
        _numberOfVectors = _riskMatrix._numberOfColumns;
        _positionsInVector.init(_numberOfVectors);
        _contractPositions.init(_numberOfOutrights);
        _shortTermFairsInVector.init(_numberOfVectors);
        _longTermFairsInVector.init(_numberOfVectors);
        initializeContracts(config);
    }

    void initializeContracts(ViperConfig& config) {
        for (auto& contract : config._allContracts) {
            ContractDefinition  cd;
            cd._sid = contract._sid;
            for (uint32_t legIndex = 0; legIndex < contract._legs.size(); ++legIndex) {
                ContractDefinition::Leg& leg = cd._legs[legIndex];
                leg._index = contract._legs[legIndex]._index;
                leg._size = contract._legs[legIndex]._ratio;
            }
        }
    }

    void calculateHedgeRatiosAndWidth() {
        for (uint32_t rowIndex = 0; rowIndex < _hedgeRatios._numberOfRows; ++rowIndex) {
            for (uint32_t columnIndex = 0; columnIndex < _hedgeRatios._numberOfColumns; ++columnIndex) {
                const double hedgeRatio = calculateHedgeRatio(rowIndex, columnIndex);
                _hedgeRatios.setValue(rowIndex, columnIndex, hedgeRatio);
                const double width = calculateWidth(rowIndex, columnIndex, hedgeRatio);
                _widths.setValue(rowIndex, columnIndex, width);
                const double quoteSize = calculateQuoteSize(rowIndex, columnIndex);
                _quoteSizes.setValue(rowIndex, columnIndex, quoteSize);
            }
        }
    }

    double calculateHedgeRatio(uint32_t contractOneIndex, uint32_t contractTwoIndex) {
        ContractDefinition& contractOneDefinition = _contracts[contractOneIndex]._definition;
        ContractDefinition& contractTwoDefinition = _contracts[contractTwoIndex]._definition;
        calculateVectorRiskExposureForContract(0, contractOneDefinition);
        calculateVectorRiskExposureForContract(1, contractTwoDefinition);
        calculatePossibleHedgeRatios();
        calculateVectorRisks();
        sumUpVectorRisks();
        return pickBestHedge();
    }

    void calculateVectorRiskExposureForContract(uint32_t contractIndex, ContractDefinition& contractDefinition) {
        for (uint32_t vectorIndex = 0; vectorIndex < _possibleHedgeRatios._numberOfRows; ++vectorIndex) {
            double exposure = 0;
            for (uint32_t legIndex = 0; legIndex < contractDefinition._numberOfLegs; ++legIndex) {
                ContractDefinition::Leg& leg = contractDefinition._legs[legIndex];
                double vectorRik = _riskMatrix.getValue(vectorIndex, leg._index);
                exposure += vectorRik * leg._size;
            }
            _possibleHedgeRatios.setValue(vectorIndex, contractIndex, exposure);
        }
    }

    void calculatePossibleHedgeRatios() {
        for (uint32_t vectorIndex = 0; vectorIndex < _possibleHedgeRatios._numberOfRows; ++vectorIndex) {
            const double hedgeValue = _possibleHedgeRatios.getValue(vectorIndex, 1);
            if (hedgeValue != 0) {
                const double hedgeRatio = _possibleHedgeRatios.getValue(vectorIndex, 0) / hedgeValue;
                _possibleHedgeRatios.setValue(vectorIndex, 2, hedgeRatio);
            }
        }
    }

    void calculateVectorRisks() {
        for (uint32_t vectorIndexRow = 0; vectorIndexRow < _hedgeVectorRiskExposures._numberOfRows; ++vectorIndexRow) {
            for (uint32_t vectorIndexColumn = 0; vectorIndexColumn < _hedgeVectorRiskExposures._numberOfRows; ++vectorIndexColumn) {
                double vectorRisk = _possibleHedgeRatios.getValue(0, vectorIndexRow) - _possibleHedgeRatios.getValue(1, vectorIndexRow) * _possibleHedgeRatios.getValue(2, vectorIndexColumn);
                _hedgeVectorRiskExposures.setValue(vectorIndexRow, vectorIndexColumn, vectorRisk);
            }
        }
    }

    void sumUpVectorRisks() {
        for (uint32_t vectorIndex = 0; vectorIndex < _hedgeVectorRiskExposuresSums._numberOfColumns; ++vectorIndex) {
            double sum = _hedgeVectorRiskExposures.multiplyAbsColumn(vectorIndex, _fareMoveForOneUnitPosition);
            _hedgeVectorRiskExposures.setValue(0, vectorIndex, sum);
        }
    }

    double pickBestHedge() {
        uint32_t index = 0;
        double bestSum = _hedgeVectorRiskExposuresSums.getValue(0, 0);
        for (uint32_t vectorIndex = 0; vectorIndex < _hedgeVectorRiskExposuresSums._numberOfRows; ++vectorIndex) {
            double sum = _hedgeVectorRiskExposuresSums.getValue(0, vectorIndex);
            if (bestSum > sum) {
                bestSum = sum;
                index = vectorIndex;
            }
        }
        return _possibleHedgeRatios.getValue(index, 2);
    }

    double calculateWidth(uint32_t contractOneIndex, uint32_t contractTwoIndex, double hedge) {
        for (uint32_t vectorIndex = 0; vectorIndex < _possibleHedgeRatios._numberOfRows; ++vectorIndex) {
            double riskExposure = _possibleHedgeRatios.getValue(vectorIndex, 0) - hedge * _possibleHedgeRatios.getValue(vectorIndex, 1);
            _possibleHedgeRatios.setValue(vectorIndex, 2, riskExposure);
        }
        return _possibleHedgeRatios.multiplyAbsColumn(2, _vectorsWidth);
    }

    double calculateQuoteSize(uint32_t contractOneIndex, uint32_t contractTwoIndex) {
        double minQuoteSize = 0;
        for (uint32_t vectorIndex = 0; vectorIndex < _possibleHedgeRatios._numberOfRows; ++vectorIndex) {
            const double riskExposure = _possibleHedgeRatios.getValue(vectorIndex, 2);
            const double fairMove = _fareMoveForOneUnitPosition.getValue(vectorIndex);
            const double stepSize = _stepSize.getValue(vectorIndex);
            const double absExposure = riskExposure < 0 ? -riskExposure : riskExposure;
            const double quoteSize = stepSize / absExposure * fairMove;
            if (minQuoteSize > quoteSize) {
                minQuoteSize = quoteSize;
            }
        }
        return minQuoteSize;
    }

    void onFill(uint32_t contractIndex, hf::qty_t size, uint32_t side) {
        addPosition(contractIndex, size, side);
        calculateVectorShortTermFairs();
        calculateQuotes();
    }

    void addPosition(uint32_t contractIndex, hf::qty_t size, uint32_t side) {
        const hf::qty_t positionChange = side == BID ? size : -size;
        ContractDefinition& contractDefinition = _contracts[contractIndex]._definition;
        for (ContractDefinition::Leg& leg : contractDefinition._legs) {
            const hf::qty_t legPositionChange = positionChange * leg._size;
            _contractPositions.add(leg._index, legPositionChange);
            for (uint32_t vectorIndex = 0; vectorIndex < _riskMatrix._numberOfColumns; ++vectorIndex) {
                const double changeInVector = _riskMatrix.getValue(leg._index, vectorIndex) * legPositionChange;
                _positionsInVector.add(vectorIndex, changeInVector);
            }
        }
    }

    void calculateVectorShortTermFairs() {
        for (uint32_t vectorIndex = 0; vectorIndex < _shortTermFairsInVector._size; ++vectorIndex) {
            const double fair = _longTermFairsInVector.getValue(vectorIndex) - _positionsInVector.getValue(vectorIndex) * _fareMoveForOneUnitPosition.getValue(vectorIndex);
            _shortTermFairsInVector.setValue(vectorIndex, fair);
        }
        for (uint32_t contractIndex = 0; contractIndex < _numberOfOutrights; ++contractIndex) {
            double shortTermFair = 0;
            for (uint32_t vectorIndex = 0; vectorIndex < _riskMatrix._numberOfRows; ++vectorIndex) {
                shortTermFair += _shortTermFairsInVector.getValue(vectorIndex) * _riskMatrix.getValue(contractIndex, vectorIndex);
            }
            _shortTermFairs.setValue(contractIndex, shortTermFair);
        }
        for (uint32_t spreadIndex = _numberOfOutrights - 1; spreadIndex < _numberOfAllContracts; ++spreadIndex) {
            ContractDefinition& contractDefinition = _contracts[spreadIndex]._definition;
            double shortTermFair = 0;
            for (ContractDefinition::Leg& leg : contractDefinition._legs) {
                shortTermFair += _shortTermFairs.getValue(leg._index) * leg._size;
            }
            _shortTermFairs.setValue(spreadIndex, shortTermFair);
        }
    }

    void calculateQuotes() {
        for (uint32_t instrumentIndex = 0; instrumentIndex < _numberOfAllContracts; ++instrumentIndex) {
            Contract contract = _contracts[instrumentIndex];
            const bool canTrade = _canTrade.getValue(instrumentIndex);
            if (canTrade) {
                for (uint32_t leanIndex = 0; leanIndex < _numberOfAllContracts; ++leanIndex) {
                    const bool canLeanOn = _leanMatrix.getValue(instrumentIndex, leanIndex);
                    if (canLeanOn) {
                        const double fair = _shortTermFairs.getValue(instrumentIndex);
                        const double width = _widths.getValue(instrumentIndex, leanIndex);
                        const double leanFair = _shortTermFairs.getValue(leanIndex);

                        Contract& leanContract = _contracts[leanIndex];

                        Markets& leanBids = leanContract._markets[BID];
                        for (uint32_t leanBidIndex = 0; leanBidIndex < leanBids._numberOfMarkets; ++leanBidIndex) {
                            Market& market = leanBids._markets[leanBidIndex];
                            const double bidHedgingCost = leanFair - market._price;
                            double hedgeRatio = _hedgeRatios.getValue(instrumentIndex, leanIndex);
                            const double bidPrice = fair - width - bidHedgingCost * hedgeRatio - _commissions.getValue(instrumentIndex);
                            const hf::qty_t availableSize = static_cast<hf::qty_t>(market._size / hedgeRatio);
                            contract._quotes[BID]._leans.add(leanIndex, market._id, bidPrice, availableSize);
                        }

                        Markets& leanOffers = leanContract._markets[ASK];
                        for (uint32_t leanBidIndex = 0; leanBidIndex < leanBids._numberOfMarkets; ++leanBidIndex) {
                            Market& market = leanBids._markets[leanBidIndex];
                            const double askHedgingCost = market._price - leanFair;
                            double hedgeRatio = _hedgeRatios.getValue(instrumentIndex, leanIndex);
                            const double askPrice = fair + width + askHedgingCost * hedgeRatio + _commissions.getValue(instrumentIndex);
                            const hf::qty_t availableSize = static_cast<hf::qty_t>(market._size / hedgeRatio);
                            contract._quotes[ASK]._leans.add(leanIndex, market._id, askPrice, availableSize);
                        }
                    }
                }
            }

            Quote& bidQuote =  contract._quotes[BID];
            const bool bidChanged = bidQuote.createQuote(contract._definition._tickSize);
            if (bidChanged) {
                updateQuote(contract._definition._sid, bidQuote._quotedPrice, bidQuote._quotedSize);
            }

            Quote& askQuote = contract._quotes[ASK];
            const bool askChanged = askQuote.createQuote(contract._definition._tickSize);
            if (askChanged) {
                updateQuote(contract._definition._sid, askQuote._quotedPrice, askQuote._quotedSize);
            }
        }
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

    Matrix _hedgeRatios; // x,y - outrights, spreads - square
    Matrix _widths; // x,y - outrights, spreads - square
    Matrix _quoteSizes; // x,y - outrights, spreads - square

    std::vector<Contract> _contracts; // outrights, spreads
    uint32_t _numberOfOutrights;
    uint32_t _numberOfAllContracts;

    Vector<double> _contractPositions; // outrights
    Vector<double> _positionsInVector; // vectors

    BoolMatrix _leanMatrix;
    Vector<bool> _canTrade;
    Vector<double> _commissions;

    Vector<double> _shortTermFairs;

    Vector<double> _shortTermFairsInVector;
    Vector<double> _longTermFairsInVector;

    // working structures
    Matrix _possibleHedgeRatios;
    Matrix _hedgeVectorRiskExposures;
    Matrix _hedgeVectorRiskExposuresSums;

    ViperQueue* _inQueue;
    ViperQueue* _outQueue;
};

}

#endif //VIPER_RELATIVE_VALUE_H
