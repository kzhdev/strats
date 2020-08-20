#ifndef VIPER_COOLOG_H
#define VIPER_COOLOG_H

#include <type_traits>
#include "om/om.h"
#include <boost/format.hpp>
#include "Logger.h"
#include <iostream>
#include <hf/queue/DebugQueue.h>

namespace logging {

constexpr void setCounts(int& intCount, int& doubleCount, om::debug_t& di) {
    di.ch[0] = intCount - 1;
    di.ch[1] = doubleCount - 1;
}

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
    setCounts(intCount, doubleCount, di);
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

inline void logDebugStruct(om::debug_t& di) {
    GetLogger().logInfo(boost::str(boost::format("Template. %1%")  % di.i[0]));
}

#ifndef IS_DEV
#define KDB_LOG_BASE(id,text,...)  {                    \
  auto logger = logging::GetKdbLogger();                \
  if (BOOST_LIKELY(logger != nullptr)) {                \
      om::debug_t& di = logger->addDIEvent();           \
      di.i[0] = id;                          	        \
      logging::createLogStatement(di,  __VA_ARGS__);    \
      logger->sendDIEvent();                            \
  }                                                     \
}

#define LOG_BASE(id,text,...)  {                        \
  auto logger = logging::GetViperLogger();              \
  if (BOOST_LIKELY(logger != nullptr)) {                \
      auto index = logger->reserve();                   \
      auto& di = (*logger)[index];                      \
      di.i[0] = id;                          	        \
      logging::createLogStatement(di,  __VA_ARGS__);    \
      logger->publish(index);                           \
  }                                                     \
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
#define VALUES_10(one, two, three, four, five, six, seven, eight, nine, ten) % one % two % three % four % five % six % seven % eight % nine % ten
#define VALUES_11(one, two, three, four, five, six, seven, eight, nine, ten, eleven) % one % two % three % four % five % six % seven % eight % nine % ten % eleven
#define VALUES_12(one, two, three, four, five, six, seven, eight, nine, ten, eleven, twelve) % one % two % three % four % five % six % seven % eight % nine % ten % eleven % twelve
#define VALUES_13(one, two, three, four, five, six, seven, eight, nine, ten, eleven, twelve, thirteen) % one % two % three % four % five % six % seven % eight % nine % ten % eleven % twelve % thirteen
#define VALUES_14(one, two, three, four, five, six, seven, eight, nine, ten, eleven, twelve, thirteen, fourteen) % one % two % three % four % five % six % seven % eight % nine % ten % eleven % twelve % thirteen % fourteen

#define GET_MACRO(_0, _1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, NAME, ...) NAME
#define VALUES(...) GET_MACRO(_0, ##__VA_ARGS__, VALUES_14, VALUES_13, VALUES_12, VALUES_11, VALUES_10, VALUES_9, VALUES_8, VALUES_7, VALUES_6, VALUES_5, VALUES_4, VALUES_3, VALUES_2, VALUES_1, VALUES_0)(__VA_ARGS__)

#define KDB_LOG_BASE(id,text, ...) \
     try { \
        logging::GetLogger().logInfo(boost::str(boost::format(logging::CreateLoggerText(text)) VALUES(__VA_ARGS__))); \
     } catch (...) { \
        logging::GetLogger().logError(text); \
     }

#define LOG_BASE(id,text, ...) KDB_LOG_BASE(id, text, __VA_ARGS__)
#endif

#define LOG_KDB(id,text,...) { \
    KDB_LOG_BASE(id,text,__VA_ARGS__) \
}

#define LOG(id,text,...) { \
    LOG_BASE(id,text,__VA_ARGS__) \
}

enum class LOG_LEVEL : uint8_t {
    ERROR = 0,
    WARNING,
    INFO,
    DEBUG,
    TRACE,
};

inline LOG_LEVEL& loggingLevel() {
#ifdef IS_DEV
    static LOG_LEVEL level = LOG_LEVEL::TRACE;
#else
    static LOG_LEVEL level = LOG_LEVEL::INFO;
#endif
    return level;
}

#define BASE_LOG(level, id, text, ...) { \
    if (BOOST_LIKELY(level <= logging::loggingLevel())) { \
        LOG(id, text, __VA_ARGS__) \
    } \
}

#define TLOG(id, text, ...) \
    if (BOOST_UNLIKELY(logging::loggingLevel() > logging::LOG_LEVEL::DEBUG)) { \
        LOG(id, text, __VA_ARGS__) \
    }

#define DLOG(id, text, ...) \
    if (BOOST_UNLIKELY(logging::loggingLevel() > logging::LOG_LEVEL::INFO)) { \
        LOG(id, text, __VA_ARGS__) \
    }

#define ILOG(id, text, ...) BASE_LOG(logging::LOG_LEVEL::INFO, id, text, __VA_ARGS__)
#define WLOG(id, text, ...) BASE_LOG(logging::LOG_LEVEL::WARNING, id, text, __VA_ARGS__)
#define ELOG(id, text, ...) BASE_LOG(logging::LOG_LEVEL::ERROR, id, text, __VA_ARGS__)

}

#endif //VIPER_COOLOG_H
