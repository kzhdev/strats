#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "TestStrategy.h"
#include "RelativeValueFixture.h"
#include "ViperConfig.h"

using ::testing::_;

namespace Python {

TEST_F(RelativeValueFixture, FirstTest) {
    ViperConfig config;

    std::vector<double> v1{0.1428571429, 0.1428571429, 0.1428571429, 0.1428571429, 0.1428571429, 0.1428571429, 0.1428571429};
    std::vector<double> v2{0.3071081615, 0.1525477584, 0.0403440801, -0.0504728978, -0.1097721465, -0.1557739562, -0.1839809996};
    std::vector<double> v3{0.2281886056, -0.15639488, -0.1946270778, -0.1238039016, -0.0251741407, 0.0931372551, 0.1786741393};
    std::vector<double> v4{0.1085049486, -0.2869014275, 0.0202000397, 0.1924773971, 0.1788176146, -0.0082795158, -0.2048190568};
    std::vector<double> v5{0.0175147535, -0.092317399, 0.0684525148, 0.1649448435, -0.0741582558, -0.3335243452, 0.2490878882};
    std::vector<double> v6{0.0236740345, -0.1759250827, 0.2977044369, -0.0046987862, -0.3182348453, 0.1786215286, -0.0011412858};
    std::vector<double> v7{0.0078346195, -0.083182193, 0.2366925192, -0.3150649342, 0.2153854086, -0.1017528729, 0.0400874526};

    config._pricingMatrix.push_back({"v1", v1});
    config._pricingMatrix.push_back({"v2", v2});
    config._pricingMatrix.push_back({"v3", v3});
    config._pricingMatrix.push_back({"v4", v4});
    config._pricingMatrix.push_back({"v5", v5});
    config._pricingMatrix.push_back({"v6", v6});
    config._pricingMatrix.push_back({"v7", v7});

    config._vectorConfigs.push_back({"v1", 4, 5, 0.4, 0.2, false, 1, false});
    config._vectorConfigs.push_back({"v2", 0.3, 35, 0.03, 0.015, false, 1, false});
    config._vectorConfigs.push_back({"v3", 0.08, 250, 0.0035, 0.00175, false, 1, false});
    config._vectorConfigs.push_back({"v4", 0.07, 450, 0.001, 0.001, false, 1, false});
    config._vectorConfigs.push_back({"v5", 0.05, 850, 0.001, 0.001, false, 1, false});
    config._vectorConfigs.push_back({"v6", 0.03, 1500, 0.0004, 0.0004, false, 1, false});
    config._vectorConfigs.push_back({"v7", 0.025, 2000, 0.0004, 0.0004, false, 1, false});

    _relativeValue->initializeModel(config);

    Matrix& risk =  _relativeValue->_riskMatrix;

    ASSERT_NEAR(risk.getValue(0, 0), 1, 0.000001);
    ASSERT_NEAR(risk.getValue(0, 1), 1, 0.000001);
    ASSERT_NEAR(risk.getValue(0, 2), 1, 0.000001);
    ASSERT_NEAR(risk.getValue(0, 3), 1, 0.000001);
    ASSERT_NEAR(risk.getValue(0, 4), 1, 0.000001);
    ASSERT_NEAR(risk.getValue(0, 5), 1, 0.000001);
    ASSERT_NEAR(risk.getValue(0, 6), 1, 0.000001);

    ASSERT_NEAR(risk.getValue(1, 0), 1.600139583, 0.000001);
    ASSERT_NEAR(risk.getValue(1, 1), 0.7948265048, 0.000001);
    ASSERT_NEAR(risk.getValue(1, 2), 0.2102065904, 0.000001);
    ASSERT_NEAR(risk.getValue(1, 3), -0.2629812284, 0.000001);
    ASSERT_NEAR(risk.getValue(1, 4), -0.5719507939, 0.000001);
    ASSERT_NEAR(risk.getValue(1, 5), -0.8116361092, 0.000001);
    ASSERT_NEAR(risk.getValue(1, 6), -0.9586045468, 0.000001);

    ASSERT_NEAR(risk.getValue(2, 0), 1.3346763962, 0.000001);
    ASSERT_NEAR(risk.getValue(2, 1), -0.9147545048, 0.000001);
    ASSERT_NEAR(risk.getValue(2, 2), -1.1383748373, 0.000001);
    ASSERT_NEAR(risk.getValue(2, 3), -0.7241296941, 0.000001);
    ASSERT_NEAR(risk.getValue(2, 4), -0.1472436857, 0.000001);
    ASSERT_NEAR(risk.getValue(2, 5), 0.5447603124, 0.000001);
    ASSERT_NEAR(risk.getValue(2, 6), 1.0450660134, 0.000001);

    ASSERT_NEAR(risk.getValue(3, 0), 0.5279109059, 0.000001);
    ASSERT_NEAR(risk.getValue(3, 1), -1.3958662199, 0.000001);
    ASSERT_NEAR(risk.getValue(3, 2), 0.0982795846, 0.000001);
    ASSERT_NEAR(risk.getValue(3, 3), 0.9364634364, 0.000001);
    ASSERT_NEAR(risk.getValue(3, 4), 0.8700042725, 0.000001);
    ASSERT_NEAR(risk.getValue(3, 5), -0.0402824639, 0.000001);
    ASSERT_NEAR(risk.getValue(3, 6), -0.9965095157, 0.000001);

    ASSERT_NEAR(risk.getValue(4, 0), 0.0797922195, 0.000001);
    ASSERT_NEAR(risk.getValue(4, 1), -0.4205717282, 0.000001);
    ASSERT_NEAR(risk.getValue(4, 2), 0.3118501253, 0.000001);
    ASSERT_NEAR(risk.getValue(4, 3), 0.7514416422, 0.000001);
    ASSERT_NEAR(risk.getValue(4, 4), -0.3378438535, 0.000001);
    ASSERT_NEAR(risk.getValue(4, 5), -1.5194417498, 0.000001);
    ASSERT_NEAR(risk.getValue(4, 6), 1.1347733445, 0.000001);

    ASSERT_NEAR(risk.getValue(5, 0), 0.0934475021, 0.000001);
    ASSERT_NEAR(risk.getValue(5, 1), -0.6944215405, 0.000001);
    ASSERT_NEAR(risk.getValue(5, 2), 1.1751159667, 0.000001);
    ASSERT_NEAR(risk.getValue(5, 3), -0.0185473173, 0.000001);
    ASSERT_NEAR(risk.getValue(5, 4), -1.2561547681, 0.000001);
    ASSERT_NEAR(risk.getValue(5, 5), 0.7050651056, 0.000001);
    ASSERT_NEAR(risk.getValue(5, 6), -0.0045049485, 0.000001);

    ASSERT_NEAR(risk.getValue(6, 0), 0.0355115956, 0.000001);
    ASSERT_NEAR(risk.getValue(6, 1), -0.3770358449, 0.000001);
    ASSERT_NEAR(risk.getValue(6, 2), 1.0728445689, 0.000001);
    ASSERT_NEAR(risk.getValue(6, 3), -1.4280793692, 0.000001);
    ASSERT_NEAR(risk.getValue(6, 4), 0.9762668743, 0.000001);
    ASSERT_NEAR(risk.getValue(6, 5), -0.4612102545, 0.000001);
    ASSERT_NEAR(risk.getValue(6, 6), 0.1817024298, 0.000001);

    Vector<double>& range =  _relativeValue->_vectorsRange;

    ASSERT_NEAR(range.getValue(0), 4, 0.000001);
    ASSERT_NEAR(range.getValue(1), 0.3, 0.000001);
    ASSERT_NEAR(range.getValue(2), 0.08, 0.000001);
    ASSERT_NEAR(range.getValue(3), 0.07, 0.000001);
    ASSERT_NEAR(range.getValue(4), 0.05, 0.000001);
    ASSERT_NEAR(range.getValue(5), 0.03, 0.000001);
    ASSERT_NEAR(range.getValue(6), 0.025, 0.000001);

    Vector<double>& maxPos =  _relativeValue->_vectorsMaxPosition;

    ASSERT_NEAR(maxPos.getValue(0), 5, 0.000001);
    ASSERT_NEAR(maxPos.getValue(1), 35, 0.000001);
    ASSERT_NEAR(maxPos.getValue(2), 250, 0.000001);
    ASSERT_NEAR(maxPos.getValue(3), 450, 0.000001);
    ASSERT_NEAR(maxPos.getValue(4), 850, 0.000001);
    ASSERT_NEAR(maxPos.getValue(5), 1500, 0.000001);
    ASSERT_NEAR(maxPos.getValue(6), 2000, 0.000001);

    Vector<double>& width =  _relativeValue->_vectorsWidth;

    ASSERT_NEAR(width.getValue(0), 0.4, 0.000001);
    ASSERT_NEAR(width.getValue(1), 0.03, 0.000001);
    ASSERT_NEAR(width.getValue(2), 0.0035, 0.000001);
    ASSERT_NEAR(width.getValue(3), 0.001, 0.000001);
    ASSERT_NEAR(width.getValue(4), 0.001, 0.000001);
    ASSERT_NEAR(width.getValue(5), 0.0004, 0.000001);
    ASSERT_NEAR(width.getValue(6), 0.0004, 0.000001);

    Vector<double>& step =  _relativeValue->_stepSize;

    ASSERT_NEAR(step.getValue(0), 0.2, 0.000001);
    ASSERT_NEAR(step.getValue(1), 0.015, 0.000001);
    ASSERT_NEAR(step.getValue(2), 0.00175, 0.000001);
    ASSERT_NEAR(step.getValue(3), 0.001, 0.000001);
    ASSERT_NEAR(step.getValue(4), 0.001, 0.000001);
    ASSERT_NEAR(step.getValue(5), 0.0004, 0.000001);
    ASSERT_NEAR(step.getValue(6), 0.0004, 0.000001);

    Vector<double>& fareMove =  _relativeValue->_fareMoveForOneUnitPosition;

    ASSERT_NEAR(fareMove.getValue(0), 0.8, 0.000001);
    ASSERT_NEAR(fareMove.getValue(1), 0.0085714286, 0.000001);
    ASSERT_NEAR(fareMove.getValue(2), 0.00032, 0.000001);
    ASSERT_NEAR(fareMove.getValue(3), 0.0001555556, 0.000001);
    ASSERT_NEAR(fareMove.getValue(4), 0.00005882352, 0.000001);
    ASSERT_NEAR(fareMove.getValue(5), 0.00002, 0.000001);
    ASSERT_NEAR(fareMove.getValue(6), 0.0000125, 0.000001);

    Matrix& hedgeRatios = _relativeValue->_hedgeRatios;

 //   ASSERT_NEAR(hedgeRatios.getValue(0, 1), 1, 0.000001);
 //   ASSERT_NEAR(hedgeRatios.getValue(0, 7), 3, 0.000001);
 //   ASSERT_NEAR(hedgeRatios.getValue(0, 7), 3, 0.000001);

    Matrix&  widths = _relativeValue->_widths;

    ASSERT_NEAR(widths.getValue(0, 0), 0.444406493, 0.000001);

    Matrix&  quoteSizes = _relativeValue->_quoteSizes;

    ASSERT_NEAR(quoteSizes.getValue(0, 0), 0.25, 0.000001);

}

}



