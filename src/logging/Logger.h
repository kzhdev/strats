 #ifndef VIPER_LOGGER_H
#define VIPER_LOGGER_H

#include <hf/queue/DebugQueue.h>
#include "ViperLoggerQueue.h"

namespace logging {

class Logger {

public:
    void logError(const std::string& message);
    void logErrorAndThrow(const std::string& message);
    void logInfo(const std::string& message);
};

Logger& GetLogger() noexcept;

hf::queue::DebugQueue* GetKdbLogger() noexcept;
void setKdbLogger(hf::queue::DebugQueue* logger) noexcept;

hf::queue::ViperLoggerQueueResource* GetViperLogger() noexcept;
void setViperLogger(hf::queue::ViperLoggerQueueResource* logger) noexcept;

}

#endif //VIPER_LOGGER_H