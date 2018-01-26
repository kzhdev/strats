#ifndef TEST_EXECUTOR_H
#define TEST_EXECUTOR_H

namespace Python {

class TestExecutor {

public:
    void onInternalFill(uint32_t internalId, Order* order, hf::price_t price, hf::qty_t size) {
        FillMessage fill;
        fill._contractId = internalId;
        fill._price = price;
        fill._qty = size;
        fill._side = order->_side;
        _fills.push_back(fill);
    }

    void removeFills() {
        _fills.clear();
    }

    void onEndOfExternalFills(uint32_t internalId) {
    }

    void onExternalFill(uint32_t internalId, Order* order, hf::price_t price, hf::qty_t size) {
        FillMessage fill;
        fill._contractId = internalId;
        fill._price = price;
        fill._qty = size;
        fill._side = order->_side;
        _externalFills.push_back(fill);
    }

    void removeExternalFills() {
        _externalFills.clear();
    }

    std::vector<FillMessage> _fills;
    std::vector<FillMessage> _externalFills;
};

}

#endif //TEST_EXECUTOR_H
