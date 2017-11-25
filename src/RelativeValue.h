#ifndef VIPER_RELATIVE_VALUE_H
#define VIPER_RELATIVE_VALUE_H

#include <md/md.h>
#include <functional>
#include <limits>
#include <hf/Service.h>
#include <hf/ConfigManager.h>
#include "Common.h"
#include "ViperConfig.h"

namespace Python {

struct ContractDefinition {

    struct Leg {
        uint32_t _size;
        uint32_t _index;
    };

    uint32_t _numberOfLegs;
    Leg _legs[2];
};

template <typename T>
struct Vector {
    Vector divide(Vector& vector) {
        return Vector();
    }

    T getValue(uint32_t index) {
        return T();
    }

    void add(uint32_t index, T value) {
    }

    void setValue(uint32_t index, T value) {
    }

    uint32_t _size;
};

struct Matrix {
    Matrix inverse() {
        return Matrix();
    }

    void setValue(uint32_t row, uint32_t column, double value) {
    }

    double getValue (uint32_t row, uint32_t column) {
        return 0;
    }

    double multiplyAbsColumn(uint32_t index, Vector<double>& vector) {
        // take abs value of the column value
        return 0;
    }

    uint32_t _numberOfRows;
    uint32_t _numberOfColumns;
};

class RelativeValue : public virtual hf::Service {

public:

    RelativeValue() {
    }

    void onStratBegin()  {
    }

    void onStratEnd()  {
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
        populateVectorsRange(config);
        populateVectorsMaxPosition(config);
        _riskMatrix = _pricingMatrix.inverse();
        _fareMoveForOneUnitPosition = _vectorsRange.divide(_vectorsMaxPosition);
        createEmptyHedgeRatiosMatrix();
        calculateHedgeRatiosAndWidth();
    }

    void populatePricingMatrix(ViperConfig& config) {
    }

    void populateVectorsRange(ViperConfig& config) {
    }

    void populateVectorsMaxPosition(ViperConfig& config) {
    }

