#ifndef VIPER_STACKER_H
#define VIPER_STACKER_H

#include <md/md.h>
#include <om/om.h>
#include <om/om_event.h>
#include <om/om_reject_codes.h>
#include "Common.h"
#include "ViperConfig.h"
#include "logging/Coolog.h"
#include "Queue.h"

//For when we need to generate a debug build with *just* asserts enabled in this class.
//#ifdef BOOST_DISABLE_ASSERTS
//#define REENABLE_BOOST_ASSERTS=1
//#undef BOOST_DISABLE_ASSERTS
//#endif
//#include <boost/assert.hpp>

namespace Python {

static const uint32_t NUMBER_OF_PRE_CREATED_LEVELS = 6000;

inline void SetModifyTracker(const om::order_t& order, uint32_t modifyTrackerIndex) {
    om::order_t& mutableOrder = const_cast<om::order_t&>(order);
    mutableOrder.debug.i[MODIFY_TRACKER] = modifyTrackerIndex;
}

template <typename ExecutorT>
class Stacker {

public:

    struct Level;
    struct LevelStealer;

    struct ModifyTracker {

        struct Entry {
            hf::qty_t _qty;
            Level* _level;
        };

        void setNewOrder(hf::qty_t size, Level* level) {
            setQuantityToModify(size, level);
        }

        void setQuantityToModify(hf::qty_t size, Level* level) {
            _entries[_insertIndex] = { size, level };
            _insertIndex = getNextIndex(_insertIndex);
        }

        //"Target" Methods
        ///////////////////

        //This is the level we are at or will eventually get to via all pending modifies
        Level* getTargetLevel() const {
            return getTarget()->_level;
        }

        //This is the level we are at or will eventually get to via all pending modifies
        hf::qty_t getTargetQty() const {
            return getTarget()->_qty;
        }

        //This is the entry we are at or will eventually get to via all pending modifies
        const Entry* getTarget() const {
            uint32_t targetIndex = getPriorIndex(_insertIndex);
            return &_entries[targetIndex];
        }

        //"In-Flight" Methods
        /////////////////////

        //Gets the current in-flight level, ASSUMING THERE IS A LEVEL IN FLIGHT
        Level* getInFlightLevel() const {
            return getInFlightEntry()->_level;
        }

        //Gets the current in-flight quantity, ASSUMING THERE IS A LEVEL IN FLIGHT
        hf::qty_t getInFlightQty() const {
            return getInFlightEntry()->_qty;
        }

        //Gets the current in-flight entry, ASSUMING THERE IS A LEVEL IN FLIGHT
        const Entry* getInFlightEntry() const {
            return &_entries[_popIndex];
        }

        //"Resting" Methods
        ///////////////////

        //Gets the current resting level, ASSUMING THE ORDER HAS BEEN ACKED
        Level* getRestingLevel() const {
            return getRestingEntry()->_level;
        }

        //Gets the current resting qty, ASSUMING THE ORDER HAS BEEN ACKED
        hf::qty_t getRestingQty() const {
            return getRestingEntry()->_qty;
        }

        //Gets the current resting level, ASSUMING THE ORDER HAS BEEN ACKED
        const Entry* getRestingEntry() const {
            uint32_t restingIndex = getPriorIndex(_popIndex);
            return &_entries[restingIndex];
        }

        void pop() {
            _popIndex = getNextIndex(_popIndex);
        }

        uint32_t getPriorIndex(uint32_t current) const {
            return current == 0 ? SIZE - 1 : current - 1;
        }

        uint32_t getNextIndex(uint32_t current) const {
            return current == SIZE - 1 ? 0 : current + 1;
        }

        bool isModifying() const {
            return _insertIndex != _popIndex;
        }

        template<typename ActionType>
        void performOnEachNode(const ActionType& action) {
            auto priorIndex = getPriorIndex(_popIndex);
            auto currentIndex = _popIndex;
            auto nextIndex = getNextIndex(_popIndex);
            Entry* priorEntry = &_entries[priorIndex];
            Entry* currentEntry = &_entries[currentIndex];
            action(currentEntry, priorEntry, true, nextIndex == _insertIndex);
            while(nextIndex != _insertIndex) {
                priorIndex = currentIndex;
                currentIndex = nextIndex;
                nextIndex = getNextIndex(currentIndex);
                priorEntry = &_entries[priorIndex];
                currentEntry = &_entries[currentIndex];
                action(currentEntry, priorEntry, false, nextIndex == _insertIndex);
            }
        }

        template<typename ActionType>
        void performOnEachNodeOnlyBasedOnLevel(const ActionType& action) {
            auto currentIndex = _popIndex;
            auto endIndex = _insertIndex;
            while(currentIndex != endIndex) {
                action(reinterpret_cast<Level*>(_entries[currentIndex]._level));
                currentIndex = getNextIndex(currentIndex);
            }
        }

        static const uint32_t SIZE = 50;
        Entry _entries[SIZE];
        uint32_t _index;
        uint32_t _insertIndex = 0;
        uint32_t _popIndex = 0;
    };

    class ModifyTrackerTracker {

    public:

        ModifyTrackerTracker() {
            //if (_venue == hf::VENUE::CME) {
                for (uint32_t i = 0; i < NUMBER_OF_MODIFY_TRACKERS; ++i) {
                    _availableModifyTrackers.push(i);
                    _modifyTrackers[i]._index = i;
                }
            //}
        }

        ModifyTracker* getAvailableModifyTracker() {
            uint32_t index;
            const bool hasValue = _availableModifyTrackers.pop(index);
            if (hasValue) {
                return &_modifyTrackers[index];
            }
            throw std::runtime_error("does not have available modify tracker");
        }

        ModifyTracker* getModifyTracker(const om::order_t& order) {
            uint32_t idx = static_cast<uint32_t>(order.debug.i[MODIFY_TRACKER]);
            return &_modifyTrackers[idx];
        }

    private:
        static constexpr uint32_t NUMBER_OF_MODIFY_TRACKERS = om::ORDER_NUM;
#ifdef VIPER_UNIT_TEST
        //The static data size becomes so large with this on the stack that the linker can't take it
        ModifyTracker* _modifyTrackers = new ModifyTracker[NUMBER_OF_MODIFY_TRACKERS];
#else
        ModifyTracker _modifyTrackers[NUMBER_OF_MODIFY_TRACKERS];
#endif
        Queue<uint32_t, NUMBER_OF_MODIFY_TRACKERS> _availableModifyTrackers;
    };

    struct Level {

        Level() : Level(nullptr) {}

        Level(Stacker* stacker) : _stacker(stacker) {
            _orders.reserve(8);
        }

        ModifyTracker* getAvailableModifyTracker() {
            return _stacker->getAvailableModifyTracker();
        }

        ModifyTracker* getModifyTracker(const om::order_t& order) {
            return _stacker->getModifyTracker(order);
        }

        hf::qty_t getOriginalOrderQtyForNOAck(const om::order_t& order) {
            switch(_stacker->_venue) {
                case hf::VENUE::ICE:
                    if(!isModifying(order)) {
                        return getTargetQuantity(order);
                    }
                    return getRestingQty(order);
                case hf::VENUE::CME:
                    return getInFlightQty(order);
                default:
                    throw std::runtime_error("unrecognized venue");
            }
        }

        hf::qty_t getTargetQuantity(const om::order_t& order) {
            switch (_stacker->_venue) {
                case hf::VENUE::ICE:
                    return GetQuantity(order, QUANTITY_TO_MODIFY);
                case hf::VENUE::CME:
                    return getModifyTracker(order)->getTargetQty();
                default:
                    throw std::runtime_error("unrecognized venue");
            }
        }

        Level* getTargetLevel(const om::order_t& order) {
            switch(_stacker->_venue) {
                case hf::VENUE::ICE:
                    return reinterpret_cast<Level *>(GetQuantity(order, LEVEL_POINTER));
                case hf::VENUE::CME:
                    return getModifyTracker(order)->getTargetLevel();
                default:
                    throw std::runtime_error("unrecognized venue");
            }
        }

        hf::qty_t getInFlightQty(const om::order_t& order) {
            switch (_stacker->_venue) {
                case hf::VENUE::ICE:
                    return GetQuantity(order, QUANTITY_TO_MODIFY);
                case hf::VENUE::CME:
                    return getModifyTracker(order)->getInFlightQty();
                default:
                    throw std::runtime_error("unrecognized venue");
            }
        }

        hf::qty_t getRestingQty(const om::order_t& order) {
            switch (_stacker->_venue) {
                case hf::VENUE::ICE:
                    return GetQuantity(order, PRIOR_QUANTITY);
                case hf::VENUE::CME:
                    return getModifyTracker(order)->getRestingQty();
                default:
                    throw std::runtime_error("unrecognized venue");
            }
        }

