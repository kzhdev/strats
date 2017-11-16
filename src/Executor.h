#ifndef VIPER_EXECUTOR_H
#define VIPER_EXECUTOR_H

namespace Python{

class Executor {

    void onOrderExecution(const om::OMExecutionEvent& event, const om::order_t& order) {
    }

    void onOrderAccepted(const om::OMOrderAcceptedEvent& event, const om::order_t& order) {
    }

    void onOrderRejected(const om::OMOrderRejectedEvent& event, const om::order_t& order) {
    }

    void onOrderReplaced(const om::OMOrderModifiedEvent& event, const om::order_t& order) {
    }

    void onOrderReplaceRejected(const om::OMOrderModifyRejectedEvent& event, const om::order_t& order) {
    }

    void onOrderCanceled(const om::OMOrderCanceledEvent& event, const om::order_t& order) {
    }

    void onOrderCancelRejected(const om::OMOrderCancelRejectedEvent& event, const om::order_t& order) {
    }
};

}

#endif // VIPER_EXECUTOR_H