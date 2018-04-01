#ifndef VIPER_LOGGER_H
#define VIPER_LOGGER_H

namespace Python {

class Logger {

public:
    void logError(const std::string& message);
    void logErrorAndThrow(const std::string& message);
    void logInfo(const std::string& message);
};

Logger& GetLogger();

}

#endif //VIPER_LOGGER_H