        void setNewOrderSize(om::order_t* order, hf::qty_t size) {
            switch (_stacker->_venue) {
                case hf::VENUE::ICE: {
                    SetQuantity(*order, size, QUANTITY_TO_MODIFY);
                    break;
                }
                case hf::VENUE::CME: {
                    ModifyTracker* tracker = getAvailableModifyTracker();
                    tracker->setNewOrder(size, this);
                    SetModifyTracker(*order, tracker->_index);
                    break;
                }
                default:
                    throw std::runtime_error("unrecognized venue");
            }
        }

        void setQuantityToModify(om::order_t* order, hf::qty_t size) {
            switch (_stacker->_venue) {
                case hf::VENUE::ICE:
                    SetQuantity(*order, GetQuantity(*order, QUANTITY_TO_MODIFY), PRIOR_QUANTITY);
                    SetQuantity(*order, size, QUANTITY_TO_MODIFY);
                    SetQuantity(*order, reinterpret_cast<int64_t>(this), PRIOR_LEVEL);
                    break;
                case hf::VENUE::CME:
                    getModifyTracker(*order)->setQuantityToModify(size, this);
                    break;
                default:
                    throw std::runtime_error("unrecognized venue");
            }
        }

        void setQuantityToModifyAtLevel(Level* priorLevel, Level* currentLevel, om::order_t* order, hf::qty_t size) {
            setQuantityToModify(order, size);
            order->debug.i[LEVEL_POINTER] = reinterpret_cast<uint64_t>(currentLevel);
            if(_stacker->_venue == hf::VENUE::ICE) {
                SetQuantity(*order, reinterpret_cast<int64_t>(priorLevel), PRIOR_LEVEL);
            }
        }

        void setAcceptedModifyQuantity(const om::order_t& order, hf::qty_t size) {
            switch (_stacker->_venue) {
                case hf::VENUE::ICE:
                    SetQuantity(order, size, PRIOR_QUANTITY);
                    SetQuantity(order, reinterpret_cast<uint64_t>(this), PRIOR_LEVEL);
                    break;
                case hf::VENUE::CME:
                    getModifyTracker(order)->pop();
                    break;
                default:
                    throw std::runtime_error("unrecognized venue");
            }
        }

        bool isStillModifyingAfterModifyAck(const om::order_t& order) {
            switch(_stacker->_venue) {
                case hf::VENUE::ICE:
                    return false;
                case hf::VENUE::CME:
                    return getModifyTracker(order)->isModifying();
                default:
                    throw std::runtime_error("unrecognized venue");
            }
        }

        bool canModifyOrCancelOrder(const om::order_t& order) {
            switch(_stacker->_venue) {
                case hf::VENUE::ICE:
                    //Should test in cert if can cancel with modify in flight
                    return order.is_active & !order.is_cancelling & !order.is_modifying;
                case hf::VENUE::CME:
                    return (GetQuantity(order, IS_ACKED) == 1) & order.is_active & !order.is_cancelling;
                default:
                    throw std::runtime_error("unknown venue");
            }
        }

        hf::qty_t getLeavesQty(const om::order_t& order) {
            switch (_stacker->_venue) {
                case hf::VENUE::ICE:
                    return order.leavesQty;
                case hf::VENUE::CME:
                    return getModifyTracker(order)->getTargetQty() - order.executedQty;
                default:
                    throw std::runtime_error("unknown venue");
            }
        }

        hf::qty_t getLeavesQuantityOnPriorLevelOnModify(const om::order_t& order) {
            switch (_stacker->_venue) {
                case hf::VENUE::ICE:
                    return getRestingQty(order) - order.executedQty;
                case hf::VENUE::CME:
                    return getModifyTracker(order)->getRestingQty() - order.executedQty;
                default:
                    throw std::runtime_error("unknown venue");
            }
        }

        hf::qty_t getLeavesQuantityOnCurrentLevelOnModify(const om::order_t& order) {
            switch(_stacker->_venue) {
                case hf::VENUE::ICE:
                    return getInFlightQty(order) - order.executedQty;
                case hf::VENUE::CME:
                    return getModifyTracker(order)->getInFlightQty() - order.executedQty;
                default:
                    throw std::runtime_error("unknown venue");
            }
        }

        bool isModifying(const om::order_t& order) {
            switch (_stacker->_venue) {
                case hf::VENUE::ICE:
                    return order.is_modifying;
                case hf::VENUE::CME:
                    return getModifyTracker(order)->isModifying();
                default:
                    throw std::runtime_error("unknown venue");
            }
        }

#ifdef IS_DEV
        std::string getContractName() {
            return _stacker->_name;
        }
#else
        uint32_t getContractName() {
            return _stacker->_internalId;
        }
#endif

        void updateOnlyOneLevel(hf::qty_t size, std::vector<const om::order_t*>& orders) {
            //TODO: This should really check if the level is free to prevent crossing
            _target = size;
            hf::qty_t neededSize = size;
            for (auto it = orders.begin(); it != orders.end(); ) {
                om::order_t* order = const_cast<om::order_t*>(*it);
                if (neededSize == 0) {
                    break;
                }
                setNewOrderSize(order, order->leavesQty);
                SetQuantity(*order, 1, IS_ACKED);
                _orders.push_back(order);
                _acknowledged += order->leavesQty;
                it = orders.erase(it);
                if (order->leavesQty <= neededSize) {
                    neededSize -= order->leavesQty;
                } else {
                    decreaseOrderSize(order, order->leavesQty - neededSize);
                    neededSize = 0;
                }
            }
            if (neededSize > 0) {
                createNewOrder(neededSize);
            }
            notifyLevelUsed();
        }

        bool updateOnlyOneLevel(hf::qty_t size, LevelStealer* stealer = nullptr) {
            auto dollarPrice = ToDollars(_price);
            LOG(0, "Stacker {C[i1]}. Updating Price={d1} to size {i2}",  getContractName(), dollarPrice, size)
            LOG(0, "Stacker {C[i1]}. Before. Price={d1} Target={i2} Acked={i3} Flight={i4} Trade={i5} NumFlightout={i6}", getContractName(), dollarPrice, _target, _acknowledged, _flight, _traded, _flightout)

            const bool wasZeroAndNowNo = (_target == 0) & (size > 0);

            if (!_usedByOtherStack) {
                if (size == 0) {
                    _target = 0;
                    if (isLevelFree()) {
                        LOG(0, "Stacker {C[i1]}. Setting size to 0 and has nothing to do. Just freeing level. Price={d1}", getContractName(), dollarPrice)
                        _acknowledged = 0;
                        _traded = 0;
                        notifyLevelFree();
                        LOG(0, "Stacker {C[i1]}. After. Price={d1} Target={i2} Acked={i3} Flight={i4} Trade={i5} NumFlightout={i6}", getContractName(), dollarPrice, _target, _acknowledged, _flight, _traded, _flightout)
                        return false;
                    }
                }

                if (isLevelFree()) {
                    notifyLevelUsed();
                }
            }

            _acknowledged -= _traded;
            _traded = 0;
            _target = size;


            const hf::qty_t change = getQuantityChange();
            //If target is 0 and we're increasing quantity, something very bad happened.
            //TODO: Stop if we hit this case
            BOOST_ASSERT(_target == 0 ? change <= 0 : true);

            if (!_usedByOtherStack) {
                synchronizeOrders(change, stealer);
            } else {
                LOG(0, "Stacker {C[i1]}. Price={d1} Cannot synchronize orders since level used by other side.", getContractName(), dollarPrice)
            }

            LOG(0, "Stacker {C[i1]}. After. Price={d1} Target={i2} Acked={i3} Flight={i4} Trade={i5} NumFlightout={i6}", getContractName(), dollarPrice, _target, _acknowledged, _flight, _traded, _flightout)

            return (change > 0) | wasZeroAndNowNo;
        }

        void notifyLevelUsed() {
            LOG(0, "Stacker {C[i1]}. Side={i2} Notifying level used. Price={i3}", getContractName(), _stacker->_side, _price)
            _stacker->notifyLevelUsed(_price);
        }

        hf::qty_t getTotalOrdersQuanity() const {
            return _acknowledged + _flight;
        };

        hf::qty_t getQuantityChange() {
            BOOST_ASSERT((_acknowledged + _flight) - _traded >= 0);
            BOOST_ASSERT(_flight + _acknowledged >= 0);
            hf::qty_t candidate = _target - getTotalOrdersQuanity() - _traded;
            BOOST_ASSERT(candidate <= _target);
            if(BOOST_UNLIKELY(candidate > _target)) {
                //This is on the most critical of critical paths, we're going to force the compiler to generate a call
                [this, candidate] () NEVER_INLINE {
                    bool& isInLoop = _isDisablingDueToChangeViolation[_stacker];
                    if(!isInLoop) {
                        isInLoop = true;
                        LOG(0,
                            "Stacker {C[i1]}. candidate > target, this should never happen.  candidate={i2} target={i3} acknowledged={i4} flight={i5} traded={i6}",
                            getContractName(), candidate, _target, _acknowledged, _flight, _traded);
                            _stacker->disableMarketMakerAfterStackerRejects(boost::str(boost::format( "MarketMaker candidate=%1% target=%2% candidate>target.  acknowledged=%3% flight=%4% traded=%5%")
                                                                                   % candidate % _target % _acknowledged % _flight % _traded));
                    }
                    else {
                        isInLoop = false;
                    }
                }();
                return 0;
            }
            return candidate;
        }

