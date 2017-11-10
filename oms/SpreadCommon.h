//
// Created by pkusch on 1/29/17.
//

#ifndef RHONEOMS_SPREADCOMMON_H
#define RHONEOMS_SPREADCOMMON_H

#include <hf/log.h>
#include "SpreaderQueues.h"
#include <md/md.h>

namespace oms { namespace spreader {

LOG_DECLARE();

enum class SpreaderType {Standalone, Master, Slave};

struct Books {
    md::top_t _bidBook;
    md::top_t _askBook;
    md::level_index_t _subscribedLevels = 0;
};

static bool CanChangeOrder(const om::order_t* order) {
    return order != nullptr && order->is_active && !order->is_cancelling && order->is_acked;
}

inline hf::price_t RoundToTickSize(double price, hf::price_t tickSize, hf::SIDE side) {
    const hf::price_t rawPrice = static_cast<hf::price_t>(price);
    const hf::price_t remainder = rawPrice % tickSize;

    if ((!remainder))
        return rawPrice;

    const hf::price_t truncatedPrice = rawPrice - remainder;

    if (side == hf::SIDE::BUY) {
        return ((remainder > 0) ? truncatedPrice : truncatedPrice - tickSize);
    } else {
        return ((remainder > 0) ? truncatedPrice + tickSize : truncatedPrice);
    }
}

struct HedgeOrder {
    om::order_t* _order;
    hf::price_t _priceToDoLastCross;
};

struct Leg {
    LegDefinition _definition;
    Books* _books;
    hf::price_t _quotedPrice = hf::NULL_PRICE;
    hf::qty_t _quotedQuantity = 0;
    hf::qty_t _targetQuantity = 0;
    hf::qty_t _hedgingQuantity = 0;
    hf::qty_t _filledQuantity = 0;
    om::order_t* _quoteOrder = nullptr;
    std::vector<HedgeOrder> _hedgeOrders;
    om::routing_id_t _routingId;
    hf::symbol_id_t _sid;
    hf::SIDE _side;
    hf::VENUE _venue;
    bool _waitForModifyAck;
    bool _waitForNewOrderAck;
    bool _isRemote;

    Leg() {
        _hedgeOrders.reserve(20);
    }

    void setBooks(Books* books)  { _books = books; }

    bool isQuoted() {
        return _quotedQuantity > 0;
    }

    bool canQuote() {
        return _definition._toQuote && getQuoteQuantity() > 0;
    }

    bool isHedged() {
        return !_hedgeOrders.empty();
    }

    hf::qty_t getQuoteQuantity() {
        return _targetQuantity - _filledQuantity - _hedgingQuantity;
    }

    bool hasWorkingOrders() {
        return isHedged() || (_quoteOrder != nullptr && _quoteOrder->is_active);
    }

    bool isFilled() {
        return _filledQuantity >= _targetQuantity;
    }

    HedgeOrder* getHedgeOrder(const om::order_t* order) {
        for (auto& hedgeOrder : _hedgeOrders) {
            if (hedgeOrder._order == order)
                return &hedgeOrder;
        }
        return nullptr;
    }

    void addHedgeOrder(om::order_t* order, hf::price_t payUpPriceBase) {
        const hf::price_t priceChange = _definition._tickSize * _definition._nPayUpTicks;
        const hf::price_t crossPrice = payUpPriceBase + (order->side == hf::SIDE::BUY ? priceChange : -priceChange);
        _hedgeOrders.push_back({order, crossPrice});
    }

    bool removeOrder(const om::order_t *filledOrder) {
        if (filledOrder != _quoteOrder) {
            for (auto it = _hedgeOrders.begin(); it != _hedgeOrders.end(); ++it) {
                if (it->_order == filledOrder) {
                    _hedgeOrders.erase(it);
                    break;
                }
            }
        } else {
            _quoteOrder = nullptr;
            _quotedQuantity = 0;
            _quotedPrice = hf::NULL_PRICE;
            return true;
        }
        return false;
    }

