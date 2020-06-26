 #ifndef VIPER_LOGGER_H
#define VIPER_LOGGER_H

#include <hf/queue/DebugQueue.h>

namespace logging {

class Logger {

public:
    void logError(const std::string& message);
    void logErrorAndThrow(const std::string& message);
    void logInfo(const std::string& message);
};

Logger& GetLogger();

hf::queue::DebugQueue* GetKdbLogger();
void setKdbLogger(hf::queue::DebugQueue* logger);

}

#endif //VIPER_LOGGER_H