        void synchronizeOrders(hf::qty_t change, LevelStealer* stealer = nullptr) {
            if (change > 0) {
                increaseSize(change, stealer);
            } else if (change < 0) {
                reduceSize(-change);
            }
        }

        void synchronizeOrders() {
            const hf::qty_t change = getQuantityChange();
            synchronizeOrders(change);
        }

        void increaseSize(hf::qty_t change, LevelStealer* stealer = nullptr) {
            BOOST_ASSERT(change > 0);
            if(!(stealer && stealer->tryModifyInsteadOfNew(this, change))) {
                createNewOrder(change);
            }
        }

        void reduceSize(hf::qty_t change) {
            LOG(0, "Stacker {C[i1]}. Reduce size by {i2}", getContractName(), change)
            hf::qty_t totalLevelSizeDecrease = change;
            for (auto it = _orders.rbegin(); it != _orders.rend(); ++it) {
                om::order_t* order = *it;
                if (canModifyOrCancelOrder(*order)) {
                    totalLevelSizeDecrease = decreaseOrderSize(order, totalLevelSizeDecrease);
                    if (totalLevelSizeDecrease == 0) {
                        break;
                    }
                } else {
                    LOG(0, "Stacker {C[i1]}. Could not change order. Id={i2}", getContractName(), order->oid)
                }
            }
        }

        hf::qty_t decreaseOrderSize(om::order_t* order, hf::qty_t decreaseBy) {
            hf::qty_t leavesQty = getLeavesQty(*order);
            if (leavesQty == decreaseBy) {
                _flight -= decreaseBy;
                _stacker->cancel(order);
                LOG(0, "Stacker {C[i1]}. After decrease. CANCEL_NO_REMAINDER Target={i2} Acked={i3} Flight={i4} Trade={i5} OrderLeaves={i6} NumFlightout={i7}", getContractName(), _target, _acknowledged, _flight, _traded, leavesQty, _flightout)
                return 0;
            } else if (leavesQty < decreaseBy) {
                _flight -= leavesQty;
                _stacker->cancel(order);
                LOG(0, "Stacker {C[i1]}. After decrease. CANCEL_WITH_REMAINDER Target={i2} Acked={i3} Flight={i4} Trade={i5} OrderLeaves={i6} NumFlightout={i7}", getContractName(),_target, _acknowledged, _flight, _traded, leavesQty, _flightout)
                return decreaseBy - leavesQty;
            } else {
                const hf::qty_t orderSize = getTargetQuantity(*order);
                const hf::qty_t qty = orderSize - decreaseBy;
                _flight -= decreaseBy;
                _stacker->modify(order, _price, qty);
                setQuantityToModify(order, qty);
                LOG(0, "Stacker {C[i1]}. After decrease. MODIFY Target={i2} Acked={i3} Flight={i4} Trade={i5} OrderLeaves={i6} NumFlightout={i7}", getContractName(), _target, _acknowledged, _flight, _traded, leavesQty, _flightout)
                return 0;
            }
        }

        void createNewOrder(hf::qty_t orderSize) {
            if (orderSize > 0) {
                om::order_t* order = _stacker->newOrder(this, _price, orderSize);
                if (order == nullptr) {
                    LOG(0, "Stacker {C[i1]}. Could not create order.", getContractName())
                    _stacker->onCriticalError("Stacker could not create order.");
                } else {
                    _orders.push_back(order);
                    _flight += orderSize;
                }
            }
            //Add logging here
        }

        void adjustAfterAck(om::order_t& order) {
            const hf::qty_t change = getQuantityChange();
            if (change > 0) {
                createNewOrder(change);
            } else if (change < 0) {
                if (canModifyOrCancelOrder(order)) {
                    decreaseOrderSize(&order, std::abs(change));
                }
            }
        }

        void onOrderAccepted(const om::OMOrderAcceptedEvent& event, const om::order_t& order) {
            auto dollarPrice = ToDollars(_price);
            LOG(0, "Stacker {C[i1]}. Order Accepted. Id={i2} Price={d1} Size={i3}", getContractName(), order.oid, dollarPrice, event.acceptedQty)
            LOG(0, "Stacker {C[i1]}. Before. Price={d1} Target={i2} Acked={i3} Flight={i4} Trade={i5} NumFlightout={i6}",  getContractName(), dollarPrice, _target, _acknowledged, _flight, _traded, _flightout)
            SetQuantity(order, 1, IS_ACKED);
            moveQuantity(event.acceptedQty);
            setAcceptedModifyQuantity(order, event.acceptedQty);
            adjustAfterAck(const_cast<om::order_t&>(order));
            LOG(0, "Stacker {C[i1]}. After. Price={d1} Target={i2} Acked={i3} Flight={i4} Trade={i5} NumFlightout={i6}", getContractName(), dollarPrice, _target, _acknowledged, _flight, _traded, _flightout)
        }

        void onOrderReplaced(const om::OMOrderModifiedEvent& event, const om::order_t& order, Level* priorLevel) {
            auto dollarPrice = ToDollars(_price);
            LOG(0, "Stacker {C[i1]}. Order Replaced. Id={i2} Price={d1} Size={i3}", getContractName(), order.oid, ToDollars(_price), event.acceptedQty)
            LOG(0, "Stacker {C[i1]}. Before. Price={d1} Target={i2} Acked={i3} Flight={i4} Trade={i5} NumFlightout={i6}",  getContractName(), dollarPrice, _target, _acknowledged, _flight, _traded, _flightout)
            om::order_t& mutableOrder = const_cast<om::order_t&>(order);
            mutableOrder.leavesQty = event.acceptedQty - order.executedQty;

            if(priorLevel != this) {
                LOG(0, "Stacker {C[i1]}. Prior Level (Before Price={d1}). Target={i2} Acked={i3} Flight={i4} Trade={i5} NumFlightout={i6}",  priorLevel->getContractName(), ToDollars(priorLevel->_price), priorLevel->_target, priorLevel->_acknowledged, priorLevel->_flight, priorLevel->_traded, priorLevel->_flightout)
                const hf::qty_t diff = getLeavesQuantityOnCurrentLevelOnModify(order);
                const hf::qty_t previousLevelDiff = getLeavesQuantityOnPriorLevelOnModify(order);
                priorLevel->_flightout -= previousLevelDiff;
                priorLevel->moveQuantity(-previousLevelDiff);
                moveQuantity(diff);
            }
            else {
                const hf::qty_t diff = event.acceptedQty - getRestingQty(order);
                moveQuantity(diff);
            }

            setAcceptedModifyQuantity(order, event.acceptedQty);
            mutableOrder.is_modifying = isStillModifyingAfterModifyAck(order);

            if(priorLevel != this) {
                priorLevel->synchronizeOrders();
                priorLevel->notifyIfLevelFree();
            }
            synchronizeOrders();
            notifyIfLevelFree();
            if(priorLevel != this) {
                LOG(0, "Stacker {C[i1]}. Prior Level (After Price={d1}). Target={i2} Acked={i3} Flight={i4} Trade={i5} NumFlightout={i6}", priorLevel->getContractName(), ToDollars(priorLevel->_price), priorLevel->_target, priorLevel->_acknowledged, priorLevel->_flight, priorLevel->_traded, priorLevel->_flightout);
            }
            LOG(0, "Stacker {C[i1]}. After. Price={d1} Target={i2} Acked={i3} Flight={i4} Trade={i5} NumFlightout={i6}", getContractName(), dollarPrice, _target, _acknowledged, _flight, _traded, _flightout);
        }

        void onOrderExecution(const om::OMExecutionEvent &event, const om::order_t &order) {
            auto dollarPrice = ToDollars(_price);
            LOG(0, "Stacker {C[i1]}. Fill. LevelPrice={i2}. Price={d1} Size={i3} Id={i4}", getContractName(), dollarPrice, event.filledPrice, event.filledQty, order.oid)
            LOG(0, "Stacker {C[i1]}. Before. Price={d1} Target={i2} Acked={i3} Flight={i4} Trade={i5} Leaves={i6}, NumFlightout={i7}",  getContractName(), dollarPrice, _target, _acknowledged, _flight, _traded, order.leavesQty, _flightout)
            _traded += event.filledQty;

            //Note !order.is_active is most likely because of a full fill
            //Note : we don't want to send any new orders here since _target could change

            om::order_t& mutableOrder = const_cast<om::order_t&>(order);
            mutableOrder.leavesQty = getRestingQty(order) - event.totalExecutedQty;

            if(!order.is_active) {
                removeOrder(order);
            }

            if(isModifying(order)) {
                modifyWorkingQuantityInChain(mutableOrder, event.filledQty);
                auto* targetLevel = getTargetLevel(order);
                if(order.is_cancelling && targetLevel != this) {
                    targetLevel->notifyIfLevelFree(); //Fencepost, since we don't have a way to check this in the above call
                }
            }
            else if(order.is_cancelling) {
                //We need to deal with an implicit node on the end of the modify tracker
                _flight += event.filledQty;
            }

            notifyIfLevelFree();

            //Note that WE ARE NOT calling adjustAfterAck(...) here since IME hasn't changed _target to reflect the fill!
            LOG(0, "Stacker {C[i1]}. After. Price={d1} Target={i2} Acked={i3} Flight={i4} Trade={i5} Leaves={i6} NumFlightout={i7}", getContractName(), dollarPrice, _target, _acknowledged, _flight, _traded, order.leavesQty, _flightout)
        }

