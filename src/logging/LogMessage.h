#pragma once

namespace logging {

enum VALUE_TYPE {VALUE_TYPE_INT = 0, VALUE_TYPE_DOUBLE = 1};

#pragma pack(1)

struct Value {
    uint8_t _type;
    uint8_t _sequenceNumber;
    uint64_t _value;
};

struct Header {
    uint64_t _time;
    uint32_t _size;
};

struct LogEntry {
    uint32_t _containerId;
    uint16_t _templateId;
    uint8_t _numberOfValues;
    Value _values[0];
};

struct LogMessage {
    Header _header;
    LogEntry _logEntry;
};

#pragma pack()

}