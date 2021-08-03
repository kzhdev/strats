#pragma once

#include <hf/Service.h>
#include "LockFreeQueue.h"
#include "logging/Logger.h"
#include "ClockUtil.h"
#include "Common.h"
#include <set>
#include <fstream>


namespace Python {

enum PerfId : uint8_t {
    AS_Path_Calculation,
    AS_UpdatePaths,
    AS_UpdateHedge,
    AS_UpdateQuotes,
    AS_RunCycle,
    AS_UpdateContractCount,
    AS_PathUpdateCountPerContractQuotes,
    AS_PathUpdateCountPerContractHedge,
    AS_SamePathCalcResult,
    AS_BestPathUnChangedAfterRecalc,
    AS_Run,
    AS_OnMDUpdate,

    RV_RunCycle,
    RV_HedgeProcess,
    RV_CalcAfterMDChange,
    RV_CalculateQuote,

    __PerfId_END__,
};

enum class PerfType {
    Begin,
    End,
    Value,
    Discard,
    Percentage,
};

#ifdef ENABLE_PERF

inline const char* to_string(PerfId id) noexcept {
    assert(std::underlying_type_t<PerfId>(id) <= std::underlying_type_t<PerfId>(PerfId::__PerfId_END__));
    constexpr const char* strPerfId [] = {
            "AS_Path_Calculation",
            "AS_UpdatePaths",
            "AS_UpdateHedge",
            "AS_UpdateQuotes",
            "AS_RunCycle",
            "AS_UpdateContractCount",
            "AS_PathUpdateCountPerContractQuotes",
            "AS_PathUpdateCountPerContractHedge",
            "AS_SamePathCalcResult",
            "AS_BestPathUnChangedAfterRecalc",
            "AS_Run",
            "AS_OnMDUpdate",

            "RV_RunCycle",
            "RV_HedgeProcess",
            "RV_CalcAfterMDChange",
            "RV_CalculateQuote",
    };
    static_assert(sizeof(strPerfId) / sizeof(const char*) == std::underlying_type_t<PerfId>(PerfId::__PerfId_END__),
                  "Update to_string for new PerfId element.");
    return strPerfId[id];
}

#pragma pack(1)
struct PerfPoint {
    PerfId _id;
    PerfType _type;
    uint64_t _value[2] = {0, 0};
};
#pragma pack()

class PerfMonitor : public virtual hf::Service {
    uint64_t _cursor = 0;
    hf::queue::LockFreeQueue<PerfPoint> _perfQueue;
    static PerfMonitor* _instance;
    uint64_t _nextLogTime = 0;
    uint64_t _logInterval = 0;
    double _frequency = 0.;

    struct PerfStatistics {
        double mean = 0.;
        double M2 = 0.;
        double variance = 0.;
        uint64_t min = std::numeric_limits<uint64_t>::max();
        uint64_t max = 0;
        uint64_t beginTime = 0;
        uint32_t n = 0;

        enum Type {
            Latency,
            Value,
            Count,
            Percentage,
        } type = Type::Latency;

        PerfStatistics() {
        }

        void addData(const PerfPoint& point) {
            if (point._type == PerfType::Begin) {
                assert(beginTime == 0);
                beginTime = point._value[0];
                type = Type::Latency;
                return;
            }

            if (point._type == PerfType::Discard) {
                beginTime = 0;
                return;
            }

            if (point._type == PerfType::Percentage) {
                type = Type::Percentage;
                min = point._value[0];
                max = point._value[1];
                ++n;
                return;
            }

            uint64_t value;
            if (point._type == PerfType::End) {
                if (BOOST_UNLIKELY(!beginTime)) {
                    logging::GetLogger().logInfo(boost::str(boost::format("Skip perf end point due to begin = 0. perfId=%1% n=%2%") % to_string(point._id) % n));
                    return;
                }
                value = point._value[0] - beginTime;
                beginTime = 0;
            } else {
                type = Type::Value;
                value = point._value[0];
            }

            ++n;
            double delta = (double)value - mean;
            mean += delta / n;
            M2 += delta * ((double)value - mean);
            variance = M2 / n;
            min = std::min<uint64_t>(value, min);
            max = std::max<uint64_t>(value, max);
        }
    };

    PerfStatistics _statistics[PerfId::__PerfId_END__];

public:
    static PerfMonitor* get() noexcept {
        return _instance;
    }

    void onCreate(const libconfig::Setting& config) override;

    void onRun() override {
        while (BOOST_LIKELY(!hf::Service::isExitAll())) {
            auto* perfPoint = _perfQueue.next(_cursor);
            if (perfPoint) {
                _statistics[perfPoint->_id].addData(*perfPoint);
            } else {
                log();
            }
        }
    }

