//
// Created by azhitlenok on 7/8/16.
//

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <ReadBuffer.h>

using ::testing::ReturnRef;
using ::testing::_;

namespace oms {

    const uint FAKE_MESSAGE_DATA_SIZE = 123;

    class FakeMessage {
    public:
        char m_data[FAKE_MESSAGE_DATA_SIZE];

        bool ParseFromArray(const void* data, uint size);

        const char* GetTypeName() { return "FakeMessage"; }
        const char* ShortDebugString() { return ""; }
    };

    bool FakeMessage::ParseFromArray(const void* data, uint size) {
        EXPECT_EQ(size, FAKE_MESSAGE_DATA_SIZE);

        char *datac = (char *)data;

        for(uint i = 0; i < FAKE_MESSAGE_DATA_SIZE; ++i) {
            EXPECT_EQ(m_data[i], datac[i]);
        }
        return true;
    }

    class ReadBufferFixture : public ::testing::Test, public ReadBufferPrivateAccessor {
    protected:
        virtual void SetUp() override;
        virtual void TearDown() override;
    public:
        void CleanBuffer();
        void ReadBufferTest(uint fraction[], uint fractionSize, int initValue);
        void WriteFakeMessageTest(uint fakeMessageSize, uint headerRest, uint fakeMessageRest, bool firstMessage);
        void WriteFakeData(uint fakeDataSize);
        void WriteMessageHeaderInOnePiece(const MessageHeader &messageHeader);
        void MoveToNextMessageLessThenBufferTest(uint messageSize[], uint messagesNumber);
        void MoveToNextMessageMoreThenBufferTest(uint messageSize[], uint messagesNumber);

        const uint BUFFER_SIZE = 4096;

        ReadBuffer m_readBuffer;
        int m_pipefd[2] {};
    };

    void ReadBufferFixture::SetUp() {
        pipe(m_pipefd);
    }

    void ReadBufferFixture::TearDown() {
        close(m_pipefd[0]);
        close(m_pipefd[1]);
    }

    void ReadBufferFixture::CleanBuffer() {
        TearDown();
        SetUp();
    }

    void ReadBufferFixture::WriteFakeMessageTest(uint fakeMessageSize, uint headerRest, uint fakeMessageRest, bool firstMessage) {
        //write fake message header part
        int fd = m_pipefd[0];

        MessageHeader messageHeader;
        messageHeader.m_size = fakeMessageSize;
        messageHeader.m_messageTypeID = 123;
        messageHeader.m_version = 5;
        messageHeader.m_messageID = 100;
        messageHeader.m_unixTimestamp = 12345678;

        auto messageHeaderSize = sizeof(MessageHeader);

        char buffer[messageHeaderSize];
        messageHeader.serializeToArray(buffer);

        //writes the first part of header
        write(m_pipefd[1], buffer, messageHeaderSize - headerRest);
        m_readBuffer.readFromStream(fd);
        if( firstMessage ) {
            EXPECT_FALSE(m_readBuffer.prepareToNextMessageProcessingAndCheckReadiness());
        }
        else {
            EXPECT_TRUE(m_readBuffer.prepareToNextMessageProcessingAndCheckReadiness());
        }

        //writes the rest of header
        write(m_pipefd[1], buffer + messageHeaderSize - headerRest, headerRest);
        m_readBuffer.readFromStream(fd);
        if( firstMessage ) {
            EXPECT_FALSE(m_readBuffer.prepareToNextMessageProcessingAndCheckReadiness());
        }
        else {
            EXPECT_TRUE(m_readBuffer.prepareToNextMessageProcessingAndCheckReadiness());
        }


        char messageFake[fakeMessageSize];
        //writes the first part of message
        write(m_pipefd[1], messageFake, fakeMessageSize - fakeMessageRest);
        m_readBuffer.readFromStream(fd);
        if( firstMessage ) {
            EXPECT_FALSE(m_readBuffer.prepareToNextMessageProcessingAndCheckReadiness());
        }
        else {
            EXPECT_TRUE(m_readBuffer.prepareToNextMessageProcessingAndCheckReadiness());
        }

        //writes the rest of message
        write(m_pipefd[1], messageFake + fakeMessageSize - fakeMessageRest, fakeMessageRest);
        m_readBuffer.readFromStream(fd);
        EXPECT_TRUE(m_readBuffer.prepareToNextMessageProcessingAndCheckReadiness());
    }

