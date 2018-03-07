#ifndef VIPER_HEDGES_MANAGER_H
#define VIPER_HEDGES_MANAGER_H

#include "boost/format.hpp"
#include <md/md.h>
#include <om/om.h>
#include <om/om_event.h>
#include <om/om_reject_codes.h>
#include "Common.h"
#include "Logger.h"

namespace Python {

template <typename ExecutorT>
class HedgesManager {

public:
    void initialize(uint32_t internalId, uint32_t side, const std::string& name, ExecutorT* executor) {
    }

    bool canChangeOrder(const om::order_t& order) {
        return order.is_active & !order.is_cancelling;
    }

    template <uint32_t Side>
    void hedge(hf::price_t price, hf::qty_t size) {
        _logger.logInfo(boost::str(boost::format("Hedger %1%. Updating level %2% to size %3%") % _name % ToDollars(_price) % size));
        _logger.logInfo(boost::str(boost::format("Hedger %1%. Before. Target=%2% Traded=%3%") % _name % _target % _traded));

        const hf::qty_t openSize = getOpenSize();
        BOOST_ASSERT(size >= openSize);
        const hf::qty_t change = size - openSize;

        _target = size;
        _traded = 0;

        if (_price == price) {
            createNewOrder(change);
        } else {
            _price = price;
            changeToDifferentPrice();
        }

        _logger.logInfo(boost::str(boost::format("Hedger %1%. After. Target=%2% Trade=%3%") % _name % _target % _traded));
    }

    hf::qty_t getOpenSize() {
        return  _target - _traded;
    }

    void changeToDifferentPrice() {
        hf::qty_t openSize = getOpenSize();
        std::size_t numberOfOrders = _orders.size();
        for (std::size_t i = 0; i < numberOfOrders; ++i) {
            om::order_t* order = _orders[i];
            if (canChangeOrder(*order)) {
                _executor->cancel(order);
            }
        }
        createNewOrder(openSize);
    }

    void createNewOrder(hf::qty_t orderSize) {
        if (orderSize > 0) {
            om::order_t* order = _executor->newOrder(this, _price, orderSize);
            if (order == nullptr) {
                _logger.logError(boost::str(boost::format("Hedger could not create order.")));
                _executor->onCriticalError("Hedger could not create order.");
            } else {
                _orders.push_back(order);
            }
        }
    }

    void removeOrder(const om::order_t& order) {
        for (auto it = _orders.begin(); it != _orders.end(); ++it) {
            if ((*it)->oid == order.oid) {
                _orders.erase(it);
                break;
            }
        }
    }

    void onOrderAccepted(const om::OMOrderAcceptedEvent& event, const om::order_t& order) {
        _logger.logInfo(boost::str(boost::format("Hedger %1%. Order Accepted. Id=%2% Price=%3% Size=%4%") % _name % order.oid % ToDollars(_price) % event.acceptedQty));
    }

    void onOrderExecution(const om::OMExecutionEvent &event, const om::order_t &order) {
        _logger.logInfo(boost::str(boost::format("Hedger %1%. Fill. LevelPrice=%2%. Price=%3% Size=%4%") % _name % ToDollars(_price) % event.filledPrice % event.filledQty));
        _logger.logInfo(boost::str(boost::format("Hedger %1%. Before. Target=%2% Traded=%3%") % _name % _target % _traded));
        _traded += event.filledQty;
        if (!order.is_active) {
            removeOrder(order);
        }
        _logger.logInfo(boost::str(boost::format("Hedger %1%. After. Target=%2% Trade=%3%") % _name % _target % _traded));
    }

    void onOrderCanceled(const om::OMOrderCanceledEvent& event, const om::order_t& order) {
        _logger.logInfo(boost::str(boost::format("Hedger %1%. Order Cancelled. Id=%2%") % _name % order.oid));
        _logger.logInfo(boost::str(boost::format("Hedger %1%. Before. Target=%2% Traded=%3%") % _name % _target % _traded));
        removeOrder(order);
        _logger.logInfo(boost::str(boost::format("Hedger %1%. After. Target=%2% Trade=%3%") % _name % _target % _traded));
    }

    void onOrderRejected(const om::OMOrderRejectedEvent& event, const om::order_t& order) {
        std::string message = boost::str(boost::format("Hedger %1% . New order rejected. Id=%2% Reason=%3% ReasonText=%4%") % _name % order.oid % event.reasonCodeExt % event.reasonText);
        _logger.logInfo(message);
        _executor->onCriticalError(message);
    }

    bool wasRejectedDueToBeingFilled(uint32_t rejectCode, hf::VENUE venue) {
        return rejectCode == ice_rejects::TOO_LATE_TO_CANCEL || rejectCode == ice_rejects::UNKNOWN_ORDER || rejectCode == om::toUType(om::OrderRejectReason::OrderNotActive);
    }

    void onOrderCancelRejected(const om::OMOrderCancelRejectedEvent &event, const om::order_t &order) {
        std::string message = boost::str(boost::format("Hedger %1% . Cancel order rejected. Id=%2% Reason=%3% ReasonText=%4%") % _name % order.oid % event.reasonCodeExt % event.reasonText);
        _logger.logInfo(boost::str(boost::format("Hedger %1%. Before. Target=%2% Traded=%3%") % _name % _target % _traded));
        bool rejectedDueToFill = wasRejectedDueToBeingFilled(event.reasonCodeExt, order.venue);
        if (!rejectedDueToFill) {
            _logger.logInfo(message);
            _executor->onCriticalError(message);
        }
        _logger.logInfo(boost::str(boost::format("Hedger %1%. After. Target=%2% Trade=%3%") % _name % _target % _traded));
    }

    hf::price_t _price = hf::NULL_PRICE;
    hf::qty_t _target = 0;
    hf::qty_t _traded = 0;

    std::vector<om::order_t*> _orders;

    ExecutorT* _executor;
    std::string _name;
    Logger _logger;
};

}

#endif //VIPER_HEDGES_MANAGER_H