        void modifyWorkingQuantityInChain(om::order_t& order, hf::qty_t filledQty) {

            bool modifying = isModifying(order);
            bool fullFill = order.leavesQty == 0;

            if(modifying) {
                //First node we just change flightout if proper
                //Intermediary notes we change flightout and flight
                //Lastnode we just change flight
                switch (_stacker->_venue) {
                    case hf::VENUE::ICE:
                    {
                        //Note this is the same as the CME case, except with only 1 possible modify in flight
                        Level* nextLevel = reinterpret_cast<Level*>(GetQuantity(order, LEVEL_POINTER));
                        if(nextLevel != this) {
                            _flightout -= filledQty;
                            _flight += filledQty;
                            nextLevel->_flight -= filledQty;
                        }
                    }
                    break;
                    case hf::VENUE::CME: {
                        ModifyTracker* tracker = getModifyTracker(order);

                        const auto foreachNodeOnPartialFill = [filledQty, &order](typename ModifyTracker::Entry *current, typename ModifyTracker::Entry *prior, bool isPriorLevelRestingLevel, bool isLastLevel) {

                            Level* priorLevel = reinterpret_cast<Level *>(prior->_level);
                            Level* currentLevel = reinterpret_cast<Level *>(current->_level);

                            if(priorLevel != currentLevel) {
                                priorLevel->_flight += filledQty;
                                priorLevel->_flightout -= filledQty;
                                currentLevel->_flight -= filledQty;
                            }
                            else {
                                //
                                //
                                //
                            }

                            //There is an implicit node at the end if we are cancelling, at the same price level
                            if(isLastLevel && order.is_cancelling) {
                                currentLevel->_flight += filledQty;
                            }
                        };

                        const auto foreachNodeOnFullFill = [filledQty, &order](typename ModifyTracker::Entry *current, typename ModifyTracker::Entry *prior, bool isPriorLevelRestingLevel, bool isLastLevel) {
                            //Since we want to do nothing on Modify Rejects, we need to undo the entire modify chain here
                            Level* priorLevel = reinterpret_cast<Level *>(prior->_level);
                            Level* currentLevel = reinterpret_cast<Level *>(current->_level);

                            if(priorLevel != currentLevel) {
                                hf::qty_t priorFilledQty = order.executedQty - filledQty;
                                BOOST_ASSERT(priorFilledQty >= 0);
                                hf::qty_t priorWorkingQty = prior->_qty - priorFilledQty;
                                BOOST_ASSERT(priorWorkingQty > 0);
                                hf::qty_t newWorkingQty = current->_qty - priorFilledQty;
                                BOOST_ASSERT(newWorkingQty > 0);

                                priorLevel->_flight += priorWorkingQty;
                                priorLevel->_flightout -= priorWorkingQty;
                                currentLevel->_flight -= newWorkingQty;

                                currentLevel->notifyIfLevelFree();
                            }
                            else {
                                hf::qty_t delta = current->_qty - prior->_qty;
                                currentLevel->_flight -= delta;
                            }

                            //There is an implicit node at the end if we are cancelling, at the same price level
                            if(isLastLevel && order.is_cancelling) {
                                hf::qty_t priorFilledQty = order.executedQty - filledQty;
                                currentLevel->_flight += current->_qty - priorFilledQty;
                            }
                        };

                        if(!fullFill) {
                            tracker->performOnEachNode(foreachNodeOnPartialFill);
                        }
                        else {
                            tracker->performOnEachNode(foreachNodeOnFullFill);
                        }
                        //We don't want to send new orders until the Unsoc. cancel just in case _target changes
                    }
                        break;
                    default:
                        throw std::runtime_error("unknown venue");
                }
            }
        }

        void handleModifyOnNewOrderReject(om::order_t& order, hf::qty_t inFlightQty) {
            //This is going to look very similar to Modify Reject + Cancel
            BOOST_ASSERT(_stacker->_venue == hf::VENUE::ICE);
            Level* targetLevel = getTargetLevel(order);
            if(targetLevel == this) {
                hf::qty_t diff = getTargetQuantity(order) - getRestingQty(order);
                _flight -= diff;
            }
            else {
                BOOST_ASSERT(!order.executedQty);
                _flight += getRestingQty(order);
                targetLevel->_flight -= getTargetQuantity(order);
                targetLevel->synchronizeOrders();
                targetLevel->notifyIfLevelFree();
            }
        }

        void cancelWorkingQtyInChain(om::order_t& order) {

            switch(_stacker->_venue) {
                case hf::VENUE::ICE:
                {
                    //TODO: Apply CME fix here
                    hf::qty_t overFillQty = getTargetQuantity(order) - order.executedQty;
                    Level *targetLevel = getTargetLevel(order);
                    targetLevel->_flight -= overFillQty;

                    if(targetLevel != this) {
                        _flightout += overFillQty;
                    }
                }
                break;
                case hf::VENUE::CME: {
                    const auto &foreachNode = [&order, this](typename ModifyTracker::Entry *current,
                                                     typename ModifyTracker::Entry *prior,
                                                     bool isPriorLevelRestingLevel, bool isLastLevel) {
                        Level *priorLevel = reinterpret_cast<Level *>(prior->_level);
                        Level *currentLevel = reinterpret_cast<Level *>(current->_level);

                        if(isPriorLevelRestingLevel) {
                            hf::qty_t priorLeavesQty = prior->_qty - order.executedQty;
                            if(priorLeavesQty < 0) {
                                LOG(0, "Stacker {C[i1]}. Overfill. Id={i2} current.qty={i3} executed.qty={i4}", getContractName(), order.oid, current->_qty, order.executedQty);
                            }
                            //Half of a moveQuantity(...), since the other half is done in the code below
                            priorLevel->_acknowledged -= priorLeavesQty;
                        }

                        if(priorLevel != currentLevel) {
                            //Note fillAdjustmentQty is negative for overfill
                            hf::qty_t fillAdjustmentQty = prior->_qty - order.executedQty;
                            priorLevel->_flightout -= fillAdjustmentQty;
                            priorLevel->_flight += fillAdjustmentQty;

                            hf::qty_t priorUnfilledQty = current->_qty - order.executedQty;
                            currentLevel->_flight -= priorUnfilledQty;
                            currentLevel->notifyIfLevelFree();
                        }
                        else {
                            currentLevel->_flight -= current->_qty - prior->_qty;
                        }

                        //Note we can only get here if the order is being modified, i.e. there is an implicit cancel ack in the back of the chain
                        if(order.is_cancelling & isLastLevel) {
                            currentLevel->_flight += current->_qty - order.executedQty;
                        }
                    };
                    getModifyTracker(order)->performOnEachNode(foreachNode);
                }
                break;
            }

//            targetLevel->synchronizeOrders();
//            targetLevel->notifyIfLevelFree();
//            return targetLevel == this;
        }

        void onOrderCanceled(const om::OMOrderCanceledEvent& event, const om::order_t& order) {
            LOG(0, "Stacker {C[i1]}. Order Cancelled. Id={i2}", getContractName(), order.oid)
            LOG(0, "Stacker {C[i1]}. Before. Target={i2} Acked={i3} Flight={i4} Trade={i5} NumFlightout={i6}",  getContractName(), _target, _acknowledged, _flight, _traded, _flightout)
            removeOrder(order);
            //We could have a cancel after a series of modifies, and get a cancel due to modify down, so have to check both
            if(BOOST_LIKELY(order.is_cancelling && !isModifying(order))) {
                moveQuantity(-order.leavesQty);
            }
            else {
                //No chain to worry about
                if(!isModifying(order)){
                    moveQuantityUnsolicited(-order.leavesQty);
                }
                else {
                    om::order_t& mutableOrder = const_cast<om::order_t&>(order);  //This needs to be mutable because adjustAfterAck(..) needs a mutable order
                    cancelWorkingQtyInChain(mutableOrder);
                }
            }
            notifyIfLevelFree();
            LOG(0, "Stacker {C[i1]}. After. Target={i2} Acked={i3} Flight={i4} Trade={i5} NumFlightout={i6}", getContractName(), _target, _acknowledged, _flight, _traded, _flightout)
        }

