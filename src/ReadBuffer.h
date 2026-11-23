#ifndef VIPER_READBUFFER_H
#define VIPER_READBUFFER_H

#include <algorithm>
#include <cstring>
#include <hf/hf.h>
#include <zconf.h>
#include <boost/format.hpp>

#include "MessageHeader.h"
#include "messages/python.pb.h"

#include <util/log.h>

namespace Python {
    namespace READBUFFER_H_LOGGER {
        LOG_DECLARE();
    }

using message_id_t = uint32_t;

class ReadBufferPrivateAccessor; //for unit testing

    struct ReadBuffer {
    public:
        friend class ReadBufferPrivateAccessor;

        template<typename T>
            bool parseMessage(T &message);
        inline size_t readFromStream(int fd);
        inline const MessageHeader &getMessageHeader();
        bool prepareToNextMessageProcessingAndCheckReadiness();
        inline void copyPartialMessageToTheBeginningOfTheBuffer();
        inline bool moveToNextProtoMessage();
        inline void clear();
    private:
        //data
        static const int BUFFER_SIZE = 20'971'520; //20 MiB

    private:
        inline const char *data() const;
        inline int dataSize() const;
        inline char *readPosition();
        inline size_t getRemainingSize() const;

        inline char* buf() const { return m_buffer->begin(); }

        //data
        std::unique_ptr<std::array<char, BUFFER_SIZE>> m_buffer{new std::array<char, BUFFER_SIZE>()};
        int m_currentPosition = 0;
        int m_messageStart = 0;

        MessageHeader m_messageHeader;
    };

    ///////////////////////////////////////////////////////////////
    // defines inline functions
    ///////////////////////////////////////////////////////////////

    template<typename T>
    inline bool ReadBuffer::parseMessage(T &message) {
        auto messageParsed = message.ParseFromArray(data() + MessageHeader::MESSAGE_HEADER_SIZE, m_messageHeader.m_size);
        if(proto::GUI_MESSAGE_TYPE::GUI_MESSAGE_TYPE_HEARTBEAT != static_cast<proto::GUI_MESSAGE_TYPE>(m_messageHeader.m_messageTypeID)) {
            using namespace READBUFFER_H_LOGGER;
            //Log the message
            _LOG_DEBUG((([&message, &messageParsed]() {
                if(BOOST_LIKELY(messageParsed)) {
                    return std::string(" <-- Decoded message {") + message.GetTypeName() + "}: " + message.ShortDebugString();
                }
                return std::string("Message Decoding Error");
            })()));
        }
        return messageParsed;
    }

    //true - buffer is not empty
    inline bool ReadBuffer::moveToNextProtoMessage() {
        auto messageSize = m_messageHeader.m_size + MessageHeader::MESSAGE_HEADER_SIZE;
        m_messageStart += messageSize;
        if(m_messageStart >= m_currentPosition ) {
            m_messageStart = 0;
            m_currentPosition = 0;
            return false;
        }
        return true;
    }

    inline bool ReadBuffer::prepareToNextMessageProcessingAndCheckReadiness() {
        auto data_size = dataSize();
        if(BOOST_UNLIKELY(data_size < MessageHeader::MESSAGE_HEADER_SIZE)) {
            return false;
        }

        m_messageHeader.parseFromArray(data());
        if(BOOST_UNLIKELY(data_size < static_cast<int>(m_messageHeader.m_size) + MessageHeader::MESSAGE_HEADER_SIZE)) {
            return false;
        }
        return true;
    }

    inline const char *ReadBuffer::data() const {
        return buf() + m_messageStart;
    }

    inline int ReadBuffer::dataSize() const {
        return m_currentPosition - m_messageStart;
    };

    inline const MessageHeader &ReadBuffer::getMessageHeader() {
        return m_messageHeader;
    }

    inline void ReadBuffer::copyPartialMessageToTheBeginningOfTheBuffer() {
        if( m_messageStart == 0 ) return;
        auto data_size = dataSize();
        if( data_size == 0 ){
            m_messageStart = 0;
            m_currentPosition = 0;
        } else {
            ::memmove(buf(), buf() + m_messageStart, data_size);
            m_messageStart = 0;
            m_currentPosition = data_size;
        }
    }

    inline size_t ReadBuffer::readFromStream(int fd) {
        auto n = ::read(fd, readPosition(), getRemainingSize());
        if (n >= 0) {
            m_currentPosition += n;
        }
        return n;
    }

    inline char *ReadBuffer::readPosition() {
        return buf() + m_currentPosition;
    }

    inline size_t ReadBuffer::getRemainingSize() const {
        return BUFFER_SIZE - m_currentPosition;
    }

    inline void ReadBuffer::clear() {
        m_messageStart = 0;
        m_currentPosition = 0;
    }

    //for unit test only
    class ReadBufferPrivateAccessor {
    protected:
        inline const char *buffer(ReadBuffer &readBuffer) const {
            return readBuffer.buf();
        }

        inline const char *data(ReadBuffer &readBuffer) const {
            return readBuffer.data();
        }
    };
}
#endif //VIPER_READBUFFER_H
