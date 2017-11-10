//
// Created by pkusch on 1/30/17.
//

#ifndef RHONEOMS_ENLEGSPREAD_H
#define RHONEOMS_ENLEGSPREAD_H

#include <stdint.h>
#include <hf/hf.h>
#include <md/md_event.h>
#include "SpreadCommon.h"
#include "SpreaderQueues.h"
#include "SpreadBase.h"

namespace oms { namespace spreader {

template <typename Executor>
class EnLegSpread : public SpreadBase<Executor, EnLegSpread<Executor>> {

public:
    typedef SpreadBase<Executor, EnLegSpread<Executor>> BaseT;

    void loadLotSize() {
        if (this->_allowedNumberOfClips > this->_workingNumberOfClips) {
            _LOG_INFO(boost::str(boost::format("Loading lot size. SpreadId=%1% LotSize=%2% Working=%3% Need=%4%") % this->_id % this->_lotSize % this->_workingQuantity % this->_totalQuantity));

            auto &di = this->_executor->createKDBLog();
            di.ch[0] = 'L'; di.ch[1] = 'L';
            di.i[0] = this->_id;
            di.i[1] = this->_lotSize;
            di.i[2] = this->_workingQuantity;
            di.i[3] = this->_totalQuantity;

            this->_workingQuantity += this->_lotSize;

            for (uint32_t i = 0; i < this->_nLegs; ++i) {
                Leg &leg = _legs[i];
                leg.loadLotSize(this->_lotSize);
            }

            ++this->_workingNumberOfClips;
        } else {
            _LOG_INFO(boost::str(boost::format("Could not load clip since it is not allowed. Allowed=%1% Working=%2%") % this->_allowedNumberOfClips % this->_workingNumberOfClips));

            auto &di = this->_executor->createKDBLog();
            di.ch[0] = 'L'; di.ch[1] = 'L'; di.ch[2] = 'F';
            di.i[0] = this->_id;
            di.i[1] = this->_allowedNumberOfClips;
            di.i[2] = this->_workingNumberOfClips;
        }
    }

    bool hasWorkingOrders() {
        bool hasWorkingOrders = false;
        for (uint32_t i = 0; i < this->_nLegs; ++i) {
            Leg &leg = _legs[i];
            hasWorkingOrders |= leg.hasWorkingOrders();
        }
        return hasWorkingOrders;
    }

    void unsubscribeMd() {
        for (uint32_t i = 0; i < this->_nLegs; ++i) {
            Leg &leg = _legs[i];
            this->_executor->unsubscribeMD(leg._sid, this);
        }
    }

    void cancelAllWorkingOrders(bool hardCancel) {
        for (uint32_t i = 0; i < this->_nLegs; ++i) {
            Leg &leg = _legs[i];
            this->cancelAllOrders(leg, hardCancel);
        }
    }

    void startQuoting() {
        if (this->isRunning()) {
            for (uint32_t i = 0; i < this->_nLegs; ++i) {
                Leg &leg = _legs[i];
                startQuotingLeg(leg);
            }
        }
    }

    void startHedging() {
        for (uint32_t i = 0; i < this->_nLegs; ++i) {
            Leg& hedgedLeg = _legs[i];
            doHedgingLeg(hedgedLeg);
        }
    }

    ErrorCode onRemoteFill(hf::symbol_gid_t gid, hf::qty_t fillQuantity, bool& spreadDone) {
        ErrorCode errorCode = ErrorCode::Success;
        Leg* leg = getLegByGid(gid, errorCode);
        leg->_filledQuantity += fillQuantity;
        if (errorCode == ErrorCode::Success) {
            for (uint32_t i = 0; i < this->_nLegs; ++i) {
                Leg& hedgedLeg = _legs[i];
                doHedgingLeg(hedgedLeg);
            }
        } else {
            return errorCode;
        }

        spreadDone = false;

        if (areAllLegsFilled()) {
            if (!this->hasAcquiredAllQuantity()) {
                loadLotSize();
                startQuoting();
                this->sendClipDone(false);
            } else {
                this->_state = BaseT::State::Finished;
                unsubscribeMd();
                this->sendClipDone(true);
                spreadDone =  true;
            }
        }

        return ErrorCode::Success;
    }

    Leg* getLegByGid(hf::symbol_gid_t gid, ErrorCode& code) {
        for (uint32_t i = 0; i < this->_nLegs; ++i) {
            Leg &leg = _legs[i];
            if (leg._definition._gid == gid)
                return &leg;
        }
        code = ErrorCode::LegNotFound;
        return nullptr;
    }

    hf::qty_t calculateHedgeSizeFromAllLegs(Leg& hedgedLeg) {
        hf::qty_t hedgedSize = 0;
        for (uint32_t j = 0; j < this->_nLegs; ++j) {
            Leg& leanLeg = _legs[j];
            if (&leanLeg != &hedgedLeg) {
                const hf::qty_t oneHedge = calculateWorkingHedgeQuantity(hedgedLeg, leanLeg);
                hedgedSize = std::max(hedgedSize, oneHedge);
            }
        }
        return hedgedSize;
    }