    void loadLotSize(hf::qty_t lotSize) {
        const hf::qty_t additionalQuantity = lotSize * std::abs(_definition._quantityFactor);
        _targetQuantity += additionalQuantity;
        _LOG_INFO(boost::str(boost::format("Loading lot size for leg. Sid=%1% Target=%2%") % _sid % _targetQuantity));
    }

    bool isBuy() {
        return _side == hf::SIDE::BUY;
    }

    bool canChangeQuoteOrder() {
        return CanChangeOrder(_quoteOrder);
    }

    hf::price_t getLeanPrice(Leg& quoteLeg, hf::qty_t orderSize, const md::top_t& book) {
        const hf::qty_t initiationQty = getInitiationQuantity(quoteLeg, orderSize);
        const md::top_t& crossBook = isBuy() ? _books->_askBook : _books->_bidBook;
        switch (_definition._leanPricingType) {
            case LeanPricingType::WeightedAvg: {
                return getWeightedAverageLeanPrice(initiationQty, crossBook);
            }
            case LeanPricingType::FullLevel: {
                return getFullLevelLeanPrice(initiationQty, crossBook);
            }
            case LeanPricingType::TopOrSecond: {
                return getTopOrSecondLeanPrice(initiationQty, crossBook);
            }
            case LeanPricingType::TopLevel: {
                return getTopLevelLeanPrice(initiationQty, crossBook);
            }
            case LeanPricingType::SyntheticTop: {
                return getSyntheticLeanPrice(initiationQty, crossBook);
            }
            default:
                return hf::NULL_PRICE;
        }
    }

    hf::price_t getSyntheticLeanPrice(const hf::qty_t initiationQty, const md::top_t& crossBook) {
        const md::top_t::level_t& askTop = _books->_askBook.l[0];
        const md::top_t::level_t& bidTop = _books->_bidBook.l[0];
        const md::top_t::level_t& otherTop = isBuy() ?  bidTop : askTop;
        const md::top_t::level_t& crossTop = crossBook.l[0];

        if (crossTop.q >= initiationQty) {
            if ((bidTop.pr != hf::NULL_PRICE) & (askTop.pr != hf::NULL_PRICE)) {
                const hf::price_t spread = askTop.pr - bidTop.pr;
                if (spread > _definition._tickSize) {
                    return crossTop.pr + (isBuy() ?  -_definition._tickSize : _definition._tickSize);
                } else {
                    if (crossTop.q > _definition._syntheticMinSize) {
                        if (crossTop.q > _definition._syntheticAdjustment) {
                            const hf::qty_t adjustedQty = crossTop.q - _definition._syntheticAdjustment;
                            const double price = (adjustedQty * otherTop.pr + otherTop.q * crossTop.pr) / (adjustedQty + otherTop.q);
                            return static_cast<hf::price_t>(price);
                        } else {
                            return crossTop.pr;
                        }
                    } else {
                        return crossTop.pr;
                    }
                }
            }
            return crossTop.pr;
        }

        _LOG_INFO(boost::str(boost::format("Not enough size on top level for synthetic pricing. Has=%1% Need=%2%") % crossTop.q % initiationQty));

        return hf::NULL_PRICE;
    }

    hf::qty_t getInitiationQuantity(Leg& quoteLeg, hf::qty_t orderSize) {
        switch (_definition._initiationQuantityType) {
            case InitiationQuantityType::Absolute: {
                return _definition._initiationQuantity;
            }
            case InitiationQuantityType::Relative: {
                const double hedgeRatio = static_cast<double>(_targetQuantity) / quoteLeg._targetQuantity;
                return static_cast<hf::qty_t>(round(orderSize * hedgeRatio * _definition._initiationMultiplier));
            }
            default:
                return 1000000;
        }
    }

    hf::price_t getTopLevelLeanPrice(const hf::qty_t initiationQty, const md::top_t& book) {
        const md::top_t::level_t& topLevel = book.l[0];
        if (topLevel.q >= initiationQty) {
            return topLevel.pr;
        }

        _LOG_INFO(boost::str(boost::format("Not enough size on top level. Has=%1% Need=%2%") % topLevel.q % initiationQty));

        return hf::NULL_PRICE;
    }