        void onOrderRejected(const om::OMOrderRejectedEvent& event, const om::order_t& order) {
            LOG(0, "Stacker {C[i1]}. New order rejected. Id={i2} Reason={i3}", getContractName(), order.oid, event.reasonCodeExt)
            LOG(0, "Stacker {C[i1]}. Before. Target={i2} Acked={i3} Flight={i4} Trade={i5} NumFlightout={i6}",  getContractName(), _target, _acknowledged, _flight, _traded, _flightout)

            hf::qty_t diff = getOriginalOrderQtyForNOAck(order);
            _flight -= diff;

            //Cannot happen on CME
            if(order.is_modifying) {
                om::order_t& mutableOrder = const_cast<om::order_t&>(order);  //This needs to be mutable because adjustAfterAck(..) needs a mutable order
                handleModifyOnNewOrderReject(mutableOrder, diff);
            }

            removeOrder(order);

            LOG(0, "Stacker {C[i1]}. After. Target={i2} Acked={i3} Flight={i4} Trade={i5} NumFlightout={i6}", getContractName(), _target, _acknowledged, _flight, _traded, _flightout)

            notifyIfLevelFree();
            _stacker->handleNewOrderReject(order.pr, GetGatewayRejectCodeString(event.reasonCodeExt));
        }

        void onOrderReplaceRejected(const om::OMOrderModifyRejectedEvent& event, const om::order_t& order, Level* nextLevel) {
            LOG(0, "Stacker {C[i1]}. Replace order rejected. Id={i2} Reason={i3}", getContractName(), order.oid, event.reasonCodeExt)
            LOG(0, "Stacker {C[i1]}. Before. Target={i2} Acked={i3} Flight={i4} Trade={i5} NumFlightout={i6}",  getContractName(), _target, _acknowledged, _flight, _traded, _flightout)
            om::order_t& mutableOrder = const_cast<om::order_t&>(order);
            mutableOrder.is_modifying = false;
            bool rejectedDueToFill = WasRejectedDueToBeingFilled(event.reasonCodeExt, order.venue);
            if (BOOST_LIKELY(!rejectedDueToFill)) {
                if (hasOrder(order)) {
                    _stacker->disableMarketMakerAfterStackerRejects("Stacker-Modify");
                }
            } else {
                if (order.is_active) {
                    if(BOOST_LIKELY(_stacker->_venue == hf::VENUE::ICE)) {
                        //Remember -- ICE can only have 1 modify in flight
                        if(nextLevel == this) {
                            hf::qty_t qtyDiffAtSameLevel = getInFlightQty(order) - getRestingQty(order);
                            _flight -= qtyDiffAtSameLevel;
                        }
                        else {
                            //This will closely resemble handleModifyOnNewOrderReject(...)
                            hf::qty_t qtyAtSourceLevel = getRestingQty(order) - order.executedQty;
                            _flight += qtyAtSourceLevel;
                            _flightout -= qtyAtSourceLevel;
                            nextLevel->_flight -= getInFlightQty(order) - order.executedQty;
                            synchronizeOrders();
                            notifyIfLevelFree();
                        }
                        setAcceptedModifyQuantity(order, getRestingQty(order));
                    }
                    else {
                        _stacker->disableMarketMakerAfterStackerRejects("Stacker-Modify-Unexpected-Modify-Reject");
                    }
                }
            }
            LOG(0, "Stacker {C[i1]}. After. Target={i2} Acked={i3} Flight={i4} Trade={i5} NumFlightout={i6}", getContractName(), _target, _acknowledged, _flight, _traded, _flightout)
        }

        //All the state cleanup for Cancel-Due-To-Modify-Down should be handled on the ModifyAck
        void onOrderCancelRejected(const om::OMOrderCancelRejectedEvent &event, const om::order_t &order) {
            LOG(0, "Stacker {C[i1]}. Cancel order rejected. Id={i2} Reason={i3}", getContractName(), order.oid, event.reasonCodeExt)
            LOG(0, "Stacker {C[i1]}. Before. Target={i2} Acked={i3} Flight={i4} Trade={i5} NumFlightout={i6}",  getContractName(), _target, _acknowledged, _flight, _traded, _flightout)
            bool rejectedDueToFill = WasRejectedDueToBeingFilled(event.reasonCodeExt, order.venue);
            if (!rejectedDueToFill) {
                if (hasOrder(order)) {
                    _stacker->onCriticalError("Cancel reject not due to fill and has order.");
                }
            }
            om::order_t& mutableOrder = const_cast<om::order_t&>(order);
            mutableOrder.is_cancelling = false;
            LOG(0, "Stacker {C[i1]}. After. Target={i2} Acked={i3} Flight={i4} Trade={i5} NumFlightout={i6}", getContractName(), _target, _acknowledged, _flight, _traded, _flightout)
        }

        bool isLevelFree() {
            //todo: why is this called so much?
            //todo: remove logging from final build
            //LOG(0, "Stacker {C[i1]} isLevelFree Level={d1} IsFree={i2}", getContractName(), ToDollars(_price), ((_target == 0) & ((_acknowledged - _traded) == 0) & (_flight == 0) & (_flightout == 0) & _orders.empty()));
            //LOG(0, "Stacker {C[i1]} isLevelFree Level={d1} _target={i2} _ack={i3} _traded={i4} _flight={i5} _flightout={i6} _oc={i7}",
            //        getContractName(), ToDollars(_price), _target, _acknowledged, _traded, _flight, _flightout, _orders.size());
            //Note technically _flightout == 0 might not be necessary, if we're ok with in-flight fills on free levels
            return (_target == 0) & ((_acknowledged - _traded) == 0) & ((_flight + _flightout) == 0) & _orders.empty();  //_orders.empty() might not be needed
        }

        void notifyIfLevelFree() {
            if (isLevelFree()) {
                notifyLevelFree();
            }
        }

        void notifyLevelFree() {
            LOG(0, "Stacker {C[i1]}. Side={i2} Notifying level free. Price={d1}", getContractName(), _stacker->_side, ToDollars(_price))
            _stacker->notifyLevelFree(_price);
        }

        void onLevelFree() {
            _usedByOtherStack = false;
            auto dollarPrice = ToDollars(_price);
            LOG(0, "Stacker {C[i1]}. Side={i2} Level freed. Price={d1}", getContractName(), _stacker->_side, dollarPrice)
            if (_target > 0) {
                LOG(0, "Stacker {C[i1]}. Before. Price={d1} Target={i2} Acked={i3} Flight={i4} Trade={i5} NumFlightout={i6}",  getContractName(), dollarPrice, _target, _acknowledged, _flight, _traded, _flightout)
                synchronizeOrders();
                notifyLevelUsed();
                LOG(0, "Stacker {C[i1]}. After. Price={d1} Target={i2} Acked={i3} Flight={i4} Trade={i5} NumFlightout={i6}", getContractName(), dollarPrice, _target, _acknowledged, _flight, _traded, _flightout)
            }
        }

        void onLevelUsed() {
            BOOST_ASSERT(isLevelFree() || (_target == 0 && (_flightout || _flight < 0))); //We could have a cancel or modifyPx in flight
            _usedByOtherStack = true;
            LOG(0, "Stacker {C[i1]}. Side={i2} Level used. Price={d1}", getContractName(), _stacker->_side, ToDollars(_price))
        }

        void removeOrder(const om::order_t& order) {

            //Note it's NOT SAFE to remove the modify tracker here, since we remove the order on a full fill,
            //but we need the modify tracker to handle a potention MR or CR

            Level* level;

            switch(_stacker->_venue) {
                case hf::VENUE::ICE:
                    level = reinterpret_cast<Level*>(GetQuantity(order, LEVEL_POINTER));
                    break;
                case hf::VENUE::CME:
                    level = getModifyTracker(order)->getTargetLevel();
                    break;
                default:
                    throw std::runtime_error("unknown venue");
            }


            for (auto it = level->_orders.begin(); it != level->_orders.end(); ++it) {
                if ((*it)->oid == order.oid) {
                    level->_orders.erase(it);
                    break;
                }
            }
        }

        bool hasOrder(const om::order_t& order) {
            Level* level;

            switch(_stacker->_venue) {
                case hf::VENUE::ICE:
                    level = reinterpret_cast<Level*>(GetQuantity(order, LEVEL_POINTER));
                    break;
                case hf::VENUE::CME:
                    level = getModifyTracker(order)->getTargetLevel();
                    break;
                default:
                    throw std::runtime_error("unknown venue");
            }

            for (auto it = level->_orders.begin(); it != level->_orders.end(); ++it) {
                if ((*it)->oid == order.oid) {
                    return true;
                }
            }
            return false;
        }

        void moveQuantityUnsolicited(hf::qty_t qty) {
            _acknowledged += qty;
        }

        void moveQuantity(hf::qty_t qty) {
            _acknowledged += qty;
            _flight -= qty;
        }

        hf::price_t _price = hf::NULL_PRICE;

        hf::qty_t _target = 0;
        hf::qty_t _acknowledged = 0;
        hf::qty_t _flight = 0;
        hf::qty_t _traded = 0;
        int _flightout = 0;

        bool  _usedByOtherStack = false;

        std::vector<om::order_t*> _orders;

        //std::unordered_map<om::order_id_t, ModifyTracker*> _modifyTrackers;

        Stacker* _stacker;

        Level* before = nullptr;
        Level* after = nullptr;
    };

