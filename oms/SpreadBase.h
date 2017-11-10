//
// Created by pkusch on 1/30/17.
//

#ifndef RHONEOMS_SPREADBASE_H
#define RHONEOMS_SPREADBASE_H

#include <hf/hf.h>
#include <om/om_event.h>
#include <om/om_reject_codes.h>
#include "SpreadCommon.h"

namespace oms { namespace spreader {

enum class UnsolicitedPauseType {UnsolicitedCancel, NewOrderReject, ModifyOrderReject, CancelReject};
const char RepriceQuote = 'R', RepriceHedge = 'H', MoveQuote = 'M', CreateQuote ='Q', CancelOrder = 'C';

constexpr auto CME_ORDER_IS_NOT_IN_BOOK = 2045;
constexpr auto EUREX_ORDER_NOT_FOUND = 10000;
constexpr auto EURONEXT_UNKNOWN_ORDER = 1;
constexpr auto ICE_TOO_LATE_TO_CANCEL = 0;

struct PauseType {
    bool _isUnsolicited = false;
    int _code;
    UnsolicitedPauseType _unsolicitedType;
};

template <typename Executor, typename SpreadT>
class SpreadBase {
public:

    enum class State {
        Created, Running, BeingPaused, Paused, BeingCancelled, Cancelled, Finished
    };

    bool updateNumberOfClips(hf::qty_t newNumberOfClips, bool setAllowed) {
        if (_allowedNumberOfClips < newNumberOfClips) {
            _totalQuantity = newNumberOfClips * _lotSize;
            if (setAllowed) {
                _allowedNumberOfClips = newNumberOfClips;
            }
        } else if (_allowedNumberOfClips == newNumberOfClips) {
            return setNumberOfClipsToAllowedClips();
        }
        return false;
    }

    bool setNumberOfClipsToAllowedClips() {
        _totalQuantity = _allowedNumberOfClips * _lotSize;
        return pthis()->checkIfSpreadDone();
    }

    void incrementAllowedNumberOfClips() {
        _intendedPrice = hf::NULL_PRICE;
        ++_allowedNumberOfClips;
        pthis()->loadLotSize();
        if (isRunning()) {
            pthis()->startQuotingOrReprice();
        }
    }

    ErrorCode changeSpreadPriceBy(hf::price_t change) {
        _price += change;
        if (isRunning()) {
            pthis()->startQuotingOrReprice();
        }
        return ErrorCode::Success;
    }

    ErrorCode setSpreadPrice(hf::price_t price) {
        _price = price;
        if (isRunning()) {
            pthis()->startQuotingOrReprice();
        }
        return ErrorCode::Success;
    }

    bool hasAcquiredAllQuantity() {
        return _workingQuantity >= _totalQuantity;
    }

    bool isRunning() {
        return _state == State::Running;
    }

    SpreadT* pthis() { return static_cast<SpreadT*>(this); }

    om::order_t* newOrder(Leg& leg, hf::price_t price, hf::qty_t quantity) {
        return _executor->newOrder(leg, price, quantity, pthis());
    }

    void modifyOrder(const om::order_t* order, hf::price_t price, hf::qty_t quantity) {
        _executor->modifyOrder(order, price, quantity, pthis());
    }

    void setOrderToCancel(const om::order_t& order) {
        const_cast<om::order_t&>(order).debug.ch[0] = CancelOrder;
    }

    bool isOrderToCancel(const om::order_t& order) {
        return order.debug.ch[0] == CancelOrder;
    }

    void cancelQuoteOrder(Leg& leg, bool hardCancel) {
        if ((leg._quoteOrder == nullptr) | !leg._quoteOrder->is_active | leg._quoteOrder->is_cancelling | this->isOrderToCancel(*leg._quoteOrder))
            return;

        if (cannotChangeOrder(leg._quoteOrder, leg)) {
            if (hardCancel)
                setOrderToCancel(*leg._quoteOrder);
        } else {
            doCancelQuoteOrder(leg);
        }
    }

    void doCancelQuoteOrder(Leg& leg) {
        _executor->cancelOrder(leg._quoteOrder, pthis());
        leg._quotedPrice = hf::NULL_PRICE;
        leg._quotedQuantity = 0;
    }

    bool cannotChangeOrder(const om::order_t* order, Leg& leg) {
        const bool cannotMoveNewOrder = !order->is_acked & leg._waitForNewOrderAck;
        const bool cannotMoveModify = order->is_modifying & leg._waitForModifyAck;
        return cannotMoveNewOrder | cannotMoveModify;
    }

