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
    std::string copy;
    std::size_t startIndex = 0;
    std::uint32_t variableCount = 0;
    uint32_t loopCount = 0;
    while (true) {
        std::size_t tokenIndex = text.find("{", startIndex);
        if (tokenIndex == std::string::npos) {
            copy.append(text.substr(startIndex));
            break;
        } else {
            std::string part = text.substr(startIndex, tokenIndex - startIndex);
            copy.append(part);
            copy.append("%").append(std::to_string(++variableCount)).append("%");
            tokenIndex = text.find("}", startIndex);
            if (tokenIndex == text.size() - 1) {
                break;
            }
            startIndex = tokenIndex + 1;
        }
        ++loopCount;
        if (loopCount == 20) {
            std::cout << "ERROR: " << text << std::endl;
        }
    }
    return copy;
}

#define IS_DEV ok

#ifndef IS_DEV
#define LOG(id,text,...)  {                 \
  om::debug_t& di = getDebugInfo();         \
  di.i[0] = id;                          	\
  createLogStatement(di,  __VA_ARGS__);     \
}
#else

#define VALUES_0()
#define VALUES_1(one) % one
#define VALUES_2(one, two) % one % two
#define VALUES_3(one, two, three) % one % two % three
#define VALUES_4(one, two, three, four) % one % two % three % four
#define VALUES_5(one, two, three, four, five) % one % two % three % four % five
#define VALUES_6(one, two, three, four, five, six) % one % two % three % four % five % six
#define VALUES_7(one, two, three, four, five, six, seven) % one % two % three % four % five % six % seven
#define VALUES_8(one, two, three, four, five, six, seven, eight) % one % two % three % four % five % six % seven % eight
#define VALUES_9(one, two, three, four, five, six, seven, eight, nine) % one % two % three % four % five % six % seven % eight % nine

#define GET_MACRO(_0, _1, _2, _3, _4, _5, _6, _7, _8, _9, NAME, ...) NAME
#define VALUES(...) GET_MACRO(_0, ##__VA_ARGS__, VALUES_9, VALUES_8, VALUES_7, VALUES_6, VALUES_5, VALUES_4, VALUES_3, VALUES_2, VALUES_1, VALUES_0)(__VA_ARGS__)

#define LOG(id,text, ...) \
     try { \
         getLogger().logInfo(boost::str(boost::format(CreateLoggerText(text)) VALUES(__VA_ARGS__))); \
     } catch (...) { \
        getLogger().logError(text); \
     }
#endif

}

#endif //VIPER_COOLOG_H