    template<typename ObjectType, int size>
    class ContiguousAllocator
    {
    public:

        ContiguousAllocator() : _buffer(static_cast<ObjectType*>(malloc(sizeof(ObjectType[size])))) { }

        template<typename... T>
        ObjectType* createObject(T&&... args) {
            BOOST_ASSERT(idx < size - 1); //Might be off by 1, but we shouldn't be anywhere near the limit
            //We don't want to call the move ctor since the exiting object has never been constructed
            new (_buffer + idx) ObjectType{std::forward<T>(args)...};
            return &_buffer[idx++];
        }

    private:
        int idx = 0;
        ObjectType* _buffer; //We are intentionally leaking _buffer, the Object must have a lifetime equal to the program in the current impl
    };

    //Marks a "slice" of the stacker that we can steal from, where on each level we should steal or cancel
    struct LevelStealer {
        //"end" is one past the final level
        LevelStealer(Level* begin, const Level* end) : _begin(begin), _end(end) {
            //Checks if _begin and _end are in the correct order
            BOOST_ASSERT(([this]() -> bool {
                if((_begin == nullptr) && (_end == nullptr)) { return true; } //Explicitly checked
                if(_begin == _end) { return true; } //C++ iter convention
                return _begin->_stacker->_side == BID ? _begin->_price >= _end->_price : _begin->_price <= _end->_price;
            })());
        }

        LevelStealer() : LevelStealer(nullptr, nullptr) {
        }

        bool tryModifyInsteadOfNew(Level* level, hf::qty_t change) {

            BOOST_ASSERT_MSG(change > 0, "Expected positive quantity");

            if(BOOST_UNLIKELY(change <= 0)) {
                return false;
            }

            if (!_begin) {
                return false;
            }

            while (_begin != _end) {
                for (auto iter = _begin->_orders.rbegin(); iter != _begin->_orders.rend(); ++iter) {
                    om::order_t& order = **iter;
                    if (BOOST_LIKELY(order.is_active)) {  //Theoretically redundant with "canModifyOrder" check below
                        Level &from = *_begin;
                        BOOST_ASSERT(from._price != level->_price);
                        if (_begin->canModifyOrCancelOrder(order) & (from._price != level->_price)) {
                            hf::qty_t leavesQty = from.getLeavesQty(order);
                            if(BOOST_LIKELY(leavesQty > 0)) {
                                hf::qty_t newTargetQty = change + order.executedQty; //This relies on from._price != level->price

                                //_target and _filled cannot be touched.  _acked and _flight are all we can play with
                                Level &to = *level;

                                BOOST_ASSERT(from._price != to._price);

                                from._flight -= leavesQty; //Note the caller *must* actually modify the order, or this will not work
                                to._flight += change;

                                from._flightout += leavesQty;

                                from._orders.erase(std::next(iter).base()); //reverse iter to forward iter
                                to._orders.push_back(&order);

                                //Untested, _modifyTrackers appears unused.  --cm
                                //const auto &modifyTracker = from._modifyTrackers.find(order.oid);
                                //to._modifyTrackers.emplace(order.oid, modifyTracker->second);
                                //from._modifyTrackers.erase(modifyTracker);

                                to.setQuantityToModifyAtLevel(&from, &to, &order, newTargetQty);
                                from._stacker->modify(&order, level->_price, newTargetQty);
                                return true;
                            }
                        }
                        if (from.canModifyOrCancelOrder(order)) {
                            hf::qty_t decreaseBy = from.getLeavesQty(order);
                            from._flight -= decreaseBy;
                            from._stacker->cancel(&order);
                            //LOG(0, "Stacker {C[i1]}. After decrease. Target={i2} Acked={i3} Flight={i4} Trade={i5} OrderLeaves={i6}", from.getContractName(), _target, _acknowledged, _flight, _traded, leavesQty)
                        }
                    }
                }
                //Simplest possible (slow?) way to do this
                _begin->updateOnlyOneLevel(0);
                _begin = _begin->after;
            }

            _begin = nullptr;

            return false;
        }

        void cancelAll(Stacker* stacker) {
            //Simplest possible (slow?) impl
            if(_begin) {
                while(_begin && (_begin != _end)) {
                    _begin->updateOnlyOneLevel(0);
                    _begin = _begin->after;
                }
            }
        }

    private:
        Level* _begin;
        const Level* _end;
    };

    void initialize(uint32_t internalId, uint32_t side, hf::price_t tickSize, hf::qty_t quoteSize, uint32_t numberOfLevels, ExecutorT* ime,
                    const std::string& contractName, hf::VENUE venue, uint32_t slackSize, bool useGTC) {
        _internalId = internalId;
        _side = side;
        _tickSize = tickSize;
        _quoteSize = quoteSize;
        _nLevels = numberOfLevels;
        _ime = ime;
#ifdef IS_DEV
        _name = contractName;
#endif
        _venue = venue;
        _maxSlackSize = slackSize;
        _orderType = useGTC ? om::ORDER_TYPE::GTC : om::ORDER_TYPE::LMT;
        LOG(0, "Stacker {C[i1]}. Side={i2} GTC={i3}", getContractName(), 0, (int) useGTC)
    }

    template <uint32_t Side>
    void quote(hf::price_t price, hf::qty_t size, bool goThruAllLevels) {
        auto dollarPrice = ToDollars(price);
        LOG(0, "Stacker {C[i1]}. Side={i2} Price={d1} Size={i3}", getContractName(), Side, dollarPrice, size)
        if(BOOST_UNLIKELY(size < 0)) {
            //This should theoretically never happen.  But in practice it does.
            size = 0;
        }
        if (BOOST_UNLIKELY(_topLevel == nullptr)) {
            createFirstStack<Side>(price, size);
        }
        else if (_topLevel->_price == price) {
            const bool updated = _topLevel->updateOnlyOneLevel(size);
            if (updated | goThruAllLevels) {
                if (_nLevels > 0) {
                    Level* level = _topLevel->after;
                    while ((level->updateOnlyOneLevel(_quoteSize) | goThruAllLevels) & (level != _bottomLevel)) {
                        level = level->after;
                    }
                }
            }
        }
        else if (IsPriceMovingAgainst<Side>(price, _topLevel->_price)) {

            int numLevelsMoved = 0;
            BOOST_ASSERT(_topLevel->_price != price);
            Level* lastLevel = _topLevel;
            while(lastLevel->_price != price) {
                lastLevel = lastLevel->after;
                ++numLevelsMoved;
            }

            LevelStealer stealer{_topLevel, lastLevel}; //C++ iter conventions

            _topLevel = lastLevel;
            _topLevel->updateOnlyOneLevel(size, &stealer);

            //Move to furthers level of stack, then walk back, to try to encourage stealing
            Level* stackerLevel = _topLevel;
            for(uint32_t stackLevel = 0; stackLevel < _nLevels; ++stackLevel) {
                Level* nextLevel = stackerLevel->after;
                if (BOOST_UNLIKELY(nextLevel == nullptr)) {
                    nextLevel = new Level(this);
                    nextLevel->_price = GetPriorPrice<Side>(stackerLevel->_price, _tickSize);
                    nextLevel->before = stackerLevel;
                    stackerLevel->after = nextLevel;
                }
                stackerLevel = nextLevel;
            }

            if (EqualOrBelow<Side>(stackerLevel->_price, _bottomLevel->_price)) {
                _bottomLevel = stackerLevel;
            }

            for(uint32_t stackLevel = 0; stackLevel < _nLevels; ++stackLevel) {
                stackerLevel->updateOnlyOneLevel(_quoteSize, &stealer);
                stackerLevel = stackerLevel->before;
            }

            stealer.cancelAll(this);
        }
        else {
            Level* lastLevel = _topLevel;
            while ((lastLevel->before != nullptr) & (lastLevel->_price != price)) {
                lastLevel = lastLevel->before;
            }

            if (BOOST_UNLIKELY(lastLevel->_price != price)) {
                hf::price_t levelPrice = GetNextPrice<Side>(lastLevel->_price, _tickSize);
                for (; EqualOrBelow<Side>(levelPrice, price);) {
                    Level* level = new Level(this);
                    level->_price = levelPrice;
                    level->after = lastLevel;
                    lastLevel->before = level;
                    lastLevel = level;
                    levelPrice = GetNextPrice<Side>(levelPrice, _tickSize);
                }
            }

            _topLevel = lastLevel;

            //Generate stealer
            LevelStealer stealer = [this]() -> LevelStealer {
                auto totalNumLevels = (std::abs(_bottomLevel->_price - _topLevel->_price) / _tickSize) + 1;
                auto maxNumLevels = _nLevels + _maxSlackSize + 1;
                if (totalNumLevels > maxNumLevels) {
                    Level *beginLevel = _bottomLevel;
                    for (int i = 0; i < totalNumLevels - maxNumLevels - 1; ++i) { //We already included _bottomLevel
                        beginLevel = beginLevel->before;
                    }
                    return {beginLevel, _bottomLevel->after}; //C++ iter convention
                }
                return {};
            }();

            _topLevel->updateOnlyOneLevel(size, &stealer);

            LOG(0, "Stacker {C[i1]} Price={d1} Side={i2}. Will update {i3} levels.", getContractName(), dollarPrice, _side, _nLevels)

            //Update Stack
            Level* stackerLevel = _topLevel;
            for (uint32_t stackLevel = 0; stackLevel < _nLevels; ++stackLevel) {
                stackerLevel = stackerLevel->after;
                stackerLevel->updateOnlyOneLevel(_quoteSize, &stealer);
            }

            Level* newBottomLevel = stackerLevel;

            //Discover if there is slack
            if (_maxSlackSize > 0) {
                uint32_t numberOfSlackLevels = 0;
                while (true) {
                    Level* nextLevel = stackerLevel->after;
                    if ((numberOfSlackLevels < _maxSlackSize) & (nextLevel->_target == _quoteSize)) {
                        ++numberOfSlackLevels;
                        stackerLevel = nextLevel;
                        if (stackerLevel == _bottomLevel) {
                            break;
                        }
                    } else {
                        break;
                    }
                }
                newBottomLevel = stackerLevel;

                stealer.cancelAll(this);
                //Theoretically unneeded, hence BOOST_VERIFY below
                if (stackerLevel != _bottomLevel) {
                    stackerLevel = newBottomLevel->after;
                    while (stackerLevel != _bottomLevel) {
                        BOOST_VERIFY(!stackerLevel->updateOnlyOneLevel(0));
                        stackerLevel = stackerLevel->after;
                    }
                    BOOST_VERIFY(!_bottomLevel->updateOnlyOneLevel(0));
                }
            } else {
                stackerLevel = newBottomLevel->after;

                stealer.cancelAll(this);
                //Theoretically unneeded, hence BOOST_VERIFY below
                while (stackerLevel != _bottomLevel) {
                    BOOST_VERIFY(!stackerLevel->updateOnlyOneLevel(0));
                    stackerLevel = stackerLevel->after;
                }
                BOOST_VERIFY(!_bottomLevel->updateOnlyOneLevel(0));
            }
            _bottomLevel = newBottomLevel;
        }
        _inMarket = true;
    }

