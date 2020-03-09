#ifndef VIPER_COMMON_H
#define VIPER_COMMON_H

#include "boost/format.hpp"
#include <md/md.h>
#include <functional>
#include <limits>
#include <hf/pool/DataPool.h>
#include <hf/queue/U64Queue.h>
#include <om/om.h>
#include <queue>
#include <boost/assert.hpp>
#include <boost/format.hpp>
#include "messages/python.pb.h"
#include "logging/Coolog.h"
#include "om/om_reject_codes.h"
#include <experimental/string_view>
#include <atomic>
#include <bitset>

namespace Python {

extern const std::string ViperVersion;
extern const std::string ViperGitHash;
extern const std::string RhonePublicGitHash;

static const uint32_t INTERNAL_ID = 0;
static const uint32_t LEVEL_POINTER = 1;
static const uint32_t PRIOR_QUANTITY = 2;
static const uint32_t MODIFY_TRACKER = 2;
static const uint32_t QUANTITY_TO_MODIFY = 3;
static const uint32_t IS_ACKED = 4;
static const uint32_t ORDER_OWNER = 5;
static const uint32_t ROUTE = 6;
static const uint32_t PRIOR_LEVEL = 7;
static const uint32_t POTENTIAL_OVERFILL_QTY = 8; //Theoretically can combine with IS_ACKED
static const uint32_t STACKER_ORDER_FLAGS = 9;
static const uint32_t IS_HEDGE = 10;

//Bits set with STACKER_ORDER_FLAGS
static const uint8_t SOF_DECREMENT_FAILED = 0;
static const uint8_t EXPLICIT_OUT_OF_BAND_CANCEL = 1;

//Order Manager Fields
static const uint32_t OM_STACK = 0;
static const uint32_t OM_HEDGE= 1;

//Order Owner
static const uint32_t OM_OWNER = 0; //We rely on this being the default.  Probably shouldn't
static const uint32_t GTC_OWNER = 1;
static const uint32_t UNASSIGNED_OWNER = 2;

enum class VectorParametersColumns {Position, Mkt, STF, LTF, MaxPos, Range, EMA_ON, EMA_Time, SetToMarket, Hedged};
enum class ContractParameterRVColumns {Position, BidLean, BidLeanType, BidInternalPrice, BidExternalPrice, BidCross, AskInternalPrice, AskExternalPrice, AskCross,
                               BidInternalQty, BidExternalQty, AskInternalQty, AskExternalQty, STF, Mkt, QuoteExternal, HedgeTolerance, Commission, AskLean, AskLeanType,
                               Width, ExtraWidth, PCTStfMove, MaxTicksThroughMarket, StaticQuote, QuoteSize, MaxTicksWide};
enum class PricingConfigsColumns {Weight, UseWeighted, EstablishedLevel, PricingMultiplier, QuantityMultiplier, SweepTicks, SweepQty};
enum class ManualFillsEntryColumns {Contract, Size, Type};
enum class MainControlAction {Enabled};
enum class MainControlButtons {Enable, Disable, HardShutdown, CheckQuotes, CancelGTC};
enum class FillsColumns {Number, Time, Contract, Side, Price, Size, Strategy, Type, Source, EdgeStf, EdgeMkt};
enum class ErrorsColumns {Time, Message, Type};
enum class GatewayStatusColumns {Exchange, Status};
enum class MarketMakerParametersColumns {QuoteExternal, QuoteSize, Width, HedgeOffset, JoinSize, LeaveSize, PayUpQty, PayUpOrders, PayUpMultiple, MaxPosInterrnal, MaxPosExternal, Commission, Enabled,
                                         InternalPosition, ExternalPosition, BidInternalPrice, BidExternalPrice, AskInternalPrice, AskExternalPrice, BidInternalQty, BidExternalQty, AskInternalQty, AskExternalQty,
                                        MinInternalPriceChange, EstablishedLevel, MidPoint, BidHedgeInternalPrice, BidHedgeExternalPrice, BidHedgeSize, AskHedgeInternalPrice,
                                        AskHedgeExternalPrice, AskHedgeSize, MaxTicksWide};
enum class PositionsColumns {RV, MMInternal, MMExternal, Total};
enum class StackerParametersColumns {Enabled, QuoteSize, StackLevels, StackSlack, MaxTicksOffTopLevel, UseGTC};
enum class PNLColumns {Open, Closed, Total, Drawdown, PnlLimit, DrawdownLimit};
enum class PNLRows {__start_marker__, RelativeValue, MarketMakers, Execution, Total, __end_marker__};
enum class PnlDetailColumns {MMOpen, MMClosed, RVOpen, RVClosed, Total, MMTotal, RVTotal};
enum class GlobalConfigsColumns {Value};
enum class MessageCountsColumns {Inserts, Modifies, Cancels, CancelRejects, ModifyRejects, Fills, FillsRatio};
enum class TraderSwitchColumns {Trader};
enum class Traders {Steven, Mark, Taj, Zhi};
enum class AuditColumns {Time, Trader, Grid, Row, Column, OldValue, NewValue};
enum class GTCManagementColumns {PreOpenBid, PreOpenAsk, IOP,  NumBid, NumOffer, NumTotal, CancelAllBid, Cancel_2_Bid, Cancel_1_Bid, TicksThroughBid, isCrossedBid, isCrossedAsk, TicksThroughAsk, Cancel_1_Ask, Cancel_2_Ask, CancelAllAsk, CancelAll};

enum class WindowIds {VectorParameters = 1, ContractParametersRv = 2, LeanMatrix = 3, PricingConfig = 4, PricingMatrix = 5, RiskMatrix = 6, MainControl = 7, ManualFillsEntry = 8,
    FillsDisplay = 9, Errors = 10, GatewayStatuses = 11, MarketMakerParameters = 12, StackerParameters = 13, GlobalConfigs = 14, PNL = 15, PnlDetail = 16, MessagesCounts = 17,
    HedgeRatios = 18, QuoteSizes = 19, Commissions = 20, PositionInOutrights = 21, TraderSwitch = 22, FactorHedged = 23, Audit = 24, GTCManagement = 25};

enum class Color { TRANSPARENT = 1, TRANSPARENT_RED = 2, TRANSPARENT_BID_BLUE=3, TRANSPARENT_OFFER_RED=4};

inline constexpr const char* ColorDefinition(Color color) {
    switch(color) {
        case Color::TRANSPARENT:
            return "#00FFFFFF";
        case Color::TRANSPARENT_BID_BLUE:
            return "#E6AFDBE8"; //a=90%
        case Color::TRANSPARENT_OFFER_RED:
            return "#E6FFBFCB"; //a=90%
        case Color::TRANSPARENT_RED:
            FALLTHROUGH;
        default:
            return "#BFFF0000"; //a=75%
    }
}

template<proto::DataType dt>
inline constexpr const char* NoValue() {
    static_assert(dt == proto::DataType::unsigned_type || dt == proto::DataType::signed_type, "Unsupported type");

    switch(dt) {
        case proto::DataType::unsigned_type:
            return "18446744073709551615";
        case proto::DataType::signed_type:
            return "9223372036854775807";
    }
    return "";
}

enum class TimerType : uint32_t  {FillBanner, QuoteBanner, HedgeBanner, DoNothing};

static const uint32_t STF_TO_MARKET = 1;

#define mb() asm volatile("mfence":::"memory")

static const uint32_t BID = 0;
static const uint32_t ASK = 1;

inline hf::price_t ToRhonePrice(double price) {
#ifdef VIPER_UNIT_TEST
    return (price * 100000000) + 0.5;
#else
    return price * 100000000;
#endif
}

inline double ToDollars(hf::price_t price) {
    return static_cast<double>(price) / 100000000.0;
}

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

inline bool IsBetter(hf::price_t lhs, hf::price_t rhs, uint32_t side) {
    return side == BID ? lhs > rhs : lhs < rhs;
}

inline bool IsBetterOrEqual(hf::price_t lhs, hf::price_t rhs, uint32_t side) {
    return side == BID ? lhs >= rhs : lhs <= rhs;
}

inline int64_t GetQuantity(const om::order_t& order, uint32_t key) {
    return  order.debug.i[key];
}

inline void SetQuantity(const om::order_t& order, int64_t qty, uint32_t key) {
    om::order_t& mutableOrder = const_cast<om::order_t&>(order);
    mutableOrder.debug.i[key] = qty;
}

//Added to modify the STACKER_ORDER_FLAGS bits
inline void SetBit(const om::order_t& order, uint32_t key, uint8_t offset) {
    om::order_t& mutableOrder = const_cast<om::order_t &>(order);
    mutableOrder.debug.i[key] |= static_cast<int64_t>(1) << offset;
}

inline void ClearBit(const om::order_t& order, uint32_t key, uint8_t offset) {
    om::order_t& mutableOrder = const_cast<om::order_t &>(order);
    mutableOrder.debug.i[key] &= ~(static_cast<int64_t>(1) << offset);
}

inline bool IsBitSet(const om::order_t& order, uint32_t key, uint8_t offset) {
    return order.debug.i[key] & (static_cast<int64_t>(1) << offset);
}

inline bool TestAndSetBit(const om::order_t& order, uint32_t key, uint8_t offset) {
    bool isSet = IsBitSet(order, key, offset);
    SetBit(order, key, offset);
    return isSet;
}

inline bool TestAndUnsetBit(const om::order_t& order, uint32_t key, uint8_t offset) {
    bool isSet = IsBitSet(order, key, offset);
    ClearBit(order, key, offset);
    return isSet;
}
//End "STACKER_ORDER_FLAGS" Region

inline hf::SIDE ToRhoneSide(uint32_t side) {
    return side == BID ? hf::SIDE::BUY : hf::SIDE::SELL;
}

inline uint32_t GetOtherSide(uint32_t side) {
    return 1 - side;
}

template <typename EnumT>
int32_t toInt(EnumT value) {
    return static_cast<int32_t>(value);
}

template <typename EnumT>
constexpr uint32_t toUInt(EnumT value) {
    return static_cast<uint32_t>(value);
}

enum ice_rejects : uint8_t {
    TOO_LATE_TO_CANCEL = 0,
    UNKNOWN_ORDER = 1,
    BROKER_OPTION = 2,
    ORDER_IN_PENDING_CANCEL_OR_PENDING_REPLACE = 3,
    UNABLE_TO_PROCESS_REQUEST = 4,
    DUPLICATE_CLORDID = 6,
    USER_DEFINED = 9
};

constexpr auto CME_ORDER_IS_NOT_IN_BOOK = 2045;

inline bool WasRejectedDueToBeingFilled(uint32_t rejectCode, const char* rejectText, hf::VENUE venue) {
    switch (venue) {
        case hf::VENUE::ICE: {
            return (rejectCode == ice_rejects::TOO_LATE_TO_CANCEL) | (rejectCode == ice_rejects::UNKNOWN_ORDER) | (rejectText[11] == 'I') | (rejectCode == om::toUType(om::OrderRejectReason::OrderNotActive));
        }
        case hf::VENUE::CME: {
            return (rejectCode == CME_ORDER_IS_NOT_IN_BOOK) | (rejectCode == om::toUType(om::OrderRejectReason::OrderNotActive));
        }
        default:
            throw std::runtime_error("reject - unhandled X");
    }
}

enum class SystemState {LoggedOut, LoggedIn, CheckQuotes, Enabled, Disabled, HardShutdown, CancelGtcAndDisable};
enum class StrategyId {MarketMaker, RelativeValue, Execution};
enum class DataSeries {VectorPrice, LTF, STF};

struct SharedSystemState {
    using QuoteExtBitset = std::bitset<static_cast<uint8_t>(hf::VENUE::TEST)>;
    std::atomic<SystemState> _relativeValue { SystemState::LoggedOut };
    std::atomic<SystemState> _marketMaker { SystemState::LoggedOut };
    std::atomic<QuoteExtBitset> _quoteExternally {0};
    std::atomic_bool _rvGated { false };
};

inline SystemState ToSystemStateFromButtonId(uint32_t buttonId) {
    switch (buttonId) {
        case toUInt(MainControlButtons::CheckQuotes): {
            return SystemState::CheckQuotes;
        }
        case toUInt(MainControlButtons::Enable): {
            return SystemState::Enabled;
        }
        case toUInt(MainControlButtons::Disable): {
            return SystemState::Disabled;
        }
        case toUInt(MainControlButtons::CancelGTC): {
            return SystemState::CancelGtcAndDisable;
        }
        case toUInt(MainControlButtons::HardShutdown): {
            return SystemState::HardShutdown;
        }
        default:
            return SystemState::Disabled;
    }
}

struct SizeAdjustment {
    uint32_t _hedgeId;
    hf::qty_t _adjustment  = 0;
    om::order_id_t _orderId = om::NULL_ORDERID;
};

struct Market {

