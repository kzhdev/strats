#pragma once

#include <RhoneLogger.h>
#include <cstdint>
#include <string>
#include <sstream>
#include "../LockFreeQueue.h"

namespace logging {

class ViperLogger : public logging::RhoneLogger {
    uint64_t _maxFileSize = 0;
    uint64_t _currentFileSize = 0;
    std::string _logDir;
    std::ofstream _logfile;
    hf::queue::LockFreeQueue<om::debug_t> _logQueue;
    uint_fast64_t  _logQueueCursor = 0;
    bool _logger_disabled = false;

public:
    ~ViperLogger() override = default;

    void onCreate(const libconfig::Setting &config) override;

private:
    void doRunLOG() override;
    void writeViperLog();
    bool needNewFile() const noexcept;
    void createNewFile(hf::nanos_t timestamp);
};

}