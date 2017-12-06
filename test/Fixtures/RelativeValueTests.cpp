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



}

}