    Market() {
        _sizeAdjustments.reserve(128);
    }

    template<uint32_t SIDE>
    void update(hf::price_t price, hf::qty_t size) {
        if (_price != price) {
            _sizeAdjustment = 0;
            _sizeAdjustments.clear();
            LOG(0, "FAK {C[i1]} Id={i2} Side={i3} Prices different. Flushing adjustments.", _contractId, _id, SIDE)
        }
        _price = price;
        _size = size;
        LOG(0, "FAK {C[i1]} Id={i2} Side={i3} Price={d1} AdjustedSize={i4}", _contractId, _id, ToDollars(_price), SIDE, getFakAdjustedSize())
    }

    template<uint32_t SIDE>
    void updateInternalMarket(hf::price_t price, hf::qty_t size) {
        _price = price;
        _size = size;
        LOG(0, "FAK {C[i1]} Id={i2} Side={i3} Price={d1} AdjustedSize={i4}", _contractId, _id, ToDollars(_price), SIDE, getFakAdjustedSize())
    }

    void handleInternalHedgeDone(uint32_t hedgeId) {
        for (auto it = _sizeAdjustments.begin(); it != _sizeAdjustments.end(); ++it) {
            SizeAdjustment& adj = *it;
            if (adj._hedgeId == hedgeId) {
                _sizeAdjustment -= adj._adjustment;
                LOG(0, "FAK {C[i1]} Id={i2} hedge adjustment. Done. HedgeId={i3} Adj={i4} Price={d1} AdjustedSize={i5}", _contractId, _id, adj._hedgeId, adj._adjustment, ToDollars(_price), getFakAdjustedSize())
                _sizeAdjustments.erase(it);
                break;
            }
        }
    }