    bool isDone() {
        switch (_state) {
            case State::Cancelled:
            case State::Finished:
                return true;
            default:
                return false;
        }
    }

    void setHedgeOrderToReprice(const om::order_t& order) {
        const_cast<om::order_t&>(order).debug.ch[0] = 'H';
    }

    void tryRepriceSellHedgeOrder(const HedgeOrder& order, Leg& leg, const md::top_t& book) {
        if (!order._order->is_active | order._order->is_cancelling)
            return;

        if (cannotChangeOrder(order._order, leg)) {
            setHedgeOrderToReprice(*order._order);
            return;
        }

        const hf::price_t topBid = book.l[0].pr;
        if (topBid != hf::NULL_PRICE) {
            if (topBid > order._priceToDoLastCross) {
                if (shouldCrossOnSellSide(leg)) {
                    if (order._order->pr != topBid) {
                        _LOG_INFO(boost::str(boost::format("Repricing - cross - sell hedge leg to %1%") % topBid));

                        auto& di = _executor->createKDBLog();
                        di.ch[0] = 'R';
                        di.ch[1] = 'H';
                        di.ch[2] = 'C';
                        di.i[0] = leg._sid;
                        di.i[1] = topBid;

                        modifyOrder(order._order, topBid, order._order->qty);
                    }
                } else {
                    const hf::price_t hedgeQuotePrice = getSellHedgeQuotePrice(leg);
                    if (order._order->pr != hedgeQuotePrice) {
                        _LOG_INFO(boost::str(boost::format("Repricing - quote - sell hedge leg to %1%") % hedgeQuotePrice));

                        auto& di = _executor->createKDBLog();
                        di.ch[0] = 'R';
                        di.ch[1] = 'H';
                        di.ch[2] = 'Q';
                        di.i[0] = leg._sid;
                        di.i[1] = hedgeQuotePrice;

                        modifyOrder(order._order, hedgeQuotePrice, order._order->qty);
                    }
                }
            } else {
                const hf::price_t hammerPrice = topBid - leg._definition._nTicksToDoLastCross * leg._definition._tickSize;
                if (order._order->pr != hammerPrice) {
                    _LOG_INFO(boost::str(boost::format("Repricing - hammer - sell hedge leg to %1%") % hammerPrice));

                    auto &di = _executor->createKDBLog();
                    di.ch[0] = 'R'; di.ch[1] = 'H'; di.ch[2] = 'H';
                    di.i[0] = leg._sid;
                    di.i[1] = hammerPrice;

                    modifyOrder(order._order, hammerPrice, order._order->qty);
                }
            }
        } else {
            _LOG_INFO(boost::str(boost::format("Repricing sell hedge. Can do nothing since there is no bid market.")));

            auto &di = _executor->createKDBLog();
            di.ch[0] = 'R'; di.ch[1] = 'H'; di.ch[2] = 'F';
            di.i[0] = leg._sid;
        }
    }

    void tryRepriceSellHedgeOrder(const om::order_t* order, Leg& leg, const md::top_t& book) {
        const HedgeOrder& hedgeOrder = *leg.getHedgeOrder(order);
        tryRepriceSellHedgeOrder(hedgeOrder, leg, book);
    }

    void tryRepriceBuyHedgeOrder(const om::order_t* order, Leg& leg, const md::top_t& book) {
        const HedgeOrder& hedgeOrder = *leg.getHedgeOrder(order);
        tryRepriceBuyHedgeOrder(hedgeOrder, leg, book);
    }

