#ifndef VIPER_SRVRCLIENTMESSENGER_H
#define VIPER_SRVRCLIENTMESSENGER_H

#include <chrono>
#include "messages/python.pb.h"
#include "ClientAcceptor.h"
#include "CommonHelper.h"
#include <poll.h>

namespace oms_ServerClientMessenger_LOGGER {
    LOG_DECLARE();
}

namespace Python {

    class ServerClientMessenger {
    public:
        static ServerClientMessenger *instance();

        ServerClientMessenger();

        bool doSendClientLoginReject(ClientConnection& traderConnection, proto::ERROR_CODE errorCode, const std::string &text, message_id_t messageID);
        bool doSendClientLogin(ClientConnection& traderConnection, message_id_t messageID, uint32_t heartbeatIntervalInMilliseconds, const char* trader);
        bool doSendClientLogout(ClientConnection& traderConnection, bool isTraderActive, message_id_t messageID);
        bool doSendClientRequestRejected(ClientConnection& traderConnection, proto::GUI_MESSAGE_TYPE requestType, proto::ERROR_CODE errorCode, const std::string &text, message_id_t messageID);
        bool doSendFormatError(ClientConnection& traderConnection, proto::ERROR_CODE errorCode, const std::string &text, message_id_t messageID);
        void sendGUIInitialization(ClientConnection& traderConnection, proto::GUIInitialization& data);
        void sendSwitchChangeReply(uint32_t strategy, uint32_t action, const std::string& value, uint32_t gridId, bool accepted);

        template<typename T>
        bool sendMessageToClient(ClientConnection* traderConnection, proto::SERVER_MESSAGE_TYPE responseType, T &message);

        template<typename T>
        bool sendMessageToClient(int32_t clientId, proto::SERVER_MESSAGE_TYPE responseType, T &message);

        template<typename T>
        bool sendMessageToClient(ClientConnection* traderConnection, proto::SERVER_MESSAGE_TYPE responseType, T &message, std::ostringstream& streamMessage);

    private:
        static ServerClientMessenger *s_instance;

        bool sendMessageToClientCommon(ClientConnection* traderConnection,  std::uint8_t type, std::ostringstream& streamMessage, uint32_t length);

        void log(const std::string& s);

        static constexpr int BUFFER_SIZE = 20'971'520; //20 MiB
        char buffer[BUFFER_SIZE];

        friend class ClientsAcceptor;
        std::unordered_map<int, std::shared_ptr<ClientConnection>> m_traderConnections;
    };

template<typename T>
bool ServerClientMessenger::sendMessageToClient(ClientConnection* traderConnection,  proto::SERVER_MESSAGE_TYPE responseType, T &message) {
    std::ostringstream streamMessage;
    return sendMessageToClient(traderConnection, responseType, message, streamMessage);
}

template<typename T>
bool ServerClientMessenger::sendMessageToClient(int32_t connectionId,  proto::SERVER_MESSAGE_TYPE responseType, T &message) {
    std::ostringstream streamMessage;
    if (connectionId < 0) {
        return sendMessageToClient(nullptr, responseType, message, streamMessage);
    }

    auto iter = m_traderConnections.find(connectionId);
    if (iter != m_traderConnections.end()) {
        sendMessageToClient(iter->second.get(), proto::SERVER_MESSAGE_TYPE_GRID_STATE_CHANGE, message);
    }
    return true;
}

template<typename T>
bool ServerClientMessenger::sendMessageToClient(ClientConnection* traderConnection,  proto::SERVER_MESSAGE_TYPE responseType, T &message, std::ostringstream& streamMessage) {
    message.SerializeToOstream(&streamMessage);
    streamMessage.seekp(0, std::ios::end);
    auto length = streamMessage.tellp();
    return sendMessageToClientCommon(traderConnection, responseType, streamMessage, length);
}


}

#endif //VIPER_SRVRCLIENTMESSENGER_H