    void handleFakDone(om::order_id_t id) {
        for (auto it = _sizeAdjustments.begin(); it != _sizeAdjustments.end(); ++it) {
            SizeAdjustment& adj = *it;
            if (adj._orderId == id) {
                _sizeAdjustment -= adj._adjustment;
                LOG(0, "FAK {C[i1]} Id={i2} Done. HedgeId={i3} Adj={i4} OrderId={i5} Price={d1} AdjustedSize={i6}", _contractId, _id, adj._hedgeId, adj._adjustment, id, ToDollars(_price), getFakAdjustedSize())
                _sizeAdjustments.erase(it);
                break;
            }
        }
    }

    void ackAdjustment(uint32_t hedgeId, hf::qty_t size, om::order_id_t orderId) {
        for (auto it = _sizeAdjustments.begin(); it != _sizeAdjustments.end(); ++it) {
            SizeAdjustment& adj = *it;
            if (adj._hedgeId > hedgeId) {
                break;
            } else if (adj._hedgeId == hedgeId) {
                if (size != 0) {
                    adj._orderId = orderId;
                    if (adj._adjustment != size) {
                        _sizeAdjustment -= adj._adjustment - size;
                        adj._adjustment = size;
                    }
                } else {
                    _sizeAdjustments.erase(it);
                }
                LOG(0, "FAK {C[i1]} Id={i2} Acked md adjustment. HedgeId={i3} Adj={i4} OrderId={i5} Price={d1} AdjustedSize={i6}", _contractId, _id, hedgeId, size, (uint32_t) orderId, ToDollars(_price), getFakAdjustedSize())
                break;
            }
        }
    }

