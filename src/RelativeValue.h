#ifndef VIPER_RELATIVE_VALUE_H
#define VIPER_RELATIVE_VALUE_H

#include <md/md.h>
#include <functional>
#include <limits>

namespace Python {


struct RelativeValueConfig {
};

struct ContractDefinition {

    struct Leg {
        uint32_t _size;
        uint32_t _index;
    };

    uint32_t _numberOfLegs;
    Leg _legs[2];
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

    double multiplyAbsColumn(uint32_t index, Vector& vector) {
        // take abs value of the column value
        return 0;
    }

    uint32_t _numberOfRows;
    uint32_t _numberOfColumns;
};

template <typename T>
struct Vector {
    Vector divide(Vector& vector) {
        return Vector();
    }
};

class RelativeValue {

public:
    void initialize(RelativeValueConfig& config) {
        populatePricingMatrix(config);
        populateVectorsRange(config);
        populateVectorsMaxPosition(config);
        _riskMatrix = _pricingMatrix.inverse();
        _fareMoveForOneUnitPosition = _vectorsRange.divide(_vectorsMaxPosition);
        createEmptyHedgeRatiosMatrix();
        calculateHedgeRatios();
    }

    void populatePricingMatrix(RelativeValueConfig& config) {
    }

    void populateVectorsRange(RelativeValueConfig& config) {
    }

    void populateVectorsMaxPosition(RelativeValueConfig& config) {
    }

    void createEmptyHedgeRatiosMatrix() {
    }

    void calculateHedgeRatios() {
        for (uint32_t rowIndex = 0; rowIndex < _hedgeRatios._numberOfRows; ++rowIndex) {
            for (uint32_t columnIndex = 0; columnIndex < _hedgeRatios._numberOfColumns; ++columnIndex) {
                const double hedgeRatio = calculateHedgeRatio(rowIndex, columnIndex);
                _hedgeRatios.setValue(rowIndex, columnIndex, hedgeRatio);
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
        for (uint32_t vectoIndex = 0; vectoIndex < _hedgeVectorRiskExposuresSums._numberOfRows; ++vectoIndex) {
            double sum = _hedgeVectorRiskExposuresSums.getValue(0, vectoIndex);
            if (bestSum > sum) {
               bestSum = sum;
                index = vectoIndex;
            }
        }
        return _possibleHedgeRatios.getValue(index, 2);
    }

    Matrix _pricingMatrix;
    Matrix _riskMatrix;
    Vector<double> _vectorsRange;
    Vector<hf::qty_t> _vectorsMaxPosition;
    Vector<double> _fareMoveForOneUnitPosition;
    Matrix _hedgeRatios;
    std::vector<ContractDefinition> _contractDefinitions;

    // structures for hedge ratios calculations
    Matrix _possibleHedgeRatios;
    Matrix _hedgeVectorRiskExposures;
    Matrix _hedgeVectorRiskExposuresSums;
 ;

}

#endif //VIPER_RELATIVE_VALUE_H