    void ReadBufferFixture::MoveToNextMessageLessThenBufferTest(uint messageSize[], uint messagesNumber) {
        //writes N messages
        uint totalDataSize = 0;
        for(uint i = 0; i < messagesNumber; ++i) {
            MessageHeader messageHeader;
            messageHeader.m_size = messageSize[i];
            WriteMessageHeaderInOnePiece(messageHeader);
            WriteFakeData(messageHeader.m_size);

            totalDataSize += MessageHeader::MESSAGE_HEADER_SIZE + messageSize[i];
        }

        int fd = m_pipefd[0];
        uint numberOfMessages = 0;

        uint totalRead = 0;
        uint offset = 0;
        while(true) {
            int n = m_readBuffer.readFromStream(fd);
            if( n < 0 ) {
                throw std::runtime_error("MoveToNextMessage_N_Test Read error");
            }

            totalRead += n;

            while(true) {

                EXPECT_TRUE(m_readBuffer.prepareToNextMessageProcessingAndCheckReadiness());
                auto res = m_readBuffer.moveToNextProtoMessage();
                offset += (MessageHeader::MESSAGE_HEADER_SIZE + messageSize[numberOfMessages++]);

                if(numberOfMessages < messagesNumber) {
                    EXPECT_TRUE(res);
                    EXPECT_EQ(data(m_readBuffer), buffer(m_readBuffer) + offset);
                } else {
                    EXPECT_FALSE(res);
                    EXPECT_EQ(data(m_readBuffer), buffer(m_readBuffer));
                    break;
                }
            }
            break;
        }
        EXPECT_EQ(totalDataSize, totalRead);
        EXPECT_EQ(numberOfMessages, messagesNumber);
    }


    void ReadBufferFixture::MoveToNextMessageMoreThenBufferTest(uint messageSize[], uint messagesNumber) {
        //writes N messages in size more then buffer
        uint totalDataSize = 0;
        for(uint i = 0; i < messagesNumber; ++i) {
            MessageHeader messageHeader;
            messageHeader.m_size = messageSize[i];
            WriteMessageHeaderInOnePiece(messageHeader);
            WriteFakeData(messageHeader.m_size);

            totalDataSize += MessageHeader::MESSAGE_HEADER_SIZE + messageSize[i];
        }

        int fd = m_pipefd[0];
        uint numberOfMessages = 0;

        uint totalRead = 0;

        while(true) {
            uint offset = 0;

            int n = m_readBuffer.readFromStream(fd);
            if( n < 0 ) {
                throw std::runtime_error("MoveToNextMessage_N_Test Read error");
            }

            totalRead += n;

            while(true) {

                auto res = m_readBuffer.prepareToNextMessageProcessingAndCheckReadiness();
                if(!res) {
                    m_readBuffer.copyPartialMessageToTheBeginningOfTheBuffer();
                    break;
                }
                res = m_readBuffer.moveToNextProtoMessage();
                offset += (MessageHeader::MESSAGE_HEADER_SIZE + messageSize[numberOfMessages++]);
                if(res) {
                    EXPECT_EQ(data(m_readBuffer), buffer(m_readBuffer) + offset);
                } else {
                    EXPECT_EQ(data(m_readBuffer), buffer(m_readBuffer));
                    break;
                }
            }
            if( totalRead >= totalDataSize ) {
                break;
            }
        }
        EXPECT_EQ(totalDataSize, totalRead);
        EXPECT_EQ(numberOfMessages, messagesNumber);
    }

    void ReadBufferFixture::WriteMessageHeaderInOnePiece(const MessageHeader &messageHeader){
        auto messageHeaderSize = sizeof(MessageHeader);

        char buffer[messageHeaderSize];
        messageHeader.serializeToArray(buffer);

        write(m_pipefd[1], buffer, messageHeaderSize);
    }