    void addAdjustment(uint32_t hedgeId, hf::qty_t adj) {
        _sizeAdjustment += adj;
        _sizeAdjustments.push_back({hedgeId, adj, om::NULL_ORDERID});
        LOG(0, "FAK {C[i1]} Id={i2} Adding md adjustment. HedgeId={i3} Adj={i4} Price={d1} AdjustedSize={i5}", _contractId, _id, hedgeId, adj, ToDollars(_price), getFakAdjustedSize())
    }

    hf::qty_t getFakAdjustedSize() {
        const hf::qty_t size = _size - _sizeAdjustment;
        if (size < 0) {
            LOG(0, "FAK {C[i1]} Id={i2} getFakAdjustedSize(). Is negative Adj={i3} Price={d1} Size={i4}", _contractId, _id, _sizeAdjustment, ToDollars(_price), _size)
            return 0;
        } else {
            return size;
        }
    }

    hf::qty_t getUnAdjustedSize() const {
        return _size;
    }

    hf::price_t getPrice() const {
        return _price;
    }

    uint32_t getId() {
        return _id;
    }

    void setId(uint32_t id) {
        _id = id;
    }

    void setContractId(uint32_t id) {
        _contractId = id;
    }

private:
    uint32_t _id;   // 0: MM_Hedge; 1: MM_Quote; 2: External
    hf::price_t _price = hf::NULL_PRICE;
    hf::qty_t _size = 0;
    hf::qty_t _sizeAdjustment = 0;
    std::vector<SizeAdjustment> _sizeAdjustments;
    uint32_t _contractId;
};

enum OrderIds {ORDER_ID_MM_HEDGE = 0, ORDER_ID_MM_QUOTE = 1, ORDER_ID_RV_INTERNAL_QUOTE = 2};
enum MarketIds {MARKET_ID_MM_HEDGE = 0, MARKET_ID_MM_QUOTE = 1, MARKET_ID_EXTERNAL = 2};

struct Markets {

