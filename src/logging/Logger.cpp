#include <hf/log.h>
#include "Logger.h"

LOG_DECLARE();

namespace logging {

    void Logger::logError(const std::string& message) {
        LOG_ERROR(message);
    }

    void Logger::logInfo(const std::string& message) {
        LOG_INFO(message);
    }

    void Logger::logErrorAndThrow(const std::string& message) {
        LOG_ERROR_AND_THROW(message);
    }

    Logger& GetLogger() {
        static Logger logger;
        return logger;
    }

    thread_local hf::queue::DebugQueue* kdbLogger = nullptr;

    hf::queue::DebugQueue* GetKdbLogger() {
        return kdbLogger;
    }

    void setKdbLogger(hf::queue::DebugQueue* logger) {
        kdbLogger = logger;
    }
}