    void createEmptyHedgeRatiosMatrix() {
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
        ContractDefinition& contractOneDefinition = _contractDefinitions[contractOneIndex];
        ContractDefinition& contractTwoDefinition = _contractDefinitions[contractTwoIndex];
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
                exposure += vectorIndex * leg._size;
            }
            _possibleHedgeRatios.setValue(vectorIndex, contractIndex, exposure);
        }
    }

    void calculatePossibleHedgeRatios() {
        for (uint32_t vectorIndex = 0; vectorIndex < _possibleHedgeRatios._numberOfRows; ++vectorIndex) {
            double possibleHedgeRatio = 0;
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
        calculateHedgingCosts();
        calculateQuotes();
    }

    void addPosition(uint32_t contractIndex, hf::qty_t size, uint32_t side) {
        const hf::qty_t positionChange = side == BID ? size : -size;
        ContractDefinition& contractDefinition = _contractDefinitions[contractIndex];
        for (ContractDefinition::Leg& leg : contractDefinition._legs) {
            const hf::qty_t legPositionChange = positionChange * leg._size;
            _contractPositions.add(leg._index, legPositionChange);
            for (uint32_t vectorIndex = 0; vectorIndex < _riskMatrix._numberOfColumns; ++vectorIndex) {
                const double changeInVector = _riskMatrix.getValue(leg._index, vectorIndex) * legPositionChange;
                _positionsInVector.add(vectorIndex, changeInVector);
            }
        }
    }

    enum FinalMatrix {MarketBid, MarketAsk, ShortTermFair, BidHedgeCost, AskHedgeCost, BidQuote, AskQuote, BidPath, AskPath};

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
            _finalMatrix.setValue(contractIndex, FinalMatrix::ShortTermFair, shortTermFair);
        }
        for (uint32_t spreadIndex = _numberOfOutrights - 1; spreadIndex < _finalMatrix._numberOfColumns; ++spreadIndex) {
            ContractDefinition& contractDefinition = _contractDefinitions[spreadIndex];
            double shortTermFair = 0;
            for (ContractDefinition::Leg& leg : contractDefinition._legs) {
                shortTermFair += _finalMatrix.getValue(leg._index, FinalMatrix::ShortTermFair) * leg._size;
            }
            _finalMatrix.setValue(spreadIndex, FinalMatrix::ShortTermFair, shortTermFair);
        }
    }

    void calculateHedgingCosts() {
        for (uint32_t contractIndex = 0; contractIndex < _finalMatrix._numberOfRows; ++contractIndex) {
            const double bidHedgingCost = _finalMatrix.getValue(contractIndex, FinalMatrix::MarketBid) - _finalMatrix.getValue(contractIndex, FinalMatrix::ShortTermFair);
            const double askHedgingCost = _finalMatrix.getValue(contractIndex, FinalMatrix::MarketAsk) - _finalMatrix.getValue(contractIndex, FinalMatrix::ShortTermFair);
            _finalMatrix.setValue(contractIndex, FinalMatrix::BidHedgeCost, bidHedgingCost);
            _finalMatrix.setValue(contractIndex, FinalMatrix::AskHedgeCost, askHedgingCost);
        }
    }

    void calculateQuotes() {
        for (uint32_t quoteIndex = 0; quoteIndex < _finalMatrix._numberOfRows; ++quoteIndex) {
            uint32_t bidHedgeIndex = 0;
            double finalBidPrice = std::numeric_limits<double>::min();
            uint32_t askHedgeIndex = 0;
            double finalAskPrice = std::numeric_limits<double>::max();
            for (uint32_t leanIndex = 0; leanIndex < _finalMatrix._numberOfColumns; ++ leanIndex) {
                const double fair = _finalMatrix.getValue(quoteIndex, FinalMatrix::ShortTermFair);
                const double width = _widths.getValue(quoteIndex, leanIndex);
                const double bidPrice = fair - width - _finalMatrix.getValue(leanIndex, FinalMatrix::BidHedgeCost) / _hedgeRatios.getValue(quoteIndex, leanIndex);
                if (bidPrice > finalBidPrice) {
                    finalBidPrice = bidPrice;
                    bidHedgeIndex = leanIndex;
                }
                const double askPrice = fair + width + _finalMatrix.getValue(leanIndex, FinalMatrix::AskHedgeCost) / _hedgeRatios.getValue(quoteIndex, leanIndex);
                if (askPrice < finalAskPrice) {
                    finalAskPrice = askPrice;
                    askHedgeIndex = leanIndex;
                }
            }
            _finalMatrix.setValue(quoteIndex, FinalMatrix::BidQuote, finalBidPrice);
            _finalMatrix.setValue(quoteIndex, FinalMatrix::BidPath, bidHedgeIndex);
            _finalMatrix.setValue(quoteIndex, FinalMatrix::AskQuote, finalAskPrice);
            _finalMatrix.setValue(quoteIndex, FinalMatrix::AskPath, askHedgeIndex);
        }
    }

    Matrix _pricingMatrix;
    Matrix _riskMatrix;
    Vector<double> _vectorsRange;
    Vector<double> _vectorsMaxPosition;
    Vector<double> _vectorsWidth;
    Vector<double> _stepSize;
    Vector<double> _fareMoveForOneUnitPosition;
    Matrix _hedgeRatios;
    Matrix _widths;
    Matrix _quoteSizes;
    std::vector<ContractDefinition> _contractDefinitions;
    Vector<double> _contractPositions;
    Matrix _finalMatrix;
    Vector<double> _positionsInVector;
    Vector<double> _shortTermFairsInVector;
    Vector<double> _longTermFairsInVector;
    uint32_t _numberOfOutrights;

    // working structures
    Matrix _possibleHedgeRatios;
    Matrix _hedgeVectorRiskExposures;
    Matrix _hedgeVectorRiskExposuresSums;

    ViperQueue* _inQueue;
    ViperQueue* _outQueue;
};

}

#endif //VIPER_RELATIVE_VALUE_H
