#pragma once

#include "RingBufferQueue.h"
#include "Common.h"
#include <hf/ConfigManager.h>
#include "ClockUtil.h"
#include "ViperConfig.h"
#include "LockFreeQueue.h"

namespace hf { namespace queue { class DebugQueue; }}

namespace Python {

class ViperStrategyBase;

class ViperResource {
public:
    static SharedSystemState s_systemState;
    static ViperConfig _viperConfig;

protected:
    static constexpr uint32_t DEFAULT_QUEUE_SIZE = 8388608;
    static double _frequency;
    static RingBufferQueue<>* _frontQueue;
    static RingBufferQueue<>* _executorQueue;
    static std::vector<ViperStrategyBase*> _strategies;

    mutable RingBufferQueue<> _msgQueue;
    uint64_t _msgCursor = 0;

    hf::queue::DebugQueue* _kdbLogger = nullptr;
    SharedSystemState::QuoteExtBitset _oldExchangeQuoteExt;
    SystemState _oldQuoteExt = SystemState::Disabled;


public:
    ViperResource() = default;
    virtual ~ViperResource() = default;

    virtual ResourceId id() const noexcept = 0;

    static bool canExchangeQuoteExternally(hf::VENUE venue) noexcept {
        return s_systemState._exchQuoteExternally.load(std::memory_order_relaxed).test(toUInt(venue));
    }

    static bool disableAllExchangeExternalQuoting() noexcept {
        return s_systemState._exchQuoteExternally.exchange(0, std::memory_order_release).any();
    }

    RingBufferQueue<>* queue() noexcept { return &_msgQueue; }

    void sendErrorMessage(const std::string& s, ErrorType type) const {
        static constexpr uint32_t MSG_SZ = CacheLineBuf::calculateReserveSize(sizeof(ViperMessage) + sizeof(ErrorMessage));
        auto cursor = _frontQueue->reserve(MSG_SZ);
        auto& outMessage = reinterpret_cast<ViperMessage&>((*_frontQueue)[cursor].data[0]);
        outMessage._size = MSG_SZ;
        outMessage._type = ViperMessageType::ErrorMessage;
        auto& message = reinterpret_cast<ErrorMessage&>(outMessage._data[0]);
        const uint32_t size = std::min(CRITICAL_MESSAGE_MAX_SIZE, (uint32_t) s.size());
        strncpy(message._message, s.c_str(), size);
        message._size = size;
        message._type = type;
        _frontQueue->publish(cursor, MSG_SZ);
    }

    void sendGenericMessageToFront(ViperMessageType type, int64_t value1 = 0, int64_t value2 = 0, int64_t value3 = 0) const {
        _sendGenericMessage(*_frontQueue, type, value1, value2, value3);
    }

    template<typename MsgT, typename MsgPopulateFunc>
    void sendMessageToFront(ViperMessageType type, MsgPopulateFunc&& msgPopulateFunc) const {
        _sendMessage<MsgT>(*_frontQueue, type, std::forward<MsgPopulateFunc>(msgPopulateFunc));
    }

    void sendGenericMessageToExecutor(ViperMessageType type, int64_t value1 = 0, int64_t value2 = 0, int64_t value3 = 0) const {
        _sendGenericMessage(*_executorQueue, type, value1, value2, value3);
    }

    template<typename MsgT, typename MsgPopulateFunc>
    void sendMessageToExecutor(ViperMessageType type, MsgPopulateFunc&& msgPopulateFunc, uint32_t msgDataSize = 0) const {
        _sendMessage<MsgT>(*_executorQueue, type, std::forward<MsgPopulateFunc>(msgPopulateFunc), msgDataSize);
    }

    void sendGridUpdateToExecutor(GridUpdate& update) const {
        _sendMessage<GridUpdateMessage>(
                *_executorQueue,
                ViperMessageType::GridUpdate,
                [&](GridUpdateMessage& message) {
                    message._gridUpdate = &update;
                });
    }

    void sendGridUpdate(GridUpdate& update) const {
        sendMessage<GridUpdateMessage>(
                ViperMessageType::GridUpdate,
                [&](GridUpdateMessage& message) {
                    message._gridUpdate = &update;
                });
    }