    void doHedgingLeg(Leg& hedgedLeg) {
        if (hedgedLeg._isRemote) {
            return;
        }

        hf::qty_t hedgedSize = calculateHedgeSizeFromAllLegs(hedgedLeg);
        if (hedgedSize > hedgedLeg._hedgingQuantity) {
            const hf::qty_t newHedgeQuantity = hedgedSize - hedgedLeg._hedgingQuantity;
            if (hedgedLeg.isBuy()) {
                newHedgingOrder(hedgedLeg, getHedgePrice<hf::SIDE::BUY>(hedgedLeg), newHedgeQuantity, getPayUpPriceBase<hf::SIDE::BUY>(hedgedLeg));
            } else {
                newHedgingOrder(hedgedLeg, getHedgePrice<hf::SIDE::SELL>(hedgedLeg), newHedgeQuantity, getPayUpPriceBase<hf::SIDE::SELL>(hedgedLeg));
            }
        }
    }

    void moveQuoteOrderToHedgeOrder(Leg& leg) {
        const hf::qty_t workingHedgeQuantity = calculateHedgeSizeFromAllLegs(leg);
        if (workingHedgeQuantity > leg._hedgingQuantity) {
            const hf::qty_t newHedgeQuantity = workingHedgeQuantity - leg._hedgingQuantity;
            hf::price_t price;
            hf::price_t payUpPriceBase;
            if (leg.isBuy()) {
                price = getHedgePrice<hf::SIDE::BUY>(leg);
                payUpPriceBase = getPayUpPriceBase<hf::SIDE::BUY>(leg);
            } else {
                price = getHedgePrice<hf::SIDE::SELL>(leg);
                payUpPriceBase = getPayUpPriceBase<hf::SIDE::SELL>(leg);
            }
            moveQuoteOrderToHedgeOrder(leg, price, newHedgeQuantity, payUpPriceBase);
            _LOG_INFO(boost::str(boost::format("Moving quote order to hedge order Sid=%1% Price=%2% Qty=%3%") % leg._sid % price % newHedgeQuantity));

            auto &di = this->_executor->createKDBLog();
            di.ch[0] = 'M'; di.ch[1] = 'Q';
            di.i[0] = this->_id;
            di.i[1] = leg._sid;
            di.i[2] = price;
            di.i[3] = newHedgeQuantity;
        }
    }

    bool onOrderCancelRejected(const om::OMOrderCancelRejectedEvent &event, const om::order_t &order)  {
        bool rejectedDueToFill = BaseT::onOrderCancelRejectedCommon(event, order);
        if (rejectedDueToFill) {
            switch (this->_state) {
                case BaseT::State::BeingCancelled: {
                    if (!hasWorkingOrders()) {
                        this->_state = BaseT::State::Cancelled;
                    }
                    break;
                }
                case BaseT::State::BeingPaused: {
                    if (!hasWorkingOrders()) {
                        this->_state = BaseT::State::Paused;
                    }
                    break;
                }
                default:
                    break;
            }
        }
        return rejectedDueToFill;
    }

    void onOrderRejected(const om::OMOrderRejectedEvent &event, const om::order_t &order) {
        Leg &leg = *getLeg(order.sid);
        leg.removeOrder(&order);
        BaseT::onOrderRejectedCommon(event, order);

        switch (this->_state) {
            case BaseT::State::BeingCancelled: {
                if (!hasWorkingOrders()) {
                    this->_state = BaseT::State::Cancelled;
                }
                break;
            }
            case BaseT::State::BeingPaused: {
                if (!hasWorkingOrders()) {
                    this->_state = BaseT::State::Paused;
                }
                break;
            }
            default:
                break;
        }
    }

    void onOrderAccepted(const om::OMOrderAcceptedEvent &event, const om::order_t &order) {
        if (this->isRunning()) {
            Leg &leg = *getLeg(order.sid);
            switch (order.debug.ch[0]) {
                case MoveQuote: {
                    moveQuoteOrderToHedgeOrder(leg);
                    setQuoteOrderToQuote(order);
                    break;
                }
                case RepriceQuote: {
                    repriceQuotingLeg(leg);
                    clearOrderFlag(order);
                    break;
                }
                case RepriceHedge: {
                    if (leg.isBuy()) {
                        this->tryRepriceBuyHedgeOrder(&order, leg, leg._books->_askBook);
                    } else {
                        this->tryRepriceSellHedgeOrder(&order, leg, leg._books->_bidBook);
                    }
                    clearOrderFlag(order);
                    break;
                }
                case CancelOrder: {
                    if (leg._quoteOrder == &order) {
                        this->doCancelQuoteOrder(leg);
                    } else {
                        this->doCancelHedgeOrder(&order, leg);
                    }
                    clearOrderFlag(order);
                    break;
                }
                case 0: {
                    break;
                }
                default:
                    LOG_ERROR(boost::str(boost::format("onOrderAccepted. Invalid code. OID=%1% Code=%2%") % order.oid % order.debug.ch[0]));
            }
        } else {
            if (this->_state == BaseT::State::BeingCancelled || this->_state == BaseT::State::BeingPaused) {
                Leg &leg = *getLeg(order.sid);
                if (leg._quoteOrder == &order) {
                    this->doCancelQuoteOrder(leg);
                } else {
                    this->doCancelHedgeOrder(&order, leg);
                }
            }
        }
    }