    Markets() {
        _markets[0].setId(MarketIds::MARKET_ID_MM_HEDGE);
        _markets[1].setId(MarketIds::MARKET_ID_MM_QUOTE);
    }

    Market& getMarket(uint32_t id) {
        return _markets[id];
    }

    void setContractId(uint32_t id) {
        _markets[0].setContractId(id);
        _markets[1].setContractId(id);
    }

    Market _markets[2];
    static constexpr uint32_t _numberOfMarkets = 2;
};

struct GridUpdate {
    proto::GridUpdate _update;
    std::vector<proto::GridCell> _oldValues;
    bool _ready = true;

    GridUpdate() {
        _update.set_updatetype(proto::GridUpdateType::CellsType);
    }

    void clear() {
        _ready = true;
        _oldValues.clear();
    }
};

inline uint64_t GetMicrosFromCycles(uint64_t cycles, double frequency) {
    return cycles / static_cast<uint64_t>(frequency * 1000.0);
}

inline uint64_t GetMillisFromCycles(uint64_t cycles, double frequency) {
    return cycles / static_cast<uint64_t>(frequency * 1000000);
}

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

template<uint32_t nDecimal>
inline double Round(double value) {
    double denominator = pow(10, nDecimal);
    int64_t val = round(value * denominator);
    return (double)val / denominator;
}

inline int32_t ToIntByPoint_1(double value) {
    if (value > 0) {
        return (int32_t) value + 0.1;
    } else {
        return (int32_t) value - 0.1;
    }
}

inline uint32_t ToViperSide(hf::SIDE side) {
    return side == hf::SIDE::BUY ? 0 : 1;
}

struct Book {
    hf::price_t _price = hf::NULL_PRICE;
    hf::qty_t _otherQuantity = 0;
    md::order_count_t _otherOrderCount = 0;
};

typedef uint64_t OrderId_t;

enum class ManualFillType {RelativeValue, MarketMaker};
enum class FillType {Internal, External};
enum class FillSource {Quote, MissedHedge, Overfill, Hedge, SelfMatch, RvDisable, RvSelfMatch, ViperOff};
enum class ErrorType {Critical, Normal};
enum class YesNoType {Unknown, Yes, No};
enum class LeanType {External, MM_Quote, MM_Hedge, None};

inline std::string GetStrategyString(StrategyId id) {
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

inline std::string GetFillTypeString(FillType fillType) {
    switch (fillType) {
        case FillType::External: {
            return "External";
        }
        case FillType::Internal: {
            return "Internal";
        }
    }
    return "";
}

inline std::string GetSideString(uint32_t side) {
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

inline std::string GetSystemStateString(SystemState state) {
    switch (state) {
        case SystemState::Disabled: {
            return "Disabled";
        }
        case SystemState::Enabled: {
            return "Enabled";
        }
        case SystemState::HardShutdown: {
            return "HardShutdown";
        }
        case SystemState::LoggedIn: {
            return "LoggedIn";
        }
        case SystemState::LoggedOut: {
            return "LoggedOut";
        }
        case SystemState::CheckQuotes: {
            return "CheckQuotes";
        }
        case SystemState::CancelGtcAndDisable : {
            return "CancelGtcAndDisable";
        }
    }
    return "";
}

struct PriceLevel;

struct Order {
    enum Type {Hedging, Quoting};
    enum Strategy {MarketMaker, RelativeValue};

    OrderId_t _id;
    Strategy _strategy;
    Type _type;
    uint32_t _matchOrder;

    uint32_t _side;

    hf::price_t _price;
    hf::qty_t _size;

    hf::price_t _externalPrice;
    hf::qty_t _externalSize;

    PriceLevel* priceLevel = nullptr;

    bool _isInBook = false;
    bool _inPayUp = false;
};

struct PriceLevel {
    hf::price_t _price;
    std::vector<Order*> _orders;

    hf::qty_t getSize() {
        const std::size_t size = _orders.size();
        if (size == 1) {
            return _orders[0]->_externalSize;
        } else if (size == 2) {
            Order* order = _orders[0]->_strategy == Order::Strategy::MarketMaker ? _orders[0] : _orders[1];
            return order->_externalSize;
        }
        return 0;
    }

    void remove(Order& order) {
        MAYBE_UNUSED bool orderRemoved = false;
        for (auto it = _orders.begin(); it != _orders.end(); ++it) {
            Order* o = *it;
            if (o->_id == order._id) {
                _orders.erase(it);
                o->priceLevel = nullptr;
                orderRemoved = true;
                break;
            }
        }
        BOOST_ASSERT(orderRemoved);
    }

    void addOrder(Order* order) {
        const uint32_t size = (uint32_t) _orders.size();
        switch (size) {
            case 0: {
                _orders.push_back(order);
                break;
            }
            case 1: {
                if (_orders[0]->_matchOrder < order->_matchOrder) {
                    _orders.push_back(order);
                } else {
                    _orders.push_back(_orders[0]);
                    _orders[0] = order;
                }
                break;
            }
            case 2: {
                if (_orders[0]->_matchOrder < order->_matchOrder) {
                    if (_orders[1]->_matchOrder < order->_matchOrder) {
                        _orders.push_back(order);
                    } else {
                        _orders.push_back(_orders[1]);
                        _orders[1] = order;
                    }
                } else {
                    _orders.push_back(_orders[1]);
                    _orders[1] = _orders[0];
                    _orders[0] = order;
                }
                break;
            }
            default:
                BOOST_ASSERT(false);
        }
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

inline bool IsSelfMatchReject(hf::VENUE venue, uint32_t code) {
    switch (venue) {
        case hf::VENUE::ICE:
            return (code == 11) | (code == 9);
        case hf::VENUE::CME:
            //Fairly certain this does not work
            return code == 11;
        default:
            throw std::runtime_error("unhandled venue");
    }
}

inline bool IsRejectDueToMarketClosed(hf::VENUE venue, uint32_t code) {
    switch (venue) {
        case hf::VENUE::CME:
            return code == 1003;
        case hf::VENUE::ICE:
            return code == 2;
        default:
            throw std::runtime_error("unhandled venue");
    }
}

inline bool IsExternalUnsolicitedCancel(hf::VENUE venue, uint32_t code, const char* rtxt) {
    switch(venue) {
        case hf::VENUE::CME:
            return false; //todo -- need to discover the actual code
        case hf::VENUE::ICE:
            return !std::memcmp(rtxt, "Kill", 4);
        default:
            throw std::runtime_error("unhandled venue");
    }
}

inline bool IsSMPUnsolicitedCancel(hf::VENUE venue, uint32_t code, const char* reasonText) {
    if (venue == hf::VENUE::CME) {
        return code == 103; // cancel resting on CME
    }
    return !std::memcmp(reasonText, "noSelfTrading", 4);
}

inline bool IsICESMPModifyReject(const char * reasonText) {
    return !std::memcmp(reasonText, "Trad", 4);
}

inline std::string GetGatewayRejectCodeString(uint32_t code) {
    switch (code) {
        case toUInt(om::OrderRejectReason::MaxPositionViolatedOnBid):
            return "MaxPositionBid";
        case toUInt(om::OrderRejectReason::MaxPositionViolatedOnOffer):
            return "MaxPositionOffer";
        case toUInt(om::OrderRejectReason::MaxOrderQuantityViolated):
            return "MaxOrderQuantity";
        default:
            return "";
    }
}

inline std::string GetTraderName(Traders trader) {
    switch (trader) {
        case Traders::Mark:
            return "Mark";
        case Traders::Steven:
            return "Steven";
        case Traders::Taj:
            return "Taj";
        case Traders::Zhi:
            return "Zhi";
        default:
            throw std::runtime_error("unknown trader");
    }
}

enum class ViperMessageType : uint32_t {
    Login, Logout, ClientDown, GridUpdate, MarketUpdate, HedgeOrder, QuoteOrder, Fill, MarketMakerEnabled, ErrorMessage, GuiInitialized, Quoting, AcceptedManualFill, InternalHedgeDone,
    GridUpdateAccepted, SwitchChangeReply, GatewayStatus, HedgeMDAdjsutAck, QuoteSizesUpdate, StoreLtf, InternalHeartbeat, RVQuoteExternallyOff, FillAllocation, FirstOrdersDone,
    EndOfQuotes,
    DisableExternalQuotingOnMarketMaker, //Used as a command as an an Ack between FS and MM.
    SweepConfigUpdate,
    MMQuoteExternallyOffAll,
    DisableMarketMakerInstrument,
    IOPUpdate, EndIOPTracking, AuctionMDUpdate,
    SetRhoneLimits,
    DisableUseGTC
};

enum class SoundId {Enabled = 0, Disable = 1, Shutdown = 2, CriticalMessage = 3, Fill = 4, NormalMessage = 5};

enum THROTTLING_MODE {THROTTLING_MODE_NONE, THROTTLING_MODE_DANGER_ZONE, THROTTLING_MODE_AT_CAPACITY};

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
};

struct FillMessage {
    uint32_t _contractId;
    uint32_t _side;
    hf::price_t _price;
    hf::qty_t _qty;
    hf::price_t _rvStf;
    StrategyId _strategy;
    FillType _type;
    FillSource _source;
    hf::qty_t _internalAllocation;
    hf::qty_t _externalAllocation;
    uint64_t _timestamp;
};

struct HedgeMarketDataAdjustmentAck {
    uint32_t _instrumentIndex;
    uint32_t _side;
    hf::qty_t _size;
    uint32_t _hedgeId;
    om::order_id_t _orderId;
};

struct InternalHedgeDone {
    uint32_t _instrumentIndex;
    uint32_t _marketIndex;
    uint32_t _side;
    uint32_t _hedgeId;
};

struct GatewayStatusMessage {
    bool _active;
    char _message[65];
};

struct IOPUpdate {
    uint32_t _internalId;
    hf::price_t price;
    int numBidGTCs;
    int numOfferGTCs;
    int ticksThroughBid;
    int ticksThroughAsk;
};

struct AuctionMDUpdate {
    uint32_t _internalId;
    hf::price_t _bid;
    hf::price_t _offer;
};

static constexpr uint32_t CRITICAL_MESSAGE_MAX_SIZE = 200;

struct ErrorMessage {
    char _message[CRITICAL_MESSAGE_MAX_SIZE];
    uint32_t _size;
    ErrorType _type;
};

struct MarketUpdate {
    uint32_t _instrumentIndex;
    uint32_t _marketId;
    hf::price_t _bidPrice;
    hf::qty_t _bidSize;
    hf::price_t _askPrice;
    hf::qty_t _askSize;
};

struct HedgeOrderMessage {
    uint32_t _instrumentIndex;
    uint32_t _marketId;
    uint32_t _side;
    hf::price_t _price;
    hf::qty_t _size;
    bool _isExternal;
    uint32_t _hedgeId;
    hf::price_t _stf;
};

struct QuoteOrderMessage {

    struct Quote {
        hf::price_t _internalMarketPrice;
        hf::qty_t _internalMarketSize;
        hf::price_t _externalPrice;
        hf::qty_t _externalSize;
    };

    uint32_t _instrumentIndex;
    bool _quoteExternally;
    Quote _quotes[2];
};

struct SweepConfig {
    uint32_t _internalId;
    bool _sweepTicksUpdated = false;
    uint32_t _sweepTicks;
    bool _sweepQtyUpdated = false;
    uint32_t _sweepQty;
};

struct RhoneLimit {
    uint32_t _internalId;
    int64_t _max_order_qty;
};

struct SetRhoneLimits {
    uint32_t numLimits;
    RhoneLimit _limits[0];
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

namespace DO_NOT_USE_FUNCTIONS_IN_THIS_NAMESPACE_DIRECTLY_IN_VIPER {

    //This is a quick and dirty way to make boost::format exception safe.  We cannot use this directly because in
    //this impl we assume that the format string is a compile-time constant.
    struct SafeBoostFormat {

        SafeBoostFormat(const char *format) : _formatString(format), _formatter(format) { }

        template<typename T>
        SafeBoostFormat& operator%(T &&t) {
            try {
                _formatter.operator % (std::forward<T>(t));
            }
            catch (const std::exception &ex) {
            }
            return *this;
        }

        std::string str() const {
            try {
                return _formatter.str();
            }
            catch (const std::exception &ex) {
                return "Exception when creating error message.  formatString=" + std::string(_formatString) + " what=" + ex.what();
            }
        }

        explicit operator std::string() const { return str(); }

        private:
            const char* _formatString; //We can assume this is a compile-time constant when constructed via macro
            boost::format _formatter;
    };
}

#define CREATE_SAFE_FORMATTER(formatString) \
    ( []() { \
        static_assert(__builtin_constant_p(formatString), "formatString must be compile-time const"); \
        return ::Python::DO_NOT_USE_FUNCTIONS_IN_THIS_NAMESPACE_DIRECTLY_IN_VIPER::SafeBoostFormat{formatString}; \
    }())
}

#endif //VIPER_COMMON_H
