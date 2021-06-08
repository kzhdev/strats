#include "MathStructures.h"
#include <util/log.h>

LOG_DECLARE();

namespace Python {

void Matrix::log(const std::string& message) {
    LOG_INFO(message);
}

}