    void onUnsolicitedOrderCancelled(const om::order_t &order, const om::OMOrderCanceledEvent &event) {
        Leg& leg = *getLeg(order.sid);
        leg.removeOrder(&order);

        switch (this->_state) {
            case BaseT::State::BeingCancelled: {
                if (!hasWorkingOrders()) {
                    this->_state = BaseT::State::Cancelled;
                }
                break;
            }
            case BaseT::State::BeingPaused: {
                if (!hasWorkingOrders()) {
                    this->_state = BaseT::State::Paused;
                }
                break;
            }
            case BaseT::State::Running: {
                if (!hasWorkingOrders()) {
                    this->_state = BaseT::State::Paused;
                    this->_pauseType._isUnsolicited = true;
                    this->_pauseType._code = event.reasonCodeExt;
                    this->_pauseType._unsolicitedType = UnsolicitedPauseType::UnsolicitedCancel;
                } else {
                    bool done;
                    this->pause(0, done, true, event.reasonCodeExt, UnsolicitedPauseType::UnsolicitedCancel);
                }
                break;
            }
            default:
                break;
        }
    }

    void onOrderCanceled(const om::OMOrderCanceledEvent &event, const om::order_t &order) {
        Leg& leg = *getLeg(order.sid);

        bool wasQuotingOrder = leg.removeOrder(&order);

        switch (this->_state) {
            case BaseT::State::BeingCancelled: {
                if (!hasWorkingOrders()) {
                    this->_state = BaseT::State::Cancelled;
                }
                break;
            }
            case BaseT::State::BeingPaused: {
                if (!hasWorkingOrders()) {
                    this->_state = BaseT::State::Paused;
                }
                break;
            }
            case BaseT::State::Running: {
                if (isQuoteOrderToQuote(order)) {
                    checkBothHedgeAndQuote(leg);
                } else if (wasQuotingOrder) {
                    startQuotingLeg(leg);
                }
            }
            default:
                break;
        }
    }

    hf::price_t getBidPrice() {
        double price = this->_adjustor;

        for (uint32_t i = 0; i < this->_nLegs; ++i) {
            Leg &leg = _legs[i];

            const auto topBid = leg._books->_bidBook.l[0].pr;

            if (topBid == hf::NULL_PRICE)
                return hf::NULL_PRICE;

            price += leg._definition._factor / leg._definition._divisor * topBid;
        }

        return static_cast<hf::price_t>(price);
    }

    hf::price_t getAskPrice() {
        double price = this->_adjustor;

        for (uint32_t i = 0; i < this->_nLegs; ++i) {
            Leg &leg = _legs[i];

            const auto topAsk = leg._books->_askBook.l[0].pr;

            if (topAsk == hf::NULL_PRICE)
                return hf::NULL_PRICE;

            price += leg._definition._factor / leg._definition._divisor * topAsk;
        }

        return static_cast<hf::price_t>(price);
    }

    hf::price_t getSpreadPriceUsingMidPrice() {
        double price = this->_adjustor;

        for (uint32_t i = 0; i < this->_nLegs; ++i) {
            Leg &leg = _legs[i];

            hf::price_t pricePart = leg.getPricePartUsingMidPrice();

            if (pricePart == hf::NULL_PRICE)
                return hf::NULL_PRICE;

            price += pricePart;
        }

        return static_cast<hf::price_t>(price);
    }

    Leg* getLeg(hf::symbol_id_t sid) {
        auto it = _sidToLeg.find(sid);
        if (it == _sidToLeg.end()) {
            return nullptr;
        }
        return it->second;
    }

    bool onOrderExecution(const om::OMExecutionEvent &event, const om::order_t &order) {
        Leg& leg = *getLeg(order.sid);
        leg._filledQuantity += event.filledQty;

        hedgeOtherLegs(leg);

        if (&order != leg._quoteOrder) {
            leg._hedgingQuantity -= event.filledQty;
        }

        if (!order.is_active) {
            leg.removeOrder(&order);
        }

        if (areAllLegsFilled()) {
            if (!this->hasAcquiredAllQuantity()) {
                loadLotSize();
                startQuoting();
                this->sendClipDone(false);
            } else {
                this->_state = BaseT::State::Finished;
                unsubscribeMd();
                this->sendClipDone(true);
                return true;
            }
        }

        if (this->_intendedPrice == hf::NULL_PRICE) {
            this->_intendedPrice = this->_price;
        }

        return false;
    }

    bool checkIfSpreadDone() {
        if (areAllLegsFilled()) {
            if (this->hasAcquiredAllQuantity()) {
                this->_state = BaseT::State::Finished;
                unsubscribeMd();
                this->sendClipDone(true);
                return true;
            }
        }
        return false;
    }

    bool areAllLegsFilled() {
        for (uint32_t i = 0; i < this->_nLegs; ++i) {
            Leg &leg = _legs[i];
            if (!leg.isFilled())
                return false;
        }
        return true;
    }

