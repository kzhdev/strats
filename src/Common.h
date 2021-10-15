#ifndef VIPER_COMMON_H
#define VIPER_COMMON_H

#include "boost/format.hpp"
#include <md/md.h>
#include <functional>
#include <limits>
#include <util/DataPool.h>
#include <util/U64Queue.h>
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
#include <md/md_event.h>
#include <unordered_set>

#define VIPER_LOG_IOP_UPDATES
#ifdef VIPER_LOG_IOP_UPDATES
#define IOP_LOG LOG
#else
#define LOG_IOP(...) do {} while(0)
#endif

namespace Python {

extern const std::string ViperVersion;
extern const std::string ViperGitHash;
extern const std::string RhonePublicGitHash;

static const uint32_t INTERNAL_ID = 0;
static const uint32_t LEVEL_POINTER = 1;
static const uint32_t PRIOR_QUANTITY = 2;
static const uint32_t MODIFY_TRACKER = 2;
static const uint32_t QUANTITY_TO_MODIFY = 3;
static const uint32_t LAST_QP_PRICE = 4;
static const uint32_t ORDER_OWNER = 5;
static const uint32_t ROUTE = 6;
static const uint32_t PRIOR_LEVEL = 7;
static const uint32_t POTENTIAL_OVERFILL_QTY = 8; //Theoretically can combine with SOF_IS_ACKED
static const uint32_t STACKER_ORDER_FLAGS = 9;
static const uint32_t IS_HEDGE = 10;
static const uint32_t HEDGE_ID = 11;
static const uint32_t TIMESTAMP = 12;

//Bits set with STACKER_ORDER_FLAGS
static const uint8_t SOF_DECREMENT_FAILED = 0;
static const uint8_t EXPLICIT_OUT_OF_BAND_CANCEL = 1;
static const uint8_t SOF_QUEUE_POSITION_UPDATED = 2;
static const uint8_t SOF_IS_ACKED = 3;
static const uint8_t SOF_IOP_ORDER_SHOULD_BE_CANCELLED = 4;

//Order Manager Fields
static const uint32_t OM_STACK = 0;
static const uint32_t OM_HEDGE= 1;
static const uint32_t OM_MAX_QUOTE = 2;

//Order Owner
static const uint32_t OM_OWNER = 0; //We rely on this being the default.  Probably shouldn't
static const uint32_t GTC_OWNER = 1;
static const uint32_t UNPROCESSABLE_RELOADED_ORDER = 2;
static const uint32_t UNASSIGNED_OWNER = 3; //Note this *must* be the last element or FAK code that assigns "UNASSIGNED_OWNER+Strategy" will fail

//Various *DOUBLE* Flags
static const uint32_t D_GTC_CR_RETRY_LIMIT = 9;

//Size of circular buffer used in NegativeScalp/FillCheckers
constexpr static uint32_t CHECKER_ENTRY_SIZE = 65536;
static_assert(CHECKER_ENTRY_SIZE && !(CHECKER_ENTRY_SIZE & (CHECKER_ENTRY_SIZE-1)), "CHECKER_ENTRY_SIZE must be power of 2");

enum class VectorParametersColumns {Position, Mkt, STF, LTF, MaxPos, Range, EMA_ON, EMA_Time, EMA_On_Time, EMA_Off_Time, SetToMarket, Hedged};
enum class ContractParameterRVColumns {BidLean, BidLeanType, BidInternalPrice, BidExternalPrice, BidCross, AskInternalPrice, AskExternalPrice, AskCross,
                               BidInternalQty, BidExternalQty, AskInternalQty, AskExternalQty, STF, Mkt, QuoteExternal, HedgeTolerance, Commission, AskLean, AskLeanType,
                               Width, ExtraWidth, PCTStfMove, MaxTicksThroughMarket, StaticQuote, QuoteSize};
enum class PricingConfigsColumns {Weight, UseWeighted, EstablishedLevel, PricingMultiplier, QuantityMultiplier, SweepTicks, SweepQty, MaxTicksWide, ASExtCommission};
enum class ManualFillsEntryColumns {Contract, Size, Type};
enum class MainControlAction {Enabled};
enum class MainControlButtons {Enable, Disable, HardShutdown, CheckQuotes, CancelGTC};
enum class FillsColumns {Number, Time, Contract, Side, Price, Size, Strategy, Type, Source, EdgeStf, EdgeMkt};
enum class ErrorsColumns {Time, Message, Type};
enum class GatewayStatusColumns {Exchange, Status};
enum class MarketMakerParametersColumns {
    QuoteExternal,
    QuoteSize,
    Width,
    AsiaWidth,
    EuroWidth,
    USWidth,
    SafeModeWidth, // these Width enums needs to stay together
    HedgeOffset,
    JoinSize,
    LeaveSize,

    PayUpQty,
    PayUpOrders,
    PayUpMultiple,
    MaxPosInterrnal,
    MaxPosExternal,
    Commission,
    Enabled,
    InternalPosition,
    ExternalPosition,
    BidInternalPrice,

    BidExternalPrice,
    AskInternalPrice,
    AskExternalPrice,
    BidInternalQty,
    BidExternalQty,
    AskInternalQty,
    AskExternalQty,
    MinInternalPriceChange,
    EstablishedLevel,
    EstLevelEMASeconds,

    EstLevelPctEMA,
    MidPoint,
    BidHedgeInternalPrice,
    BidHedgeExternalPrice,
    BidHedgeSize,
    AskHedgeInternalPrice,
    AskHedgeExternalPrice,
    AskHedgeSize,
    MaxTicksWide,
    QuotePctTob,

