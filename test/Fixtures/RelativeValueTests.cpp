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

    config._pricingConfigs.push_back({"G18", 1, true, 100});
    config._pricingConfigs.push_back({"H18", 1, true, 100});
    config._pricingConfigs.push_back({"J18", 1, true, 100});
    config._pricingConfigs.push_back({"K18", 1, true, 100});
    config._pricingConfigs.push_back({"M18", 1, true, 100});
    config._pricingConfigs.push_back({"N18", 1, true, 100});
    config._pricingConfigs.push_back({"Q18", 1, true, 100});
    config._pricingConfigs.push_back({"G18H18", 1, true, 100});
    config._pricingConfigs.push_back({"H18J18", 1, true, 100});
    config._pricingConfigs.push_back({"J18K18", 1, true, 100});
    config._pricingConfigs.push_back({"K18M18", 1, true, 100});
    config._pricingConfigs.push_back({"M18N18", 1, true, 100});
    config._pricingConfigs.push_back({"N18Q18", 1, true, 100});
    config._pricingConfigs.push_back({"G18J18", 1, true, 100});
    config._pricingConfigs.push_back({"H18K18", 1, true, 100});
    config._pricingConfigs.push_back({"J18M18", 1, true, 100});
    config._pricingConfigs.push_back({"K18N18", 1, true, 100});
    config._pricingConfigs.push_back({"M18Q18", 1, true, 100});
    config._pricingConfigs.push_back({"G18K18", 1, true, 100});
    config._pricingConfigs.push_back({"K18Q18", 1, true, 100});
    config._pricingConfigs.push_back({"G18Q18", 1, true, 100});

    config._allContracts.push_back(createContract("G18", 0, 0));
    config._allContracts.push_back(createContract("H18", 1, 1));
    config._allContracts.push_back(createContract("J18", 2, 2));
    config._allContracts.push_back(createContract("K18", 3, 3));
    config._allContracts.push_back(createContract("M18", 4, 4));
    config._allContracts.push_back(createContract("N18", 5, 5));
    config._allContracts.push_back(createContract("Q18", 6, 6));
    config._allContracts.push_back(createContract("G18H18", 7, 7, 0, 1, 1, -1));
    config._allContracts.push_back(createContract("H18J18", 8, 8, 1, 1, 2, -1));
    config._allContracts.push_back(createContract("J18K18", 9, 9, 2, 1, 3, -1));
    config._allContracts.push_back(createContract("K18M18", 10, 10, 3, 1, 4, -1));
    config._allContracts.push_back(createContract("M18N18", 11, 11, 4, 1, 5, -1));
    config._allContracts.push_back(createContract("N18Q18", 12, 12, 5, 1, 6, -1));
    config._allContracts.push_back(createContract("G18J18", 13, 13, 0, 1, 2, -1));
    config._allContracts.push_back(createContract("H18K18", 14, 14, 1, 1, 3, -1));
    config._allContracts.push_back(createContract("J18M18", 15, 15, 2, 1, 4, -1));
    config._allContracts.push_back(createContract("K18N18", 16, 16, 3, 1, 5, -1));
    config._allContracts.push_back(createContract("M18Q18", 17, 17, 4, 1, 6, -1));
    config._allContracts.push_back(createContract("G18K18", 18, 18, 0, 1, 3, -1));
    config._allContracts.push_back(createContract("K18Q18", 19, 19, 3, 1, 6, -1));
    config._allContracts.push_back(createContract("G18Q18", 20, 20, 0, 1, 6, -1));

    config._numberOfOutrights = 7;
    uint32_t numberOfAllContracts = config._allContracts.size();

    for (uint32_t contractIndex = 0; contractIndex < config._allContracts.size(); ++contractIndex) {
        ConfigContract& contract = config._allContracts[contractIndex];
        std::vector<int> values;
        for (uint32_t outRightIndex = 0; outRightIndex < config._numberOfOutrights; ++outRightIndex) {
            int value = 0;
            if (contract._legs.size() > 0) {
                for (uint32_t legIndex = 0; legIndex < contract._legs.size(); ++legIndex) {
                    ConfigLeg& leg = contract._legs[legIndex];
                    if (legIndex == outRightIndex) {
                        value = leg._ratio;
                    }
                }
            } else {
                if (contractIndex == outRightIndex) {
                    value = 1;
                }
            }
            values.push_back(value);
        }
        config._pricingDefinitions.push_back({"c", values});
    }

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

    auto one = hedgeRatios.getValue(7, 8);
    auto two = hedgeRatios.getValue(8, 7);

    ASSERT_NEAR(hedgeRatios.getValue(7, 8), 1.377499, 0.000001);


    std::vector<std::vector<double>> expectedHedgeRatios = {
            {0,1,1,1,1,1,1,1.986978257,2.7370596735,3.3816161625,5.1789552609,6.6760016008,10.8876407751,1.1512350534,1.5126941642,2.0458025724,2.9164774402,4.1384302622,0.8588490727,2.3002960568,0.6253613096},
            {1,0,1,1,1,1,1,0.986978257,1.3595611207,1.6797273015,2.5725073832,3.316125076,5.4081440886,0.5718451938,0.7513903306,1.0161976609,1.4486820932,2.0556544454,0.4266102851,1.1426104864,0.3106314894},
            {1,1,0,1,1,1,1,0.2610246812,0.3595611207,0.4442349974,0.6803472264,0.8770106989,1.4302838759,0.1512350534,0.1987190905,0.2687522938,0.3831308126,0.5436558937,0.1128249917,0.302184507,0.082152251},
            {1,1,1,0,1,1,1,-0.3265577532,-0.4498328324,-0.5557650026,-0.8511557627,-1.0971937288,-1.7893721264,-0.1892042508,-0.2486096694,-0.3362254665,-0.4793199506,-0.6801465911,-0.1411509273,-0.3780511987,-0.1027774629},
            {1,1,1,1,0,1,1,-0.7102216569,-0.9783293046,-1.2087183267,-1.8511557303,-2.3862570724,-3.8916572157,-0.4114952261,-0.5406944701,-0.7312477062,-1.0424600434,-1.4792324916,-0.3069853479,-0.822213364,-0.2235279344},
            {1,1,1,1,1,0,1,-1.007851642,-1.3883141784,-1.715251483,-2.6269127733,-3.3862570724,-5.5225197328,-0.5839390214,-0.7672813187,-1.0376890006,-1.4793199506,-2.0991290269,-0.4356325718,-1.166775303,-0.3172009661},
            {1,1,1,1,1,1,0,-1.1903501555,-1.6397056166,-2.0258436703,-3.1025856365,-3.9994295437,-6.5225197328,-0.6896768095,-0.9062181366,-1.2255903665,-1.7471904196,-2.4792325175,-0.514515508,-1.3780511987,-0.3746386904},
            {0,0,0,0,0,0,0,0,1.3774985529,1.701888861,2.6064478777,3.3598765248,5.4794966865,0.5793898597,0.7613038335,1.0296049115,1.467795347,2.0827758169,0.4322387876,1.1576855704,0.3147298202},
            {0,0,0,0,0,0,0,0.7259535757,0,1.2354923041,1.8921601567,2.4391143771,3.9778602127,0.4206101403,0.5526712402,0.7474453671,1.0655512806,1.5119985517,0.3137852934,0.8404259794,0.2284792384},
            {0,0,0,0,0,0,0,0.5875824344,0.8093939531,0,1.5315029891,1.9742044277,3.2196560023,0.3404393042,0.4473287598,0.6049777604,0.8624507632,1.2238024848,0.253975919,0.6802357057,0.1849297139},
            {0,0,0,0,0,0,0,0.3836639162,0.5284964893,0.6529533452,0,1.2890633853,2.1022851573,0.2222909825,0.2920848102,0.3950222524,0.563140111,0.7990859264,0.1658344259,0.4441621796,0.1207504754},
            {0,0,0,0,0,0,0,0.2976299851,0.4099848738,0.5065331563,0.775757043,0,1.6308625171,0.1724437953,0.2265868486,0.3064412945,0.4368599073,0.6198965353,0.1286472239,0.3445619391,0.0936730317},
            {0,0,0,0,0,0,0,0.1824985135,0.2513914382,0.3105921873,0.4756728632,0.6131724713,0,0.1057377881,0.1389368179,0.1879013658,0.267870469,0.3801034905,0.0788829362,0.2112758957,0.0574377243},
            {0,0,0,0,0,0,0,1.7259535757,2.3774985529,2.9373811651,4.4986080344,5.7989909019,9.4573568992,0,1.3139750737,1.7770502786,2.5333466277,3.5947743685,0.746024081,1.9981115498,0.5432090586},
            {0,0,0,0,0,0,0,1.3135360101,1.8093939531,2.2354923041,3.4236631459,4.4133188048,7.197516215,0.7610494445,0,1.3524231274,1.9280020438,2.7358010365,0.5677612124,1.5206616851,0.4134089523},
            {0,0,0,0,0,0,0,0.9712463381,1.3378904253,1.6529533241,2.5315029567,3.2632677713,5.3219410916,0.5627302795,0.7394135605,0,1.4255908559,2.0228883853,0.4198103396,1.124397871,0.3056801854},
            {0,0,0,0,0,0,0,0.6812938888,0.9384813459,1.1594864804,1.7757570106,2.2890633436,3.7331476064,0.3947347706,0.5186716493,0.7014635341,0,1.4189824358,0.2944816445,0.7887241043,0.2144235031},
            {0,0,0,0,0,0,0,0.4801284862,0.6613762949,0.8171253225,1.2514298738,1.6131724296,2.6308624491,0.2781815762,0.3655236571,0.4943426475,0.704730358,0,0.2075301547,0.5558378204,0.1511107521},
            {0,0,0,0,0,0,0,2.3135360101,3.1868925059,3.9373811651,6.0301110236,7.7731953296,12.6770129015,1.3404393042,1.7613038335,2.3820280389,3.3957973909,4.8185768533,0,2.6783472555,0.7281387725},
            {0,0,0,0,0,0,0,0.8637924023,1.1898727842,1.4700786677,2.2514298738,2.9022358149,4.7331476064,0.5004725588,0.6576084673,0.8893648999,1.267870469,1.7990859264,0.3733645807,0,0.2718612275},
            {0,0,0,0,0,0,0,3.1773284124,4.3767652901,5.4074598328,8.2815408974,10.6754311445,17.4101605079,1.840911863,2.4189123008,3.2713929388,4.6636678598,6.6176627797,1.3733645807,3.6783472555,0}
    };

    for (uint32_t rowIndex = 0; rowIndex < numberOfAllContracts; ++rowIndex) {
        for (uint32_t columnIndex = 0; columnIndex < numberOfAllContracts; ++columnIndex) {
            double expected = expectedHedgeRatios[rowIndex][columnIndex];
            double has = hedgeRatios.getValue(rowIndex, columnIndex);
            ASSERT_NEAR(expected, has, 0.000001);
        }
    }

    Matrix&  widths = _relativeValue->_widths;

    std::vector<std::vector<double>> expectedWidths = {
            {1, 2},
            {1, 2}
    };

    for (uint32_t rowIndex = 0; rowIndex < numberOfAllContracts; ++rowIndex) {
        for (uint32_t columnIndex = 0; columnIndex < numberOfAllContracts; ++columnIndex) {
            ASSERT_NEAR(widths.getValue(rowIndex, columnIndex), expectedWidths[rowIndex][columnIndex], 0.000001);
        }
    }

    Matrix&  quoteSizes = _relativeValue->_quoteSizes;

    std::vector<std::vector<double>> expectedQuoteSizes = {
            {1, 2},
            {1, 2}
    };

    for (uint32_t rowIndex = 0; rowIndex < numberOfAllContracts; ++rowIndex) {
        for (uint32_t columnIndex = 0; columnIndex < numberOfAllContracts; ++columnIndex) {
            ASSERT_NEAR(quoteSizes.getValue(rowIndex, columnIndex), expectedQuoteSizes[rowIndex][columnIndex], 0.000001);
        }
    }


}

}



