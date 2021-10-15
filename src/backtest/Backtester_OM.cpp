#ifdef VIPER_BACKTEST

#include "Backtester.h"

using namespace Python;

om::order_t* BackTester::createOrder(om::instance_id_t iid,
                                     om::RISK_TYPE risk,
                                     hf::VENUE venue,
                                     uint32_t sid,
                                     om::ORDER_TYPE type,
                                     hf::SIDE side,
                                     hf::price_t price,
                                     hf::qty_t qty,
                                     om::STP_ACTION stp,
                                     const char (*tag)[om::TAG_SIZE]) {
    om::order_t* order;
    if (!_reusable_orders.empty()) {
        order = _reusable_orders.back();
        memset(order, 0, sizeof(om::order_t));
        _reusable_orders.pop_back();
    } else {
        order = &_order_pool.next();
        memset(order, 0, sizeof(om::order_t));
    }

    order->sid = sid;
    order->venue = venue;
    order->iid = iid;
    order->ot = type;
    order->side = side;
    order->pr = price;
    order->qty = qty;
    om::clear_flags(order->action_flags);
    order->action_flags.is_active = true;

    pushEvent<om::pr::SendOmsOrderNewEvent>(
            ViperMessageType::OM_SubmitOrder,
            [this, sid, side, price, qty, type, venue](ViperMessage& viperMsg, om::pr::SendOmsOrderNewEvent& event) {
                //TODO:
                viperMsg._timestamp = hf::util::TimeUtils::clock_gettime();
                viperMsg._venue = venue;
                event.venue = venue;
                event.sid = sid;
                event.side = side;
                event.strat_oid = ++_nextStratOid;
                event.price = price;
                event.qty = qty;
                event.ot = type;
            });
    _orders.emplace(order->oid, order);
    return order;
}

void BackTester::submitOrder(const om::pr::SendOmsOrderNewEvent& event) {
    auto contractId = sidToInternalId(event.sid);
    auto& limitBook = _limitBooks[contractId];
    limitBook.submitOrder(event);
}

void BackTester::modifyOrder(const om::pr::SendOmsOrderModifyEvent& event) {
    auto contractId = sidToInternalId(event.orderSnapshot.sid);
    auto& limitBook = _limitBooks[contractId];
    limitBook.modifyOrder(event);
}

void BackTester::cancelOrder(const om::pr::SendOmsOrderCancelEvent& event) {
    auto contractId = sidToInternalId(event.orderSnapshot.sid);
    auto& limitBook = _limitBooks[contractId];
    limitBook.cancelOrder(event);
}

#endif