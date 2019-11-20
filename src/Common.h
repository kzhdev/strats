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
static const uint32_t PAY_UP = 6;
static const uint32_t ROUTE = 7;
static const uint32_t PAY_UP_INITIAL_EXECUTED = 8;

static const uint32_t PRIOR_LEVEL = 9;

static const uint32_t POTENTIAL_OVERFILL_QTY = 10; //Theoritically can combine with IS_ACKED

static const uint32_t STACKER_ORDER = 0;
static const uint32_t HEDGER_ORDER = 1;
static const uint32_t GTC_ORDER = 2;
static const uint32_t UNASSIGNED_ORDER = 3;

static const uint32_t PAY_UP_YES = 0;
static const uint32_t PAY_UP_NO = 1;

enum class VectorParametersColumns {Position, Mkt, STF, LTF, MaxPos, Range, EMA_ON, EMA_Time, SetToMarket, Hedged};
enum class ContractParameterRVColumns {Position, BidLean, BidLeanType, BidInternalPrice, BidExternalPrice, BidCross, AskInternalPrice, AskExternalPrice, AskCross,
                               BidInternalQty, BidExternalQty, AskInternalQty, AskExternalQty, STF, Mkt, QuoteExternal, HedgeTolerance, Commission, AskLean, AskLeanType,
                               Width, ExtraWidth, PCTStfMove, MaxTicksThroughMarket};
enum class PricingConfigsColumns {Weight, UseWeighted, EstablishedLevel, PricingMultiplier, QuantityMultiplier};
enum class ManualFillsEntryColumns {Contract, Size, Type};
enum class MainControlAction {Enabled};
enum class MainControlButtons {Enable, Disable, HardShutdown, CheckQuotes, CancelGTC};
enum class FillsColumns {Number, Time, Contract, Side, Price, Size, Strategy, Type, Source, EdgeStf, EdgeMkt};
enum class ErrorsColumns {Time, Message, Type};
enum class GatewayStatusColumns {Exchange, Status};
enum class MarketMakerParametersColumns {QuoteExternal, QuoteSize, Width, JoinSize, LeaveSize, PayUpQty, PayUpOrders, PayUpMultiple, MaxPosInterrnal, MaxPosExternal, Commission, Enabled,
                                         InternalPosition, ExternalPosition, BidInternalPrice, BidExternalPrice, AskInternalPrice, AskExternalPrice, BidInternalQty, BidExternalQty, AskInternalQty, AskExternalQty,
                                        MinInternalPriceChange, EstablishedLevel, MidPoint, BidHedgeInternalPrice, BidHedgeExternalPrice, BidHedgeSize, AskHedgeInternalPrice,
                                        AskHedgeExternalPrice, AskHedgeSize};
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

enum class WindowIds {VectorParameters = 1, ContractParametersRv = 2, LeanMatrix = 3, PricingConfig = 4, PricingMatrix = 5, RiskMatrix = 6, MainControl = 7, ManualFillsEntry = 8,
    FillsDisplay = 9, Errors = 10, GatewayStatuses = 11, MarketMakerParameters = 12, StackerParameters = 13, GlobalConfigs = 14, PNL = 15, PnlDetail = 16, MessagesCounts = 17,
    HedgeRatios = 18, QuoteSizes = 19, Commissions = 20, PositionInOutrights = 21, TraderSwitch = 22, FactorHedged = 23, Audit = 24};

enum class TimerType : uint32_t  {FillBanner, QuoteBanner, CheckHedgeQuote, HedgeBanner, DoNothing};

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

inline int64_t GetQuantity(const om::order_t& order, uint32_t key) {
    return  order.debug.i[key];
}

inline void SetQuantity(const om::order_t& order, int64_t qty, uint32_t key) {
    om::order_t& mutableOrder = const_cast<om::order_t&>(order);
    mutableOrder.debug.i[key] = qty;
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
constexpr uint32_t toUInt(EnumT value) {
    return static_cast<uint32_t>(value);
}

enum ice_rejects : uint8_t {
    TOO_LATE_TO_CANCEL = 0,
    UNKNOWN_ORDER = 1
};

constexpr auto CME_ORDER_IS_NOT_IN_BOOK = 2045;

inline bool WasRejectedDueToBeingFilled(uint32_t rejectCode, hf::VENUE venue) {
    switch (venue) {
        case hf::VENUE::ICE: {
            return (rejectCode == ice_rejects::TOO_LATE_TO_CANCEL) | (rejectCode == ice_rejects::UNKNOWN_ORDER) | (rejectCode == om::toUType(om::OrderRejectReason::OrderNotActive));
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
    SystemState _relativeValue = SystemState::LoggedOut;
    SystemState _marketMaker = SystemState::LoggedOut;
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

    void update(hf::price_t price, hf::qty_t size) {
        if (_price != price) {
            _sizeAdjustment = 0;
            _sizeAdjustments.clear();
            LOG(0, "FAK {C[i1]} Id={i2} Prices different. Flushing adjustments. ", _contractId, _id)
        }
        _price = price;
        _size = size;
        LOG(0, "FAK {C[i1]} Id={i2} Price={d1} AdjustedSize={i3}", _contractId, _id, ToDollars(_price), getFakAdjustedSize())
    }

    void updateInternalMarket(hf::price_t price, hf::qty_t size) {
        _price = price;
        _size = size;
        LOG(0, "FAK {C[i1]} Id={i2} Price={d1} AdjustedSize={i3}", _contractId, _id, ToDollars(_price), getFakAdjustedSize())
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
    uint32_t _id;
    hf::price_t _price = hf::NULL_PRICE;
    hf::qty_t _size = 0;
    hf::qty_t _sizeAdjustment = 0;
    std::vector<SizeAdjustment> _sizeAdjustments;
    uint32_t _contractId;
};

enum OrderIds {ORDER_ID_MM_HEDGE = 0, ORDER_ID_MM_QUOTE = 1, ORDER_ID_RV_INTERNAL_QUOTE = 2};

struct Markets {

    Markets() {
        _markets[0].setId(OrderIds::ORDER_ID_MM_HEDGE);
        _markets[1].setId(OrderIds::ORDER_ID_MM_QUOTE);
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

inline bool IsSMPUnsolicitedCancel(hf::VENUE venue, uint32_t code) {
    (void)venue;
    return code == 107; //cancel aggressor on CME
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
    DisableExternalQuotingOnMarketMaker //Used as a command as an an Ack between FS and MM.
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
