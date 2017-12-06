#ifndef RELATIVE_VALUE_FIXTURE_H
#define RELATIVE_VALUE_FIXTURE_H

#include "RelativeValue.h"

namespace Python {

class RelativeValueFixture : public ::testing::Test {
protected:
    RelativeValueFixture() {}

    void SetUp() override {
        _relativeValue = new RelativeValue();
    }

    void TearDown() override{
    }

    ConfigContract createContract(const std::string& name, uint32_t gid, uint32_t sid) {
        ConfigContract contract;
        contract._name = name;
        contract._sid = sid;
        contract._gid = gid;
        return contract;
    }

    ConfigContract createContract(const std::string& name, uint32_t gid, uint32_t sid, uint32_t legOne, int32_t legOneRatio, uint32_t legTwo, int32_t legTwoRatio) {
        ConfigContract contract;
        contract._name = name;
        contract._sid = sid;
        contract._gid = gid;
        ConfigLeg one;
        one._index = legOne;
        one._ratio = legOneRatio;
        contract._legs.push_back(one);
        ConfigLeg two;
        two._index = legTwo;
        two._ratio = legTwoRatio;
        contract._legs.push_back(two);
        return contract;
    }

    RelativeValue* _relativeValue;
};

}

#endif //RELATIVE_VALUE_FIXTURE_H
