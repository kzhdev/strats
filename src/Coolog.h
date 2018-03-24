#ifndef VIPER_COOLOG_H
#define VIPER_COOLOG_H

#include <type_traits>
#include "om/om.h"
#include <boost/format.hpp>
#include "Logger.h"

namespace Python {

template <typename TypeT>
constexpr void generateLogLine(int& intCount, int& doubleCount, om::debug_t& di, TypeT v) {
    if (std::is_integral<TypeT>::value) {
        di.i[intCount++] = v;
    }
    if (std::is_floating_point<TypeT>::value) {
        di.d[doubleCount++] = v;
    }
}	

template <typename T>
constexpr void generateLogLines(int& intCount, int& doubleCount, om::debug_t& di, T v) {
    generateLogLine(intCount, doubleCount, di, v);
}

template <typename T, typename... Ts>
constexpr void generateLogLines(int& intCount, int& doubleCount, om::debug_t& di, T head, Ts... tail) {
    generateLogLine(intCount, doubleCount, di, head);
    generateLogLines(intCount, doubleCount, di, tail...);
}

template<typename... Ts>
static constexpr void createLogStatement(om::debug_t& di, const Ts... tail) {
    int intCount = 1;
    int doubleCount = 1;
    generateLogLines(intCount, doubleCount, di, tail...);
}

#define IS_DEV ok

#ifndef IS_DEV
#define COOLOG(id,text,...)  { \
  om::debug_t& di = getDebugInfo();         \
  di.i[0] = id;         	\
  createLogStatement(di,  __VA_ARGS__);            \
}
#else
#define COOLOG(id,text,...)  { \
  auto& logger = getLogger();         \
  logger.logInfo(boost::str(boost::format(text))); \
}
#endif

}

#endif //VIPER_COOLOG_H