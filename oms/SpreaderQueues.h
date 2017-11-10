//
// Created by pkusch on 12/26/16.
//

#ifndef RHONEOMS_SPREADERQUEUES_H
#define RHONEOMS_SPREADERQUEUES_H

#include <boost/format.hpp>
#include <hf/queue/U64QueueResource.h>
#include <hf/pool/DataPool.h>
#include <hf/mem/DataBlock.h>
#include <hf/queue/U64Queue.h>
#include <om/om.h>

namespace oms { namespace spreader  {

#define mb() asm volatile("mfence":::"memory")

constexpr hf::qty_t NULL_QTY = std::numeric_limits<hf::qty_t>::max();

enum class ErrorCode {Success, DuplicateOrderId, SpreadNotFound, CannotPauseSpreadNotRunning, CannotCancelAlreadyFinished, CannotResumeNotPaused, WrongNumberOfLegs,
                      Unsolicited, WorkBothSidesNotSupported, LegNotFound, UnsupportedProduct};

enum class LegOrderAction : uint32_t {New, Modify, Cancel, Fill};

struct LegCross {
    hf::VENUE _venue;
    hf::symbol_gid_t _gid;
    hf::price_t  _quotePrice;
    hf::price_t _hedgePrice;
    bool _quoteCross;
};

struct LegsCrossed {
    std::vector<LegCross> _legs;
    bool _crossedForQuotingLegs;
};

struct LegsCrossInfo {
    int _id;
    bool _isCrossed;
    int _nLegs;
    char _data[0];
};

struct IndividualLegOrderStatus {
    int _spreadId;
    hf::symbol_id_t _sid;
    om::order_id_t _oid;
    LegOrderAction _action;
    hf::SIDE _side;
    hf::price_t _price;
    hf::qty_t _qty;
    bool _rejected;
};

struct SpreadPrice {
    int _id;
    hf::price_t _bid;
    hf::price_t _ask;
};

struct SpreadDefinitionReply {
    char _name[20];
    ErrorCode _errorCode;
};

struct SpreadOrderReply {
    uint32_t _messageId;
    int _id;
    uint32_t _rejectCode;
    hf::VENUE _rejectVenue;
    bool _rejectedByExchange;
    char _errorMessage[20];
    ErrorCode _errorCode;
};

struct ClipDone {
    int _id;
    bool _spreadDone;
    hf::price_t _price;
};

struct OMStatus {
    hf::VENUE _venue;
    om::port_id_t _id;
    bool _active;
};

struct RemoteFill {
    int _spreadId;
    hf::symbol_gid_t _gid;
    hf::qty_t _filledQty;
};

struct InstrumentMdStatus {
    hf::symbol_id_t _sid;
    hf::VENUE _venue;
    bool _status;
};

enum class SpreadMessageType : uint32_t {
    NewSpreadOrder, NewSpreadOrderReply, CancelSpreadOrder, CancelSpreadOrderReply, PauseSpreadOrder, PauseSpreadOrderReply, UnsolicitedSpreadPause,
    ResumeSpreadOrder, ResumeSpreadOrderReply, Price, SpreadFinished, UnsolicitedCancel, Logout, ClientDown, LogoutReply, Login, LoginReply, OnClientErrorReply,
    LegOrderStatus, GatewayStatus, InstrumentStatus, LegsCrossInfo, AcceptCrossedOrder, DiscardCrossedOrder, AcceptCrossedOrderReply, DiscardCrossedOrderReply,
    IncrementAllowedNumberOfClips, ChangeSpreadPriceBy, ClipDone, SetSpreadPrice, NumberOfClipsToAllowedClips, ModifySpreadOrder, ModifySpreadOrderReply,
    RemoteFill, ForwardSpreadToSlave
};

struct SpreaderMessage {
    SpreadMessageType _type;
    char _data[0];
};

struct SpreadOrder {
    char _spreadName[20];
    int _clientOrderId;
    hf::SIDE _side;
    hf::price_t _price;
    hf::qty_t _lotSize;
    hf::qty_t _numberOfClips;
    hf::qty_t _allowedNumberOfClips;
    hf::price_t _numberOfTicksFromTopToQuote;
    bool _test;
    bool _skipCrossTest;
};

enum class LeanPricingType {TopLevel, WeightedAvg, TopOrSecond, FullLevel, SyntheticTop};

enum class InitiationQuantityType {Absolute, Relative};

struct LegDefinition {
    hf::qty_t _initiationQuantity;
    hf::qty_t _triggerQuantity;
    double _factor;
    double _divisor;
    hf::qty_t _quantityFactor;
    int _toQuote;
    hf::price_t _tickSize;
    hf::symbol_gid_t _gid;
    hf::price_t _nPayUpTicks;
    hf::price_t _nTicksToDoLastCross;
    LeanPricingType _leanPricingType;
    InitiationQuantityType _initiationQuantityType;
    double _initiationMultiplier;
    hf::qty_t _syntheticAdjustment;
    hf::qty_t _syntheticMinSize;
};

struct SpreadDefinition {
    hf::VENUE _venue;
    hf::price_t _adjustor;
    uint32_t _nLegs;
    char _legs[0];
};

struct Spread {
    uint32_t _messageId;
    SpreadOrder _order;
    SpreadDefinition _definition;
};

struct ClientOrderId {
    int _id;
    uint32_t _messageId;
};

struct IntUpdate {
    int _id;
    int64_t _update;
};

enum class AlgoMessageType {Theo, CoverOrder, SMACancelled, SMAResumed, SMAPaused, OrdePrice};

struct SpreadCoverIds {
    int _original;
    int _cover;
    hf::price_t _price;
};

struct AlgoMessage {
    AlgoMessageType _type;
    char _data[50];
};

class SpreaderPool : public hf::pool::DataPool {
public:

    void create();

    SpreaderMessage &getNextMessage() const {
        return *static_cast<SpreaderMessage *> (getNextP());
    }

    SpreaderMessage &getMessage(const uint64_t idx) const {
        return *static_cast<SpreaderMessage *> (getData(idx));
    }
};

class SpreaderQueue : public hf::queue::U64Queue, protected hf::mem::DataBlock {
public:

    struct my_queue_t {
        uint64_t memsize;
        uint8_t data[0];
    };

    void create();

    void set_pool(SpreaderPool *ptr) { m_pPool = ptr; }

    SpreaderPool *pool() const { return m_pPool; }

private:
    SpreaderPool *m_pPool = nullptr;
};

}}

#endif //RHONEOMS_SPREADERQUEUES_H
