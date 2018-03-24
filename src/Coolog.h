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

inline std::string CreateLoggerText(const std::string& text) {
    return text;
}

#define IS_DEV ok

#ifndef IS_DEV
#define COOLOG(id,text,...)  { \
  om::debug_t& di = getDebugInfo();         \
  di.i[0] = id;         	\
  createLogStatement(di,  __VA_ARGS__);            \
}
#else

#define VALUES_0()
#define VALUES_1(one) % one
#define VALUES_2(one, two) % one % two

#define GET_MACRO(_0, _1, _2, NAME, ...) NAME
#define VALUES(...) GET_MACRO(_0, ##__VA_ARGS__, VALUES_2, VALUES_1, VALUES_0)(__VA_ARGS__)

#define COOLOG(id,text, ...) \
     getLogger().logInfo(boost::str(boost::format(CreateLoggerText(text)) VALUES(__VA_ARGS__)));

#endif

}

#endif //VIPER_COOLOG_H
