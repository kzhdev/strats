#ifndef RHONEDS_QUEUE_H
#define RHONEDS_QUEUE_H

namespace Python {

    template <typename Type, uint32_t Size>
    class Queue {

    public:
        void push(Type value) {
            _values[_pushIndex] = value;
            ++_pushIndex;
        }

        bool pop(Type& value) {
            const bool hasValue = _popIndex != _pushIndex;
            if (hasValue) {
                value = _values[_popIndex];
                ++_popIndex;
                return true;
            }
            return false;
        }

    private:
        Type _values[Size];
        uint32_t _popIndex = 0;
        uint32_t _pushIndex = 0;
    };

}


#endif //RHONEDS_QUEUE_H