    void hedgeOtherLegs(Leg& fillLeg) {
        for (uint32_t i = 0; i < this->_nLegs; ++i) {
            Leg &leg = _legs[i];
            if (&leg != &fillLeg) {
                if (leg.isBuy()) {
                    hedgeLeg<hf::SIDE::BUY>(leg, fillLeg);
                } else {
                    hedgeLeg<hf::SIDE::SELL>(leg, fillLeg);
                }
            }
        }
    }

    hf::qty_t calculateWorkingHedgeQuantity(Leg& hedgedLeg, Leg& filledLeg) {
        const double hedgeRatio = static_cast<double>(hedgedLeg._targetQuantity) / filledLeg._targetQuantity;
        const hf::qty_t totalHedgeQuantity = static_cast<hf::qty_t>(round(filledLeg._filledQuantity * hedgeRatio));
        const hf::qty_t workingHedgeQuantity = totalHedgeQuantity - hedgedLeg._filledQuantity;
        return workingHedgeQuantity;
    }

    void newHedgingOrder(Leg& hedgedLeg, hf::price_t price, hf::qty_t quantity, hf::price_t payUpPrice) {
        if (this->_state != BaseT::State::Running)
            return;

        om::order_t* hedgeOrder = this->newOrder(hedgedLeg, price, quantity);
        hedgedLeg.addHedgeOrder(hedgeOrder, payUpPrice);
        hedgedLeg._hedgingQuantity += quantity;
    }

    template <hf::SIDE side>
    void hedgeLeg(Leg& hedgedLeg, Leg& filledLeg) {
        if (hedgedLeg._isRemote) {
            return;
        }

        if (hedgedLeg.isQuoted()) {
            if (this->cannotChangeOrder(hedgedLeg._quoteOrder, hedgedLeg)) {
                if (!this->isOrderToCancel(*hedgedLeg._quoteOrder))
                    setQuoteOrderToMove(*hedgedLeg._quoteOrder);
                return;
            }
        }

        const hf::qty_t workingHedgeQuantity = calculateWorkingHedgeQuantity(hedgedLeg, filledLeg);
        if (workingHedgeQuantity > hedgedLeg._hedgingQuantity) {
            const hf::qty_t newHedgeQuantity = workingHedgeQuantity - hedgedLeg._hedgingQuantity;
            const hf::price_t price = getHedgePrice<side>(hedgedLeg);
            const hf::price_t payUpPriceBase = getPayUpPriceBase<side>(hedgedLeg);

            _LOG_INFO(boost::str(boost::format("Hedging Sid=%1% Price=%2% Qty=%3%") % hedgedLeg._sid % price % newHedgeQuantity));

            auto &di = this->_executor->createKDBLog();
            di.ch[0] = 'H'; di.ch[1] = 'L';
            di.i[0] = this->_id;
            di.i[1] = hedgedLeg._sid;
            di.i[2] = price;
            di.i[3] = newHedgeQuantity;

            if (!hedgedLeg.isQuoted()) {
                newHedgingOrder(hedgedLeg, price, newHedgeQuantity, payUpPriceBase);
            } else {
                setQuoteOrderToQuote(*hedgedLeg._quoteOrder);
                moveQuoteOrderToHedgeOrder(hedgedLeg, price, newHedgeQuantity, payUpPriceBase);
            }
        }
    }

    void setQuoteOrderToMove(const om::order_t& order) {
        const_cast<om::order_t&>(order).debug.ch[0] = MoveQuote;
    }

    bool isQuoteOrderToMove(const om::order_t& order) {
        return order.debug.ch[0] == MoveQuote;
    }

    void moveQuoteOrderToHedgeOrder(Leg& leg, hf::price_t hedgePrice, hf::qty_t hedgeQuantity, hf::price_t payUpPriceBase) {
        om::order_t* hedgeOrder = leg._quoteOrder;
        leg.removeOrder(leg._quoteOrder);
        this->modifyOrder(hedgeOrder, hedgePrice, hedgeQuantity + hedgeOrder->executedQty);
        leg.addHedgeOrder(hedgeOrder, payUpPriceBase);
        leg._hedgingQuantity += hedgeQuantity;
    }

    void clearOrderFlag(const om::order_t& order) {
        const_cast<om::order_t&>(order).debug.ch[0] = 0;
    }

    void setQuoteOrderToQuote(const om::order_t& order) {
        const_cast<om::order_t&>(order).debug.ch[0] = CreateQuote;
    }

    bool isQuoteOrderToQuote(const om::order_t& order) {
        return order.debug.ch[0] == CreateQuote;
    }

    void setQuoteOrderToReprice(const om::order_t& order) {
        const_cast<om::order_t&>(order).debug.ch[0] = RepriceQuote;
    }

