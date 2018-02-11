#ifndef VIPER_COMMON_H
#define VIPER_COMMON_H

#include <md/md.h>
#include <functional>
#include <limits>
#include <hf/pool/DataPool.h>
#include <hf/queue/U64Queue.h>
#include "messages/omsproto.pb.h"

namespace Python {

enum class VectorParametersColumns {Position, Mkt, STF, LTF, MaxPos, Range, Step, EMA_ON, EMA_Time, SetToMarket};
enum class ContractParameterRVColumns {Position, BidLean, BidPrice, BidCross, AskPrice, AskCross, BidQty, AskQty, STF, Mkt, QuoteExternal, HedgeTolerance, Commission, AskLean, Width};
enum class PricingConfigsColumns {Weight, UseWeighted, EstablishedLevel};
enum class ManualFillsEntryColumns {Contract, Size, Strategy};
enum class MainControlAction {Enabled, Quoting};
enum class MainControlButtons {Enable, Disable, HardShutdown, CheckQuotes};
enum class FillsColumns {Time, Contract, Side, Price, Size, Strategy, Type};
enum class ErrorsColumns {Time, Message, Type};
enum class GatewayStatusColumns {Exchange, Status};
enum class MarketMakerParametersColumns {QuoteExternal, QuoteSize, Width, JoinSize, LeaveSize, PayUpQty, PayUpOrders, PayUpMultiple, MaxPosInterrnal, MaxPosExternal, Commission, Enabled,
                                         Position, BidPrice, AskPrice, BidQty, AskQty};
enum class StackerParametersColumns {Enabled, QuoteSize, StackLevels};
enum class PNLColumns {Open, Closed, Total, Drawdown, PnlLimit, DrawdownLimit};
enum class PNLRows {__start_marker__, RelativeValue, MarketMakers, Execution, Total, __end_marker__};
enum class PnlDetailColumns {MMOpen, MMClosed, RVOpen, RVClosed, ExecOpen, ExecClosed};
enum class GlobalConfigsColumns {Key, Value};
enum class MessageCountsColumns {Inserts, Modifies, Cancels};

enum class WindowIds {VectorParameters = 1, ContractParametersRv = 2, LeanMatrix = 3, PricingConfig = 4, PricingMatrix = 5, RiskMatrix = 6, MainControl = 7, ManualFillsEntry = 8,
    FillsDisplay = 9, Errors = 10, GatewayStatuses = 11, MarketMakerParameters = 12, StackerParameters = 13, GlobalConfigs = 14, PNL = 15, PnlDetail = 16, MessagesCounts = 17};

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

template <uint32_t Side>
inline bool EqualOrBelow(hf::price_t one, hf::price_t two);

template <>
inline bool EqualOrBelow<BID>(hf::price_t one, hf::price_t two) {
    return one <= two;
}

template <>
inline bool EqualOrBelow<ASK>(hf::price_t one, hf::price_t two) {
    return two <= one;
}

template <uint32_t Side>
inline bool IsBetterPrice(hf::price_t one, hf::price_t two);

template <>
inline bool IsBetterPrice<BID>(hf::price_t one, hf::price_t two) {
    return one > two;
}

template <>
inline bool IsBetterPrice<ASK>(hf::price_t one, hf::price_t two) {
    return one < two;
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

enum ice_rejects : uint8_t {
    TOO_LATE_TO_CANCEL = 0,
    UNKNOWN_ORDER = 1
};

enum class SystemState {LoggedOut, LoggedIn, CheckQuotes, Enabled, Disabled, HardShutdown};
enum class StrategyId {MarketMaker, RelativeValue, Execution};
enum class DataSeries {VectorPrice, LTF, STF};

struct Market {
    hf::price_t _price = hf::NULL_PRICE;
    hf::qty_t _size = 0;
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

    void clear() {
        _ready = true;
    }
};

inline uint64_t GetCyclesFromSeconds(double seconds, double frequency) {
    return static_cast<uint64_t>(seconds * 1000000000 * frequency);
}

inline uint64_t GetCyclesFromMicroSeconds(uint64_t microSeconds, double frequency) {
    return static_cast<uint64_t>(frequency * microSeconds * 1000);
}

inline uint64_t GetCyclesFromMilliSeconds(uint64_t microSeconds, double frequency) {
    return static_cast<uint64_t>(frequency * microSeconds * 1000000);
}

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

inline uint32_t ToViperSide(hf::SIDE side) {
    return side == hf::SIDE::BUY ? 1 : 0;
}

struct Book {
    hf::price_t _price = hf::NULL_PRICE;
    hf::qty_t _otherQuantity = 0;
    md::order_count_t _otherOrderCount = 0;
};

typedef uint64_t OrderId_t;

enum OrderIds {ORDER_ID_RV_INTERNAL_QUOTE = 0, ORDER_ID_MM_QUOTE = 1, ORDER_ID_MM_HEDGE = 2};

enum class FillType {Internal, External, ExternalOverFill};
enum class ErrorType {Critical, Normal};
enum class YesNoType {Unknown, Yes, No};

inline std::string getStrategyString(StrategyId id) {
    switch(id) {
        case StrategyId::MarketMaker: {
            return "MM";
        }
        case StrategyId::RelativeValue: {
            return "RV";
        }
        case StrategyId::Execution: {
            return "EX";
        }
    }
    return "";
}

inline std::string getFillTypeString(FillType fillType) {
    switch (fillType) {
        case FillType::External: {
            return "External";
        }
        case FillType::Internal: {
            return "Internal";
        }
        case FillType::ExternalOverFill: {
            return "ExternalOverFill";
        }
    }
    return "";
}

inline std::string getSideString(uint32_t side) {
    switch (side) {
        case BID: {
            return "BID";
        }
        case ASK: {
            return "ASK";
        }
        default:
            return "";
    }
}

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

    void addOrder(Order* order) {
        _orders.push_back(order);
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
    Login, Logout, ClientDown, GridUpdate, MarketUpdate, SwitchChange, HedgeOrder, QuoteOrder, Fill, MarketMakerEnabled, ErrorMessage, GuiInitialized, DisableAll, GetOutOfMarket,
    GridUpdateAccepted, GridUpdateRejected, SwitchChangeReply, PositionUpdate, STFUpdate, QuotesUpdate, OverdeterminedPricesUpdate, LTFUpdate, LogoutReply, LoginReply, GatewayStatus
};

enum class SoundId {Enabled = 1, Fill, Error};

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
};

struct FillMessage {
    uint32_t _contractId;
    uint32_t _side;
    hf::price_t _price;
    hf::qty_t _qty;
    StrategyId _strategy;
    FillType _type;
};

struct GatewayStatusMessage {
    bool _active;
    char _message[65];
};

static constexpr uint32_t CRITICAL_MESSAGE_MAX_SIZE = 100;

struct ErrorMessage {
    char _message[CRITICAL_MESSAGE_MAX_SIZE];
    uint32_t _size;
    ErrorType _type;
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

struct QuoteOrderMessage {
    uint32_t _instrumentIndex;
    uint32_t _side;
    hf::price_t _internalMarketPrice;
    hf::qty_t _internalMarketSize;
    hf::price_t _externalPrice;
    hf::qty_t _externalSize;
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
