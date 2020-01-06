#include "StackerFixture.h"

namespace Python {

ConfigContract iceContract {"name", 0, 0, Python::ToRhonePrice(0.01), 0, hf::VENUE::ICE, 1};
ConfigContract cmeContract {"name", 0, 0, Python::ToRhonePrice(0.01), 0, hf::VENUE::CME, 1};
StackerConfig config_60_0_0 {"", 60, 0, true, 0, -1, false};
StackerConfig config_60_2_0 {"", 60, 2, true, 0, -1, false};
StackerConfig config_60_5_2 {"", 60, 5, true, 2, -1, false};
StackerConfig config_100_5_0 {"", 100, 5, true, 0, -1, false};

}