    void onOrderReplaced(const om::OMOrderModifiedEvent &event, const om::order_t &order) {
        if (this->isRunning()) {
            Leg &leg = *getLeg(order.sid);
            switch (order.debug.ch[0]) {
                case MoveQuote: {
                    moveQuoteOrderToHedgeOrder(leg);
                    setQuoteOrderToQuote(order);
                    break;
                }
                case RepriceQuote: {
                    repriceQuotingLeg(leg);
                    clearOrderFlag(order);
                    break;
                }
                case RepriceHedge: {
                    if (leg.isBuy()) {
                        this->tryRepriceBuyHedgeOrder(&order, leg, leg._books->_askBook);
                    } else {
                        this->tryRepriceSellHedgeOrder(&order, leg, leg._books->_bidBook);
                    }
                    clearOrderFlag(order);
                    break;
                }
                case CreateQuote: {
                    startQuotingLeg(leg);
                    clearOrderFlag(order);
                    break;
                }
                case CancelOrder: {
                    if (leg._quoteOrder == &order) {
                        this->doCancelQuoteOrder(leg);
                    } else {
                        this->doCancelHedgeOrder(&order, leg);
                    }
                    clearOrderFlag(order);
                    break;
                }
                case 0: {
                    break;
                }
                default:
                    LOG_ERROR(boost::str(boost::format("onOrderReplaced. Invalid code. OID=%1% Code=%2%") % order.oid % order.debug.ch[0]));
            }
        } else {
            if (this->_state == BaseT::State::BeingCancelled || this->_state == BaseT::State::BeingPaused) {
                Leg &leg = *getLeg(order.sid);
                if (leg._quoteOrder == &order) {
                    this->doCancelQuoteOrder(leg);
                } else {
                    this->doCancelHedgeOrder(&order, leg);
                }
            }
        }
    }

    void onOrderReplaceRejected(const om::OMOrderModifyRejectedEvent &event, const om::order_t &order) {
        if (!this->wasRejectedDueToBeingFilled(event.reasonCodeExt, order.venue)) {
            _LOG_INFO(boost::str(boost::format("Order replace reject. Reason=%1% %2%") % event.reasonCodeExt % event.reasonText));
            bool done;
            this->pause(0, done, true, event.reasonCodeExt, UnsolicitedPauseType::ModifyOrderReject);
        } else {
            if (this->isRunning()) {
                Leg& leg = *getLeg(order.sid);
                checkBothHedgeAndQuote(leg);
            } else {
                if (this->_state == BaseT::State::BeingCancelled || this->_state == BaseT::State::BeingPaused) {
                    Leg &leg = *getLeg(order.sid);
                    if (leg._quoteOrder == &order) {
                        this->doCancelQuoteOrder(leg);
                    } else {
                        this->doCancelHedgeOrder(&order, leg);
                    }
                }
            }
            clearOrderFlag(order);
        }
    }

    void checkBothHedgeAndQuote(Leg& leg) {
        doHedgingLeg(leg);
        startQuotingLeg(leg);
    }

    void onMDUpdate(hf::VENUE venue, const md::MDUpdateEvent &event) {
        Leg* legPtr = getLeg(event.sid);

        if (legPtr == nullptr)
            return;

        Leg &leg = *legPtr;

        if (this->isRunning()) {
            if (leg.isBuy()) {
                checkQuotingOtherLegs(leg, leg._books->_askBook, event.sell.updatedQtyNum);
                if (leg.isHedged()) {
                    repriceBuyHedgeOrders(leg, leg._books->_askBook, event.sell.updatedQtyNum);
                }
                checkQuotingThisLeg(leg, leg._books->_bidBook, event.buy.updatedQtyNum);
            } else {
                checkQuotingOtherLegs(leg, leg._books->_bidBook, event.buy.updatedQtyNum);
                if (leg.isHedged()) {
                    repriceSellHedgeOrders(leg, leg._books->_bidBook, event.buy.updatedQtyNum);
                }
                checkQuotingThisLeg(leg, leg._books->_askBook, event.sell.updatedQtyNum);
            }
        }
    }

    void checkQuotingThisLeg(Leg& leg, const md::top_t& book, md::level_index_t updatedLevel) {
        if (leg.isQuoted()) {
            if (updatedLevel != 1)
                return;

            if (!IsLegQuotePriceCloseToTop(leg._quotedPrice, leg)) {
                this->cancelQuoteOrder(leg, false);
            }
        } else {
            if (updatedLevel == 0)
                return;

            startQuotingLeg(leg);
        }
    }

    void repriceAllHedgingOrders() {
        for (uint32_t i = 0; i < this->_nLegs; ++i) {
            Leg& leg = _legs[i];
            for (HedgeOrder& hedgeOrder : leg._hedgeOrders) {
                if (leg.isBuy()) {
                    this->tryRepriceBuyHedgeOrder(hedgeOrder._order, leg, leg._books->_askBook);
                } else {
                    this->tryRepriceSellHedgeOrder(hedgeOrder._order, leg, leg._books->_bidBook);
                }
            }
        }
    }

    void repriceBuyHedgeOrders(Leg& leg, const md::top_t& book, md::level_index_t updatedLevel) {
        if (updatedLevel != 1)
            return;

        for (auto order : leg._hedgeOrders) {
            this->tryRepriceBuyHedgeOrder(order, leg, book);
        }
    }