    template <uint32_t Side>
    void createFirstStack(hf::price_t price, hf::qty_t size) {
        if (_stackerStart == nullptr) {
            const uint32_t totalNumberOfLevel = _nLevels + 2 * NUMBER_OF_PRE_CREATED_LEVELS + 1;
            ContiguousAllocator<Level, 100 + 2 * NUMBER_OF_PRE_CREATED_LEVELS + 1> allocator;

            hf::price_t levelPrice = price + _tickSize * NUMBER_OF_PRE_CREATED_LEVELS * (_side == BID ? 1 : -1);

            Level* level = allocator.createObject(this);
            level->_price = levelPrice;
            _stackerStart = level;

            for (uint32_t levelIndex = 1; levelIndex < totalNumberOfLevel; ++levelIndex) {
            bool setBottomLevel = false;
                Level* nextLevel = allocator.createObject(this);
                level->after = nextLevel;
                nextLevel->before = level;
                levelPrice = GetPriorPrice<Side>(levelPrice, _tickSize);
                nextLevel->_price = levelPrice;
                if (_usedPrices.count(levelPrice)) {
                    nextLevel->onLevelUsed();
                }
                if (levelIndex < NUMBER_OF_PRE_CREATED_LEVELS) {
                    // nothing just pre-created
                }
                if (levelIndex == NUMBER_OF_PRE_CREATED_LEVELS) {
                    _topLevel = nextLevel;
                    auto& gtc = getGTCs(nextLevel->_price);
                    _topLevel->updateOnlyOneLevel(size, gtc);
                    setBottomLevel = true;
                }
                if (levelIndex > NUMBER_OF_PRE_CREATED_LEVELS && levelIndex <= NUMBER_OF_PRE_CREATED_LEVELS + _nLevels) {
                    auto& gtc = getGTCs(nextLevel->_price);
                    nextLevel->updateOnlyOneLevel(_quoteSize, gtc);
                    setBottomLevel = true;
                }
                if (levelIndex > NUMBER_OF_PRE_CREATED_LEVELS + _nLevels && levelIndex <= NUMBER_OF_PRE_CREATED_LEVELS + _nLevels + _maxSlackSize) {
                    auto& gtc = getGTCs(nextLevel->_price);
                    bool hasGtc = !gtc.empty();
                    if (hasGtc) {
                        nextLevel->updateOnlyOneLevel(_quoteSize, gtc);
                    }
                    setBottomLevel = hasGtc;
                }
                if (levelIndex > NUMBER_OF_PRE_CREATED_LEVELS + _nLevels + _maxSlackSize) {
                    // nothing just pre-created
                }
                if (setBottomLevel) {
                    _bottomLevel = nextLevel;
                }
                level = nextLevel;
            }
        } else {
            BOOST_ASSERT(false);
        }
    }

    void cancel(om::order_t* order) {
        LOG(0, "Stacker {C[i1]}. Cancel order. Id={i2}", getContractName(), order->oid)
        _ime->_executor->cancelOrder(_internalId, order);
    }

    void modify(const om::order_t* order, hf::price_t price, hf::qty_t size) {
        LOG(0, "Stacker {C[i1]}. Modify order. Id={i2} Price={d1} Size={i3}", getContractName(), order->oid, ToDollars(price), size)
        _ime->_executor->modifyOrder(_internalId, order, price, size);
    }

    om::order_t* newOrder(Level* level, hf::price_t price, hf::qty_t size) {
        om::order_t* order = _ime->_executor->newLimitOrder(_internalId, _side, price, size, _orderType);
        LOG(0, "Stacker {C[i1]}. New order. Id={i2} Price={d1} Size={i3}", getContractName(), order->oid, ToDollars(price), size)
        //TODO: Will segfault if order == nullptr
        order->debug.i[LEVEL_POINTER] = reinterpret_cast<uint64_t>(level);
        order->debug.i[ORDER_OWNER] = STACKER_ORDER;
        order->leavesQty = size;
        level->setNewOrderSize(order, size);
        SetQuantity(*order, 0, IS_ACKED);
        return order;
    }

    void onOrderAccepted(const om::OMOrderAcceptedEvent& event, const om::order_t& order) {
        Level* level = getLevelForNewModifyAcknowledgement(order);
        level->onOrderAccepted(event, order);
    }

    void onOrderReplaced(const om::OMOrderModifiedEvent& event, const om::order_t& order) {
        Level* level = getLevelForNewModifyAcknowledgement(order);
        level->onOrderReplaced(event, order, getRestingLevel(order));
    }

    void onOrderExecution(const om::OMExecutionEvent &event, const om::order_t &order) {
        Level* level = getLevelForExchangeAction(order);
        level->onOrderExecution(event, order);
    }

    void onOrderCanceled(const om::OMOrderCanceledEvent& event, const om::order_t& order) {
        Level* level = getLevelForCancel(order);
        level->onOrderCanceled(event, order);
    }

    void onOrderRejected(const om::OMOrderRejectedEvent& event, const om::order_t& order) {
        Level* level = getLevelForNewModifyAcknowledgement(order);
        level->onOrderRejected(event, order);
    }

    void onOrderReplaceRejected(const om::OMOrderModifyRejectedEvent& event, const om::order_t& order) {
        Level* level = getRestingLevel(order);
        level->onOrderReplaceRejected(event, order, getLevelForNewModifyAcknowledgement(order));

    }

    void onOrderCancelRejected(const om::OMOrderCancelRejectedEvent &event, const om::order_t &order) {
        Level* level = getLevelForCancel(order);
        level->onOrderCancelRejected(event, order);
    }

    Level* getLevelForNewModifyAcknowledgement(const om::order_t& order) {
        Level* level;
        switch(_venue) {
            case hf::VENUE::CME:
                level = _mtt.getModifyTracker(order)->getInFlightLevel();
                break;
            case hf::VENUE::ICE:
                level = reinterpret_cast<Level*>(GetQuantity(order, LEVEL_POINTER));
                break;
            default:
                throw std::runtime_error("unrecognized venue");
        }
        return level;
    }

    Level* getLevelForExchangeAction(const om::order_t& order) {
        return getRestingLevel(order);
    }

    Level* getRestingLevel(const om::order_t& order) {
        Level* level;

        switch(_venue) {
            case hf::VENUE::CME:
                level = _mtt.getModifyTracker(order)->getRestingLevel();
                break;
            case hf::VENUE::ICE:
                level = reinterpret_cast<Level*>(GetQuantity(order, PRIOR_LEVEL)); //TODO: Almost certainly wrong
                break;
            default:
                throw std::runtime_error("unrecognized venue");
        }
        return level;
    }

    Level* getInFlightLevel(const om::order_t& order) {
        Level* level;
        switch(_venue) {
            case hf::VENUE::CME:
                level = _mtt.getModifyTracker(order)->getInFlightLevel();
                break;
            case hf::VENUE::ICE:
                level = reinterpret_cast<Level*>(GetQuantity(order, PRIOR_LEVEL));
                break;

        }
        return level;
    }

