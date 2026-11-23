#include "ViperLoggerQueue.h"
#include <util/ResourceManager.h>
#include <util/ConfigManager.h>

using namespace Python;

LOG_DECLARE();

static rhone::util::ResourceRegistrator<ViperLoggerQueueResource> s_registrator("Python::ViperLoggerQueue");

void ViperLoggerQueueResource::onCreate(const libconfig::Setting &config) {
    auto queue_size = hf::ConfigManager::getULong(config, "size", 1<<22);
    LOG_INFO(boost::format("log_queue_size=%1%") % queue_size);

    auto shmname = hf::ConfigManager::getString(config, "shmname", "./shm/viper_log_queue.queue");
    if (!_logQueue.createQueue("ViperLoggerQueue", queue_size, true, shmname)) {
        LOG_ERROR_AND_THROW("failed to create log queue");
    }

    _logQueue.clear();
}