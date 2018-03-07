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
        _logger.logInfo(boost::str(boost::format("Hedger %1%. Updating level %2% to size %3%") % _name % ToDollars(_price) % qty));

        _logger.logInfo(boost::str(boost::format("Hedger %1%. Before. Target=%2% Acked=%3% Flight=%4% Trade=%5%") % _name % _target % _acknowledged % _flight % _traded));

        _acknowledged -= _traded;
        _traded = 0;
        _target = qty;

        const hf::qty_t change = getQuantityChange();
        synchronizeOrders(change);

        _logger.logInfo(boost::str(boost::format("Hedger %1%. After. Target=%2% Acked=%3% Flight=%4% Trade=%5%") % _name % _target % _acknowledged % _flight % _traded));
    }

    void synchronizeOrders(hf::qty_t change) {
        if (change > 0) {
            increaseSize(change);
        } else if (change < 0) {
            BOOST_ASSERT(false);
        }
    }

    hf::qty_t getTotalOrdersQuanity() {
        return _acknowledged + _flight;
    };

    hf::qty_t getQuantityChange() {
        return _target - getTotalOrdersQuanity() - _traded;
    }

    void increaseSize(hf::qty_t change) {
        createNewOrder(change);
    }

    void createNewOrder(hf::qty_t orderSize) {
        if (orderSize > 0) {
            om::order_t* order = _executor->newOrder(this, _price, orderSize);
            if (order == nullptr) {
                _logger.logError(boost::str(boost::format("Hedger could not create order.")));
                _executor->onCriticalError("Hedger could not create order.");
            } else {
                _orders.push_back(order);
                _flight += orderSize;
            }
        }
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

    hf::price_t _price = hf::NULL_PRICE;

    hf::qty_t _target = 0;
    hf::qty_t _acknowledged = 0;
    hf::qty_t _flight = 0;
    hf::qty_t _traded = 0;

    std::vector<om::order_t*> _orders;

    ExecutorT* _executor;
    std::string _name;
    Logger _logger;
};

}

#endif //VIPER_ONE_LEVEL_ORDER_MANAGER_H