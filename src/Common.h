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

static const uint32_t BID = 0;
static const uint32_t ASK = 1;

template <uint32_t Side>
inline bool IsPriceMovingAgainst(hf::price_t price, hf::price_t another);

template <>
inline bool IsPriceMovingAgainst<BID>(hf::price_t price, hf::price_t another) {
    return price < another;
}

template <>
inline bool IsPriceMovingAgainst<ASK>(hf::price_t price, hf::price_t another) {
    return price > another;
}

template <uint32_t Side>
inline hf::price_t GetNextPrice(hf::price_t price, hf::price_t tickSize);

template <>
inline hf::price_t GetNextPrice<BID>(hf::price_t price, hf::price_t tickSize) {
    return price + tickSize;
}

template <>
inline hf::price_t GetNextPrice<ASK>(hf::price_t price, hf::price_t tickSize) {
    return price - tickSize;
}

template <uint32_t Side>
inline hf::price_t GetPriorPrice(hf::price_t price, hf::price_t tickSize);

template <>
inline hf::price_t GetPriorPrice<BID>(hf::price_t price, hf::price_t tickSize) {
    return price - tickSize;
}

template <>
inline hf::price_t GetPriorPrice<ASK>(hf::price_t price, hf::price_t tickSize) {
    return price + tickSize;
}

inline hf::SIDE ToRhoneSide(uint32_t side) {
    return side == BID ? hf::SIDE::BUY : hf::SIDE::SELL;
}

inline uint32_t GetOtherSide(uint32_t side) {
    return side == 0 ? 1 : 0;
}

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

const static double EPSILON = 0.00000001;

inline bool IsZero(double value) {
    return  value > 0 ? value <  EPSILON : value > -EPSILON;
}

inline bool IsSame(double value1, double value2) {
    const double diff = value1 - value2;
    return IsZero(diff);
}

inline int32_t ToIntByPoint_1(double value) {
    if (value > 0) {
        return (int32_t) value + 0.1;
    } else {
        return (int32_t) value - 0.1;
    }
}

inline hf::price_t ToRhonePrice(double price) {
    return price * 10000000;
}

inline double ToDollars(hf::price_t price) {
    return static_cast<double>(price) / 10000000.0;
}

struct Book {
    hf::price_t _price = hf::NULL_PRICE;
    hf::qty_t _otherQuantity = 0;
    md::order_count_t _otherOrderCount = 0;
    hf::qty_t _ourQuantity = 0;
    md::order_count_t _ourOrderCount = 0;
};

typedef uint64_t OrderId_t;

enum OrderIds {ORDER_ID_RV_INTERNAL_QUOTE = 0, ORDER_ID_MM_QUOTE = 1, ORDER_ID_MM_HEDGE = 2};

struct PriceLevel;

struct Order {
    enum Type {Hedging, Quoting};
    enum Strategy {MarketMaker, RelativeValue, Stacker};

    OrderId_t _id;
    Strategy _strategy;
    Type _type;

    uint32_t _side;

    hf::price_t _price;
    hf::qty_t _size;

    hf::price_t _externalPrice;
    hf::qty_t _externalSize;

    PriceLevel* priceLevel;

    bool _isInBook = false;
};

struct PriceLevel {
    hf::price_t _price;
    hf::qty_t _size;
    std::vector<Order*> _orders;

    void remove(Order& order) {
        for (auto it = _orders.begin(); it != _orders.end(); ++it) {
            Order* o = *it;
            if (o->_id == order._id) {
                _orders.erase(it);
                break;
            }
        }
        _size -= order._externalSize;
    }
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
    Login, Logout, ClientDown, GridUpdate, MarketUpdate, SwitchChange, HedgeOrder, QuoteOrder, Fill,
    GridUpdateAccepted, GridUpdateRejected, SwitchChangeReply, PositionUpdate, STFUpdate, QuotesUpdate, OverdeterminedPricesUpdate, LTFUpdate
};

struct ViperMessage {
    ViperMessageType _type;
    char _data[0];
};

struct GridUpdateMessage {
    GridUpdate* _gridUpdate;
};

struct GenericMessage {
    int64_t _number_1;
    int64_t _number_2;
    int64_t _number_3;
    int64_t _number_4;
};

struct MarketUpdate {
    uint32_t _instrumentIndex;
    uint32_t _marketId;
    uint32_t _side;
    hf::price_t _price;
    hf::qty_t _size;
};

struct HedgeOrderMessage {
    uint32_t _instrumentIndex;
    uint32_t _side;
    hf::price_t _price;
    hf::qty_t _size;
    bool _isExternal;
};

struct FillMessage {
    uint32_t _contractIndex;
    uint32_t _side;
    hf::price_t _price;
    hf::qty_t _size;
};

struct QuoteOrderMessage {
    uint32_t _instrumentIndex;
    uint32_t _side;

    hf::price_t _internalMarketPrice;
    hf::qty_t _internalMarketSize;

    hf::price_t _externalPrice;
    hf::qty_t _externalSize;
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
