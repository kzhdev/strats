#ifndef VIPER_COMMON_H
#define VIPER_COMMON_H

#include <md/md.h>
#include <functional>
#include <limits>

namespace Python {

static const uint32_t BID = 0;
static const uint32_t ASK = 1;

struct Book {
    hf::price_t _price;
    hf::qty_t _otherQuantity;
    md::order_count_t _otherOrderCount;
    hf::qty_t _ourQuantity;
    md::order_count_t _ourOrderCount;
};

typedef uint64_t OrderId_t;
static OrderId_t NoOrderId = std::numeric_limits<OrderId_t>::max() ;

struct Order {

    void onFullFill() {
        _onFill(_size);
    }

    void onPartialFill(hf::qty_t size) {
        _onFill(size);
    }

    double _price;
    hf::qty_t _size;
    uint32_t _side;
    OrderId_t _id;
    uint32_t _type;
    std::function<void(hf::qty_t)>& _onFill;
};

inline hf::price_t RoundToTickSize(double price, hf::price_t tickSize, uint32_t side) {
    const hf::price_t rawPrice = static_cast<hf::price_t>(price);
    const hf::price_t remainder = rawPrice % tickSize;

    if ((!remainder))
        return rawPrice;

    const hf::price_t truncatedPrice = rawPrice - remainder;

    if (side == BID) {
        return ((remainder > 0) ? truncatedPrice : truncatedPrice - tickSize);
    } else {
        return ((remainder > 0) ? truncatedPrice + tickSize : truncatedPrice);
    }
}

}

#endif //VIPER_COMMON_H