    Level* getLevelForCancel(const om::order_t& order) {
        //A cancel, solicited or unsolicited, resembles an exchange action in that it can't change the price, i.e.
        //it doesn't touch the modify trackers
        return getLevelForExchangeAction(order);
    }

    void onCriticalError(const std::string& message) {
        _ime->_executor->onCriticalError(message);
    }

    void getOutOfMarket(bool force = false) {
        if (_inMarket) {
            if (!useGTC() | force) {
                Level* level = _topLevel;
                while (level != _bottomLevel) {
                    level->updateOnlyOneLevel(0);
                    level = level->after;
                }
                _bottomLevel->updateOnlyOneLevel(0);
                LOG(0, "Stacker {C[i1]} Side={i2}. Got out of market.", getContractName(), _side)
                _inMarket = false;
            }
        }
    }

    hf::qty_t getRestingSize() {
        if (!_inMarket) {
            return 0;
        } else {
            return _topLevel->_target - _topLevel->_traded;
        }
    }

    hf::price_t getPrice() {
        if (!_inMarket) {
            return hf::NULL_PRICE;
        } else {
            return _topLevel->_price;
        }
    }

    void updateQuoteSize(hf::qty_t size, bool enabled) {
        _quoteSize = size;
        if (_nLevels > 0) {
            if (_topLevel != nullptr) {
                Level* level = _topLevel->after;
                while (level != _bottomLevel) {
                    if (enabled) {
                        level->updateOnlyOneLevel(_quoteSize);
                    } else {
                        level->_target = _quoteSize;
                    }
                    level = level->after;
                }
                if (enabled) {
                    _bottomLevel->updateOnlyOneLevel(_quoteSize);
                } else {
                    _bottomLevel->_target = _quoteSize;
                }
            }
        }
    }

    template <uint32_t Side>
    void updateMaxSlackSize(uint32_t slackSize) {
        LOG(0, "Stacker {C[i1]} Side={i2}. Update number slack size. N={i3}", getContractName(), _side, slackSize)
        if (slackSize < _maxSlackSize) {
            if (_topLevel != nullptr) {
                Level* stackerLevel = _topLevel;
                for (uint32_t n = 0; n < _nLevels; ++n) {
                    stackerLevel = stackerLevel->after;
                }
                Level* newBottom = stackerLevel;

                while (stackerLevel != _bottomLevel) {
                    stackerLevel->updateOnlyOneLevel(0);
                    stackerLevel = stackerLevel->after;
                }
                _bottomLevel->updateOnlyOneLevel(0);

                _bottomLevel = newBottom;
                LOG(0, "Stacker {C[i1]} Side={i2}. BottomPrice={d1}", getContractName(), _side, ToDollars(_bottomLevel->_price))
            }
        }
        _maxSlackSize = slackSize;
    }

    template <uint32_t Side>
    void updateNumberOfLevels(uint32_t nLevels, bool enabled) {
        LOG(0, "Stacker {C[i1]} Side={i2}. Update number of levels. N={i3}", getContractName(), _side, nLevels)
        if (nLevels != _nLevels) {
            if (_topLevel != nullptr) {
                Level* stackLevel = _topLevel;
                for (uint64_t i = 0; i < nLevels; ++i) {
                    stackLevel = stackLevel->after;
                    if (enabled) {
                        stackLevel->updateOnlyOneLevel(_quoteSize);
                    } else {
                        stackLevel->_target = _quoteSize;
                    }
                }
                if (nLevels > _nLevels) {
                    if (EqualOrBelow<Side>(stackLevel->_price, _bottomLevel->_price)) {
                        _bottomLevel = stackLevel;
                    }
                }
                else {
                    Level* newBottomLevel = stackLevel;

                    stackLevel = stackLevel->after;
                    while (stackLevel != _bottomLevel) {
                        if (enabled) {
                            stackLevel->updateOnlyOneLevel(0);
                        } else {
                            stackLevel->_target = 0;
                        }
                        stackLevel = stackLevel->after;
                    }
                    if (enabled) {
                        _bottomLevel->updateOnlyOneLevel(0);
                    } else {
                        _bottomLevel->_target = 0;
                    }

                    _bottomLevel = newBottomLevel;
                }
                LOG(0, "Stacker {C[i1]} Side={i2}. BottomPrice={d1}", getContractName(), _side, ToDollars(_bottomLevel->_price))
            }
            _nLevels = nLevels;
        }
    }

    void notifyLevelFree(hf::price_t price) {
        _ime->notifyLevelFree(_side, price);
    }

    void notifyLevelUsed(hf::price_t price) {
        _ime->notifyLevelUsed(_side, price);
    }

    void onLevelFree(hf::price_t price) {
        Level* level = _topLevel;
        if (level != nullptr) {
            const bool betterPrice = IsBetter(price, level->_price, _side);
            if (betterPrice) {
                while (level != nullptr && level->_price != price) {
                    level = level->before;
                }
            } else {
                while (level != nullptr && level->_price != price) {
                    level = level->after;
                }
            }
            if (level != nullptr) {
                level->onLevelFree();
            } else {
                LOG(0, "Stacker {C[i1]} Side={i2}. onLevelFree. Did not find level. Price={d1}", getContractName(), _side, ToDollars(price))
                //BOOST_ASSERT(false);
            }
        } else {
            auto it = _usedPrices.find(price);
            if (it != _usedPrices.end()) {
                _usedPrices.erase(it);
            }
        }
    }

    void onLevelUsed(hf::price_t price) {
        Level* level = _topLevel;
        if (level != nullptr) {
            const bool betterPrice = IsBetter(price, level->_price, _side);
            if (betterPrice) {
                while (level != nullptr && level->_price != price) {
                    level = level->before;
                }
            } else {
                while (level != nullptr && level->_price != price) {
                    level = level->after;
                }
            }
            if (level != nullptr) {
                level->onLevelUsed();
            } else {
                LOG(0, "Stacker {C[i1]} Side={i2}. onLevelUsed. Did not find level. Price={d1}", getContractName(), _side, ToDollars(price))
                BOOST_ASSERT(false);
            }
        } else {
            LOG(0, "Stacker {C[i1]} Side={i2}. onLevelUsed. Does not have top level. Storing. Price={d1}", getContractName(), _side, ToDollars(price))
            _usedPrices.insert(price);
        }
    }

    void disableMarketMakerAfterStackerRejects(const std::string& reason) {
        _ime->_executor->disableMarketMakerAfterStackerRejects(_internalId, reason);
    }

    void handleNewOrderReject(hf::price_t price, const std::string& reason) {
        _ime->handleStackerNewOrderReject(_side, price, reason);
    }

    std::vector<const om::order_t*>& getGTCs(hf::price_t price) {
        return _ime->getGTCs(price, _side);
    }

    bool useGTC() {
        return _orderType == om::ORDER_TYPE::GTC;
    }

    void updateUseGtc(bool useGtc) {
        _orderType = useGtc ? om::ORDER_TYPE::GTC : om::ORDER_TYPE::LMT;
        if (_inMarket) {
            hf::price_t price = _topLevel->_price;
            hf::qty_t size = _topLevel->_target;
            getOutOfMarket(true);
            if (_side == BID) {
                quote<BID>(price, size, true);
            } else {
                quote<ASK>(price, size, true);
            }
        }
    }

    static ModifyTracker* getAvailableModifyTracker() {
        return _mtt.getAvailableModifyTracker();
    }

    static ModifyTracker* getModifyTracker(const om::order_t& order) {
        return _mtt.getModifyTracker(order);
    }

#ifdef IS_DEV
    std::string getContractName() {
        return _name;
    }
#else
    uint32_t getContractName() {
            return _internalId;
        }
#endif

    std::set<hf::price_t> _usedPrices;

    uint32_t _internalId;
    uint32_t _side;
    hf::price_t _tickSize;
    hf::qty_t _quoteSize;
    uint32_t _nLevels;
    uint32_t _maxSlackSize;

    Level* _stackerStart = nullptr;
    Level* _topLevel = nullptr;
    Level* _bottomLevel = nullptr;

    hf::VENUE _venue;

    ExecutorT* _ime;

    bool _inMarket = false;
#ifdef IS_DEV
    std::string _name;
#endif

    om::ORDER_TYPE _orderType = om::ORDER_TYPE::LMT;
    static ModifyTrackerTracker _mtt;
    static std::unordered_map<Stacker<ExecutorT>*, bool> _isDisablingDueToChangeViolation;
};

template<typename ExecutorT>
typename Stacker<ExecutorT>::ModifyTrackerTracker Stacker<ExecutorT>::_mtt;

template<typename ExecutorT>
std::unordered_map<Stacker<ExecutorT>*, bool>  Stacker<ExecutorT>::_isDisablingDueToChangeViolation;

}

#ifdef REENABLE_BOOST_ASSERTS
#define BOOST_DISABLE_ASSERTS=1
#endif

#endif //VIPER_STACKER_H