    void repriceSellHedgeOrders(Leg& leg, const md::top_t& book, md::level_index_t updatedLevel) {
        if (updatedLevel != 1)
            return;

        for (auto order : leg._hedgeOrders) {
            this->tryRepriceSellHedgeOrder(order, leg, book);
        }
    }

    void checkQuotingOtherLegs(Leg& leanLeg, const md::top_t& book, md::level_index_t updatedLevel) {
        if (updatedLevel == 0)
            return;

        for (uint32_t i = 0; i < this->_nLegs; ++i) {
            Leg& leg = _legs[i];
            if (&leg != &leanLeg) {
                const hf::price_t leanPrice = leanLeg.getLeanPrice(leg, leg.getQuoteQuantity(), book);
                if (leanPrice == hf::NULL_PRICE) {
                    if (leg.isQuoted()) {
                        stopQuotingLeg(leg);
                    }
                } else {
                    startQuotingOrReprice(leg);
                }
            }
        }
    }

    void startQuotingOrReprice(Leg& leg) {
        if (leg.isQuoted()) {
            repriceQuotingLeg(leg);
        } else {
            startQuotingLeg(leg);
        }
    }

    void startQuotingOrReprice() {
        for (uint32_t i = 0; i < this->_nLegs; ++i)
            startQuotingOrReprice(_legs[i]);
    }

    void stopQuotingLeg(Leg& leg) {
        if ((leg._quoteOrder == nullptr) | !leg._quoteOrder->is_active | leg._quoteOrder->is_cancelling | this->isOrderToCancel(*leg._quoteOrder))
            return;

        if (this->cannotChangeOrder(leg._quoteOrder, leg)) {
            if (!isQuoteOrderToMove(*leg._quoteOrder))
                setQuoteOrderToReprice(*leg._quoteOrder);
            return;
        }

        this->doCancelQuoteOrder(leg);
    }

    void repriceQuotingLeg(Leg& leg) {
        _LOG_INFO(boost::str(boost::format("Repricing quoting leg. SpreadId=%1% Sid=%2%") % this->_id % leg._sid));

        if ((leg._quoteOrder == nullptr) | !leg._quoteOrder->is_active | leg._quoteOrder->is_cancelling | this->isOrderToCancel(*leg._quoteOrder))
            return;

        if (this->cannotChangeOrder(leg._quoteOrder, leg)) {
            if (!isQuoteOrderToMove(*leg._quoteOrder))
                setQuoteOrderToReprice(*leg._quoteOrder);
            return;
        }

        const hf::qty_t quotedQuantity = leg.getQuoteQuantity();
        const hf::price_t quotedPrice = calculatePrice(leg, quotedQuantity);
        if (quotedPrice != hf::NULL_PRICE) {
            if (quotedPrice != leg._quotedPrice) {
                if (IsLegQuotePriceCloseToTop(quotedPrice, leg)) {
                    leg._quotedPrice = quotedPrice;
                    leg._quotedQuantity = quotedQuantity;
                    this->modifyOrder(leg._quoteOrder, quotedPrice, leg._quoteOrder->qty);
                } else {
                    this->doCancelQuoteOrder(leg);
                }
            }
        } else {
            this->doCancelQuoteOrder(leg);
        }
    }

    bool IsLegQuotePriceCloseToTop(hf::price_t price, Leg& leg) {
        return leg.isBuy() ? IsBuyLegQuotePriceCloseToTop(price, leg, this->_numberOfTicksFromTopToQuote) :
                             IsSellLegQuotePriceCloseToTop(price, leg, this->_numberOfTicksFromTopToQuote);
    }

