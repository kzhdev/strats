#ifndef VIPER_CLOCKUTIL_H
#define VIPER_CLOCKUTIL_H

#include <iostream>
#include <time.h>
#include <map>

namespace clockUtil {

inline uint64_t GetCycles() {
    uint64_t cycles;
    __asm__ __volatile__(
    "rdtsc\n\t"
            "shl $32, %%rdx\n\t"
            "or %%rdx, %%rax\n\t"
    : "=a"(cycles)::"memory", "%rdx");
    return cycles;
}

inline uint64_t GetCyclesSerialized() {
    uint64_t tsc;
    uint32_t ui;
    asm volatile("":::"memory");
    tsc = __builtin_ia32_rdtscp(&ui);
    asm volatile("":::"memory");
    return tsc;
}

inline __attribute__((always_inline)) uint64_t getCpuTicksStart()
{
    uint32_t lo, hi;
    asm volatile ("CPUID\n\t"
                  "RDTSC\n\t"
                  "mov %%edx, %0\n\t"
                  "mov %%eax, %1\n\t": "=r" (hi), "=r" (lo)::
                  "%rax", "%rbx", "%rcx", "%rdx");
    return (uint64_t)hi << 32 | lo;
}

inline __attribute__((always_inline)) uint64_t getCpuTicksEnd()
{
    uint32_t lo, hi;
    asm volatile("RDTSCP\n\t"
                 "mov %%edx, %0\n\t"
                 "mov %%eax, %1\n\t"
                 "CPUID\n\t": "=r" (hi), "=r" (lo)::
                 "%rax", "%rbx", "%rcx", "%rdx");
    return (uint64_t)hi << 32 | lo;
}

inline uint64_t GetMonotonicClockResolution() {
    timespec ts;
    clock_getres(CLOCK_MONOTONIC, &ts);
    return ts.tv_nsec;
}

inline uint64_t GetMonotonicTime() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_nsec;
}

inline double GetCpuFrequency() {

#ifdef VIPER_UNIT_TEST
    //We don't want to have to wait for this process for every test.  Returning a sample value from corlsd005.
    return 2.5969980000000001;
#endif


    uint64_t resolution = GetMonotonicClockResolution();
    std::cout << "Resolution=" << resolution << std::endl << std::endl;

    const uint64_t increment = 100000000;

    std::map <uint64_t, uint32_t> histogram;
    typedef std::map<uint64_t, uint32_t>::iterator It;

    for (int i = 0; i < 15; ++i) {
        const uint64_t startCycles = GetCyclesSerialized();
        const uint64_t startTime = GetMonotonicTime();

        while (true) {
            const uint64_t timeDelta = GetMonotonicTime() - startTime;
            if (timeDelta > increment) {
                const uint64_t endCycles = GetCyclesSerialized();
                const uint64_t cyclesDelta = endCycles - startCycles;
                const uint64_t frequency = cyclesDelta * 1000000 / timeDelta;

                std::cout << "cycles=" << cyclesDelta << " time=" << timeDelta << " frequency=" << frequency << std::endl;

                if (frequency > 1) {
                    std::pair<It, bool> it = histogram.insert(std::pair<uint64_t, uint32_t>(frequency, 1));
                    if (!it.second) {
                        it.first->second++;
                    }
                } else {
                    std::cout << "Frequency not added." << std::endl;
                }

                break;
            }
        }
    }

    std::pair <uint64_t, uint32_t> chosen(0, 0);
    for (It it = histogram.begin(); it != histogram.end(); ++it) {
        if (it->second > chosen.second) {
            chosen = *it;
        }
    }

    std::cout << std::endl << "Selected frequency = " << chosen.first << " (cycles/millisec) with " << chosen.second << "/15 (" << (chosen.second / 15.0 * 100.0) << "%) " << std::endl << std::endl;

    return double(chosen.first) / 1000000;
}

}

#endif //VIPER_CLOCKUTIL_H
