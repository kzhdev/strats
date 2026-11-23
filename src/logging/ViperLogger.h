#pragma once

#include <RhoneLogger.h>
#include <cstdint>
#include <string>
#include <fstream>
#include "../LockFreeQueue.h"
#include "ViperLoggerQueue.h"

namespace logging {

class ViperLogger : public logging::RhoneLogger {
    uint64_t _maxFileSize = 0;
    uint64_t _currentFileSize = 0;
    std::string _logDir;
    std::ofstream _logfile;
    std::weak_ptr<Python::ViperLoggerQueueResource> _logQueue;

public:
    ~ViperLogger() override = default;

    void onCreate(const libconfig::Setting &config) override;

private:
    bool doRunLOG() override;
    bool doLogin(ConnectionInfo& ci, int fd, std::string path, const std::string& loggerName, std::vector<std::string>& response) override;
    void writeViperLog();
    bool needNewFile() const noexcept;
    void createNewFile(hf::nanos_t timestamp);
};

}