    void startQuotingLeg(Leg& quoteLeg) {
        if (quoteLeg._isRemote) {
            _LOG_INFO(boost::str(boost::format("Did not start quoting leg. Leg is remote. SpreadId=%1% Sid=%2%") % this->_id % quoteLeg._sid));
            return;
        }

        if (!quoteLeg.canQuote()) {
            _LOG_INFO(boost::str(boost::format("Did not start quoting leg. Cannot quote. SpreadId=%1% Sid=%2%") % this->_id % quoteLeg._sid));

            auto &di = this->_executor->createKDBLog();
            di.ch[0] = 'Q'; di.ch[1] = 'F'; di.ch[2] = 'C';
            di.i[0] = this->_id;
            di.i[1] = quoteLeg._sid;

            return;
        }

        if (quoteLeg._quoteOrder != nullptr) {
            _LOG_INFO(boost::str(boost::format("Did not start quoting leg. Already quoting. SpreadId=%1% Sid=%2%") % this->_id % quoteLeg._sid));

            auto &di = this->_executor->createKDBLog();
            di.ch[0] = 'Q'; di.ch[1] = 'F'; di.ch[2] = 'Q';
            di.i[0] = this->_id;
            di.i[1] = quoteLeg._sid;

            return;
        }

        const hf::price_t price = calculatePrice(quoteLeg, quoteLeg.getQuoteQuantity());

        if (price == hf::NULL_PRICE) {
            _LOG_INFO(boost::str(boost::format("Did not start quoting leg. Quote price is null. SpreadId=%1% Sid=%2%") % this->_id % quoteLeg._sid));

            auto &di = this->_executor->createKDBLog();
            di.ch[0] = 'Q'; di.ch[1] = 'F'; di.ch[2] = 'P';
            di.i[0] = this->_id;
            di.i[1] = quoteLeg._sid;

            return;
        }

        if (!IsLegQuotePriceCloseToTop(price, quoteLeg)) {
            _LOG_INFO(boost::str(boost::format("Did not start quoting leg. Quote price too far from top. SpreadId=%1% Sid=%2%") % this->_id % quoteLeg._sid));

            //auto &di = this->_executor->createKDBLog();
            //di.ch[0] = 'Q'; di.ch[1] = 'F'; di.ch[2] = 'F';
            //di.i[0] = this->_id;
            //di.i[1] = quoteLeg._sid;

            return;
        }

        quoteLeg._quotedPrice = price;
        quoteLeg._quotedQuantity = quoteLeg.getQuoteQuantity();
        quoteLeg._quoteOrder = this->newOrder(quoteLeg, quoteLeg._quotedPrice, quoteLeg._quotedQuantity);

        _LOG_INFO(boost::str(boost::format("Started quoting leg. SpreadId=%1% Sid=%2%") % this->_id % quoteLeg._sid));

        auto &di = this->_executor->createKDBLog();
        di.ch[0] = 'Q'; di.ch[1] = 'S';
        di.i[0] = this->_id;
        di.i[1] = quoteLeg._sid;
    }

    hf::price_t calculatePrice(Leg& quoteLeg, hf::qty_t orderSize) {
        double price = this->_price - this->_adjustor;
        for (uint32_t i = 0; i < this->_nLegs; ++i) {
            Leg& leg = _legs[i];
            if (&leg != &quoteLeg) {
                md::top_t& book = leg.isBuy() ? leg._books->_askBook : leg._books->_bidBook;
                const hf::price_t leanPrice = leg.getLeanPrice(quoteLeg, orderSize, book);
                if (leanPrice == hf::NULL_PRICE) {
                    _LOG_INFO(boost::str(boost::format("No lean price. Sid=%1%") % leg._sid));

                    auto &di = this->_executor->createKDBLog();
                    di.ch[0] = 'C'; di.ch[1] = 'P'; di.ch[2] = 'F';
                    di.i[0] = this->_id;
                    di.i[1] = leg._sid;

                    return hf::NULL_PRICE;
                }
                price -= leg.getPricePart(quoteLeg, leanPrice);
            }
        }
        price /= (quoteLeg._definition._factor / quoteLeg._definition._divisor);
        const hf::price_t roundedPrice = RoundToTickSize(price, quoteLeg._definition._tickSize, quoteLeg._side);
        return roundedPrice;
    }

    void registerLeg(hf::symbol_id_t sid, Leg& leg) {
        _sidToLeg.insert(std::pair<hf::symbol_id_t, Leg*>(sid, &leg));
    }

    Leg& getLegByIndex(uint32_t index) {
        return _legs[index];
    }

    LegsCrossed getLegsCrossInfo() {
        LegsCrossed legs;
        legs._crossedForQuotingLegs = false;
        for (uint32_t i = 0; i < this->_nLegs; ++i) {
            Leg& leg = _legs[i];
            LegCross legCross = this->getLegCross(leg, calculatePrice(leg, this->_lotSize));
            if (leg._definition._toQuote) {
                legs._crossedForQuotingLegs |= legCross._quoteCross;
            }
            legs._legs.push_back(legCross);
        }
        return legs;
    }

    void subscribeAll() {
        for (uint32_t i = 0; i < this->_nLegs; ++i) {
            Leg &leg = _legs[i];
            this->_executor->subscribeAll(this, leg);
        }
    }

