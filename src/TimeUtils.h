#ifndef VIPER_TIMEUTILS_H
#define VIPER_TIMEUTILS_H

#include <hf/hf.h>
#include <sys/time.h>
#include <string>

namespace Python {

    class TimeUtils {
    public:

        TimeUtils();

        static constexpr hf::nanos_t ONE_SECOND = 1'000'000'000LL;
        static constexpr hf::nanos_t ONE_MS = ONE_SECOND / 1000;
        static constexpr hf::nanos_t ONE_NS = ONE_MS / 1000;
        static constexpr hf::nanos_t ONE_MINUTE = ONE_SECOND * 60;

        static hf::nanos_t nanos(const time_t tv_sec, const hf::nanos_t tv_nsec) {
            return hf::nanos_t(tv_sec) * ONE_SECOND + tv_nsec;
        }

        static time_t time(const hf::nanos_t nanos) {
            return nanos / ONE_SECOND;
        }

        static struct timespec timespec(const hf::nanos_t nanos) {
            struct timespec tm;
            to_timespec(tm, nanos);
            return tm;
        }

        static void to_timespec(struct timespec &tm, const hf::nanos_t nanos) {
            tm.tv_sec = nanos / ONE_SECOND;
            tm.tv_nsec = nanos % ONE_SECOND;
        }

        void set_midnightEPOCH(time_t secs);

        hf::nanos_t get_midnight() const {
            return m_midnightNanos;
        }

        time_t get_midnightEPOCH() const {
            return m_midnightSecs;
        }

        static std::string asctime(hf::nanos_t nanos);

        static std::string strftime(const char *__restrict format, hf::nanos_t nanos);

        static hf::nanos_t clock_gettime() {
            // http://linuxmogeb.blogspot.com/2013/10/how-does-clockgettime-work.html
            struct timespec ts;
            ::clock_gettime(CLOCK_REALTIME, &ts);
            //
            return TimeUtils::nanos(ts.tv_sec, ts.tv_nsec);
        }

        static constexpr hf::nanos_t to_nanos(const int hh = 0, const int mm = 0, const int ss = 0, const hf::nanos_t nanos = 0) {
            return ((hh * 60L + mm) * 60L + ss) * ONE_SECOND + nanos;
        }

    private:
        time_t m_midnightSecs = 0;
        hf::nanos_t m_midnightNanos = 0;
    };
}

#endif	/* VIPER_TIMEUTILS_H */

