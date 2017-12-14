#ifndef VIPER_COMMON_H
#define VIPER_COMMON_H

#include <md/md.h>
#include <functional>
#include <limits>
#include <hf/pool/DataPool.h>
#include <hf/queue/U64Queue.h>
#include "messages/omsproto.pb.h"

namespace Python {

#define mb() asm volatile("mfence":::"memory")


template <typename EnumT>
int32_t toInt(EnumT value) {
    return static_cast<int32_t>(value);
}

template <typename EnumT>
uint32_t toUInt(EnumT value) {
    return static_cast<uint32_t>(value);
}

enum class SystemState {Enabled, Disabled, HardShutdown};
enum class StrategyId {Unknown = 0, MarketMaker = 1, RelativeValue = 2};

struct Market {
    hf::price_t _price = hf::NULL_PRICE;
    hf::qty_t _size;
    uint32_t _id;
};

struct Markets {
    Market& getMarket(uint32_t id) {
        return _markets[id];
    }

    Market _markets[5];
    uint32_t _numberOfMarkets = 0;
};

struct GridUpdate {
    proto::GridUpdate _update;
    bool _ready = true;
    StrategyId _strategyId;
    uint32_t _gridId;

    void clear() {
        _ready = true;
        _strategyId = StrategyId::Unknown;
        _gridId = 0;
    }
};

inline bool IsZero(double value) {
    return  value > 0 ? value < 0.00000001 : value > -0.00000001;
}

inline hf::price_t ToRhonePrice(double price) {
    return price * 10000000;
}

inline double ToDollars(hf::price_t price) {
    return static_cast<double>(price) / 10000000.0;
}

static const uint32_t BID = 0;
static const uint32_t ASK = 1;

struct Book {
    hf::price_t _price;
    hf::qty_t _otherQuantity;
    md::order_count_t _otherOrderCount;
    hf::qty_t _ourQuantity;
    md::order_count_t _ourOrderCount;
};

typedef uint64_t OrderId_t;

struct Order {
    enum Type {Hedging, Quoting};
    enum Strategy {MarketMaker, RelativeValue, Stacker};

    void onFullFill() {
        _onFill(_size);
    }

    void onPartialFill(hf::qty_t size) {
        _onFill(size);
    }

    OrderId_t _id;
    Strategy _strategy;
    Type _type;

    hf::price_t _price;
    hf::price_t _roundedPrice;
    hf::qty_t _size;
    uint32_t _side;

    std::function<void(hf::qty_t)>& _onFill;
};

inline hf::price_t RoundToTickSize(double price, hf::price_t tickSize, uint32_t side) {
    const hf::price_t rawPrice = static_cast<hf::price_t>(price);
    const hf::price_t remainder = rawPrice % tickSize;

    if ((!remainder))
        return rawPrice;

    const hf::price_t truncatedPrice = rawPrice - remainder;

    if (side == BID) {
        return ((remainder > 0) ? truncatedPrice : truncatedPrice - tickSize);
    } else {
        return ((remainder > 0) ? truncatedPrice + tickSize : truncatedPrice);
    }
}

inline void ShutdownOnError(const std::string& message) {
}

enum class ViperMessageType : uint32_t {
    Login, Logout, ClientDown, GridUpdate, MarketUpdate, SwitchChange,
    GridUpdateAccepted, GridUpdateRejected, SwitchChangeReply, PositionUpdate, STFUpdate
};

struct ViperMessage {
    ViperMessageType _type;
    char _data[0];
};

struct GridUpdateMessage {
    GridUpdate* _gridUpdate;
};

struct GenericMessage {
    int32_t _number_1;
    int32_t _number_2;
};

struct MarketUpdate {
    uint32_t _instrumentIndex;
    uint32_t _marketId;
    uint32_t _side;
    hf::price_t _price;
    hf::qty_t _size;
};

struct GridUpdateReply {
    uint32_t _gridId;
};

class ViperPool : public hf::pool::DataPool {
public:

    void create();

    ViperMessage& getNextMessage() const {
        return *static_cast<ViperMessage*> (getNextP());
    }

    ViperMessage& getMessage(const uint64_t idx) const {
        return *static_cast<ViperMessage*> (getData(idx));
    }
};

class ViperQueue : public hf::queue::U64Queue, protected hf::mem::DataBlock {
public:

    struct my_queue_t {
        uint64_t memsize;
        uint8_t data[0];
    };

    void create();

    void set_pool(ViperPool *ptr) { m_pPool = ptr; }

    ViperPool *pool() const { return m_pPool; }

private:
    ViperPool *m_pPool = nullptr;
};

}

#endif //VIPER_COMMON_H
