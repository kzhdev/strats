#ifndef VIPER_STACKER_H
#define VIPER_STACKER_H

#include <md/md.h>
#include <om/om.h>
#include "Common.h"

namespace Python {

class Stacker {

    struct Level {

        void updateOnlyOneLevel(hf::qty_t size) {
            if (size < _target) {
                const hf::qty_t diff = _target - size;
                for(auto it = _orders.rbegin(); it != _orders.rend(); ++it) {
                    om::order_t* order = *it;
                    if (!order->is_modifying) {
                        if (order->qty == diff) {
                            _downInflight += diff;
                            _stacker->cancel(order);
                            break;
                        } else if (order->qty < diff){
                            _downInflight += order->qty;
                            _stacker->cancel(order);
                        } else {
                            _downInflight += diff;
                            _stacker->modify(order, _price, diff);
                            break;
                        }
                    }
                }
            } else if (size > _target) {

            }

            _target = size;
        }

        hf::price_t _price = hf::NULL_PRICE;

        hf::qty_t _target = 0;
        hf::qty_t _acked = 0;
        hf::qty_t _downInflight = 0;

        std::vector<om::order_t*> _orders;

        Stacker* _stacker;
    };

public:
    void stack(hf::price_t price, hf::qty_t size) {
        if (_price == price) {
            _topLevel->updateOnlyOneLevel(size);
        } else if (_price > price) {

        } else if (_price < price) {

        }
    }

    void cancel(om::order_t* order) {
    }

    void modify(om::order_t* order, hf::price_t price, hf::qty_t size) {
    }

private:
    hf::price_t _price;
    hf::qty_t _size;

    hf::qty_t _quoteSize;
    uint32_t _nLevels;

    Level* _topLevel;
};

}

#endif //VIPER_STACKER_H
