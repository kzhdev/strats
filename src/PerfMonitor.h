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
    AS_DrainMD,
    AS_DrainBatchCount,
    AS_DrainInternalMessage,
    AS_DrainInternalMessageCount,
    AS_CheckSysState,
    AS_CreateQuote,

    RV_RunCycle,
    RV_HedgeProcess,
    RV_CalcAfterMDChange,
    RV_CalculateQuote,

    FillArbRate,

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
            "AS_DrainMD",
            "AS_DrainBatchCount",
            "AS_DrainInternalMessage",
            "AS_DrainInternalMessageCount",
            "AS_CheckSysState",
            "AS_CreateQuote",

            "RV_RunCycle",
            "RV_HedgeProcess",
            "RV_CalcAfterMDChange",
            "RV_CalculateQuote",

            "FillArbRate",
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
        PerfId _id;
        double mean[2] {0., 0.};    // 0: normal, 1: outliers
        double M2 {0.};
        double variance = 0.;
        uint64_t min[2] {std::numeric_limits<uint64_t>::max(), std::numeric_limits<uint64_t>::max()};
        uint64_t max[2] {0, 0};
        uint64_t beginTime = 0;
        uint32_t n[2] = {0, 0};
        static uint64_t outlierThreshold;

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
                min[0] = point._value[0];
                max[0] = point._value[1];
                ++n[0];
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

                if (value > outlierThreshold) {
                    calc<1>(value);
                    return;
                }
            } else {
                type = Type::Value;
                value = point._value[0];
            }

            calc<0>(value);
        }

    private:
        template<uint32_t index>
        inline void calc(uint64_t value) {
            ++n[index];
            double delta = (double)value - mean[index];
            mean[index] += delta / n[index];
            if (!index) {
                M2 += delta * ((double)value - mean[0]);
                variance = M2 / n[0];
            }
            min[index] = std::min<uint64_t>(value, min[index]);
            max[index] = std::max<uint64_t>(value, max[index]);
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

    void addPerfPointBegin(PerfId id) {
        auto index = _perfQueue.reserve();
        auto& perfPoint = *_perfQueue[index];
        perfPoint._id = id;
        perfPoint._type = PerfType::Begin;
        perfPoint._value[1] = 0;
        perfPoint._value[0] = clockUtil::getCpuTicksStart();
        _perfQueue.publish(index);
    }

    void addPerfPointEnd(PerfId id) {
        auto index = _perfQueue.reserve();
        auto& perfPoint = *_perfQueue[index];
        perfPoint._id = id;
        perfPoint._type = PerfType::End;
        perfPoint._value[1] = 0;
        perfPoint._value[0] = clockUtil::getCpuTicksEnd();
        _perfQueue.publish(index);
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
                if (stat.n[0] < 100) {
                    continue;
                }
                log << to_string(static_cast<PerfId>(i)) << ":" << std::endl;
                switch(stat.type) {
                    case PerfStatistics::Type::Latency:
                        log << std::setw(25) << "num_observations: " << stat.n[0] << std::endl;
                        log << std::setw(25) << "avg_latency(ns): " << stat.mean[0] / _frequency << std::endl;
                        log << std::setw(25) << "min_latency(ns): " << stat.min[0] / _frequency << std::endl;
                        log << std::setw(25) << "max_latency(ns): " << stat.max[0] / _frequency << std::endl;
                        log << std::setw(25) << "std_div: " << sqrt(stat.variance) / _frequency << std::endl;
                        log << std::setw(25) << "num_outliers: " << stat.n[1] << std::endl;
                        log << std::setw(25) << "outlier_avg(ns): " << stat.mean[1] / _frequency << std::endl;
                        log << std::setw(25) << "outlier_min(ns): " << stat.min[1] / _frequency << std::endl;
                        log << std::setw(25) << "outlier_max(ns): " << stat.max[1] / _frequency << std::endl;
                        break;

                    case PerfStatistics::Type::Value:
                        log << std::setw(25) << "num_observations: " << stat.n[0] << std::endl;
                        log << std::setw(25) << "avg: " << stat.mean[0] << std::endl;
                        log << std::setw(25) << "min: " << stat.min[0] << std::endl;
                        log << std::setw(25) << "max: " << stat.max[0] << std::endl;
                        log << std::setw(25) << "std_div: " << sqrt(stat.variance) << std::endl;
                        break;

                    case PerfStatistics::Type::Percentage:
                        log << std::setw(25) << stat.min[0] << "/" << stat.max[0] << " = " << (double)stat.min[0] / (double)stat.max[0] * 100 << "%" << std::endl;
                        break;

                    case PerfStatistics::Type::Count:
                        log << std::setw(25) << stat.max[0] << std::endl;
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
        PerfMonitor::get()->addPerfPointBegin(id);
    }

    ~ScopedPerfPoint() {
        PerfMonitor::get()->addPerfPointEnd(_id);
    }
};

struct ConditionalScopedPerfPoint {
    PerfId _id;
    bool _enabled;

    template<typename Condition>
    ConditionalScopedPerfPoint(PerfId id, Condition&& condition)
        : _id(id), _enabled(condition()) {
        if (_enabled) {
            PerfMonitor::get()->addPerfPointBegin(id);
        }
    }

    ~ConditionalScopedPerfPoint() {
        if (_enabled) {
            PerfMonitor::get()->addPerfPointEnd(_id);
        }
    }
};

#define SCOPED_PERF_POINT(id) ScopedPerfPoint perf(id)
#define ADD_PERF_POINT_BEGIN(id) PerfMonitor::get()->addPerfPointBegin(id)
#define ADD_PERF_POINT_END(id) PerfMonitor::get()->addPerfPointEnd(id)
#define ADD_PERF_VALUE(id, value) PerfMonitor::get()->addPerfPoint(id, PerfType::Value, value)
#define ADD_PERF_PERCENTAGE(id, numerator, denominator) PerfMonitor::get()->addPerfPoint(id, PerfType::Percentage, numerator, denominator)
#define CONDITIONAL_SCOPED_PERF_POINT(id, condition) ConditionalScopedPerfPoint perf(id, condition)

#else   // ENABLE_PERF

class PerfMonitor : public virtual hf::Service {
public:
    void onRun() {}
};

#define SCOPED_PERF_POINT(...) do {} while(0)
#define ADD_PERF_POINT_BEGIN(...) do {} while(0)
#define ADD_PERF_POINT_END(...) do {} while(0)
#define ADD_PERF_VALUE(...) do {} while(0)
#define ADD_PERF_PERCENTAGE(...) do {} while(0)
#define CONDITIONAL_SCOPED_PERF_POINT(...) do {} while(0)

#endif  // ENABLE_PERF
}