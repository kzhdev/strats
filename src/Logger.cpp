#include <hf/log.h>
#include "Logger.h"

LOG_DECLARE();

namespace Python {

    void Logger::logError(const std::string& message) {
        LOG_ERROR(message);
    }

    void Logger::logInfo(const std::string& message) {
        //_LOG_INFO(message);
    }

    void Logger::logErrorAndThrow(const std::string& message) {
        LOG_ERROR_AND_THROW(message);
    }

}