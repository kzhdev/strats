#ifndef VIPER_ONE_LEVEL_ORDER_MANAGER_H
#define VIPER_ONE_LEVEL_ORDER_MANAGER_H

#include "boost/format.hpp"
#include <md/md.h>
#include <om/om.h>
#include <om/om_event.h>
#include <om/om_reject_codes.h>
#include "Common.h"
#include "Logger.h"

namespace Python {

template <typename ExecutorT>
class OneLevelOrderManager {

public:
    void initialize(uint32_t internalId, uint32_t side, const std::string& name, ExecutorT* executor) {
    }

    template <uint32_t Side>
    void quote(hf::price_t price, hf::qty_t qty) {
    }

    void onOrderAccepted(const om::OMOrderAcceptedEvent& event, const om::order_t& order) {
    }

    void onOrderExecution(const om::OMExecutionEvent &event, const om::order_t &order) {
    }

    void onOrderCanceled(const om::OMOrderCanceledEvent& event, const om::order_t& order) {
    }

    void onOrderRejected(const om::OMOrderRejectedEvent& event, const om::order_t& order) {
    }

    void onOrderCancelRejected(const om::OMOrderCancelRejectedEvent &event, const om::order_t &order) {
    }

    void onOrderReplaced(const om::OMOrderModifiedEvent& event, const om::order_t& order) {
    }

    void onOrderReplaceRejected(const om::OMOrderModifyRejectedEvent& event, const om::order_t& order) {
    }

    void getOutOfMarket() {
    }

    hf::qty_t getRestingSize() {
        return 0;
    }

    hf::price_t getPrice() {
        return 0;
    }

    void updateQuoteSize(hf::qty_t size) {
    }
};

}

#endif //VIPER_ONE_LEVEL_ORDER_MANAGER_H