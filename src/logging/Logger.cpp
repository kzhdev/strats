#include <util/log.h>
#include "Logger.h"

LOG_DECLARE();

namespace logging {

    void Logger::logError(const std::string& message) {
        LOG_ERROR(message);
    }

    void Logger::logInfo(const std::string& message) {
        LOG_INFO(message);
    }

    void Logger::logWarning(const std::string& message) {
        LOG_WARN(message);
    }

    void Logger::logErrorAndThrow(const std::string& message) {
        LOG_ERROR_AND_THROW(message);
    }

    Logger& GetLogger() noexcept {
        static Logger logger;
        return logger;
    }

    thread_local hf::queue::DebugQueue* kdbLogger = nullptr;

    hf::queue::DebugQueue* GetKdbLogger() noexcept {
        return kdbLogger;
    }

    void setKdbLogger(hf::queue::DebugQueue* logger) noexcept {
        kdbLogger = logger;
    }

    Python::ViperLoggerQueueResource* viperLogger = nullptr;

    Python::ViperLoggerQueueResource* GetViperLogger() noexcept {
        return viperLogger;
    }

    void setViperLogger(Python::ViperLoggerQueueResource* logger) noexcept {
        viperLogger = logger;
    }
}