    void ReadBufferFixture::WriteFakeData(uint fakeDataSize) {
        char messageFake[fakeDataSize];
        for(uint i = 0; i < fakeDataSize; ++i) {
            messageFake[i] = (char)i;
        }
        write(m_pipefd[1], messageFake, fakeDataSize);
    }

    void ReadBufferFixture::ReadBufferTest(uint fraction[], uint fractionSize, int initValue) {
        int fd = m_pipefd[0];
        uint totalSize = 0;
        bool bufferFilled = false;

        //buffer size 4096
        char buf[BUFFER_SIZE];
        for(uint i = 0; i < fractionSize; ++i) {
            for(uint j = 0; j < fraction[i]; ++j) {
                buf[j] = (char)(j + initValue);
            }
            write(m_pipefd[1], buf, fraction[i]);

            auto size = m_readBuffer.readFromStream(fd);

            uint expectedSize = 0;
            if( !bufferFilled ) {
                expectedSize = fraction[i];
                if (totalSize + fraction[i] >= BUFFER_SIZE) {
                    expectedSize = BUFFER_SIZE - totalSize;
                    bufferFilled = true;
                }
                totalSize += expectedSize;
            }
            EXPECT_EQ(size, expectedSize);
        }

        uint bufferPosition = 0;
        bool testDone = false;
        const auto *dataPtr = data(m_readBuffer);
        for(uint i = 0; i < fractionSize; ++i) {
            for (uint j = 0; j < fraction[i]; ++j) {
                if( bufferPosition++ < BUFFER_SIZE ) {
                    char c = *(dataPtr++);
                    EXPECT_EQ(c, (char)(j + initValue));
                } else {
                    testDone = true;
                    break;
                }
            }
            if( testDone ) break;
        }
    }

    TEST_F(ReadBufferFixture, ReadFromStream_1_Test) {
        uint fraction[] = {200, 210, 240, 5, 120, 100, 245, 1000, 4000, 210, 200};
        uint fractionSize = sizeof(fraction) / sizeof(uint);
        ReadBufferTest(fraction, fractionSize, 5);
    }

    TEST_F(ReadBufferFixture, ReadFromStream_2_Test) {
        uint fraction[] = {4000, 200, 210};
        uint fractionSize = sizeof(fraction) / sizeof(uint);
        ReadBufferTest(fraction, fractionSize, 7);
    }

    TEST_F(ReadBufferFixture, Clear_Test) {
        uint fraction[] = {200, 210, 240, 5, 120, 100};
        uint fractionSize = sizeof(fraction) / sizeof(uint);
        ReadBufferTest(fraction, fractionSize, 3);

        CleanBuffer();

        m_readBuffer.clear();
        ReadBufferTest(fraction, fractionSize, 5);
    }

    TEST_F(ReadBufferFixture, PrepareToNextMessageProcessingAndCheckReadiness_Test) {
        WriteFakeMessageTest(20, 2, 5, true);
    }

    TEST_F(ReadBufferFixture, MoveToNextMessage_1_Test) {
        WriteFakeMessageTest(20, 2, 5, true);

        EXPECT_FALSE(m_readBuffer.moveToNextProtoMessage());
        EXPECT_EQ(data(m_readBuffer), buffer(m_readBuffer));
    }

    TEST_F(ReadBufferFixture, MoveToNextMessage_2_Test) {
        uint fakeMessageSize = 20;
        WriteFakeMessageTest(fakeMessageSize, 2, 5, true);
        WriteFakeMessageTest(30, 6, 4, false);

        EXPECT_TRUE(m_readBuffer.moveToNextProtoMessage());
        EXPECT_EQ(data(m_readBuffer), buffer(m_readBuffer) + fakeMessageSize + MessageHeader::MESSAGE_HEADER_SIZE);

        EXPECT_TRUE(m_readBuffer.prepareToNextMessageProcessingAndCheckReadiness());

        EXPECT_FALSE(m_readBuffer.moveToNextProtoMessage());
        EXPECT_EQ(data(m_readBuffer), buffer(m_readBuffer));
    }