    void updateLeg(LegDefinition& legUpdate) {
        for (uint32_t i = 0; i < this->_nLegs; ++i) {
            Leg &leg = _legs[i];
            if (leg._definition._gid == legUpdate._gid) {
                _LOG_INFO(boost::str(boost::format("Modifying leg. SpreadId=%1% Sid=%2%") % this->_id % leg._sid));

                {
                    auto& di = this->_executor->createKDBLog();
                    di.ch[0] = 'M'; di.ch[1] = 'L';
                    di.i[0] = this->_id;
                    di.i[1] = leg._sid;
                }


                if (legUpdate._toQuote != -1) {
                    leg.updateToQuote(legUpdate._toQuote);
                    _LOG_INFO(boost::str(boost::format("Modified Quote=%1%") % legUpdate._toQuote));

                    auto &di = this->_executor->createKDBLog();
                    di.ch[0] = 'M'; di.ch[1] = 'L'; di.ch[2] = 'Q';
                    di.i[0] = this->_id;
                    di.i[1] = legUpdate._toQuote;
                }

                if (legUpdate._triggerQuantity != NULL_QTY) {
                    leg.updateTriggerQuantity(legUpdate._triggerQuantity);
                    _LOG_INFO(boost::str(boost::format("Modified TQ=%1%") % legUpdate._triggerQuantity));

                    auto &di = this->_executor->createKDBLog();
                    di.ch[0] = 'M'; di.ch[1] = 'L'; di.ch[2] = 'T';
                    di.i[0] = this->_id;
                    di.i[1] = legUpdate._triggerQuantity;
                }

                if (legUpdate._initiationQuantity != NULL_QTY) {
                    leg.updateInitiationQuantity(legUpdate._initiationQuantity);
                    _LOG_INFO(boost::str(boost::format("Modified IQ=%1%") % legUpdate._initiationQuantity));

                    auto &di = this->_executor->createKDBLog();
                    di.ch[0] = 'M'; di.ch[1] = 'L'; di.ch[2] = 'I';
                    di.i[0] = this->_id;
                    di.i[1] = legUpdate._initiationQuantity;
                }

                if (legUpdate._nPayUpTicks != hf::NULL_PRICE) {
                    leg.updatePayUpTicks(legUpdate._nPayUpTicks);
                    _LOG_INFO(boost::str(boost::format("Modified PayUpTicks=%1%") % legUpdate._nPayUpTicks));

                    auto &di = this->_executor->createKDBLog();
                    di.ch[0] = 'M'; di.ch[1] = 'L'; di.ch[2] = 'P';
                    di.i[0] = this->_id;
                    di.i[1] = legUpdate._nPayUpTicks;
                }

                if (legUpdate._nTicksToDoLastCross != hf::NULL_PRICE) {
                    leg.updateTicksToDoLastCross(legUpdate._nTicksToDoLastCross);
                    _LOG_INFO(boost::str(boost::format("Modified HammerSize=%1%") % legUpdate._nTicksToDoLastCross));

                    auto &di = this->_executor->createKDBLog();
                    di.ch[0] = 'M'; di.ch[1] = 'L'; di.ch[2] = 'H';
                    di.i[0] = this->_id;
                    di.i[1] = legUpdate._nTicksToDoLastCross;
                }
            }
        }
    }

    ErrorCode updateSpreadOrder(Spread& spread) {
        _LOG_INFO(boost::str(boost::format("Modifying spread. SpreadId=%1%") % this->_id));

        {
            auto& di = this->_executor->createKDBLog();
            di.ch[0] = 'M'; di.ch[1] = 'S';
            di.i[0] = this->_id;
        }

        bool isSpreadDone = false;

        if (spread._order._numberOfClips != NULL_QTY) {
            bool setAllowedNumberOfClips = spread._order._allowedNumberOfClips != NULL_QTY;
            isSpreadDone = this->updateNumberOfClips(spread._order._numberOfClips, setAllowedNumberOfClips);
            _LOG_INFO(boost::str(boost::format("Modified ClipSize=%1%") % spread._order._numberOfClips));

            auto &di = this->_executor->createKDBLog();
            di.ch[0] = 'M'; di.ch[1] = 'S'; di.ch[2] = 'C';
            di.i[0] = this->_id;
            di.i[1] = spread._order._numberOfClips;
        }

        if (!isSpreadDone) {
            if (spread._order._lotSize != NULL_QTY) {
                this->_lotSize = spread._order._lotSize;
                _LOG_INFO(boost::str(boost::format("Modified LotSize=%1%") % spread._order._lotSize));

                auto &di = this->_executor->createKDBLog();
                di.ch[0] = 'M'; di.ch[1] = 'S'; di.ch[2] = 'L';
                di.i[0] = this->_id;
                di.i[1] = spread._order._lotSize;
            }

            if (spread._order._price != hf::NULL_PRICE) {
                this->_price = spread._order._price;
                _LOG_INFO(boost::str(boost::format("Modified SpreadPrice=%1%") % this->_price));

                auto &di = this->_executor->createKDBLog();
                di.ch[0] = 'M'; di.ch[1] = 'S'; di.ch[2] = 'P';
                di.i[0] = this->_id;
                di.i[1] = this->_price;
            }

            if (spread._order._numberOfTicksFromTopToQuote != hf::NULL_PRICE) {
                this->_numberOfTicksFromTopToQuote = spread._order._numberOfTicksFromTopToQuote;
                _LOG_INFO(boost::str(boost::format("Modified MaxTicks=%1%") % this->_numberOfTicksFromTopToQuote));

                auto &di = this->_executor->createKDBLog();
                di.ch[0] = 'M'; di.ch[1] = 'S'; di.ch[2] = 'T';
                di.i[0] = this->_id;
                di.i[1] = this->_price;
            }

            for (uint32_t legNumber = 0; legNumber < spread._definition._nLegs; ++legNumber) {
                LegDefinition& definition = reinterpret_cast<LegDefinition&> (spread._definition._legs[legNumber * sizeof(LegDefinition)]);
                updateLeg(definition);
            }

            repriceAllHedgingOrders();
            startQuotingOrReprice();
        }

        return ErrorCode::Success;
    }

    Leg _legs[20];
    std::unordered_map<hf::symbol_id_t, Leg*> _sidToLeg;
};

}}

#endif //RHONEOMS_ENLEGSPREAD_H
