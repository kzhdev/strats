#ifndef VIPER_TESTSTRATEGY_H
#define VIPER_TESTSTRATEGY_H

#include "bb/bb_event.h"
#include <hf/hf.h>
#include <md/md_event.h>
#include <om/om_event.h>
#include <hf/ConfigManager.h>

namespace Python {

class TestStrategy {
public:

    virtual void onMDUpdate(const md::MDUpdateEvent &event) {
    }

    virtual void onMDTrade(const md::MDTradeEvent &event) {
    }

    virtual void onMDStatus(const md::MDStatusEvent &event) {
    }

    virtual void onMDGap(const md::MDGapEvent &event) {
    }

    virtual void onMDHeartbeat(const md::MDHeartbeatEvent &event) {
    }

    virtual void onOMGWStatusUpdate(const om::GWStatusUpdate &event) {
    }

    virtual void onOMPortStatusUpdate(const om::PortStatusUpdate &event) {
    }

    virtual void onOrderAccepted(const om::OMOrderAcceptedEvent &event, const om::order_t &order) {
    }

    virtual void onOrderRejected(const om::OMOrderRejectedEvent &event, const om::order_t &order) {
    }

    virtual void onOrderReplaced(const om::OMOrderModifiedEvent &event, const om::order_t &order) {
    }

    virtual void onOrderReplaceRejected(const om::OMOrderModifyRejectedEvent &event, const om::order_t &order) {
    }

    virtual void onOrderCanceled(const om::OMOrderCanceledEvent &event, const om::order_t &order) {
    }

    virtual void onMassCancelAccepted(const om::OMMassCancelAcceptedEvent &event) {
    }

    virtual void onMassCancelRejected(const om::OMMassCancelRejectedEvent &event) {
    }

    virtual void onOrderCancelRejected(const om::OMOrderCancelRejectedEvent &event, const om::order_t &order) {
    }

    virtual void onOrderExecution(const om::OMExecutionEvent &event, const om::order_t &order) {
    }

    virtual void onOrderSpreadLegExecution(const om::OMSpreadLegExecutionEvent &event, const om::order_t &order) {
    }

    virtual void onTimer(const hf::timer_id_t, uint64_t u64) {
    }

    virtual void onAdmin(const char *const command, const char *const data, std::string &result, const int h) {
    }

    virtual bool onAdminAsync(const char *const command, const char *const data, std::string &result, const int h) {
        return false;
    }

    virtual void onStratBegin() {
    }

    virtual void onStratEnd() {
    }

    virtual void onStratLoop() {
    }

    virtual void onStratIdle() {
    }

    virtual void onBloombergEvent(const bb::BBReleaseEvent &event) {
    }

    virtual void onPoolThresholdExceeded(const std::string &queue_name, uint64_t usage_bytes) {
    }

    virtual void onQueueThresholdExceeded(const std::string &queue_name, uint64_t usage_count) {
    }

    virtual void onMDBatch(const md::MDBatch &batch) {
    }

    virtual void onOMBatch(const om::OMBatch &batch) {
    }

    virtual void onCreate(const libconfig::Setting &config) {
    }

    virtual void onPostCreate(const libconfig::Setting &config) {
    }

    virtual void onBeforeStart(const libconfig::Setting &config) {
    }

    class Context {
    public:

        std::vector<om::Gateway> getGateways() {
            return std::vector<om::Gateway>();
        }

        void ctlSendCommandOM(hf::VENUE venue, om::port_id_t pid, uint32_t code) {
        }

        void init(std::vector<hf::symbol_info_t> &symbols) {
            for(auto &symbol : symbols) {
                symbol.sid = getSymbolID(symbol.gid);
                _sidToSymbol[symbol.sid] = symbol;
            }
        }

        md::symbol_info_t& getSymbolTop(hf::VENUE venue, hf::symbol_id_t sid) {
            static md::symbol_info_t info;
            info.bid.top.pr = hf::NULL_PRICE;
            info.bid.top.q = 0;
            info.ask.top.pr = hf::NULL_PRICE;
            info.ask.top.q = 0;
            return info;
        }

        const hf::symbol_info_t &getSymbolInfo(hf::symbol_id_t sid) const {
            auto it = _sidToSymbol.find(sid);
            if(it != _sidToSymbol.end()) {
                return it->second;
            }

            static hf::symbol_info_t si;
            si.tick_size = .25 * 10000000;
            si.cme.enabled = true;
            return si;
        }

        hf::symbol_id_t getSymbolID(hf::symbol_gid_t gid) {
            auto it = _gidToSid.find(gid);
            if (it != _gidToSid.end())
                return it->second;
            _gidToSid.insert(std::pair<hf::symbol_gid_t, hf::symbol_id_t>(gid, ++_sid));
            return _sid;
        }

        om::routing_id_t getRoutingId(hf::VENUE venue, hf::symbol_id_t sid) {
            return 0;
        }

        void subscribeMD(hf::VENUE venue, hf::symbol_id_t sid, uint8_t levelNum, bool sendQtyFlag, bool sendTradesFlag) {
        }

        void subscribeOM(hf::symbol_id_t sid, bool oae = true, bool ore = true, bool oce = true, bool cae = false, bool cre = false, bool ome = true, bool mae = false, bool mre = false) {
        }

        om::order_t *createOrder(om::instance_id_t iid, om::RISK_TYPE risk, om::routing_id_t routingId, hf::symbol_id_t sid, om::ORDER_TYPE type, hf::SIDE side, hf::price_t price, hf::qty_t qty, om::STP_ACTION stp = om::STP_ACTION::CANCEL_AGGRESSOR) {
            om::order_t *order = new om::order_t();
            order->sid = sid;
            order->is_acked = false;
            order->is_active = true;
            order->side = side;
            order->pr = price;
            order->qty = qty;
            order->oid = ++_orderId;
            order->ot = type;

            auto symbolInfo = getSymbolInfo(sid);
            order->venue = symbolInfo.getVenue();

            _orders.insert(std::pair<om::order_id_t, om::order_t *>(order->oid, order));
            return order;
        }

        bool modifyOrder(om::order_id_t oid, hf::price_t price, hf::qty_t qty, om::STP_ACTION stp = om::STP_ACTION::DEFAULT) {
            om::order_t *order = _orders.find(oid)->second;
            order->pr = price;
            order->qty = qty;
            order->is_modifying = true;
            return true;
        }

        bool cancelOrder(om::order_id_t oid) {
            om::order_t *order = _orders.find(oid)->second;
            order->is_cancelling = true;
            return true;
        }

        const om::order_t &getOrder(om::order_id_t oid) const {
            return *_orders.find(oid)->second;
        }

        om::debug_t& createDebug(om::routing_id_t routingId, const hf::symbol_id_t sid) {
            static om::debug_t sharedDebug;
            return sharedDebug;
        }

        hf::symbol_id_t _sid = 0;
        int _orderId = 0;
        std::unordered_map<om::order_id_t, om::order_t *> _orders;
        std::unordered_map<hf::symbol_gid_t, hf::symbol_id_t> _gidToSid;
        std::unordered_map<hf::symbol_id_t, hf::symbol_info_t> _sidToSymbol;
    };

    typedef Context ContextT;

    Context &context() {
        return _ctx;
    }

    Context _ctx;
};

}

#endif //VIPER_TESTSTRATEGY_H