    void tryRepriceBuyHedgeOrder(const HedgeOrder& order, Leg& leg, const md::top_t& book) {
        if (!order._order->is_active | order._order->is_cancelling)
            return;

        if (cannotChangeOrder(order._order, leg)) {
            setHedgeOrderToReprice(*order._order);
            return;
        }

        const hf::price_t topAsk = book.l[0].pr;
        if (topAsk != hf::NULL_PRICE) {
            if (topAsk < order._priceToDoLastCross) {
                if (shouldCrossOnBuySide(leg)) {
                    if (order._order->pr != topAsk) {
                        _LOG_INFO(boost::str(boost::format("Repricing - cross buy hedge leg to sid=%1% price=%2% qty=%3%") % order._order->sid % topAsk % order._order->qty));

                        auto &di = _executor->createKDBLog();
                        di.ch[0] = 'R'; di.ch[1] = 'H'; di.ch[2] = 'C';
                        di.i[0] = leg._sid;
                        di.i[1] = topAsk;

                        modifyOrder(order._order, topAsk, order._order->qty);
                    }
                } else {
                    const hf::price_t hedgeQuotePrice = getBuyHedgeQuotePrice(leg);
                    if (order._order->pr != hedgeQuotePrice) {
                        _LOG_INFO(boost::str(boost::format("Repricing - quote - buy hedge leg to sid=%1% price=%2% qty=%3%") % order._order->sid % hedgeQuotePrice % order._order->qty));

                        auto &di = _executor->createKDBLog();
                        di.ch[0] = 'R'; di.ch[1] = 'H'; di.ch[2] = 'Q';
                        di.i[0] = leg._sid;
                        di.i[1] = hedgeQuotePrice;

                        modifyOrder(order._order, hedgeQuotePrice, order._order->qty);
                    }
                }
            } else {
                const hf::price_t hammerPrice = topAsk + leg._definition._nTicksToDoLastCross * leg._definition._tickSize;
                if (order._order->pr != hammerPrice) {
                    _LOG_INFO(boost::str(boost::format("Repricing - hammer - buy hedge leg to sid=%1% price=%2% qty=%3%") % order._order->sid % hammerPrice % order._order->qty));

                    auto &di = _executor->createKDBLog();
                    di.ch[0] = 'R'; di.ch[1] = 'H'; di.ch[2] = 'H';
                    di.i[0] = leg._sid;
                    di.i[1] = hammerPrice;

                    modifyOrder(order._order, hammerPrice, order._order->qty);
                }
            }
        } else {
            _LOG_INFO(boost::str(boost::format("Repricing buy hedge. Can do nothing since there is no offer market.")));

            auto &di = _executor->createKDBLog();
            di.ch[0] = 'R'; di.ch[1] = 'H'; di.ch[2] = 'F';
            di.i[0] = leg._sid;
        }
    }

    ErrorCode pause(uint32_t messageId, bool& sendReply, bool isUnsolicited = false, int code = 0, UnsolicitedPauseType type = UnsolicitedPauseType::UnsolicitedCancel) {
        _LOG_INFO(boost::str(boost::format("Pausing spread. SpreadId=%1%") % _id));

        auto &di = _executor->createKDBLog();
        di.ch[0] = 'S'; di.ch[1] = 'P';
        di.i[0] = _id;

        if (_state == State::Running) {
            _pauseType._isUnsolicited = isUnsolicited;
            _pauseType._code = code;
            _pauseType._unsolicitedType = type;

            if (pthis()->hasWorkingOrders()) {
                _messageId = messageId;
                pthis()->cancelAllWorkingOrders(true);
                _state = State::BeingPaused;
            } else {
                sendReply = true;
                _state = State::Paused;
            }
            return ErrorCode::Success;
        }
        sendReply = true;
        return ErrorCode::CannotPauseSpreadNotRunning;
    }

    ErrorCode cancel(uint32_t messageId, bool& sendReply) {
        _LOG_INFO(boost::str(boost::format("Cancelling spread. SpreadId=%1%") % _id));

        auto &di = _executor->createKDBLog();
        di.ch[0] = 'S'; di.ch[1] = 'C'; di.ch[2] = 'C';
        di.i[0] = _id;

        pthis()->unsubscribeMd();

        switch (_state) {
            case State::Running: {
                if (pthis()->hasWorkingOrders()) {
                    _messageId = messageId;
                    pthis()->cancelAllWorkingOrders(true);
                    _state = State::BeingCancelled;
                } else {
                    sendReply = true;
                    _state = State::Cancelled;
                }
                return ErrorCode::Success;
            }
            case State::Paused: {
                _state = State::Cancelled;
                sendReply = true;
                return ErrorCode::Success;
            }
            case State::BeingCancelled:
            case State::BeingPaused: {
                _messageId = messageId;
                _state = State::BeingCancelled;
                return ErrorCode::Success;
            }
            case State::Finished: {
                sendReply = true;
                return ErrorCode::CannotCancelAlreadyFinished;
            }
            case State::Created: {
                sendReply = true;
                _state = State::Cancelled;
                return ErrorCode::Success;
            }
            default:
                break;
        }

        BOOST_ASSERT(false);
        throw std::runtime_error("Unhandled cancel case");
    }

    ErrorCode resume() {
        _LOG_INFO(boost::str(boost::format("Resuming spread. SpreadId=%1%") % _id));

        auto &di = _executor->createKDBLog();
        di.ch[0] = 'S'; di.ch[1] = 'R'; di.ch[2] = 'R';
        di.i[0] = _id;

        if (_state == State::Paused) {
            _state = State::Running;
            resumeTrading();
            return ErrorCode::Success;
        }
        return ErrorCode::CannotResumeNotPaused;
    }

    void resumeTrading() {
        pthis()->startHedging();
        pthis()->startQuoting();
    }

