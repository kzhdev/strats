#ifndef VIPER_MESSAGEHEADER_H
#define VIPER_MESSAGEHEADER_H

#include <cstdint>
#include <sstream>
#include <netinet/in.h>

namespace Python {

#pragma pack(push, 1)
    struct MessageHeader {
        uint32_t m_size;
        uint8_t m_messageTypeID;
        uint8_t m_version;
        uint32_t m_messageID;
        int64_t m_unixTimestamp;

        static const int32_t MESSAGE_HEADER_SIZE = sizeof(m_size) + sizeof(m_messageTypeID) + sizeof(m_version) + sizeof(m_messageID) + sizeof(m_unixTimestamp);

        void serializeToArray(char *buffer) const {
            memcpy(buffer, this, sizeof(MessageHeader));
        }

        void parseFromArray(const char *buffer) {
            memcpy(this, buffer, sizeof(MessageHeader));
        }
    };
#pragma pack(pop)
}

#endif //VIPER_MESSAGEHEADER_H