    void sendGenericMessage(ViperMessageType type, int64_t value1 = 0, int64_t value2 = 0, int64_t value3 = 0) const {
        _sendGenericMessage(_msgQueue, type, value1, value2, value3);
    }

    template<typename MsgT, typename MsgPopulateFunc>
    void sendMessage(ViperMessageType type, MsgPopulateFunc&& msgPopulateFunc, uint32_t msgDataSize = 0) const {
        _sendMessage<MsgT>(_msgQueue, type, std::forward<MsgPopulateFunc>(msgPopulateFunc), msgDataSize);
    }

    bool changeExchangeExternalQuoting(hf::VENUE venue, SystemState newState) {
        LOG(0, "{i1} change {i2} external quoting to {i3}", resourceName(id()), toUInt(venue), toUInt(newState))
        auto currentExtQuote = s_systemState._exchQuoteExternally.load(std::memory_order_relaxed);
        auto index = static_cast<uint8_t>(venue);
        bool rt = currentExtQuote.test(index);
        std::bitset<static_cast<uint8_t>(hf::VENUE::ANY)> newExtQuote;
        do {
            newExtQuote = currentExtQuote;
            if (newState == SystemState::Enabled) {
                newExtQuote.set(index);
            }
            else
            {
                newExtQuote.set(index, 0);
            }
        }
        while (!s_systemState._exchQuoteExternally.compare_exchange_strong(currentExtQuote, newExtQuote, std::memory_order_release,std::memory_order_relaxed));
        return rt;
    }

    static const std::string& getContractName(uint32_t id) noexcept {
        return _viperConfig._allContracts[id]._name;
    }

    ViperStrategyBase* getStrategy(StrategyId id) noexcept {
        return _strategies[toUInt(id)];
    }

    template<typename T>
    T* getStrategy(StrategyId id) noexcept {
        return reinterpret_cast<T*>(_strategies[toUInt(id)]);
    }

#ifdef IS_DEV
    std::string getContract(uint32_t id) {
        return _viperConfig._allContracts[id]._name;
    }
#else
    uint32_t getContract(uint32_t id) {
        BOOST_ASSERT_MSG(id == _viperConfig._allContracts[id]._internalId, "If this isn't true . . . a lot of the log statements are broken");
        return _viperConfig._allContracts[id]._internalId;
    }
#endif

    uint32_t sidToInternalId(uint32_t sid) const {
        return _viperConfig.getInternalId(sid);
    }

protected:
    void _onCreate(const libconfig::Setting& config) {
        _msgQueue.init(hf::ConfigManager::getULong(config, "queue_size", DEFAULT_QUEUE_SIZE));
    }

    void _onPostCreate(const libconfig::Setting& config);

    template<typename MsgT, typename MessagePopulateFunc>
    void _sendMessage(RingBufferQueue<>& queue,
                      ViperMessageType type,
                      MessagePopulateFunc&& msgPopulateFunc,
                      uint32_t msgDataSize = 0) const {
        uint32_t sz = CacheLineBuf::calculateReserveSize(sizeof(ViperMessage) + sizeof(MsgT) + msgDataSize);
        static_assert(sizeof(MsgT) <= 58 || std::is_same<MsgT, FillMessage>::value, "Message too big");
#ifdef IS_DEV
        if ((sizeof(MsgT) + msgDataSize) > 58) {
            LOG(0, "!!!! Can we reduce message size? msgType={i1} msgSize={i2} msgDataSize={i3}", toUInt(type), sizeof(MsgT), msgDataSize)
        }
#endif
        assert(sz);
        auto cursor = queue.reserve(sz);
        auto& outMessage = reinterpret_cast<ViperMessage&>(queue[cursor].data[0]);
        outMessage._size = sz;
        outMessage._type = type;
        outMessage._source = id();
        auto& message = reinterpret_cast<MsgT&>(outMessage._data[0]);
        msgPopulateFunc(message);
        queue.publish(cursor, sz);
    }

    void _sendGenericMessage(RingBufferQueue<>& queue, ViperMessageType type, int64_t value1, int64_t value2, int64_t value3) const {
        _sendMessage<GenericMessage>(queue, type, [=](GenericMessage& message) {
            message._number_1 = value1;
            message._number_2 = value2;
            message._number_3 = value3;
        });
    }
};

}