    void addPerfPoint(PerfId id, PerfType type) {
        addPerfPoint(id, type, clockUtil::GetCycles());
    }

    ALWAYS_INLINE
    void addPerfPoint(PerfId id, PerfType type, uint64_t value1, uint64_t value2 = 0) {
        auto index = _perfQueue.reserve();
        auto& perfPoint = *_perfQueue[index];
        perfPoint._id = id;
        perfPoint._type = type;
        perfPoint._value[0] = value1;
        perfPoint._value[1] = value2;
        _perfQueue.publish(index);
    }

private:
    void log() {
        auto now = clockUtil::GetCycles();
        if (now >= _nextLogTime) {
            _nextLogTime = now + _logInterval;
            std::ofstream log("./perf_log.log");
            if (!log.is_open()) {
                logging::GetLogger().logErrorAndThrow("Failed to open perf_log.log");
                return;
            }

            for (std::underlying_type_t<PerfId> i = 0; i < std::underlying_type_t<PerfId>(PerfId::__PerfId_END__); ++i) {
                auto& stat = _statistics[i];
                if (stat.n < 100) {
                    continue;
                }
                log << to_string(static_cast<PerfId>(i)) << ":" << std::endl;
                switch(stat.type) {
                    case PerfStatistics::Type::Latency:
                        log << std::setw(25) << "num_observations: " << stat.n << std::endl;
                        log << std::setw(25) << "avg_latency(ns): " << stat.mean / _frequency << std::endl;
                        log << std::setw(25) << "min_latency(ns): " << stat.min / _frequency << std::endl;
                        log << std::setw(25) << "max_latency(ns): " << stat.max / _frequency << std::endl;
                        log << std::setw(25) << "std_div: " << sqrt(stat.variance) / _frequency << std::endl;
                        break;

                    case PerfStatistics::Type::Value:
                        log << std::setw(25) << "num_observations: " << stat.n << std::endl;
                        log << std::setw(25) << "avg: " << stat.mean << std::endl;
                        log << std::setw(25) << "min: " << stat.min << std::endl;
                        log << std::setw(25) << "max: " << stat.max << std::endl;
                        log << std::setw(25) << "std_div: " << sqrt(stat.variance) << std::endl;
                        break;

                    case PerfStatistics::Type::Percentage:
                        log << std::setw(25) << stat.min << "/" << stat.max << " = " << (double)stat.min / (double)stat.max << "%" << std::endl;
                        break;

                    case PerfStatistics::Type::Count:
                        log << std::setw(25) << stat.max << std::endl;
                        break;
                }
                log << std::endl;
            }

            log.close();
        }
    }
};

struct ScopedPerfPoint {
    PerfId _id;
    ScopedPerfPoint(PerfId id) : _id(id) {
        PerfMonitor::get()->addPerfPoint(id, PerfType::Begin);
    }

    ~ScopedPerfPoint() {
        PerfMonitor::get()->addPerfPoint(_id, PerfType::End);
    }
};

struct ConditionalScopedPerfPoint {
    PerfId _id;
    bool _enabled;

    template<typename Condition>
    ConditionalScopedPerfPoint(PerfId id, Condition&& condition)
        : _id(id), _enabled(condition()) {
        if (_enabled) {
            PerfMonitor::get()->addPerfPoint(id, PerfType::Begin);
        }
    }

    ~ConditionalScopedPerfPoint() {
        if (_enabled) {
            PerfMonitor::get()->addPerfPoint(_id, PerfType::End);
        }
    }
};

#define SCOPED_PERF_POINT(id) ScopedPerfPoint perf(id)
#define ADD_PERF_POINT(id, type) PerfMonitor::get()->addPerfPoint(id, type)
#define ADD_PERF_VALUE(id, value) PerfMonitor::get()->addPerfPoint(id, PerfType::Value, value)
#define ADD_PERF_PERCENTAGE(id, numerator, denominator) PerfMonitor::get()->addPerfPoint(id, PerfType::Percentage, numerator, denominator)
#define CONDITIONAL_SCOPED_PERF_POINT(id) ConditionalScopedPerfPoint perf(id)

#else   // ENABLE_PERF

class PerfMonitor : public virtual hf::Service {
public:
    void onRun() {}
};

#define SCOPED_PERF_POINT(...) do {} while(0)
#define ADD_PERF_POINT(...) do {} while(0)
#define ADD_PERF_VALUE(...) do {} while(0)
#define ADD_PERF_PERCENTAGE(...) do {} while(0)
#define CONDITIONAL_SCOPED_PERF_POINT(...) do {} while(0)

#endif  // ENABLE_PERF
}