    void cancelAllOrders(Leg& leg, bool hardCancel) {
        if (leg.isQuoted()) {
            cancelQuoteOrder(leg, hardCancel);
        }
        for (auto order : leg._hedgeOrders) {
            pthis()->cancelHedgeOrder(order._order, leg, hardCancel);
        }
    }

    void cancelHedgeOrder(const om::order_t* order, Leg& leg, bool hardCancel) {
        if (!order->is_active | order->is_cancelling)
            return;

        if (cannotChangeOrder(order, leg)) {
            if (hardCancel)
                setOrderToCancel(*order);
        } else {
            doCancelHedgeOrder(order, leg);
        }
    }

    void doCancelHedgeOrder(const om::order_t* order, Leg& leg) {
        _executor->cancelOrder(order, pthis());
        leg._hedgingQuantity -= order->qty;
    }

    void stopTrading() {
        LOG_INFO(boost::str(boost::format("Stopping trading. SpreadId=%1%") % _id));
        _executor->stopTrading();
    }

    void onOrderRejectedCommon(const om::OMOrderRejectedEvent &event, const om::order_t &order) {
        _LOG_INFO(boost::str(boost::format("New order reject. Reason=%1% %2%") % event.reasonCodeExt % event.reasonText));
        bool done;
        pause(0, done, true, event.reasonCodeExt, UnsolicitedPauseType::NewOrderReject);
    }

    bool wasRejectedDueToBeingFilled(uint32_t rejectCode, hf::VENUE venue) {
        switch (venue) {
            case hf::VENUE::CME:
                return rejectCode == CME_ORDER_IS_NOT_IN_BOOK || rejectCode == om::toUType(om::OrderRejectReason::OrderNotActive);
            case hf::VENUE::EUREX:
                return rejectCode == EUREX_ORDER_NOT_FOUND || rejectCode == om::toUType(om::OrderRejectReason::OrderNotActive);
            case hf::VENUE::ICE:
                return rejectCode == ICE_TOO_LATE_TO_CANCEL || rejectCode == om::toUType(om::OrderRejectReason::OrderNotActive);
            case hf::VENUE::EURONEXT:
                return rejectCode == EURONEXT_UNKNOWN_ORDER || rejectCode == om::toUType(om::OrderRejectReason::OrderNotActive);
            default:
                BOOST_ASSERT(false);
        }
    }

    bool onOrderCancelRejectedCommon(const om::OMOrderCancelRejectedEvent &event, const om::order_t &order) {
        bool rejectedDueToFill = wasRejectedDueToBeingFilled(event.reasonCodeExt, order.venue);
        if (!rejectedDueToFill) {
            _LOG_INFO(boost::str(boost::format("Order cancel reject. Will stop trading. Reason=%1% %2%") % event.reasonCodeExt % event.reasonText));
            stopTrading();
        }
        return rejectedDueToFill;
    }

    LegCross getLegCross(Leg& leg, hf::price_t quotePrice) {
        hf::price_t topBid = leg._books->_bidBook.l[0].pr;
        hf::price_t topAsk = leg._books->_askBook.l[0].pr;
        LegCross legCross;
        legCross._quotePrice = quotePrice;
        if (quotePrice != hf::NULL_PRICE) {
            legCross._quoteCross = leg.isBuy() ? quotePrice >= topAsk : quotePrice <= topBid;
        } else {
            legCross._quoteCross = false;
        }
        legCross._hedgePrice = leg.isBuy() ? topAsk : topBid;
        legCross._gid = leg._definition._gid;
        legCross._venue = leg._venue;
        return legCross;
    }

    PauseType& getPauseType() {
        return _pauseType;
    }

    void sendClipDone(bool done) {
        _executor->sendClipDone(_id, done, _intendedPrice);
    }

    void createMarketData(hf::symbol_id_t sid, Leg& leg) {
        _executor->createMarketData(sid, leg);
    }

    hf::price_t _price;
    hf::price_t _numberOfTicksFromTopToQuote;
    hf::price_t _adjustor;
    hf::qty_t _totalQuantity;
    hf::qty_t _lotSize;
    hf::qty_t _workingQuantity = 0;
    hf::qty_t _allowedNumberOfClips = 0;
    hf::qty_t _workingNumberOfClips = 0;
    State _state = State::Created;
    PauseType _pauseType;
    uint32_t _messageId = 0;
    int _id;
    uint32_t _nLegs;
    hf::price_t _intendedPrice = hf::NULL_PRICE;
    Executor* _executor;
};


}}

#endif //RHONEOMS_SPREADBASE_H