    hf::price_t getTopOrSecondLeanPrice(const hf::qty_t initiationQty, const md::top_t& book) {
        const md::top_t::level_t& topLevel = book.l[0];

        if (topLevel.pr != hf::NULL_PRICE && topLevel.q >= initiationQty)
            return topLevel.pr;

        const md::top_t::level_t& secondLevel = book.l[1];

        if (secondLevel.pr == hf::NULL_PRICE)
            return hf::NULL_PRICE;

        return secondLevel.pr;
    }

    hf::price_t getFullLevelLeanPrice(const hf::qty_t initiationQty, const md::top_t& book) {
        md::level_index_t levelIndex = 0;

        while (true) {
            const md::top_t::level_t& level = book.l[levelIndex];

            if (level.pr == hf::NULL_PRICE)
                return hf::NULL_PRICE;

            if (level.q >= initiationQty) {
                return level.pr;
            }

            ++levelIndex;

            if (levelIndex == _books->_subscribedLevels - 1)
                return hf::NULL_PRICE;
        }
    }

    hf::price_t getWeightedAverageLeanPrice(const hf::qty_t initiationQty, const md::top_t& book) {
        hf::qty_t remainingQty = initiationQty;
        md::level_index_t levelIndex = 0;
        double priceTotal = 0;

        while (true) {
            const md::top_t::level_t& level = book.l[levelIndex];

            if (level.pr == hf::NULL_PRICE)
                return hf::NULL_PRICE;

            hf::qty_t usedQty = std::min(remainingQty, level.q);
            priceTotal += usedQty * level.pr;
            remainingQty -= usedQty;

            if (remainingQty == 0) {
                double price = priceTotal / initiationQty;
                return (hf::price_t) price;
            }

            ++levelIndex;

            if (levelIndex == _books->_subscribedLevels - 1)
                return hf::NULL_PRICE;
        }
    }

    hf::price_t getPricePart(Leg& quoteLeg, const hf::price_t price) {
        return static_cast<hf::price_t>(_definition._factor / _definition._divisor * price);
    }

    hf::price_t getPricePartUsingMidPrice() {
        if (_books->_askBook.l[0].pr != hf::NULL_PRICE && _books->_bidBook.l[0].pr != hf::NULL_PRICE) {
            const hf::price_t price = (_books->_askBook.l[0].pr + _books->_bidBook.l[0].pr) / 2;
            return static_cast<hf::price_t>(_definition._factor / _definition._divisor * price);
        } else {
            return hf::NULL_PRICE;
        }
    }

    void updatePayUpTicks(hf::price_t newTicks) {
        hf::price_t currentPayup = _definition._nPayUpTicks * _definition._tickSize;
        hf::price_t newPayup = newTicks * _definition._tickSize;
        for (auto& hedgeOrder : _hedgeOrders) {
            hedgeOrder._priceToDoLastCross = hedgeOrder._priceToDoLastCross + (isBuy() ? (currentPayup - newPayup) : (newPayup - currentPayup));
        }
        _definition._nPayUpTicks = newTicks;
    }

    void updateToQuote(int newToQuote) {
        _definition._toQuote = newToQuote;
    }

    void updateTriggerQuantity(hf::qty_t newTQ) {
        _definition._triggerQuantity = newTQ;
    }

    void updateInitiationQuantity(hf::qty_t newIQ) {
        _definition._initiationQuantity = newIQ;
    }

