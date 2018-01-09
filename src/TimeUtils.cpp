#include <boost/format.hpp>
#include <hf/log.h>
#include "TimeUtils.h"

LOG_DECLARE();

namespace Python{

TimeUtils::TimeUtils() {
    ::time_t current;
    ::time(&current);
    set_midnightEPOCH(current);
}

std::string TimeUtils::asctime(const hf::nanos_t nanos) {
    return boost::str(boost::format("%1%.%2$09d") % TimeUtils::strftime("%a %Y-%m-%d %T", nanos) % long(nanos % 1000000000LL));
}

std::string TimeUtils::strftime(const char* const __restrict fmt, const hf::nanos_t nanos) {
    const time_t time = TimeUtils::time(nanos);
    struct tm localtm;
    localtime_r(&time, &localtm);
    char buffer[100];
    if (BOOST_UNLIKELY(!::strftime(buffer, sizeof buffer, fmt, &localtm))) return "TSERROR";
    return buffer;
}

void TimeUtils::set_midnightEPOCH(const time_t secs) {
    struct tm midnightTM;
    localtime_r(&(secs), &midnightTM);
    midnightTM.tm_hour = midnightTM.tm_min = midnightTM.tm_sec = 0;
    m_midnightSecs = ::mktime(&midnightTM);
    m_midnightNanos = nanos(m_midnightSecs, 0);
}

}
