#include "ViperLogger.h"
#include <../utils/LoggerUtils.h>
#include <hf/log.h>
#include "LogMessage.h"

LOG_DECLARE();

using namespace logging;

static hf::ResourceRegistrator<logging::ViperLogger> s_registrator("logging::ViperLogger");


namespace {

constexpr uint32_t headerSize = sizeof(Header);
constexpr uint32_t entrySize = sizeof(LogEntry);
constexpr uint32_t valueSize = sizeof(Value);
}

void ViperLogger::onCreate(const libconfig::Setting &config) {
    RhoneLogger::onCreate(config);

    _logDir = hf::ConfigManager::getString(config, "log_dir", "/home/pythonsvc/logfiles/");
    if (_logDir[_logDir.size() -1] != '/') {
        _logDir.append("/");
    }
    file_mkdir(_logDir.c_str());
    _maxFileSize = hf::ConfigManager::getULong(config, "max_file_size", 1000000000ul);
    LOG_INFO(boost::format("log_dir=%1% max_file_size=%2%") % _logDir % _maxFileSize);

    auto& log_queue_config = config["viper_log_queue"];
    auto queue_size = hf::ConfigManager::getULong(log_queue_config, "log_queue_size", 1<<22);
    LOG_INFO(boost::format("log_queue_size=%1%") % queue_size);

    auto shmname = hf::ConfigManager::getString(log_queue_config, "shmname", "./shm/viper_log_queue.queue");
    if (!_logQueue.createQueue("ViperLoggerQueue", queue_size, true, shmname, hf::mem::DataBlock::ShmExitMode::SHMCTL)) {
        LOG_ERROR_AND_THROW("failed to create log queue");
    }

    _logQueue.clear();
}

bool ViperLogger::doRunLOG() {
    bool quiescent = RhoneLogger::doRunLOG();
    writeViperLog();
    return quiescent;
}

void ViperLogger::writeViperLog() {
    uint64_t end;
    while(!_logger_disabled && _logQueueCursor != (end = _logQueue.end())) {

        if (BOOST_UNLIKELY((end - _logQueueCursor) > _logQueue.capacity())) {
            LOG_ERROR("!!!! ViperLogger is falling behind");
            _logQueueCursor = end - _logQueue.capacity() - 100;
        }

        auto& di = _logQueue[_logQueueCursor++];
        auto ts = hf::util::TimeUtils::clock_gettime();

        uint32_t nInts = di.ch[0];
        uint32_t nDoubles = di.ch[1];

        if ((nInts + nDoubles) == 0) {
            return;
        }

        if (needNewFile()) {
            _logfile.close();
            createNewFile(ts);
        }

        uint8_t nValues = nInts + nDoubles;
        assert(nInts <= 14 && nDoubles <= 14);
        assert(nValues <= 14);
        auto logSize = entrySize + nValues * valueSize;
        auto totalSize = headerSize + logSize;

        _currentFileSize += totalSize;
        Header header;
        header._time = ts;
        header._size = logSize;
        _logfile.write((char*)&header, sizeof(header));

        LogEntry entry;
        entry._templateId = di.i[0];
        entry._numberOfValues = nValues;
        _logfile.write((char*)(&entry), sizeof(entry));

        Value value;
        for (uint8_t i = 1; i <= nDoubles; ++i) {;
            value._value = *reinterpret_cast<const uint64_t *>(&di.d[i]);
            value._type = logging::VALUE_TYPE_DOUBLE;
            value._sequenceNumber = i;
            _logfile.write((char*)(&value), sizeof(value));
        }

        for (uint8_t i = 1; i <= nInts; ++i) {
            value._value = *reinterpret_cast<const uint64_t *>(&di.i[i]);
            value._type = logging::VALUE_TYPE_INT;
            value._sequenceNumber = i;
            _logfile.write((char*)(&value), sizeof(value));
        }

        _logfile.flush();
    }
}

bool ViperLogger::needNewFile() const noexcept {
    return !_logfile.is_open() || _logfile.bad() || _currentFileSize >= _maxFileSize;
}

void ViperLogger::createNewFile(hf::nanos_t timestamp) {
    std::string fileName = _logDir + "log_" + std::to_string(timestamp);
    _logfile.open(fileName, std::ofstream::out|std::ios::binary);
    if (!_logfile.is_open()) {
        LOG_ERROR("Could not open log file. " + fileName + " Stop writing log to file.");
        _logger_disabled = true;
    }
    _currentFileSize = 0;
}