    TEST_F(ReadBufferFixture, MoveToNextMessageLessThenBufferTest_Test) {
        {
            uint messageSize[] = {200, 210, 240, 5, 120, 100};
            uint messagesNumber = sizeof(messageSize) / sizeof(uint);
            MoveToNextMessageLessThenBufferTest(messageSize, messagesNumber);
        }

        {
            uint messageSize[] = {201, 211, 240, 56, 121, 10};
            uint messagesNumber = sizeof(messageSize) / sizeof(uint);
            MoveToNextMessageLessThenBufferTest(messageSize, messagesNumber);
        }
    }

    TEST_F(ReadBufferFixture, MoveToNextMessageMoreThenBufferTest_Test) {
        {
            uint messageSize[] = {200, 210, 240, 5, 120, 100, 1000, 550, 631, 225, 224, 172, 551, 320, 441};
            uint messagesNumber = sizeof(messageSize) / sizeof(uint);
            MoveToNextMessageMoreThenBufferTest(messageSize, messagesNumber);
        }

        {
            uint messageSize[] = {205, 215, 210, 15, 12, 105, 1000, 550, 631, 225, 224, 172, 551, 320, 444};
            uint messagesNumber = sizeof(messageSize) / sizeof(uint);
            MoveToNextMessageMoreThenBufferTest(messageSize, messagesNumber);
        }
    }

    TEST_F(ReadBufferFixture, MoveToNextMessageComplexTest_Test) {
        {
            uint messageSize[] = {200, 210, 240, 5, 120, 100};
            uint messagesNumber = sizeof(messageSize) / sizeof(uint);
            MoveToNextMessageLessThenBufferTest(messageSize, messagesNumber);
        }

        {
            uint messageSize[] = {200, 210, 240, 5, 120, 100, 1000, 550, 631, 225, 224, 172, 551, 320, 441};
            uint messagesNumber = sizeof(messageSize) / sizeof(uint);
            MoveToNextMessageMoreThenBufferTest(messageSize, messagesNumber);
        }


        {
            uint messageSize[] = {205, 215, 210, 15, 12, 105, 1000, 550, 631, 225, 224, 172, 551, 320, 444};
            uint messagesNumber = sizeof(messageSize) / sizeof(uint);
            MoveToNextMessageMoreThenBufferTest(messageSize, messagesNumber);
        }

        {
            uint messageSize[] = {201, 211, 240, 56, 121, 10};
            uint messagesNumber = sizeof(messageSize) / sizeof(uint);
            MoveToNextMessageLessThenBufferTest(messageSize, messagesNumber);
        }

        {
            uint messageSize[] = {201, 211, 240, 56, 121, 10};
            uint messagesNumber = sizeof(messageSize) / sizeof(uint);
            MoveToNextMessageLessThenBufferTest(messageSize, messagesNumber);
        }

        {
            uint messageSize[] = {205, 215, 210, 15, 12, 105, 1000, 550, 631, 225, 224, 172, 551, 320, 444};
            uint messagesNumber = sizeof(messageSize) / sizeof(uint);
            MoveToNextMessageMoreThenBufferTest(messageSize, messagesNumber);
        }
    }

    TEST_F(ReadBufferFixture, GetMessageHeader_Test) {

        int fd = m_pipefd[0];

        MessageHeader messageHeader;
        messageHeader.m_size = 234;
        messageHeader.m_messageTypeID = 123;
        messageHeader.m_version = 5;
        messageHeader.m_messageID = 100;
        messageHeader.m_unixTimestamp = 12345678;

        WriteMessageHeaderInOnePiece(messageHeader);
        WriteFakeData(messageHeader.m_size);

        m_readBuffer.readFromStream(fd);
        EXPECT_TRUE(m_readBuffer.prepareToNextMessageProcessingAndCheckReadiness());

        auto messageHeader1 = m_readBuffer.getMessageHeader();
        EXPECT_EQ(messageHeader.m_size, messageHeader1.m_size);
        EXPECT_EQ(messageHeader.m_messageTypeID, messageHeader1.m_messageTypeID);
        EXPECT_EQ(messageHeader.m_version, messageHeader1.m_version);
        EXPECT_EQ(messageHeader.m_messageID, messageHeader1.m_messageID);
        EXPECT_EQ(messageHeader.m_unixTimestamp, messageHeader1.m_unixTimestamp);
    }