    WorsenPayupCondPercent,
    QueueImportance,
    QuoteGap,
};
enum class ArbieSpreaderParameterColumns {
    Enabled,
    QuoteExternal,
    Edge,
    ActualEdge,
    QuoteSize,
    STF,
    HedgeOffset,
    MaxTicksThroughMarket,
    BidLeanPath,
    AskLeanPath,
    BidLeanPrice,
    AskLeanPrice,
    BidInternalPrice,
    BidExternalPrice,
    AskInternalPrice,
    AskExternalPrice,
    BidInternalSize,
    BidExternalSize,
    AskInternalSize,
    AskExternalSize,
    BidCross,
    AskCross,
};
enum class ArbieSpreaderPathsColumns { Commission, Enabled, STFOffset};
enum class PositionsColumns {RV, AS, MMInternal, MMExternal, Total};
enum class StackerParametersColumns { QuoteSize, QuoteGap, MaxQuote, StackLevels, StackSlack, MaxTicksOffTopLevel, UseGTC, AllowMultipleNewHedge, EnableQuoteSprayer, RedNumberPull, DarkRedNumberPull };
enum class PNLColumns {Open, Closed, Total, Drawdown, PnlLimit, DrawdownLimit, AsiaPnlLimit, AsiaDrawdownLimit, EuropePnlLimit,EuropeDrawdownLimit, USPnlLimit, USDrawdownLimit, SafeModePnlLimit, SafeModeDrawdownLimit};
enum class PNLRows {MarketMakers, RelativeValue, ArbieSpreader, Total, __end_marker__};
enum class PnlDetailColumns {MMOpen, MMClosed, RVOpen, RVClosed, ASOpen, ASClosed, Total, MMTotal, RVTotal, ASTotal};
enum class GlobalConfigsColumns {Value, Asia, Europe, US, Release};
enum class MessageCountsColumns {Inserts, Modifies, Cancels, CancelRejects, ModifyRejects, Fills, FillsRatio};
enum class TraderSwitchColumns {Trader, Shift};
enum class Traders {Steven, Mark};
enum class Shifts {Asia, Europe, US, SafeMode, __current_marker__};
enum class AuditColumns {Time, Trader, Grid, Row, Column, OldValue, NewValue};
enum class GTCManagementColumns {PreOpenBid, PreOpenAsk, IOP,  NumBid, NumOffer, NumTotal, CancelAllBid, Cancel_2_Bid, Cancel_1_Bid, TicksThroughBid, isCrossedBid, isCrossedAsk, TicksThroughAsk, Cancel_1_Ask, Cancel_2_Ask, CancelAllAsk, CancelAll};
enum class StatusDebugColumns { Contract, StatusEvent };
enum class LoggingLevelColumns {LoggingLevel};
enum class NegativeScalpCheckerColumns { Period, ClipCount, Pnl };
enum class EconomicNumberColumns {Time, MMDuration, RVDuration, ASDuration, Color, Description};

enum class WindowIds {
    VectorParameters = 1,
    ContractParametersRv = 2,
    LeanMatrix = 3,
    PricingConfig = 4,
    PricingMatrix = 5,
    RiskMatrix = 6,
    MainControl = 7,
    ManualFillsEntry = 8,
    FillsDisplay = 9,
    Errors = 10,
    GatewayStatuses = 11,
    MarketMakerParameters = 12,
    StackerParameters = 13,
    GlobalConfigs = 14,
    PNL = 15,
    PnlDetail = 16,
    MessagesCounts = 17,
    HedgeRatios = 18,
    QuoteSizes = 19,
    Commissions = 20,
    PositionInOutrights = 21,
    TraderSwitch = 22,
    FactorHedged = 23,
    Audit = 24,
    GTCManagement = 25,
    StatusDebug = 26,
    LoggingLevel=27,
    NegativeScalpChecker = 28,
    EconomicNumbers = 29,
    ArbieSpreaderParameters = 30,
    ArbieSpreaderPaths = 31,
};

enum class Color {
    TRANSPARENT = 1,
    TRANSPARENT_RED = 2,
    TRANSPARENT_GREEN = 3,
    TRANSPARENT_BID_BLUE = 4,
    TRANSPARENT_OFFER_RED = 5,
    TRANSPARENT_SWITCH_ENABLED = 6,
    TRANSPARENT_SWITCH_DISABLED = 7,
};

inline constexpr const char* ColorDefinition(Color color) {
    switch(color) {
        case Color::TRANSPARENT:
            return "#00FFFFFF";
        case Color::TRANSPARENT_BID_BLUE:
            return "#E6AFDBE8"; //a=90%
        case Color::TRANSPARENT_OFFER_RED:
            return "#E6FFBFCB"; //a=90%
        case Color::TRANSPARENT_GREEN:
            return "#BF00FF00"; //a=75%
        case Color::TRANSPARENT_SWITCH_ENABLED:
            return "#BF8BC34A";
        case Color::TRANSPARENT_SWITCH_DISABLED:
            return "#BFEF5350";
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

enum class PersistantDICharType : char { Fill = 'f', RVLatencyTracking = 'v'};

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

template<uint32_t Side>
inline hf::price_t BetterPrice(hf::price_t lhs, hf::price_t rhs) {
    static_assert(Side == BID || Side == ASK, "bad side");
    if(lhs == hf::NULL_PRICE) return rhs;
    if(rhs == hf::NULL_PRICE) return lhs;
    return IsBetterPrice<Side>(lhs, rhs) ? lhs : rhs;
}

template <uint32_t Side>
inline bool IsPriceBetterOrEqual(hf::price_t one, hf::price_t two);

template <>
inline bool IsPriceBetterOrEqual<BID>(hf::price_t one, hf::price_t two) {
    return one >= two;
}

template <>
inline bool IsPriceBetterOrEqual<ASK>(hf::price_t one, hf::price_t two) {
    return one <= two;
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

template<uint32_t Side>
inline constexpr uint32_t GetOtherSide() {
    return 1 - Side;
}

template <typename EnumT>
int32_t toInt(EnumT value) {
    return static_cast<int32_t>(value);
}

template <typename EnumT>
constexpr uint32_t toUInt(EnumT value) {
    return static_cast<uint32_t>(value);
}

template<uint32_t Side>
inline hf::price_t applyPriceOffset(hf::price_t price, hf::price_t offset);

template<>
inline hf::price_t applyPriceOffset<Python::BID>(hf::price_t price, hf::price_t offset) { return price + offset; }

template<>
inline hf::price_t applyPriceOffset<Python::ASK>(hf::price_t price, hf::price_t offset) { return price - offset; }

template<uint32_t Side>
inline hf::price_t calculateTicksThroughMarket(hf::price_t calculatedPrice, hf::price_t otherSideBest) noexcept;

template<>
inline hf::price_t calculateTicksThroughMarket<BID>(hf::price_t calculatedPrice, hf::price_t otherSideBest) noexcept {
    return calculatedPrice - otherSideBest;
}

template<>
inline hf::price_t calculateTicksThroughMarket<ASK>(hf::price_t calculatedPrice, hf::price_t otherSideBest) noexcept {
    return otherSideBest - calculatedPrice;
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
constexpr auto EUREX_ORDER_NOT_FOUND = 10000;

inline bool WasRejectedDueToBeingFilled(uint32_t rejectCode, const char* rejectText, hf::VENUE venue) {
    switch (venue) {
        case hf::VENUE::ICE: {
            return (rejectCode == ice_rejects::TOO_LATE_TO_CANCEL) | (rejectCode == ice_rejects::UNKNOWN_ORDER) | (rejectText[11] == 'I') | (rejectCode == om::toUType(om::OrderRejectReason::OrderNotActive));
        }
        case hf::VENUE::B3:
        case hf::VENUE::CME: {
            return (rejectCode == CME_ORDER_IS_NOT_IN_BOOK) | (rejectCode == om::toUType(om::OrderRejectReason::OrderNotActive));
        }
        case hf::VENUE::EUREX: {
            return (rejectCode == EUREX_ORDER_NOT_FOUND) | (rejectCode == om::toUType(om::OrderRejectReason::OrderNotActive));
        }
        default:
            throw std::runtime_error("reject - unhandled X");
    }
}

enum class SystemState {LoggedOut, LoggedIn, CheckQuotes, Enabled, Disabled, HardShutdown, CancelGtcAndDisable};
enum class StrategyId : uint8_t {MarketMaker, RelativeValue, ArbieSpreader, MAX};  // Execution must the last
enum class ResourceId : uint8_t {MarketMaker, RelativeValue, ArbieSpreader, Execution, FrontSide, _Resource_MAX_};   // Strategy resource must added before Execution
enum class DataSeries {VectorPrice, LTF, STF};

inline constexpr const char* toString(StrategyId id) {
    constexpr const char* strategies[] = { "MM", "RV", "AS",  "MAX"};
    static_assert((sizeof(strategies) / sizeof(char*)) == toUInt(StrategyId::MAX) + 1, "Not match with StrategyId");
    return strategies[toUInt(id)];
}

inline constexpr const char* toString(ResourceId id) {
    constexpr const char* resources[] = { "MM", "RV", "AS", "EXE", "FrontSide" };
    static_assert((sizeof(resources) / sizeof(char*)) == 5, "Not match with ResourceId");
    return resources[toUInt(id)];
}

inline constexpr const char* strategyFullName(StrategyId id) {
    constexpr const char* strategies[] = { "MarketMaker", "RelativeValue", "ArbieSpreader" };
    static_assert((sizeof(strategies) / sizeof(char*)) == toUInt(StrategyId::MAX), "Not match with StrategyId");
    return strategies[toUInt(id)];
}

#ifdef IS_DEV
inline std::string strategyName(StrategyId strategyId) {
    return toString(strategyId);
}

inline std::string resourceName(ResourceId resourceId) {
    return toString(resourceId);
}
#else
inline uint32_t strategyName(StrategyId strategyId) {
    return toUInt(strategyId);
}

inline uint32_t resourceName(ResourceId resourceId) {
    return toUInt(resourceId);
}
#endif

class VolatileAtomicBool {
public:
    VolatileAtomicBool(bool initialState = false) : _flag(initialState) {}
    operator bool() const { return _flag.load(std::memory_order_relaxed); }
    void operator=(bool val) { _flag.store(val, std::memory_order_release); }
private:
    std::atomic_bool _flag;
};

class VenueBitset {
private:
    auto load() const { return _bitSet.load(std::memory_order_relaxed); }
    void assignBitToValue(int index, bool value) {
        auto current = load();
        decltype(current) candidate;
        do {
            candidate = current;
            if(value){
                candidate.set(index);
            }
            else {
                candidate.reset(index);
            }
        } while(!_bitSet.compare_exchange_weak(current, candidate, std::memory_order_release, std::memory_order_relaxed));
    }
public:
    bool test(hf::VENUE venue) const { return _bitSet.load(std::memory_order_relaxed).test(static_cast<int>(venue)); }
    bool any() const { return load().any(); }
    bool none() const { return load().none(); }
    void set(hf::VENUE venue) { assignBitToValue(static_cast<int>(venue), true); }
    void reset(hf::VENUE venue) { assignBitToValue(static_cast<int>(venue), false); }
private:
    std::atomic<std::bitset<static_cast<uint8_t>(hf::VENUE::ANY)>> _bitSet;
};

struct SharedSystemState {
    using QuoteExtBitset = std::bitset<static_cast<uint8_t>(hf::VENUE::ANY)>;
    std::atomic<QuoteExtBitset> _exchQuoteExternally {0};
    //VolatileAtomicBool _rvGated; /* This is no longer set, however it's not completely obvious if that's intention */
    static constexpr bool _rvGated_placeholder = false;
    VenueBitset _inMDGap;

    bool rvEnableBlocked() const {
        return _rvGated_placeholder || _inMDGap.any();
    }

    bool strategyEnableBlocked(StrategyId id) const {
        //Until _rvGated_placeholder does something, both these paths do the same thing
        if(id == StrategyId::RelativeValue) {
            return rvEnableBlocked();
        }
        return _inMDGap.any();
    }
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

enum MarketIds : uint8_t {MARKET_ID_MM_HEDGE = 0, MARKET_ID_MM_QUOTE = 1, Market_ID_RV_QUOTE = 2, Market_ID_AS_QUOTE = 3, MARKET_ID_EXTERNAL = 4, __end__};

struct Market {
    enum Type : uint8_t {
        Internal,
        External,
        __NumberOfMarkets__,
    };

    Market() {
        for (uint32_t i = 0; i < toUInt(MarketIds::__end__); ++i) {
            _fakSizeAdjustment[i] = 0;
            _fakSizeAdjustments[i].reserve(128);
        }
        _fakPreAdjustedMarkets.reserve(128);
    }

    void init(StrategyId strategy, uint32_t side, uint32_t id, uint32_t contractId) noexcept {
        _strategy = toUInt(strategy);
        _side = side;
        _id = id;
        _contractId = contractId;
    }

    // For RV
    void update(hf::price_t price, hf::qty_t size) {
        assert(_id == MarketIds::MARKET_ID_EXTERNAL && _strategy == toUInt(StrategyId::RelativeValue));
        if (_price != price) {
            LOG(0, "{i1}. Market {C[i2]} Id={i3} Side={i4} Prices different.", _strategy, _contractId, _id, _side)
            if (_fakSizeAdjustment[_id]) {
                _fakSizeAdjustment[_id] = 0;
                _fakSizeAdjustments[_id].clear();
                LOG(0, "{i1}. Market {C[i2]} Id={i3} Side={i4}. Flushing FAK adjustments.", _strategy, _contractId, _id, _side)
            }
            _fakPreAdjustment = 0;
        }
        _price = price;
        _size = size;
        DLOG(0, "{i1}. Market {C[i2]} Id={i3} Side={i4} MD updated. FAKAdj={i5} asQuoteAdj={i6} FAKPreAdj={i7} Price={d1} Size={i8} AdjustedSize={i9}", _strategy,
             _contractId, _id, _side, _fakSizeAdjustment[_id], _asQuoteAdjustment, _fakPreAdjustment, ToDollars(_price),
             getUnAdjustedSize(), getAdjustedSize())
    }

    bool update(hf::price_t price, hf::qty_t size, std::unordered_set<Market*>& affected_markets) {
        assert(_id == MarketIds::MARKET_ID_EXTERNAL);
        bool changed = (_price != price) | (_size != size);
        if (_price != price) {
            LOG(0, "{i1}. Market {C[i2]} Id={i3} Side={i4} Prices different.", _strategy, _contractId, _id, _side)
            if (_fakSizeAdjustment[_id]) {
                _fakSizeAdjustment[_id] = 0;
                _fakSizeAdjustments[_id].clear();
                LOG(0, "{i1}. Market {C[i2]} Id={i3} Side={i4}. Flushing FAK adjustments.", _strategy, _contractId, _id, _side)
            }
            undoAsQuoteAdjustment(affected_markets);
            _fakPreAdjustment = 0;
        }
        _price = price;
        _size = size;
        DLOG(0, "{i1}. Market {C[i2]} Id={i3} Side={i4} MD updated. FAKAdj={i5} asQuoteAdj={i6} FAKPreAdj={i7} Price={d1} Size={i8} AdjustedSize={i9}", _strategy,
             _contractId, _id, _side, _fakSizeAdjustment[_id], _asQuoteAdjustment, _fakPreAdjustment, ToDollars(_price),
             getUnAdjustedSize(), getAdjustedSize())
         return changed;
    }

    void updateInternalMarket(hf::price_t price, hf::qty_t size) {
        _price = price;
        _size = size;
        DLOG(0, "{i1}. Market {C[i2]} Id={i3} Side={i4} MD updated. FAKAdj={i5} asQuoteAdj={i6} FAKPreAdj={i7} Price={d1} Size={i8} AdjustedSize={i9}", _strategy,
             _contractId, _id, _side, _fakSizeAdjustment[_id], _asQuoteAdjustment, _fakPreAdjustment, ToDollars(_price),
             getUnAdjustedSize(), getAdjustedSize())
    }

    bool handleInternalHedgeDone(uint32_t marketId, uint32_t hedgeId) {
        assert(marketId < MarketIds::MARKET_ID_EXTERNAL);
        for (auto it = _fakSizeAdjustments[marketId].begin(); it != _fakSizeAdjustments[marketId].end(); ++it) {
            SizeAdjustment& adj = *it;
            if (adj._hedgeId == hedgeId) {
                _fakSizeAdjustment[marketId] -= adj._adjustment;
                LOG(0, "{i1}. Market {C[i2]} Id={i3} Side={i4} internal hedge adjustment Done. HedgeId={i5} Adj={i6} FAKAdj={i7} asQuoteQdj={i8} FAKPreAdj={i9} Price={d1} Size={i10} AdjustedSize={i11}", _strategy,
                    _contractId, marketId, _side, adj._hedgeId, adj._adjustment, _fakSizeAdjustment[marketId],
                    _asQuoteAdjustment, _fakPreAdjustment, ToDollars(_price), getUnAdjustedSize(),
                    getAdjustedSize(marketId))
                _fakSizeAdjustments[marketId].erase(it);
                return true;
            }
        }
        return false;
    }

    bool handleFakDone(om::order_id_t id) {
        assert(_id == MarketIds::MARKET_ID_EXTERNAL);
        if (_fakDoneReceived.test(id)) {
            return false;
        }

        _fakDoneReceived.set(id);

        for (auto it = _fakSizeAdjustments[_id].begin(); it != _fakSizeAdjustments[_id].end(); ++it) {
            SizeAdjustment& adj = *it;
            if (adj._orderId == id) {
                _fakSizeAdjustment[_id] -= adj._adjustment;
                LOG(0, "{i1}. Market {C[i2]} Id={i3} Side={i4} FAK Done. HedgeId={i5} Adj={i6} OrderId={i7} FAKAdj={i8} asQuoteAdj={i9} FAKPreAdj={i10} Price={d1} Size={i11} AdjustedSize={i12}", _strategy,
                    _contractId, _id, _side, adj._hedgeId, adj._adjustment, id, _fakSizeAdjustment[_id],
                    _asQuoteAdjustment, _fakPreAdjustment, ToDollars(_price), getUnAdjustedSize(), getAdjustedSize())
                _fakSizeAdjustments[_id].erase(it);
                return true;
            }
        }
        return false;
    }

    bool ackFAKAdjustment(uint32_t hedgeId, hf::qty_t size, om::order_id_t orderId) {
        assert(_id == MarketIds::MARKET_ID_EXTERNAL);
        for (auto it = _fakSizeAdjustments[_id].begin(); it != _fakSizeAdjustments[_id].end(); ++it) {
            SizeAdjustment& adj = *it;
            if (adj._hedgeId > hedgeId) {
                break;
            } else if (adj._hedgeId == hedgeId) {
                if (size != 0) {
                    adj._orderId = orderId;
                    if (adj._adjustment != size) {
                        _fakSizeAdjustment[_id] -= adj._adjustment - size;
                        adj._adjustment = size;
                    }
                } else {
                    //Is this a bug?  If we don't send a FAK, i.e. ack with size=0, shouldn't we undo the adjustment just like the adj._adjustment != size case above?
                    _fakSizeAdjustment[_id] -= adj._adjustment;
                    _fakSizeAdjustments[_id].erase(it);
                }
                LOG(0, "{i1}. Market {C[i2]} Id={i3} Side={i4} Acked FAK adjustment. HedgeId={i5} Adj={i6} FAKAdj={i7} asQuoteAjd={i8} FAKPreAdj={i9} OrderId={i10} Price={d1} Size={i11} AdjustedSize={i12}", _strategy,
                    _contractId, _id, _side, hedgeId, size, _fakSizeAdjustment[_id], _asQuoteAdjustment,
                    _fakPreAdjustment, orderId, ToDollars(_price), getUnAdjustedSize(), getAdjustedSize())
                return true;
            }
        }
        return false;
    }

    void addFAKAdjustment(uint32_t hedgeId, hf::qty_t adj) {
        _fakSizeAdjustment[_id] += adj;
        _fakSizeAdjustments[_id].push_back({hedgeId, adj, om::NULL_ORDERID});
        LOG(0, "{i1}. Market {C[i2]} Id={i3} Side={i4} Adding FAK adjustment. HedgeId={i5} Adj={i6} FAKAdj={i7} asQuoteAdj={i8} FAKPreAdj={i9} Price={d1} Size={i10} AdjustedSize={i11}", _strategy,
            _contractId, _id, _side, hedgeId, adj, _fakSizeAdjustment[_id], _asQuoteAdjustment, _fakPreAdjustment,
            ToDollars(_price), getUnAdjustedSize(), getAdjustedSize())
    }

    void addFAKPreAdjustment(Market* market) {
        _fakPreAdjustedMarkets.push_back(market);
        market->addFAKPreAdjustment(_asQuoteAdjustment);
    }

    void addFAKPreAdjustment(hf::qty_t adj) {
        _fakPreAdjustment += adj;
        LOG(0, "{i1}. Market {C[i2]} Id={i3} Side={i4} Adding FAK Pre adjustment. Adj={i5} FAKPreAdj={i6} asQuoteAdj={i7} FAKAdj={i8} Price={d1} Size={i9} AdjustedSize={i10}", _strategy,
            _contractId, _id, _side, adj, _fakPreAdjustment, _asQuoteAdjustment, _fakSizeAdjustment[_id],
            ToDollars(_price), getUnAdjustedSize(), getAdjustedSize())
    }

    void undoFAKPreAdjustment(hf::qty_t adj) {
        if (BOOST_LIKELY(_fakPreAdjustment != 0)) {
            _fakPreAdjustment = std::max<hf::qty_t>(0, _fakPreAdjustment - adj);
            LOG(0, "{i1}. Market {C[i2]} Id={i3} Side={i4} Undo FAK Pre adjustment. Adj={i5} FAKPreAdj={i6} asQuoteAdj={i7} FAKAdj={i8} Price={d1} Size={i9} AdjustedSize={i10}", _strategy,
                _contractId, _id, _side, adj, _fakPreAdjustment, _asQuoteAdjustment, _fakSizeAdjustment[_id],
                ToDollars(_price), getUnAdjustedSize(), getAdjustedSize())
        }
    }

    void addAsQuoteAdjustment(hf::qty_t adj, std::unordered_set<Market*>& affected_contract) noexcept {
        if (_asQuoteAdjustment) {
            WLOG(0, "{i1}. Market {C[i2]} Id={i3} Side={i4} Adding multiple AS Quote adjustment.", _strategy, _contractId, _id, _side)
            for (auto* market : _fakPreAdjustedMarkets) {
                market->undoFAKPreAdjustment(_asQuoteAdjustment);
                affected_contract.emplace(market);
            }
            _fakPreAdjustedMarkets.clear();
        }
        _asQuoteAdjustment = adj;
        LOG(0, "{i1}. Market {C[i2]} Id={i3} Side={i4} Adding AS Quote adjustment. Adj={i5} asQuoteAdj={i6} FAKPreAdj={i7} FAKAdj={i8} Price={d1} Size={i9} AdjustedSize={i10}", _strategy,
            _contractId, _id, _side, adj, _asQuoteAdjustment, _fakPreAdjustment, _fakSizeAdjustment[_id],
            ToDollars(_price), getUnAdjustedSize(), getAdjustedSize())
    }

    bool undoAsQuoteAdjustment(std::unordered_set<Market*>& affected_markets) noexcept {
        return undoAsQuoteAdjustment(_asQuoteAdjustment, affected_markets);
    }

    bool undoAsQuoteAdjustment(hf::qty_t adj, std::unordered_set<Market*>& affected_markets) noexcept {
        if (_asQuoteAdjustment) {
            _asQuoteAdjustment = std::max<hf::qty_t>(0, _asQuoteAdjustment - adj);
            LOG(0, "{i1}. Market {C[i2]} Id={i3} Side={i4}. Undo AS Quote Adjustment. adj={i5} asQuoteAdjustment={i6} Price={d1} Size={i7} AdjustedSize={i8}", _strategy,
                _contractId, _id, _side, adj, _asQuoteAdjustment, ToDollars(_price), getUnAdjustedSize(), getAdjustedSize())
            for (auto* market : _fakPreAdjustedMarkets) {
                market->undoFAKPreAdjustment(adj);
                affected_markets.emplace(market);
            }
            assert(_asQuoteAdjustment >= 0);
            if (_asQuoteAdjustment <=0) {
                _fakPreAdjustedMarkets.clear();
            }
            return true;
        }
        return false;
    }

    hf::qty_t getASQuoteAdjustment() const noexcept {
        return _asQuoteAdjustment;
    }

    hf::qty_t getFAKPreAdjustment() const noexcept {
        return _fakPreAdjustment;
    }

    hf::qty_t getAdjustedSize() const noexcept {
        return _id == MarketIds::__end__ ? 0 : getAdjustedSize(_id);
    }

    hf::qty_t getAdjustedSizeExcludeFAKPreAdj() const noexcept {
        return _id == MarketIds::__end__ ? 0 : std::max<hf::qty_t>(_size - _fakSizeAdjustment[_id] - _asQuoteAdjustment, 0);
    }

    hf::qty_t getAdjustedSize(uint32_t marketId) const noexcept {
        const hf::qty_t size = _size - _fakSizeAdjustment[marketId] - _asQuoteAdjustment - _fakPreAdjustment;
        if (size < 0) {
            WLOG(0, "{i1}. Market {C[i2]} Id={i3} Side={i4} getAdjustedSize(). Is negative FAKAdj={i5} asQuoteAdj={i6} FAKPreAdj={i7} Price={d1} Size={i8}", _strategy,
                 _contractId, marketId, _side, _fakSizeAdjustment[marketId], _asQuoteAdjustment, _fakPreAdjustment,
                 ToDollars(_price), _size)
            return 0;
        }
        return size;
    }

    hf::qty_t getUnAdjustedSize() const {
        return _size;
    }

    hf::price_t getPrice() const {
        return _price;
    }

    uint32_t getId() const {
        return _id;
    }

    void setId(uint32_t id) {
        _id = id;
    }

    uint32_t getContractId() const noexcept {
        return _contractId;
    }

    uint32_t getSide() const noexcept {
        return _side;
    }
private:
    uint32_t _strategy;
    uint32_t _id;   // 0: MM_Hedge; 1: MM_Quote; 2: RV_Quote; 3: AS_Quote; 4: External
    uint32_t _contractId;
    uint32_t _side;
    hf::price_t _price = hf::NULL_PRICE;
    hf::qty_t _size = 0;
    hf::qty_t _fakSizeAdjustment[MarketIds::__end__] = {0};
    hf::qty_t _asQuoteAdjustment = 0;
    hf::qty_t _fakPreAdjustment = 0;
    std::vector<SizeAdjustment> _fakSizeAdjustments[MarketIds::__end__];
    std::vector<Market*> _fakPreAdjustedMarkets;
    std::bitset<om::ORDER_NUM> _fakDoneReceived;
};

template<uint32_t Side>
inline Market& BetterMarket(Market& lhs, Market& rhs) {
    static_assert(Side == BID || Side == ASK, "bad side");
    if(lhs.getPrice() == hf::NULL_PRICE) return rhs;
    if(rhs.getPrice() == hf::NULL_PRICE) return lhs;
    return IsPriceBetterOrEqual<Side>(lhs.getPrice(), rhs.getPrice()) ? lhs : rhs;
}

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

inline uint64_t GetCyclesFromMilliSeconds(uint64_t milliSeconds, double frequency) {
    return static_cast<uint64_t>(frequency * milliSeconds * 1000000);
}

const static double EPSILON = 0.00000001;

inline bool IsZero(double value) {
    return  value > 0 ? value <=  EPSILON : value >= -EPSILON;
}

inline bool IsSame(double value1, double value2) {
    return IsZero(value1 - value2);
}

inline int Compare(double value1, double value2) {
    return IsSame(value1, value2) ? 0 : (value1 < value2 ? -1 : 1);
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
    return side == hf::SIDE::BUY ? BID : ASK;
}

struct Book {
    hf::price_t _price = hf::NULL_PRICE;
    hf::qty_t _otherQuantity = 0;
    md::order_count_t _otherOrderCount = 0;
};

typedef uint64_t OrderId_t;

enum class ManualFillType {MarketMaker, RelativeValue, ArbieSpreader};
enum class FillType : uint8_t {Internal, External, Phantom, NegativeScalpCheck};
//Note "Manual" only used for reporting, internally it's a Qoute/Hedge
enum class FillSource : uint8_t {
    Quote,
    MissedHedge,
    Overfill,
    Hedge,
    SelfMatch,
    RvDisable,
    RvHedgeReject,
    ViperOff,
    ViperStart,
    Manual,

    AsDisable,
    AsHedgeReject,
    ASUnableToHedge,
    AsToRv,
};
enum class ErrorType : uint8_t {Critical, Normal};
enum class YesNoType {Unknown, Yes, No};
enum class LeanType {MM_Hedge, MM_Quote, RV_Quote, AS_Quote, External, None};
enum class StatusDebugType { PreOpen, Open, Close, CME_NoCancel, CME_VelocityHalt, CME_VelocityHaltEnd };

inline const char* StatusDebugTypeToString(StatusDebugType type) {
    switch(type) {
        case StatusDebugType::PreOpen:
            return "PreOpen";
        case StatusDebugType::Open:
            return "Open";
        case StatusDebugType::Close:
            return "Close";
        case StatusDebugType::CME_NoCancel:
            return "CME_NoCancel";
        case StatusDebugType::CME_VelocityHalt:
            return "CME_VelocityHalt";
        case StatusDebugType::CME_VelocityHaltEnd:
            return "CME_VelocityHaltEnd";
    }
    return "UNKNOWN";
}

inline constexpr const char* GetFillTypeString(FillType fillType) {
    constexpr const char* strFillTypes[] = {"Internal", "External", "Phantom", "NegativeScalpCheck"};
    static_assert(sizeof(strFillTypes) / sizeof(const char*) == (toUInt(FillType::NegativeScalpCheck) + 1),
                  "FillType enum has changed");
    return strFillTypes[toUInt(fillType)];
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

inline std::string GetMDChannel(hf::VENUE venue, const hf::symbol_info_t& si) {
    BOOST_ASSERT(venue == hf::VENUE::CME || venue == hf::VENUE::ICE || venue == hf::VENUE::EUREX);
    switch(venue) {
        case hf::VENUE::CME:
            BOOST_ASSERT(si.cme.enabled);
            return si.cme.md_channel;
        case hf::VENUE::ICE:
            BOOST_ASSERT(si.ice.enabled);
            return si.ice.md_channel;
        case hf::VENUE::EUREX:
            BOOST_ASSERT(si.eurex.enabled);
            return si.eurex.md_channel;
        default:
            BOOST_ASSERT(false);
            return "";
    }
}

struct PriceLevel;

struct Order {
    enum Type : uint8_t {Hedging, Quoting};

    hf::price_t _price = hf::NULL_PRICE;
    hf::qty_t _size = 0;

    hf::price_t _externalPrice = hf::NULL_PRICE;
    hf::qty_t _externalSize = 0;

    PriceLevel* priceLevel = nullptr;
    uint32_t _side;

    OrderId_t _id;
    StrategyId _strategy;
    Type _type;
    om::ORDER_TYPE _hedgeType;
    bool _inPayUp = false;
    bool _leanOnRV = false;
};

constexpr uint8_t orderId(StrategyId strategyId, Order::Type type) {
    return toUInt(strategyId) * 2 + type;
}
constexpr uint8_t MAX_ORDER_ID = toUInt(StrategyId::MAX) * 2;
constexpr Order::Type getOrderType(uint8_t orderId) {
    return ((orderId & 1) == 0) ? Order::Type::Hedging : Order::Type::Quoting;
}

struct PriceLevel {
    hf::price_t _price;
    std::vector<Order*> _orders;

    hf::qty_t getSize() const noexcept {
        return _orders.empty() ? 0 : _orders[0]->_externalSize;
    }

    int64_t getOrderId() const noexcept {
        return _orders.empty() ? -1 : static_cast<int64_t>(_orders[0]->_id);
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
        if (_orders.empty()) {
            _orders.push_back(order);
        } else if (order->_strategy == StrategyId::MarketMaker) {
            auto it = _orders.begin();
            if ((*it)->_strategy == StrategyId::MarketMaker) {
                *it = order;
            } else {
                _orders.insert(it, order);
            }
        } else {
            for (auto it = _orders.begin(); it != _orders.end(); ++it) {
                if ((*it)->_strategy == StrategyId::MarketMaker) {
                    continue;
                }

                if (order->_externalSize > (*it)->_externalSize) {
                    _orders.insert(it, order);
                    return;
                }
            }
            _orders.emplace_back(order);
        }
    }
};

struct OrderRequest {
    struct Quote {
        hf::price_t _internalPrice = hf::NULL_PRICE;
        hf::price_t _externalPrice = hf::NULL_PRICE;
        hf::qty_t _internalSize = 0;
        hf::qty_t _externalSize = 0;
        bool _persist = false;
        bool _leanOnRV = false;
    };

    struct Hedge {
        hf::price_t _internalPrice = hf::NULL_PRICE;
        hf::price_t _externalPrice = hf::NULL_PRICE;
        hf::price_t _stf = hf::NULL_PRICE;
        hf::qty_t _size = 0;
        uint32_t _marketId = 0;
        uint32_t _hedgeId = 0;
        bool _inPayUp = false;
        bool _isExternal = false;
    };

    union {
        Quote _quote;
        Hedge _hedge;
    };

    uint8_t _id;
    uint8_t _side = BID;
    uint32_t _orderExecutionToken = 0;

    OrderRequest() : _quote() {}
};

struct CancelStrategyOrders {
    uint32_t _internalId;
    StrategyId _strategy;
    bool _disableUseGTC;
};

struct OrderRequestNotification {
    uint32_t _internalId;
    StrategyId _strategy;
};

inline hf::price_t RoundToTickSize(hf::price_t price, hf::price_t tickSize, uint32_t side) {
    const hf::price_t rawPrice = price;
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

inline void ShutdownOnError(const std::string& /*message*/) {
}

inline bool IsSelfMatchReject(hf::VENUE venue, uint32_t code) {
    switch (venue) {
        case hf::VENUE::ICE:
            return (code == 11) | (code == 9);
        case hf::VENUE::B3:
        case hf::VENUE::CME:
            //Fairly certain this does not work
            return code == 11;
        case hf::VENUE::EUREX:
            //SMP not enabled for Eurex
            return false;
        default:
            throw std::runtime_error("unhandled venue");
    }
}

inline bool IsRejectDueToMarketClosed(hf::VENUE venue, uint32_t code) {
    switch (venue) {
        case hf::VENUE::B3:
        case hf::VENUE::CME:
            return code == 1003;
        case hf::VENUE::ICE:
            return code == 2;
        case hf::VENUE::EUREX:
            //Likely 10011 catches everything reasonable, but there are 10008/103 rejects in kdb that corresponds to special states
            //after/before normal market hours.
            return (code == 10011) // Order maintenance nor allowed in current state
                | (code == 10008)  // GFD order is not executable on current business day))
                | (code == 103)    // Service not available, this CAN mean wrong partition
                | (code == 102);   // Service temporarily not available
        default:
            throw std::runtime_error("unhandled venue");
    }
}

inline bool IsExternalUnsolicitedCancel(hf::VENUE venue, uint32_t code, const char* rtxt) {
    switch(venue) {
        case hf::VENUE::B3:
        case hf::VENUE::CME:
            return code == 105;
        case hf::VENUE::ICE:
            return !std::memcmp(rtxt + 4, "Kill", 4);
        case hf::VENUE::EUREX:
            //todo_eurex
            return false;
        default:
            throw std::runtime_error("unhandled venue");
    }
}

inline bool IsSMPUnsolicitedCancel(hf::VENUE venue, uint32_t code, const char* reasonText) {
    if (venue == hf::VENUE::CME || venue == hf::VENUE::B3) {
        return code == 103; // cancel resting on CME
    }
    if(venue == hf::VENUE::EUREX) {
        return false; //No SMP on Eurex
    }
    return !std::memcmp(reasonText, "noSelfTrading", 4);
}

inline bool IsICESMPModifyReject(const char * reasonText) {
    return !std::memcmp(reasonText, "Trad", 4);
}

inline bool IsEurexModifyThrottleReject(const char* reasonText) {
    return !std::memcmp(reasonText, "Thro", 4); //"Throttle limit exceeded"
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
        default:
            throw std::runtime_error("unknown trader");
    }
}

inline constexpr const char* const to_string(Shifts shift) {
    assert(toUInt(shift) < toUInt(Shifts::__current_marker__));
    constexpr const char* shifts[] = { "Asia", "Europe", "US", "SafeMode" };
    static_assert(sizeof(shifts) / sizeof(const char*) == toUInt(Shifts::__current_marker__),
            "Update to_string for new element. Update _globalConfigHeader, GlobalConfigsColumns and populateGlobalConfigs");
    return shifts[toUInt(shift)];
}

COLD
inline bool verifyHHMMSSTime(uint32_t time, uint32_t& hour, uint32_t& min, uint32_t& sec) {
    hour = time / 10000;
    min = (time % 10000) / 100;
    sec = time % 100;
    return !(hour > 23 || min > 59 || sec > 59);
}

inline uint64_t convertTimeToCycles(uint32_t hour, uint32_t minute, uint32_t sec, uint64_t now_cycles, double frequency) {
    // get current time
    std::time_t t = std::time(nullptr);
    auto tm = std::localtime(&t);

    auto to_seconds = [](uint32_t h, uint32_t m, uint32_t s = 0) {
        return h * 3600 + m * 60 + s;
    };

    auto now = to_seconds(tm->tm_hour, tm->tm_min, tm->tm_sec);

    auto timeSec = to_seconds(hour, minute, sec);
    if (now >= timeSec) {
        // time already passed, set the time to next day
        timeSec = to_seconds(hour + 24, minute, sec);
    }

    auto diff = timeSec - now;

    //Check if a sunday before or at 17:00, if true advance one day
    tm->tm_sec += diff;
    tm->tm_isdst = -1;
    std::mktime(tm);
    if(!tm->tm_wday && (tm->tm_hour < 17 || (tm->tm_hour == 17 && tm->tm_min == 0))) {
        std::time_t base = std::mktime(tm);
        tm->tm_isdst = -1;
        tm->tm_mday += 1;
        diff += std::mktime(tm) - base;
    }

    return now_cycles + GetCyclesFromSeconds(diff, frequency);
}

inline bool parseDateTime(const std::string& datatime_str, time_t& timeOut) {
    struct tm t{};
    std::istringstream ss(datatime_str);
    ss >> std::get_time(&t, "%Y-%m-%d %H:%M:%S");

    if (ss.fail()) {
        return false;
    }
    t.tm_isdst = -1;
    timeOut = mktime(&t);
    return true;
}

inline hf::price_t GetPriceForManualFill(uint32_t side, hf::price_t bidPrice, hf::qty_t bidQty, hf::price_t askPrice, hf::qty_t askQty) {
    auto valid = [](hf::price_t price, hf::qty_t qty) { return price != hf::NULL_PRICE && qty > 0; };

    hf::price_t price;
    if(side == BID) {
        price = valid(askPrice, askQty) ? askPrice : valid(bidPrice, bidQty) ? bidPrice : hf::NULL_PRICE;
    }
    else {
        price = valid(bidPrice, bidQty) ? bidPrice : valid(askPrice, askQty) ? askPrice : hf::NULL_PRICE;
    }
    return price;
}

enum class ViperMessageType : uint8_t {
    Login,
    Logout,
    ClientDown,
    GridUpdate,
    MarketUpdate,
    Fill,
    MarketMakerEnabled,
    ErrorMessage,
    GuiInitialized,
    AcceptedManualFill,

    InternalHedgeDone,
    GridUpdateAccepted,
    SwitchChangeReply,
    GatewayStatus,
    HedgeMDAdjsutAck,
    QuoteSizesUpdate,
    StoreLtf,
    InternalHeartbeat,
    QuoteExternallyOff,
    EndOfQuotes,
    
    DisableMarketMakerInstrument,
    IOPUpdate,
    EndIOPTracking,
    AuctionMDUpdate,
    NumGTCUpdate,
    SetRhoneLimits,
    ChangeUseGTC,
    DebugStatuses,
    OrderRequest,
    EndOfFill,
    
    RegisterOrderClient,
    CancelStrategyOrders,
    ShiftUpdate,
    MMWidthPercentageUpdate,
    EmaOnOff,
    OrderUpdate,
    QueuePositionUpdate,
    DisableMMInstrumentAndGTCAndExternalQuote,
    ShiftChanged,
    EconomicNumberStarted,

    EconomicNumberEnded,
    QuoteExternallyOn,
    CanEnableUseGTC,
    RVLatencyTracking,
    InternalBookUpdate,
    ASInternalMatchingDone,
    SubscribeSTFUpdate,
    STFUpdate,
    TransferPositionToRV,
    VectorParameterUpdate,

    FAKDone,

#ifdef VIPER_BACKTEST
    MD_Add_Order,
    MD_Delete_Order,
    MD_Modify_Order,
    MD_Status,
    MD_TradeDetails,
    OM_SubmitOrder,
    OM_ModifyOrder,
    OM_CancelOrder,
    OM_OrderAccepted,
    OM_OrderModified,
    OM_OrderCanceled,
    OM_OrderExecution,
    OM_OrderRejected,
    OM_OrderModifyRejected,
    OM_OrderCancelRejected,
#endif
};

enum class SoundId {Enabled = 0, Disable = 1, Shutdown = 2, CriticalMessage = 3, Fill = 4, NormalMessage = 5};

enum THROTTLING_MODE {THROTTLING_MODE_NONE, THROTTLING_MODE_DANGER_ZONE, THROTTLING_MODE_AT_CAPACITY};

#pragma pack(1)
struct ViperMessage {
#ifdef VIPER_BACKTEST
    uint64_t _timestamp;
#endif
    uint32_t _size;
    ViperMessageType _type;
    ResourceId _source;
    char _data[0];
};
#pragma pack()

struct GridUpdateMessage {
    GridUpdate* _gridUpdate;
};

struct GenericMessage {
    int64_t _number_1;
    int64_t _number_2;
    int64_t _number_3;
};


struct LatencyTrackingFields {
    static_assert(std::is_same<hf::batch_id_t, uint_fast32_t>::value, "type changed");
    static_assert(std::is_same< om::order_id_t, uint32_t>::value, "type changed");
    uint64_t _extid;  //Note for RV Hedge Messages, which are send when the order is sent, this is hedge ID
    om::order_id_t _oid;
    uint32_t _bid;

    void unset() {
        _extid = 0;
    }

    bool isset() {
        return _extid;
    }
};

struct RVLatencyTrackingMessage {
    LatencyTrackingFields _fields;
    uint32_t _hedgeId;
};

#pragma pack(1)
struct FillMessage {
    uint64_t _timestamp;
    hf::price_t _price;
    union {
        hf::price_t _rvStf;
        struct {
            uint32_t _internalAllocation;
            uint32_t _externalAllocation;
        };
    };
    LatencyTrackingFields _latencyTracking;
    int32_t _qty;
    uint32_t _contractId;
    uint32_t _side;
    StrategyId _strategy;
    FillType _type;
    FillSource _source;
    bool _forceOverFill;
    bool _checkLimit;
};
#pragma pack()

struct ASFillsToRV {
    uint32_t _numFills;
    FillMessage _fills[0];
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

#pragma pack(1)
struct GatewayStatusMessage {
    bool _active;
    char _message[56];
};
#pragma pack()

struct IOPUpdate {
    uint32_t _internalId;
    hf::price_t price;
    int numBidGTCs;
    int numOfferGTCs;
    int ticksThroughBid;
    int ticksThroughAsk;
    bool forceCrossed;
};

struct AuctionMDUpdate {
    uint32_t _internalId;
    hf::price_t _bid;
    hf::price_t _offer;
};

static constexpr uint32_t CRITICAL_MESSAGE_MAX_SIZE = 200;

#pragma pack(1)
struct ErrorMessage {
    uint32_t _size;
    ErrorType _type;
    char _message[CRITICAL_MESSAGE_MAX_SIZE];
};
#pragma pack()

struct MarketUpdate {
    hf::price_t _bidPrice;
    hf::qty_t _bidSize;
    hf::price_t _askPrice;
    hf::qty_t _askSize;
    uint32_t _bidMarketId;
    uint32_t _askMarketId;
    uint32_t _instrumentIndex;
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
    uint32_t _numLimits;
    RhoneLimit _limits[0];
};

struct DebugStatusEvent {
    uint64_t _extSeqNum;
    hf::symbol_id_t _sid;
    hf::VENUE _venue;
    uint8_t _extStatus;
    uint8_t _cmeSecurityTradingEvent = -1;
};

struct DebugStatusEvents {
    uint32_t _numDebugStatusEvents;
    DebugStatusEvent _statusEvents[0];
};

struct EndOfFill {
    uint32_t _internalId;
    uint32_t _side;
    uint32_t _execToken;
    bool _isInternal;
};

struct SweepDetected {
    hf::price_t _sweepTicksAtPrice;
    uint32_t _tradedQty;
    uint32_t _internalId;
    hf::price_t _eventDiff;
    hf::qty_t _eventQty;
    hf::VENUE _venue;
};

struct RegisterOrderClient {
    uint32_t _internalId;
    uint32_t _subscriptionCount;
    StrategyId _strategy;
    StrategyId _internalMarketSubscriptions[0];
};

struct OrderUpdate {
    hf::price_t price;
    hf::qty_t qty;
    hf::qty_t qp;
    uint32_t orderId;
    uint32_t contractId;
    uint32_t side;
};

struct QueuePositionUpdate {
    uint32_t orderId;
    uint32_t contractId;
    hf::qty_t qp;
};

struct AcceptedManualFillMessage {
    uint32_t _contractId;
    bool _isInternal;
    hf::qty_t _size;
    hf::price_t _price;
    hf::price_t _stf;
};

struct UndoActionsAfterEconomicNumberMessage {
    uint32_t _contractId;
    uint32_t _nLevel;
    uint32_t _maxSlack;
    uint32_t _maxOffTopLevel;
};

#pragma pack(1)
struct STFUpdate {
    uint32_t _numSTF;
    struct STF {
        uint32_t _contractId;
        hf::price_t _fair;
    } _data[0];
};
#pragma pack()

struct InternalBookUpdate {
    uint32_t _contractId;
    uint32_t _side;
    hf::batch_id_t _lastMDbid;
    md::top_t _book;
};

struct VectorParameterUpdate {
    uint32_t _numUpdate;
    struct Update {
        VectorParametersColumns _type;
        uint32_t _vectorIndex;
        double _value;
    } _data[0];
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

template<typename MDConsumer>
COLD NEVER_INLINE
void HandleDebugStatusEvents(const DebugStatusEvents& dses, MDConsumer* mdConsumer) {
    const DebugStatusEvent* dse = dses._statusEvents;
    md::MDStatusEvent status;
    for(unsigned i = 0; i < dses._numDebugStatusEvents; ++i) {
        std::memset(&status, 0, sizeof(status));
        status.type = md::MDEvent::TYPE::STATUS;
        status.statusType = md::MDStatusEvent::TYPE::TRADING_STATUS;
        status.sid = dse->_sid;
        status.extStatus = dse->_extStatus;
        if(dse->_cmeSecurityTradingEvent != std::numeric_limits<decltype(dse->_cmeSecurityTradingEvent)>::max()){
            status.ext2.cme.securityTradingEvent = dse->_cmeSecurityTradingEvent;
        }
        mdConsumer->onMDStatus(dse->_venue, dse->_extSeqNum, status, mdConsumer->sidToInternalId(dse->_sid));
        ++dse;
    }
}

//Theoretically we should allow cancels if we detect an overfill potential because of Modify Reject
inline bool CanModifyOrCancelOrder(hf::VENUE venue, const om::order_t& order) {
    switch(venue) {
        case hf::VENUE::ICE:
            //Should test in cert if can cancel with modify in flight
            return order.action_flags.is_active & !order.action_flags.is_cancelling &
                   !order.action_flags.is_modifying & (GetQuantity(order, POTENTIAL_OVERFILL_QTY) == 0);
        case hf::VENUE::CME:
            return IsBitSet(order, STACKER_ORDER_FLAGS, SOF_IS_ACKED) & order.action_flags.is_active &
                   !order.action_flags.is_cancelling & (GetQuantity(order, POTENTIAL_OVERFILL_QTY) == 0);
        case hf::VENUE::EUREX:
            return order.action_flags.is_active & !order.action_flags.is_cancelling & (GetQuantity(order, POTENTIAL_OVERFILL_QTY) == 0);
        case hf::VENUE::B3:
            return IsBitSet(order, STACKER_ORDER_FLAGS, SOF_IS_ACKED) & order.action_flags.is_active &
                   !order.action_flags.is_modifying & !order.action_flags.is_cancelling &
                   (GetQuantity(order, POTENTIAL_OVERFILL_QTY) == 0);
        default:
            throw std::runtime_error("unknown venue");
    }
}

class ViperQueue : public hf::queue::U64Queue, protected rhone::util::DataBlock {
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
