#include "PerfMonitor.h"
#include <util/ResourceManager.h>

#ifdef ENABLE_PERF
#include <hf/ConfigManager.h>
#endif

using namespace Python;

static rhone::util::ResourceRegistrator<PerfMonitor> s_registrator("PerfMonitor");

#ifdef ENABLE_PERF

PerfMonitor* PerfMonitor::_instance = nullptr;

void PerfMonitor::onCreate(const libconfig::Setting& config) {
    if (!_instance) {
        _instance = this;
    }

    auto queue_size = hf::ConfigManager::getULong(config, "size", 1<<22);
    logging::GetLogger().logInfo(boost::str(boost::format("perf_queue_size=%1%") % queue_size));

    auto shmname = hf::ConfigManager::getString(config, "shmname", "./shm/perf_queue.queue");
    if (!_perfQueue.createQueue("PerfQueue", queue_size, true, shmname)) {
        logging::GetLogger().logErrorAndThrow("failed to create log queue");
    }

    _perfQueue.clear();

    _frequency = clockUtil::GetCpuFrequency();
    _logInterval = GetCyclesFromSeconds(60, _frequency);
}

#endif