    TEST_F(ReadBufferFixture, prepareToNextMessageProcessingAndCheckReadiness_Test) {
        uint fraction[] = {200, 210, 240, 5, 120, 100};
        uint fractionSize = sizeof(fraction) / sizeof(uint);
        ReadBufferTest(fraction, fractionSize, 4);

        m_readBuffer.prepareToNextMessageProcessingAndCheckReadiness();
    }

    TEST_F(ReadBufferFixture, CopyPartialMessageToTheBeginningOfTheBuffer_Test) {

        int fd = m_pipefd[0];

        MessageHeader messageHeaderToCheck;
        messageHeaderToCheck.m_size = 30;
        messageHeaderToCheck.m_messageTypeID = 78;
        messageHeaderToCheck.m_version = 7;
        messageHeaderToCheck.m_messageID = 101;
        messageHeaderToCheck.m_unixTimestamp = 453657;

        {
            MessageHeader messageHeader;
            messageHeader.m_size = 234;
            messageHeader.m_messageTypeID = 123;
            messageHeader.m_version = 5;
            messageHeader.m_messageID = 100;
            messageHeader.m_unixTimestamp = 12345678;

            WriteMessageHeaderInOnePiece(messageHeader);
            WriteFakeData(messageHeader.m_size);
        }

        {
            WriteMessageHeaderInOnePiece(messageHeaderToCheck);
            WriteFakeData(messageHeaderToCheck.m_size - 5);
        }

        m_readBuffer.readFromStream(fd);
        EXPECT_TRUE(m_readBuffer.prepareToNextMessageProcessingAndCheckReadiness());
        EXPECT_TRUE(m_readBuffer.moveToNextProtoMessage());
        EXPECT_FALSE(m_readBuffer.prepareToNextMessageProcessingAndCheckReadiness());

        m_readBuffer.copyPartialMessageToTheBeginningOfTheBuffer();

        EXPECT_EQ(buffer(m_readBuffer), data(m_readBuffer));
        auto messageHeader1 = m_readBuffer.getMessageHeader();

        EXPECT_EQ(messageHeaderToCheck.m_size, messageHeader1.m_size);
        EXPECT_EQ(messageHeaderToCheck.m_messageTypeID, messageHeader1.m_messageTypeID);
        EXPECT_EQ(messageHeaderToCheck.m_version, messageHeader1.m_version);
        EXPECT_EQ(messageHeaderToCheck.m_messageID, messageHeader1.m_messageID);
        EXPECT_EQ(messageHeaderToCheck.m_unixTimestamp, messageHeader1.m_unixTimestamp);

    }

    TEST_F(ReadBufferFixture, ReadBufferFixture_ParseMessage_Test) {
        FakeMessage fakeMessage;

        for(uint i = 0; i < FAKE_MESSAGE_DATA_SIZE; ++i) {
            fakeMessage.m_data[i] = (char)i;
        }

        {
            MessageHeader messageHeader;
            messageHeader.m_size = FAKE_MESSAGE_DATA_SIZE;
            messageHeader.m_messageTypeID = 123;
            messageHeader.m_version = 5;
            messageHeader.m_messageID = 100;
            messageHeader.m_unixTimestamp = 12345678;

            WriteMessageHeaderInOnePiece(messageHeader);
        }

        write(m_pipefd[1], fakeMessage.m_data, FAKE_MESSAGE_DATA_SIZE);


        int fd = m_pipefd[0];
        m_readBuffer.readFromStream(fd);
        m_readBuffer.prepareToNextMessageProcessingAndCheckReadiness();
        EXPECT_TRUE(m_readBuffer.parseMessage(fakeMessage)); //tests in fakeMessage callback method
    }
}