    void updateTicksToDoLastCross(hf::price_t newTicks) {
        _definition._nTicksToDoLastCross = newTicks;
    }
};

inline bool IsSellLegQuotePriceCloseToTop(hf::price_t price, Leg& leg, hf::price_t numberOfTicks) {
    const hf::price_t topAsk = leg._books->_askBook.l[0].pr;
    if (topAsk == hf::NULL_PRICE)
        return true;
    const hf::price_t numberOfTicksFromTop = (price - topAsk) / leg._definition._tickSize;
    return numberOfTicksFromTop <= numberOfTicks;
}

inline bool IsBuyLegQuotePriceCloseToTop(hf::price_t price, Leg& leg, hf::price_t numberOfTicks) {
    const hf::price_t topBid = leg._books->_bidBook.l[0].pr;
    if (topBid == hf::NULL_PRICE)
        return true;
    const hf::price_t numberOfTicksFromTop = (topBid - price) / leg._definition._tickSize;
    return numberOfTicksFromTop <= numberOfTicks;
}

inline bool shouldCrossOnBuySide(Leg& leg) {
    return leg._books->_askBook.l[0].q < leg._definition._triggerQuantity;
}

inline bool shouldCrossOnSellSide(Leg& leg) {
    return leg._books->_bidBook.l[0].q < leg._definition._triggerQuantity;
}

inline hf::price_t getSellHedgeQuotePrice(Leg& leg) {
    return leg._books->_bidBook.l[0].pr + leg._definition._tickSize;
}

inline hf::price_t getBuyHedgeQuotePrice(Leg& leg) {
    return leg._books->_askBook.l[0].pr - leg._definition._tickSize;
}

template <hf::SIDE side>
inline bool shouldCross(Leg& leg);

template<>
inline bool shouldCross<hf::SIDE::BUY>(Leg& leg) {
    return shouldCrossOnBuySide(leg);
}

template<>
inline bool shouldCross<hf::SIDE::SELL>(Leg& leg) {
    return shouldCrossOnSellSide(leg);
}

template <hf::SIDE side>
inline hf::price_t getHedgeCrossPrice(Leg& leg);

template <>
inline hf::price_t getHedgeCrossPrice<hf::SIDE::BUY>(Leg& leg) {
    return leg._books->_askBook.l[0].pr;
}

template <>
inline hf::price_t getHedgeCrossPrice<hf::SIDE::SELL>(Leg& leg) {
    return leg._books->_bidBook.l[0].pr;
}

template <hf::SIDE side>
inline hf::price_t getHedgeQuotePrice(Leg& leg);

template <>
inline hf::price_t getHedgeQuotePrice<hf::SIDE::BUY>(Leg& leg) {
    return getBuyHedgeQuotePrice(leg);
}

template <>
inline hf::price_t getHedgeQuotePrice<hf::SIDE::SELL>(Leg& leg) {
    return getSellHedgeQuotePrice(leg);
}

template <hf::SIDE side>
inline hf::price_t getHedgePrice(Leg& leg) {
    return shouldCross<side>(leg) ? getHedgeCrossPrice<side>(leg) : getHedgeQuotePrice<side>(leg);
}

template <hf::SIDE side>
inline hf::price_t getPayUpPriceBase(Leg& leg) {
    return getHedgeCrossPrice<side>(leg);
}

inline bool getWaitForModifyAck(hf::VENUE venue) {
    switch (venue) {
        case hf::VENUE::ICE:
            return true;
        case hf::VENUE::CME:
        case hf::VENUE::EUREX:
        case hf::VENUE::EURONEXT:
            return false;
        default:
            throw std::runtime_error("getWaitForModifyAck - Invalid venue");
    }
}

inline bool getWaitForNewOrderAck(hf::VENUE venue) {
    switch (venue) {
        case hf::VENUE::ICE:
        case hf::VENUE::EUREX:
        case hf::VENUE::EURONEXT:
            return false;
        case hf::VENUE::CME:
            return true;
        default:
            throw std::runtime_error("getWaitForNewOrderAck - Invalid venue");
    }
}

template <typename ContextT>
static ErrorCode PopulateLeg(Leg& leg, LegDefinition& def, hf::SIDE side, ContextT& context, hf::VENUE venue, SpreaderType spreaderType) {
    leg._definition = def;
    leg._sid = context.getSymbolID(leg._definition._gid);
    if(leg._sid == hf::NULL_SYMBOLID) {
        return ErrorCode::UnsupportedProduct;
    }
    const hf::symbol_info_t& symbolInfo = context.getSymbolInfo(leg._sid);
    leg._venue = symbolInfo.getVenue();
    leg._routingId = context.getRoutingId(leg._venue, leg._sid);
    leg._definition._tickSize = symbolInfo.tick_size;
    leg._side = side;
    leg._waitForModifyAck = getWaitForModifyAck(leg._venue);
    leg._waitForNewOrderAck = getWaitForNewOrderAck(leg._venue);
    leg._isRemote = spreaderType == SpreaderType::Standalone ? false : (leg._venue != venue);
    return ErrorCode::Success;
}

template <typename SpreadT, typename ContextT>
static ErrorCode PopulateSpread(Spread& order, ContextT& context, bool isFor2Legs, SpreadT* spread, hf::VENUE venue, SpreaderType spreaderType, std::string &errorMessage ) {
    if (isFor2Legs) {
        if (order._definition._nLegs != 2) {
            _LOG_INFO(boost::str(boost::format("Rejecting new spread. Running 2 Leg spreader but the spread does not have 2 legs. Id=%1%") % order._order._clientOrderId));
            return ErrorCode::WrongNumberOfLegs;
        }

        Leg &buyLeg = spread->getLegByIndex(0);
        Leg &sellLeg = spread->getLegByIndex(1);

        LegDefinition *leg1 = &reinterpret_cast<LegDefinition &> (order._definition._legs[0]);
        LegDefinition *leg2 = &reinterpret_cast<LegDefinition &> (order._definition._legs[sizeof(LegDefinition)]);

        LegDefinition *buyDef = nullptr;
        LegDefinition *sellDef = nullptr;

        if (order._order._side == hf::SIDE::BUY) {
            buyDef = leg1->_quantityFactor > 0 ? leg1 : leg2;
            sellDef = leg1->_quantityFactor < 0 ? leg1 : leg2;
        } else {
            sellDef = leg1->_quantityFactor > 0 ? leg1 : leg2;
            buyDef = leg1->_quantityFactor < 0 ? leg1 : leg2;
        }

        PopulateLeg(buyLeg, *buyDef, hf::SIDE::BUY, context, venue, spreaderType);
        PopulateLeg(sellLeg, *sellDef, hf::SIDE::SELL, context, venue, spreaderType);

    } else {
        if (order._definition._nLegs < 2) {
            _LOG_INFO(boost::str(boost::format("Rejecting new spread. Running N Leg spreader but the spread has only 1 leg. Id=%1%") % order._order._clientOrderId));
            return ErrorCode::WrongNumberOfLegs;
        }

        for (uint32_t legNumber = 0; legNumber < order._definition._nLegs; ++legNumber) {
            LegDefinition& definition = reinterpret_cast<LegDefinition &> (order._definition._legs[legNumber * sizeof(LegDefinition)]);
            bool isLegBuy = definition._quantityFactor > 0;
            hf::SIDE side = order._order._side == hf::SIDE::BUY ? (isLegBuy ? hf::SIDE::BUY : hf::SIDE::SELL) : (isLegBuy ? hf::SIDE::SELL : hf::SIDE::BUY);
            Leg& leg = spread->getLegByIndex(legNumber);
            auto errorCode = PopulateLeg(leg, definition, side, context, venue, spreaderType);
            if( errorCode != ErrorCode::Success ) {
                errorMessage = std::to_string(definition._gid);
                return errorCode;
            }
            spread->registerLeg(leg._sid, leg);
            spread->createMarketData(leg._sid, leg);
        }
        spread->_nLegs = order._definition._nLegs;
    }

    spread->_price = order._order._price;
    spread->_numberOfTicksFromTopToQuote = order._order._numberOfTicksFromTopToQuote;
    spread->_adjustor = order._definition._adjustor;
    spread->_id = order._order._clientOrderId;
    spread->_lotSize = order._order._lotSize;
    spread->_totalQuantity = order._order._lotSize * order._order._numberOfClips;
    spread->_allowedNumberOfClips = order._order._allowedNumberOfClips;

    return ErrorCode::Success;
}

}}


#endif //RHONEOMS_SPREADCOMMON_H
