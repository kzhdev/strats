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

template<uint32_t Side>
inline hf::price_t GetFirstPriceOffset(hf::price_t tickSize);

template<>
inline hf::price_t GetFirstPriceOffset<BID>(hf::price_t tickSize) {
    return tickSize * NUMBER_OF_PRE_CREATED_LEVELS;
}

template<>
inline hf::price_t GetFirstPriceOffset<ASK>(hf::price_t tickSize) {
    return -tickSize * NUMBER_OF_PRE_CREATED_LEVELS;
}

struct SMPModifyRejectReason {
    static const std::string VALUE;
};

inline void SetModifyTracker(const om::order_t& order, uint32_t modifyTrackerIndex) {
    om::order_t& mutableOrder = const_cast<om::order_t&>(order);
    mutableOrder.debug.i[MODIFY_TRACKER] = modifyTrackerIndex;
}

template<typename IME_T>
class OrderManager {

    enum class TargetType : uint8_t {
        Quote = 0,
        Stack = 1,
        Hedge = 2,
    };

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

        template<typename ActionType, bool StopAfterFirstIteration = false>
        void performOnEachNode(const ActionType&& action) {
            auto priorIndex = getPriorIndex(_popIndex);
            auto currentIndex = _popIndex;
            auto nextIndex = getNextIndex(_popIndex);
            Entry* priorEntry = &_entries[priorIndex];
            Entry* currentEntry = &_entries[currentIndex];
            action(currentEntry->_qty, currentEntry->_level, priorEntry->_qty, priorEntry->_level, true, nextIndex == _insertIndex);
            if(!StopAfterFirstIteration) {
                while (nextIndex != _insertIndex) {
                    priorIndex = currentIndex;
                    currentIndex = nextIndex;
                    nextIndex = getNextIndex(currentIndex);
                    priorEntry = &_entries[priorIndex];
                    currentEntry = &_entries[currentIndex];
                    action(currentEntry->_qty, currentEntry->_level, priorEntry->_qty, priorEntry->_level, false,
                           nextIndex == _insertIndex);
                }
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
        static_assert(om::ORDER_NUM == 2'000'000, "om::ORDER_NUM has changed");
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

        Level(OrderManager* om, hf::price_t price) : _price(price), _dollar(ToDollars(price)), _om(om) {
            _orders.reserve(8);
        }

        ModifyTracker* getAvailableModifyTracker() {
            return _om->getAvailableModifyTracker();
        }

        ModifyTracker* getModifyTracker(const om::order_t &order) const {
            return _om->getModifyTracker(order);
        }

        hf::qty_t getOriginalOrderQtyForNOAck(const om::order_t& order) const {
            switch (_om->_venue) {
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

        hf::qty_t getTargetQuantity(const om::order_t& order) const {
            switch (_om->_venue) {
                case hf::VENUE::ICE:
                    return GetQuantity(order, QUANTITY_TO_MODIFY);
                case hf::VENUE::CME:
                    return getModifyTracker(order)->getTargetQty();
                default:
                    throw std::runtime_error("unrecognized venue");
            }
        }

        Level* getTargetLevel(const om::order_t& order) const {
            switch (_om->_venue) {
                case hf::VENUE::ICE:
                    return reinterpret_cast<Level *>(GetQuantity(order, LEVEL_POINTER));
                case hf::VENUE::CME:
                    return getModifyTracker(order)->getTargetLevel();
                default:
                    throw std::runtime_error("unrecognized venue");
            }
        }

        //Gets the current in-flight quantity, ASSUMING THERE IS A LEVEL IN FLIGHT
        hf::qty_t getInFlightQty(const om::order_t& order) const {
            switch (_om->_venue) {
                case hf::VENUE::ICE:
                    return GetQuantity(order, QUANTITY_TO_MODIFY);
                case hf::VENUE::CME:
                    return getModifyTracker(order)->getInFlightQty();
                default:
                    throw std::runtime_error("unrecognized venue");
            }
        }

        hf::qty_t getRestingQty(const om::order_t& order) const {
            switch (_om->_venue) {
                case hf::VENUE::ICE:
                    return GetQuantity(order, PRIOR_QUANTITY);
                case hf::VENUE::CME:
                    return getModifyTracker(order)->getRestingQty();
                default:
                    throw std::runtime_error("unrecognized venue");
            }
        }

        void setNewOrderSize(om::order_t* order, hf::qty_t size) {
            switch (_om->_venue) {
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
            switch (_om->_venue) {
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
            if (_om->_venue == hf::VENUE::ICE) {
                SetQuantity(*order, reinterpret_cast<int64_t>(priorLevel), PRIOR_LEVEL);
            }
        }

        void setAcceptedModifyQuantity(const om::order_t& order, hf::qty_t size) {
            switch (_om->_venue) {
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

        bool isStillModifyingAfterModifyAck(const om::order_t& order) const {
            switch (_om->_venue) {
                case hf::VENUE::ICE:
                    return false;
                case hf::VENUE::CME:
                    return getModifyTracker(order)->isModifying();
                default:
                    throw std::runtime_error("unrecognized venue");
            }
        }

        //Theoretically we should allow cancels if we detect an overfill potential because of Modify Reject
        bool canModifyOrCancelOrder(const om::order_t& order) const {
            switch (_om->_venue) {
                case hf::VENUE::ICE:
                    //Should test in cert if can cancel with modify in flight
                    return order.is_active & !order.is_cancelling & !order.is_modifying & (GetQuantity(order, POTENTIAL_OVERFILL_QTY) == 0);
                case hf::VENUE::CME:
                    return (GetQuantity(order, IS_ACKED) == 1) & order.is_active & !order.is_cancelling & (GetQuantity(order, POTENTIAL_OVERFILL_QTY) == 0);
                default:
                    throw std::runtime_error("unknown venue");
            }
        }

        //This tests if we can modify/cancel an order solely based on what is in lifhgt
        bool inflightStatePreventsDecrement(const om::order_t& order) {
            switch(_om->_venue) {
                case hf::VENUE::ICE:
                    return order.is_active & order.is_modifying;
                case hf::VENUE::CME:
                    return !(GetQuantity(order, IS_ACKED) == 1);
                default:
                    throw std::runtime_error("unknown venue");
            }
        }

        hf::qty_t getLeavesQty(const om::order_t& order) const {
            switch (_om->_venue) {
                case hf::VENUE::ICE:
                case hf::VENUE::CME:
                    return getTargetQuantity(order) - order.executedQty;
                default:
                    throw std::runtime_error("unknown venue");
            }
        }

        hf::qty_t getLeavesQuantityOnPriorLevelOnOrderReplaced(const om::order_t& order) const {
            return getRestingQty(order) - order.executedQty;
        }

        hf::qty_t getLeavesQuantityOnCurrentLevelOnOrderReplaced(const om::order_t& order) const {
            return getInFlightQty(order) - order.executedQty;
        }

        bool isModifying(const om::order_t& order) const {
            switch (_om->_venue) {
                case hf::VENUE::ICE:
                    return order.is_modifying;
                case hf::VENUE::CME:
                    return getModifyTracker(order)->isModifying();
                default:
                    throw std::runtime_error("unknown venue");
            }
        }

#ifdef IS_DEV
        std::string getContractName() const {
            return _om->_ime->_name;
        }

        static const char* getComponentName(TargetType type) noexcept {
            static const char* components[] = {"Quote", "Stack", "Hedge"};
            return components[static_cast<std::underlying_type_t<TargetType>>(type)];
        }
#else
        uint32_t getContractName() const {
            return _om->_internalId;
        }

        static uint32_t getComponentName(TargetType type) noexcept {
            return std::underlying_type_t<TargetType>(type);
        }
#endif

        hf::qty_t target() const noexcept {
            return _target[STACKER_ORDER] + _target[HEDGER_ORDER];
        }

        //Note that this does not check things like if the order can actually decrease in size,
        //which means that this really is only safe to call when creating the first stack
        void updateOnlyOneLevel(hf::qty_t size, std::vector<const om::order_t*>& orders, bool isHedge) {
            //TODO: This should really check if the level is free to prevent crossing
            _target[isHedge] = size;
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
                    decreaseOrderSize(order, order->leavesQty - neededSize, isHedge);
                    neededSize = 0;
                }
            }
            if (neededSize > 0) {
                createNewOrder(neededSize, isHedge);
            }

            if (!isHedge) {
                notifyLevelUsed();
            }
        }

        bool updateOnlyOneLevel(hf::qty_t size, TargetType targetType, LevelStealer* stealer = nullptr) {
            bool isHedge = targetType == TargetType::Hedge;
            hf::qty_t orgQuoteTarget = _target[0];
            hf::qty_t orgHedgeTarget = _target[1];
            const bool wasZeroAndNowNo = (target() == 0) & (size > 0);
            const bool isLevelCurrentFree = isLevelFree();
            _target[isHedge] = size;
            LOG(0, "OM {C[i1]}. Updating {i2} Price={d1} to size {i3}. Target={i4}|{i5}->{i6}|{i7}", getContractName(), getComponentName(targetType), _dollar, size, orgQuoteTarget, orgHedgeTarget, _target[0], _target[1])
            if (!_usedByOtherStack) {
                if (target() == 0) {
                    if (isLevelFree()) {
                        LOG(0, "OM {C[i1]}. Setting size to 0 and has nothing to do. Just freeing level. Price={d1}", getContractName(), _dollar)
                        _acknowledged = 0;
                        _traded = 0;
                        notifyLevelFree();
                        return false;
                    }
                }

                if (isLevelCurrentFree && _target[0]  > 0) {
                    notifyLevelUsed();
                }
            }

            _acknowledged -= _traded;
            _traded = 0;

            const hf::qty_t change = getQuantityChange();
            //If target is 0 and we're increasing quantity, something very bad happened.
            //TODO: Stop if we hit this case
            BOOST_ASSERT(target() == 0 ? change <= 0 : true);

            if (!_usedByOtherStack) {
                synchronizeOrders(change, isHedge, stealer);
            } else if (targetType == TargetType::Hedge) {
                if (_om->_venue == hf::VENUE::CME || _om->_iceCancelResting) {
                    LOG(0, "OM {C[i1]}. Price={d1} Level is still used by other side. Let hedge order go through anyway. Resting order will be canceled", getContractName(), _dollar)
                    synchronizeOrders(change, isHedge, stealer);
                }
                else if (change != 0) {
                    Level* otherSideLevel = getOtherSideTopLevel();
                    if (BOOST_LIKELY(otherSideLevel != nullptr)) {
                        LOG(0, "OM {C[i1]}. Price={d1} Level is still used by other side. OtherSide TopLevel={d1}", getContractName(), _dollar, otherSideLevel->_dollar)
                        uint32_t otherSide = 1 - _om->_side;

                        if (IsBetterOrEqual(_price, otherSideLevel->_price, otherSide)) {
                            otherSideLevel = getOtherSide();
                            if (otherSideLevel->target() == 0) {
                                // Target on other side is ready removed. Order cancel probably in-flight.
                                LOG(0, "OM {C[i1]}. Other side target is 0. Let hedge order go through.", getContractName())
                                synchronizeOrders(change, isHedge, stealer);
                            }
                            else if (change < 0) {
                                // Allow hedge decrease because the order of other side should be rejected due to SMP
                                LOG(0, "OM {C[i1]}. Let hedge target decrease go through", getContractName())
                                synchronizeOrders(change, isHedge, stealer);
                            }
                            else {
                                LOG(0, "OM {C[i1]}. Cannot send hedge order. Level used by other side.", getContractName())
                            }
                        } else {
                            bool canSend = true;
                            while (otherSideLevel && IsBetterOrEqual(_price, otherSideLevel->_price, otherSide)) {
                                if (otherSideLevel->target() != 0) {
                                    canSend = false;
                                    break;
                                }
                                otherSideLevel = otherSideLevel->after;
                            }
                            if (canSend) {
                                // Target on other side is ready removed. Order cancel probably in-flight.
                                LOG(0, "OM {C[i1]}. Other side targets are set to 0. Let hedge order go through.", getContractName())
                                synchronizeOrders(change, isHedge, stealer);
                            }
                            else if (change < 0) {
                                // Allow hedge decrease because the order of other side should be rejected due to SMP
                                LOG(0, "OM {C[i1]}. Let hedge target decrease go through", getContractName())
                                synchronizeOrders(change, isHedge, stealer);
                            }
                            else {
                                LOG(0, "OM {C[i1]}. Cannot send hedge order. Level used by other side.", getContractName())
                            }
                        }
                    } else {
                        BOOST_ASSERT_MSG(false, "Other sided used but Level is NULL");
                        synchronizeOrders(change, isHedge, stealer);
                    }
                }
            } else {
                LOG(0, "OM {C[i1]}. Price={d1} Cannot synchronize orders since level used by other side.", getContractName(), _dollar)
            }

            return (change > 0) | wasZeroAndNowNo;
        }

        Level* getOtherSideTopLevel() const noexcept {
            OrderManager<IME_T>& otherSideOM = _om->_ime->_om[1 - _om->_side];
            return otherSideOM._topLevel;
        }

        Level* getOtherSide() const noexcept {
            OrderManager<IME_T>& otherSideOM = _om->_ime->_om[1 - _om->_side];
            return (otherSideOM._side == BID) ? otherSideOM.findLevel<BID>(_price) : otherSideOM.findLevel<ASK>(_price);
        }

        void notifyLevelUsed() {
            LOG(0, "OM {C[i1]}. Side={i2} Notifying level used. Price={i3}", getContractName(), _om->_side, _price)
            _om->notifyLevelUsed(_price);
        }

        hf::qty_t getTotalOrdersQuanity() const {
            //We should consider always considering modifying _traded here, like we currently do in quote().
            return (_acknowledged -_traded) + _flight + _potentialOverfill;
        };

        hf::qty_t getQuantityChange() {
            BOOST_ASSERT(getTotalOrdersQuanity() >= 0);
            BOOST_ASSERT(_flight + _acknowledged + _potentialOverfill >= 0);
            hf::qty_t target_ = target();
            hf::qty_t candidate = target_ - getTotalOrdersQuanity();
            BOOST_ASSERT(candidate <= target_);
            if (BOOST_UNLIKELY(candidate > target_)) {
                //This is on the most critical of critical paths, we're going to force the compiler to generate a call
                candidate = [this, candidate, target_]() NEVER_INLINE COLD -> hf::qty_t {
                    bool &isInLoop = _isDisablingDueToChangeViolation[_om];
                    if (!isInLoop) {
                        LOG(0, "OM {C[i1]}. {i2} {d1} candidate > target; this should never happen. candidate={i3} target={i4}|{i5} acknowledged={i6} flight={i7} traded={i8} po={i9}", getContractName(), _om->_side, _dollar, candidate, _target[0], _target[1], _acknowledged, _flight, _traded, _potentialOverfill)

                        if(_potentialOverfill < 0) {
                            hf::qty_t newCandidate = candidate + _potentialOverfill;
                            if(newCandidate >= 0 && newCandidate <= target_) {
                                LOG(0,  "OM {C[i1]} candidate makes sense after resetting _potentialOverfill.  Returning candidate={i2}", getContractName(), newCandidate);

                                std::string errorMessage = std::string{ "MarketMaker candidate > target before resetting _potentialOverfill.  After resetting _potentialOverfill candidate makes sense so not shutting down.  TELL A DEV.  contract="} +
                                                           _om->_ime->_name + " side=" + std::to_string(_om->_side) + " oldCandidate=" + std::to_string(newCandidate) + " oldPotentialOverfill=" + std::to_string(_potentialOverfill) + " newTarget=" + std::to_string(newCandidate);
                                _om->sendErrorMessage(errorMessage);

                                _potentialOverfill = 0;
                                return newCandidate;
                            }
                        }

                        isInLoop = true;
                        _om->disableMarketMakerAfterStackerRejects(boost::str(boost::format( "MarketMaker side=%1% price=%2% candidate=%3% target=%4% candidate>target: unrecoverable.  acknowledged=%5% flight=%6% traded=%7% po=%8%")
                                                                              % _om->_side % _dollar % candidate % target_ % _acknowledged % _flight % _traded % _potentialOverfill));
                    } else {
                        isInLoop = false;
                    }
                    return 0;
                }();
            }
            return candidate;
        }

        void synchronizeOrders(hf::qty_t change, bool isHedge, LevelStealer* stealer = nullptr) {
            if (change > 0) {
                increaseSize(change, isHedge, stealer);
            } else if (change < 0) {
                reduceSize(-change, isHedge);
            }
        }

        void synchronizeOrders(bool isHedge = false) {
            const hf::qty_t change = getQuantityChange();
            synchronizeOrders(change, isHedge);
        }

        void increaseSize(hf::qty_t change, bool isHedge, LevelStealer* stealer = nullptr) {
            BOOST_ASSERT(change > 0);
            if(BOOST_UNLIKELY(_om->_overfillProtection.blocked())) {
                LOG(0, "OM {C[i1]}. Price={d1} cannot increaseSize due to overfill protection. IsHedge={i3}", getContractName(), _dollar, isHedge)
                if (!isHedge) {
                    return;
                }
            }

            if(stealer) {
                auto result = stealer->tryModifyInsteadOfNew(this, change, isHedge);
                switch (result) {
                    case LevelStealer::Result::MODIFIED:
                        return;
                    case LevelStealer::Result::CAN_NOT_MODIFY:
                        // always let a hedge order go through
                        if (!isHedge) {
                            return;
                        }
                        break;
                    case LevelStealer::Result::NOTHING_DONE:
                        break;
                }
            }
            createNewOrder(change, isHedge);
        }

        bool reduceSize(hf::qty_t change, bool isHedge) {
            LOG(0, "OM {C[i1]}. Reduce size by {i2} @ {d1}", getContractName(), change, _dollar)
            hf::qty_t totalLevelSizeDecrease = change;
            for (auto it = _orders.rbegin(); it != _orders.rend(); ++it) {
                om::order_t* order = *it;
                if (canModifyOrCancelOrder(*order)) {
                    totalLevelSizeDecrease = decreaseOrderSize(order, totalLevelSizeDecrease, isHedge);
                    if (totalLevelSizeDecrease == 0) {
                        return true;
                    }
                }
                else {
                    LOG(0, "OM {C[i1]}. Could not change order. Id={i2}", getContractName(), order->oid)
                }
            }

            //There is quantity left, we're going to tag one order that is preventing the decrement.  Maybe we should tag all?
            LOG(0, "OM {C[i1]}. Quantity left after reduceSize, looking for order preventing decrement", getContractName());
            for(auto it = _orders.rbegin(); it != _orders.rend(); ++it) {
                om::order_t* order = *it;
                if(inflightStatePreventsDecrement(*order)) {
                    bool alreadyCounted = _om->_overfillProtection.incrementIfNecessary(*order, _om);
                    LOG(0, "OM {C[i1]}. Found candidate blocking order. Side={i2} Id={i3} alreadyCounted={i4} _markedOrder={i5}", getContractName(), _om->_side, order->oid, alreadyCounted, _om->_overfillProtection.getMarkedOrderCountForLogging());
                    return false;
                }
            }

            //We used to be able to get here because of a bug in get getQuantityChange(...).  The current ASSERT really should be true.
            [this] () NEVER_INLINE  {
                BOOST_ASSERT_MSG(false, "Leftover quantity after decrement");
                LOG(0, "OM {C[i1]}.  Could not reduce quantity to zero, but no candidate blocking order found.  Level={i2}", getContractName(), _price);
                LOG(0, "OM {C[i1]}.  _target={i2}|{i3} _ack={i4} _flight={i5} _traded={i6} _potentialOverfill={i7} _flightout={i8} orderCount={i9}", getContractName(), _target[0], _target[1],
                    _acknowledged, _flight, _traded, _potentialOverfill, _flightout, _orders.size());
                for(unsigned i = 0 ; i < _orders.size(); ++i) {
                    const auto &order = *_orders[i];
                    LOG(0, "OM {C[i1]}. _orderIndex={i2} active={i3} acked={i4} cancelling={i5} modifying={i6} px={i7} qty={i8}", getContractName(), i, order.is_active, order.is_acked,
                        order.is_cancelling, order.is_modifying, order.pr, order.qty);
                }
                _om->sendErrorMessage("OM: Leftover quantity after decrement in getQuantityChange().");
            }();
            return false;
        }

        hf::qty_t decreaseOrderSize(om::order_t* order, hf::qty_t decreaseBy, bool isHedge) {
            hf::qty_t leavesQty = getLeavesQty(*order);
            if (leavesQty == decreaseBy) {
                _flight -= decreaseBy;
                _om->cancel(order);
                LOG(0, "OM {C[i1]}. After decrease. CANCEL_NO_REMAINDER Target={i2}|{i3} Acked={i4} Flight={i5} Trade={i6} OrderLeaves={i7} PotentialOverfill={i8} NumFlightout={i9}", getContractName(), _target[0], _target[1], _acknowledged, _flight, _traded, leavesQty, _potentialOverfill, _flightout)
                return 0;
            } else if (leavesQty < decreaseBy) {
                _flight -= leavesQty;
                _om->cancel(order);
                LOG(0, "OM {C[i1]}. After decrease. CANCEL_WITH_REMAINDER Target={i2}|{i3} Acked={i4} Flight={i5} Trade={i6} OrderLeaves={i7} PotentialOverfill={i8} NumFlightout={i9}", getContractName(), _target[0], _target[1], _acknowledged, _flight, _traded, leavesQty, _potentialOverfill, _flightout)
                return decreaseBy - leavesQty;
            } else {
                const hf::qty_t orderSize = getTargetQuantity(*order);
                const hf::qty_t qty = orderSize - decreaseBy;
                _flight -= decreaseBy;
                _om->modify(order, _price, qty, isHedge);
                setQuantityToModify(order, qty);
                LOG(0, "OM {C[i1]}. After decrease. MODIFY Target={i2}|{i3} Acked={i4} Flight={i5} Trade={i6} OrderLeaves={i7} PotentialOverfill={i8} NumFlightout={i9}", getContractName(), _target[0], _target[1], _acknowledged, _flight, _traded, leavesQty, _potentialOverfill, _flightout)
                return 0;
            }
        }

        void createNewOrder(hf::qty_t orderSize, bool isHedge) {
            if (BOOST_LIKELY(orderSize > 0)) {
                om::order_t* order = _om->newOrder(this, _price, orderSize, isHedge);
                if (order == nullptr) {
                    LOG(0, "OM {C[i1]}. Could not create order.", getContractName())
                    _om->onCriticalError("OM could not create order.");
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
                if(!_om->_overfillProtection.blocked()) {
                    createNewOrder(change, false);
                }
            } else if (change < 0) {
                if (canModifyOrCancelOrder(order)) {
                    decreaseOrderSize(&order, std::abs(change), false);
                }
                else {
                    //Example when canModifyOrcancelOrder==true, inflightStatePreventsDecrement==false: NO Ack calling "adjust after ack" on ICE with Cancel in flight
                    if(inflightStatePreventsDecrement(order)) {
                        LOG(0, "OM {C[i1]} Side={i2} adjustAfterAck cannot decrease order size due to blocking order. oid={i3} is_active={i4} is_acked={i5} is_cancelling={i6} is_modifying={i7}",
                                getContractName(), _om->_side, order.oid, order.is_active, order.is_acked, order.is_cancelling, order.is_modifying);
                        _om->_overfillProtection.incrementIfNecessary(order, _om);
                    }
                }
            }
        }

        void onOrderAccepted(const om::OMOrderAcceptedEvent& event, const om::order_t &order) {
            LOG(0, "OM {C[i1]}. Order Accepted. Side={i2} Id={i3} Price={d1} Size={i4}", getContractName(), _om->_side, order.oid, _dollar, event.acceptedQty)
            LOG(0, "OM {C[i1]}. Before. Price={d1} Target={i2}|{i3} Acked={i4} Flight={i5} Trade={i6} PotentialOverfill={i7} NumFlightout={i8}", getContractName(), _dollar, _target[0], _target[1], _acknowledged, _flight, _traded, _potentialOverfill, _flightout)
            SetQuantity(order, 1, IS_ACKED);
            SetQuantity(order, 0, IS_HEDGE);    // hedge action complete, reset it
            moveQuantity(event.acceptedQty);
            setAcceptedModifyQuantity(order, event.acceptedQty);
            adjustAfterAck(const_cast<om::order_t&>(order));
            LOG(0, "OM {C[i1]}. After. Price={d1} Target={i2}|{i3} Acked={i4} Flight={i5} Trade={i6} PotentialOverfill={i7} NumFlightout={i8}", getContractName(), _dollar, _target[0], _target[1], _acknowledged, _flight, _traded, _potentialOverfill, _flightout)
        }

        void onOrderReplaced(const om::OMOrderModifiedEvent& event, const om::order_t& order, Level* priorLevel) {
            LOG(0, "OM {C[i1]}. Order Replaced. Side={i2} Id={i3} Price={d1} Size={i4}", getContractName(), _om->_side, order.oid, _dollar, event.acceptedQty)
            LOG(0, "OM {C[i1]}. Before. Price={d1} Target={i2}|{i3} Acked={i4} Flight={i5} Trade={i6} PotentialOverfill={i7} NumFlightout={i8}", getContractName(), _dollar, _target[0], _target[1], _acknowledged, _flight, _traded, _potentialOverfill, _flightout)
            om::order_t& mutableOrder = const_cast<om::order_t &>(order);
            mutableOrder.leavesQty = event.acceptedQty - order.executedQty;

            SetQuantity(order, 0, IS_HEDGE);    // hedge action complete, reset it

            if (priorLevel != this) {
                LOG(0, "OM {C[i1]}. Prior Level (Before Price={d1}). Target={i2}|{i3} Acked={i4} Flight={i5} Trade={i6} PotentialOverfill={i7} NumFlightout={i8}", priorLevel->getContractName(), priorLevel->_dollar, priorLevel->_target[0], priorLevel->_target[1], priorLevel->_acknowledged, priorLevel->_flight, priorLevel->_traded, priorLevel->_potentialOverfill, priorLevel->_flightout)
                const hf::qty_t diff = getLeavesQuantityOnCurrentLevelOnOrderReplaced(order);
                const hf::qty_t previousLevelDiff = getLeavesQuantityOnPriorLevelOnOrderReplaced(order);
                priorLevel->_flightout -= previousLevelDiff;
                priorLevel->moveQuantity(-previousLevelDiff);
                moveQuantity(diff);
            } else {
                const hf::qty_t diff = event.acceptedQty - getRestingQty(order);
                moveQuantity(diff);
            }

            setAcceptedModifyQuantity(order, event.acceptedQty);
            mutableOrder.is_modifying = isStillModifyingAfterModifyAck(order);

            if (priorLevel != this) {
                priorLevel->synchronizeOrders();
                priorLevel->notifyIfLevelFree();
            }
            synchronizeOrders();
            notifyIfLevelFree();

            if (GetQuantity(order, POTENTIAL_OVERFILL_QTY) != 0) {
                getTargetLevel(order)->synchronizeOrders();
            }

            if (priorLevel != this) {
                LOG(0, "OM {C[i1]}. Prior Level (After Price={d1}). Target={i2}|{i3} Acked={i4} Flight={i5} Trade={i6} PotentialOverfill={i7} NumFlightout={i8}", priorLevel->getContractName(), priorLevel->_dollar, priorLevel->_target[0], priorLevel->_target[1], priorLevel->_acknowledged, priorLevel->_flight, priorLevel->_traded, priorLevel->_potentialOverfill, priorLevel->_flightout)
            }
            LOG(0, "OM {C[i1]}. After. Price={d1} Target={i2}|{i3} Acked={i4} Flight={i5} Trade={i6} PotentialOverfill={i7} NumFlightout={i8}", getContractName(), _dollar, _target[0], _target[1], _acknowledged, _flight, _traded, _potentialOverfill, _flightout)
        }

        void onOrderExecution(const om::OMExecutionEvent &event, const om::order_t &order) {
            LOG(0, "OM {C[i1]}. Fill. Side={i2} LevelPrice={d1} Price={d2} Size={i3} Id={i4}", getContractName(), _om->_side, _dollar, ToDollars(event.filledPrice), event.filledQty, order.oid)
            LOG(0, "OM {C[i1]}. Before. Price={d1} Target={i2}|{i3} Acked={i4} Flight={i5} Trade={i6} Leaves={i7} PotentialOverfill={i8} NumFlightout={i9}", getContractName(), _dollar, _target[0], _target[1], _acknowledged, _flight, _traded, order.leavesQty, _potentialOverfill, _flightout)

            _traded += event.filledQty;

            //Note !order.is_active is most likely because of a full fill
            //Note : we don't want to send any new orders here since _target could change

            om::order_t& mutableOrder = const_cast<om::order_t &>(order);
            mutableOrder.leavesQty = getRestingQty(order) - event.totalExecutedQty;

            if (!order.is_active) {
                removeOrder(order);
            }

            if (isModifying(order)) {
                modifyWorkingQuantityInChain(mutableOrder, event.filledQty);
                auto* targetLevel = getTargetLevel(order);

                //BUGBUGBUG
                hf::qty_t potentialOverfillQty = -(getTargetQuantity(order) - order.executedQty);
                if (potentialOverfillQty > 0) {
                    if (!mutableOrder.leavesQty) { //If full fill, we won't get the MO/CO so we're in a terminal state
                        potentialOverfillQty = 0;
                    }
                    targetLevel->_potentialOverfill = potentialOverfillQty;
                    SetQuantity(order, potentialOverfillQty, POTENTIAL_OVERFILL_QTY);
                }

                if (order.is_cancelling && targetLevel != this) {
                    targetLevel->notifyIfLevelFree(); //Fencepost, since we don't have a way to check this in the above call
                }
            } else if (order.is_cancelling) {
                //We need to deal with an implicit node on the end of the modify tracker
                _flight += event.filledQty;
            }

            notifyIfLevelFree();

            //Note that WE ARE NOT calling adjustAfterAck(...) here since IME hasn't changed _target to reflect the fill!
            LOG(0, "OM {C[i1]}. After. Price={d1} Target={i2}|{i3} Acked={i4} Flight={i5} Trade={i6} Leaves={i7} PotentialOverfill={i8} NumFlightout={i9}", getContractName(), _dollar, _target[0], _target[1], _acknowledged, _flight, _traded, order.leavesQty, _potentialOverfill, _flightout)
        }

        void modifyWorkingQuantityInChain(om::order_t& order, hf::qty_t filledQty) {

            bool modifying = isModifying(order);
            bool fullFill = order.leavesQty == 0;

            if (modifying) {
                //CME and ICE are doing the same thing conceptually, it's just with ICE we can't have a chain with more than 1 level
                const auto foreachNodeOnPartialFill = [filledQty, &order](hf::qty_t currentQty, Level* currentLevel, hf::qty_t priorQty, Level* priorLevel, bool isPriorLevelRestingLevel, bool isLastLevel) ALWAYS_INLINE {

                    if(priorLevel != currentLevel) {
                        priorLevel->_flight += filledQty;
                        priorLevel->_flightout -= filledQty;
                        currentLevel->_flight -= filledQty;
                    }
                    else {
                        //
                        // Intentionally Blank
                        //
                    }

                    //There is an implicit node at the end if we are cancelling, at the same price level
                    if(isLastLevel && order.is_cancelling) {
                        currentLevel->_flight += filledQty;
                    }
                };

                const auto foreachNodeOnFullFill = [filledQty, &order](hf::qty_t currentQty, Level* currentLevel, hf::qty_t priorQty, Level* priorLevel, bool isPriorLevelRestingLevel, bool isLastLevel) ALWAYS_INLINE {
                    //Since we want to do nothing on Modify Rejects, we need to undo the entire modify chain here

                    if(priorLevel != currentLevel) {
                        hf::qty_t priorFilledQty = order.executedQty - filledQty;
                        //BOOST_ASSERT(priorFilledQty >= 0); //Can fail on overfill in chain
                        hf::qty_t priorWorkingQty = priorQty - priorFilledQty;
                        //BOOST_ASSERT(priorWorkingQty > 0); //Can fail on overfill in chain
                        hf::qty_t newWorkingQty = currentQty - priorFilledQty;
                        //BOOST_ASSERT(newWorkingQty > 0); //Can fail on overfill in chain

                        priorLevel->_flight += priorWorkingQty;
                        priorLevel->_flightout -= priorWorkingQty;
                        currentLevel->_flight -= newWorkingQty;

                        currentLevel->notifyIfLevelFree();
                    }
                    else {
                        hf::qty_t delta = currentQty - priorQty;
                        currentLevel->_flight -= delta;
                    }

                    //There is an implicit node at the end if we are cancelling, at the same price level
                    if(isLastLevel && order.is_cancelling) {
                        hf::qty_t priorFilledQty = order.executedQty - filledQty;
                        currentLevel->_flight += currentQty - priorFilledQty;
                    }
                };

                if (_om->_venue == hf::VENUE::CME) {
                    ModifyTracker* tracker = getModifyTracker(order);
                    if(fullFill) {
                        tracker->performOnEachNode(std::move(foreachNodeOnFullFill));
                    }
                    else {
                        tracker->performOnEachNode(std::move(foreachNodeOnPartialFill));
                    }
                }
                else if (_om->_venue == hf::VENUE::ICE) {
                    Level* currentLevel = getTargetLevel(order);
                    Level* priorLevel = this;
                    const hf::qty_t& current_qty = GetQuantity(order, QUANTITY_TO_MODIFY);
                    const hf::qty_t& prior_qty = GetQuantity(order, PRIOR_QUANTITY);
                    bool isLastLevel = true;
                    bool isPriorLevelRestingLevel = true;

                    if(fullFill) {
                        foreachNodeOnFullFill(current_qty, currentLevel, prior_qty, priorLevel, isPriorLevelRestingLevel, isLastLevel);
                    }
                    else {
                        foreachNodeOnPartialFill(current_qty, currentLevel, prior_qty, priorLevel, isPriorLevelRestingLevel, isLastLevel);
                    }
                }
                else {
                    throw std::runtime_error("unknown venue");
                }
                //We don't want to send new orders until the Unsoc. cancel just in case _target changes
            }
        }

        void handleModifyOnNewOrderReject(om::order_t& order, hf::qty_t inFlightQty) {
            //This is going to look very similar to Modify Reject + Cancel
            BOOST_ASSERT(_om->_venue == hf::VENUE::ICE);
            Level* targetLevel = getTargetLevel(order);
            hf::qty_t restingQuantity = getRestingQty(order);
            if(targetLevel == this) {
                hf::qty_t diff = getTargetQuantity(order) - restingQuantity;
                _flight -= diff;
            }
            else {
                BOOST_ASSERT(!order.executedQty);
                _flight += restingQuantity;
                _flightout -= restingQuantity;
                targetLevel->_flight -= getTargetQuantity(order);
                targetLevel->notifyIfLevelFree();
            }
        }

        void handleCancelOnNewOrderReject(om::order_t& order, hf::qty_t cancelledQty) {
            BOOST_ASSERT(_om->_venue == hf::VENUE::ICE);
            _flight += cancelledQty;
        }

        //If we need to undo whatever is in the front of the chain, probably due to a modify reject.  Different enough from cancelWorkingInChain
        //because of how we have to handle _flight and _acknowledged quantities that we don't want to overload it
        void undoModifyInFrontChain(om::order_t& order) {

            //See comments in Modify Reject method, I'm not even sure how we get here for CME
            if (BOOST_LIKELY(_om->_venue == hf::VENUE::ICE)) {
                //We can take advantage of the fact we can only have 1 Modify in flight for ICE

                Level *priorLevel = this;
                Level *currentLevel = getTargetLevel(order);
                const hf::qty_t& priorQty = GetQuantity(order, PRIOR_QUANTITY);
                const hf::qty_t& currentQty = GetQuantity(order, QUANTITY_TO_MODIFY);
                if(priorLevel != currentLevel) {
                    //Undo the previous modify at this level
                    hf::qty_t fillAdjustmentQty = priorQty - order.executedQty;
                    priorLevel->_flightout -= fillAdjustmentQty;
                    priorLevel->_flight += fillAdjustmentQty;

                    //Undo the target level
                    hf::qty_t priorUnfilledQty = currentQty - order.executedQty;
                    currentLevel->_flight -= priorUnfilledQty;
                    currentLevel->notifyIfLevelFree();

                    //Move order back to correct level
                    auto iter = std::find(std::begin(currentLevel->_orders), std::end(currentLevel->_orders), &order);
                    BOOST_ASSERT(iter != std::end(currentLevel->_orders));
                    currentLevel->_orders.erase(iter);
                    priorLevel->_orders.emplace_back(&order);

                    //We're going to cancel the order.  We can't deal with live levels not in the stack
                    priorLevel->_flight -= fillAdjustmentQty;
                    //TODO: If this gets rejected, probably due to halt, it will get lost
                    _om->cancel(&order);
                }
                else {
                    _flight -= currentQty - priorQty;
                }
                SetQuantity(order, reinterpret_cast<int64_t>(this), LEVEL_POINTER);
                SetQuantity(order, reinterpret_cast<int64_t>(this), PRIOR_LEVEL);
                SetQuantity(order, priorQty, PRIOR_QUANTITY);
                SetQuantity(order, priorQty, QUANTITY_TO_MODIFY);
            }
            else {
                //For CME, we could have modifies and/or cancels behind this modify
                //The way we're going to handle this is that we're goign to undo this modify, then
                //duplicate the prior entry in the modify tracker to make it look like a modify
                //happened to the same price and quantity.



            }


//
//
//            const auto foreachNode = [&order, this](hf::qty_t currentQty, Level* currentLevel, hf::qty_t priorQty, Level* priorLevel,
//                                                    bool isPriorLevelRestingLevel, bool isLastLevel) ALWAYS_INLINE {
//                hf::qty_t priorLeavesQty = priorQty - order.executedQty;
//                if(BOOST_UNLIKELY(priorLeavesQty < 0)) {
//                    LOG(0, "OM {C[i1]}. Overfill. Id={i2} current.qty={i3} executed.qty={i4}", getContractName(), order.oid, currentQty, order.executedQty);
//                }
//
//                if(priorLevel != currentLevel) {
//                    hf::qty_t fillAdjustmentQty = priorQty - order.executedQty;
//                    priorLevel->_flightout -= fillAdjustmentQty;
//                    priorLevel->_flight += fillAdjustmentQty;
//
//                    hf::qty_t priorUnfilledQty = currentQty - order.executedQty;
//                    currentLevel->_flight -= priorUnfilledQty;
//                    currentLevel->notifyIfLevelFree();
//                }
//
//
//                if(isPriorLevelRestingLevel) {
//                    hf::qty_t priorLeavesQty = priorQty - order.executedQty;
//                    if(priorLeavesQty < 0) {
//                        LOG(0, "OM {C[i1]}. Overfill. Id={i2} current.qty={i3} executed.qty={i4}", getContractName(), order.oid, currentQty, order.executedQty);
//                    }
//                    //Half of a moveQuantity(...), since the other half is done in the code below
//                    priorLevel->_acknowledged -= priorLeavesQty;
//                }
//
//                if(priorLevel != currentLevel) {
//                    //Note fillAdjustmentQty is negative for overfill
//                    hf::qty_t fillAdjustmentQty = priorQty - order.executedQty;
//                    priorLevel->_flightout -= fillAdjustmentQty;
//                    priorLevel->_flight += fillAdjustmentQty;
//
//                    hf::qty_t priorUnfilledQty = currentQty - order.executedQty;
//                    currentLevel->_flight -= priorUnfilledQty;
//                    currentLevel->notifyIfLevelFree();
//                }
//                else {
//                    currentLevel->_flight -= currentQty - priorQty;
//                }
//
//                //Note we can only get here if the order is being modified, i.e. there is an implicit cancel ack in the back of the chain
//                if(order.is_cancelling & isLastLevel) {
//                    currentLevel->_flight += currentQty - order.executedQty;
//                }
//
//            };
//
//            if(_om->_venue == hf::VENUE::CME) {
//                auto* modifyTracker = getModifyTracker(order);
//                modifyTracker->template performOnEachNode<decltype(foreachNode), true>(std::move(foreachNode));
//            }
//            else if (_om->_venue == hf::VENUE::ICE) {
//                Level *priorLevel = this;
//                Level *currentLevel = getTargetLevel(order);
//                bool isLastLevel = true;
//                bool isPriorLevelRestingLevel = true;
//                const hf::qty_t& prior_qty = GetQuantity(order, PRIOR_QUANTITY);
//                const hf::qty_t& current_qty = GetQuantity(order, QUANTITY_TO_MODIFY);
//                foreachNode(current_qty, currentLevel, prior_qty, priorLevel, isPriorLevelRestingLevel, isLastLevel);
//            }
        }

        //We can get here on an Order Reject or a Modify Reject.  Order Reject is a terminal state, but Modify Rejects only effect the first node.
        void cancelWorkingInChain(om::order_t& order) {

            const auto foreachNode = [&order, this](hf::qty_t currentQty, Level* currentLevel, hf::qty_t priorQty,
                                                    Level* priorLevel,
                                                    bool isPriorLevelRestingLevel, bool isLastLevel) ALWAYS_INLINE {

                if (isPriorLevelRestingLevel) {
                    hf::qty_t priorLeavesQty = priorQty - order.executedQty;
                    if (priorLeavesQty < 0) {
                        LOG(0, "OM {C[i1]}. Overfill. Id={i2} current.qty={i3} executed.qty={i4}",
                            getContractName(), order.oid, currentQty, order.executedQty);
                    }
                    //Half of a moveQuantity(...), since the other half is done in the code below
                    priorLevel->_acknowledged -= priorLeavesQty;
                }

                if(priorLevel != currentLevel) {
                    //Note fillAdjustmentQty is negative for overfill
                    hf::qty_t fillAdjustmentQty = priorQty - order.executedQty;
                    priorLevel->_flightout -= fillAdjustmentQty;
                    priorLevel->_flight += fillAdjustmentQty;

                    hf::qty_t priorUnfilledQty = currentQty - order.executedQty;
                    currentLevel->_flight -= priorUnfilledQty;
                    currentLevel->notifyIfLevelFree();
                }
                else {
                    currentLevel->_flight -= currentQty - priorQty;
                }

                //Note we can only get here if the order is being modified, i.e. there is an implicit cancel ack in the back of the chain
                if(order.is_cancelling & isLastLevel) {
                    currentLevel->_flight += currentQty - order.executedQty;
                }
            };

            if (_om->_venue == hf::VENUE::CME) {
                auto* modifyTracker = getModifyTracker(order);
                modifyTracker->performOnEachNode(std::move(foreachNode));
            }
            else if (_om->_venue == hf::VENUE::ICE) {
                Level *priorLevel = this;
                Level *currentLevel = getTargetLevel(order);
                bool isLastLevel = true;
                bool isPriorLevelRestingLevel = true;
                const hf::qty_t& prior_qty = GetQuantity(order, PRIOR_QUANTITY);
                const hf::qty_t& current_qty = GetQuantity(order, QUANTITY_TO_MODIFY);
                foreachNode(current_qty, currentLevel, prior_qty, priorLevel, isPriorLevelRestingLevel, isLastLevel);
            }
        }

        void onOrderCanceled(const om::OMOrderCanceledEvent& event, const om::order_t& order) {
            LOG(0, "OM {C[i1]}. Order Cancelled. Side={i2} Id={i3}", getContractName(), _om->_side, order.oid)
            LOG(0, "OM {C[i1]}. Before. Target={i2}|{i3} Acked={i4} Flight={i5} Trade={i6} PotentialOverfill={i7} NumFlightout={i8}", getContractName(), _target[0], _target[1], _acknowledged, _flight, _traded, _potentialOverfill, _flightout)

            Level *targetLevel = nullptr;
            removeOrder(order);
            //TODO: The only time this can be false is on ICE
            if(GetQuantity(order, IS_ACKED)) {
                //We could have a cancel after a series of modifies, and get a cancel due to modify down, so have to check both
                if (BOOST_LIKELY(order.is_cancelling && !isModifying(order))) {
                    moveQuantity(-order.leavesQty);
                } else {
                    //No chain to worry about
                    if (!isModifying(order)) {
                        moveQuantityUnsolicited(-order.leavesQty);
                    } else {
                        om::order_t &mutableOrder = const_cast<om::order_t &>(order);  //This needs to be mutable because adjustAfterAck(..) needs a mutable order
                        cancelWorkingInChain(mutableOrder);
                    }
                }

                hf::qty_t overfillQty = -(getTargetQuantity(order) - order.executedQty);
                if (overfillQty > 0) {
                    targetLevel = getTargetLevel(order);
                    targetLevel->_potentialOverfill -= overfillQty;
                }
            }
            else {
                BOOST_ASSERT(_om->_venue == hf::VENUE::ICE);
                moveQuantity(order.leavesQty);
            }

            //Note in v1 of the stacker we did not synchrnoze in this method
            notifyIfLevelFree();
            if(targetLevel) {
                targetLevel->notifyIfLevelFree();
            }
            LOG(0, "OM {C[i1]}. After. Target={i2}|{i3} Acked={i4} Flight={i5} Trade={i6} PotentialOverfill={i7} NumFlightout={i8}", getContractName(), _target[0], _target[1], _acknowledged, _flight, _traded, _potentialOverfill, _flightout)
        }

        void onOrderRejected(const om::OMOrderRejectedEvent &event, const om::order_t &order) {
#ifdef IS_DEV
            LOG(0, "OM {C[i1]}. New order rejected. Id={i2} Reason={i3}({i4})", getContractName(), order.oid, event.reasonText, event.reasonCodeExt)
#else
            LOG(0, "OM {C[i1]}. New order rejected. Id={i2} Reason={i3}", getContractName(), order.oid, event.reasonCodeExt)
#endif
            LOG(0, "OM {C[i1]}. Before. Target={i2}|{i3} Acked={i4} Flight={i5} Trade={i6} PotentialOverfill={i8} NumFlightout={i9}", getContractName(), _target[0], _target[1], _acknowledged, _flight, _traded, _potentialOverfill, _flightout)

            hf::qty_t diff = getOriginalOrderQtyForNOAck(order);

            _flight -= diff;

            //Cannot happen on CME
            if (_om->_venue == hf::VENUE::ICE) {
                BOOST_ASSERT(order.is_modifying == order.is_cancelling ? !order.is_modifying : true);
                if (order.is_modifying) {
                    om::order_t &mutableOrder = const_cast<om::order_t &>(order);  //This needs to be mutable because adjustAfterAck(..) needs a mutable order
                    handleModifyOnNewOrderReject(mutableOrder, diff);
                }
                else if (order.is_cancelling) {
                    om::order_t mutableOrder = const_cast<om::order_t &>(order);
                    handleCancelOnNewOrderReject(mutableOrder, diff);
                }
            }

            removeOrder(order);

            LOG(0, "OM {C[i1]}. After. Target={i2}|{i3} Acked={i4} Flight={i5} Trade={i6} PotentialOverfill={i7} NumFlightout={i8}", getContractName(), _target[0], _target[1], _acknowledged, _flight, _traded, _potentialOverfill, _flightout)

            notifyIfLevelFree();
        }

        void onOrderReplaceRejected(const om::OMOrderModifyRejectedEvent& event, const om::order_t& order, Level* nextLevel) {
            LOG(0, "OM {C[i1]}. Replace order rejected. Id={i2} Reason={i3}", getContractName(), order.oid, event.reasonCodeExt)
            LOG(0, "OM {C[i1]}. Before. Target={i2}|{i3} Acked={i4} Flight={i5} Trade={i6} PotentialOverfill={i7} NumFlightout={i8}", getContractName(), _target[0], _target[1], _acknowledged, _flight, _traded, _potentialOverfill, _flightout)
            om::order_t &mutableOrder = const_cast<om::order_t &>(order);
            bool rejectedDueToFill = WasRejectedDueToBeingFilled(event.reasonCodeExt, event.reasonText, _om->_venue);
            if (BOOST_UNLIKELY(!rejectedDueToFill)) {
                if (BOOST_LIKELY(hasOrder(mutableOrder))) {
                    undoModifyInFrontChain(mutableOrder);
                    mutableOrder.is_modifying = isStillModifyingAfterModifyAck(order);
                    if (BOOST_LIKELY((_om->_venue == hf::VENUE::ICE) && IsICESMPModifyReject(event.reasonText))) {
                        if (GetQuantity(order, IS_HEDGE) && _om->_hedge && _om->_hedge == getTargetLevel(order)) {
                            _om->_hedge->synchronizeOrders(true);
                            _om->handleSMPModifyReject(_price, true);
                        }
                        else {
                            _om->handleSMPModifyReject(_price, false);
                        }
                    } else if (BOOST_LIKELY(!IsRejectDueToMarketClosed(order.venue, event.reasonCodeExt))) {
                        _om->disableMarketMakerAfterStackerRejects("OM::onOrderReplaceRejected");
                    }
                }
            } else {
                if (BOOST_UNLIKELY(mutableOrder.is_active)) {
                    //How the heck do we get here?  Rejected due to fill, but is active !?  I think this was bad logic in old stacker.
                    //We're going to treat it as best as we can, similar to an ice SMP reject that leaves the order working.
                    undoModifyInFrontChain(mutableOrder);
                    mutableOrder.is_modifying = isStillModifyingAfterModifyAck(order);
                    _om->disableMarketMakerAfterStackerRejects("OM::onOrderReplaceRejected (rejectedDueToFill && is_active");
                } else {
                    mutableOrder.is_modifying = false;
                }
            }
            LOG(0, "OM {C[i1]}. After. Target={i2}|{i3} Acked={i4} Flight={i5} Trade={i6} PotentialOverfill={i7} NumFlightout={i8}", getContractName(), _target[0], _target[1], _acknowledged, _flight, _traded, _potentialOverfill, _flightout)
        }

        //All the state cleanup for Cancel-Due-To-Modify-Down should be handled on the ModifyAck
        void onOrderCancelRejected(const om::OMOrderCancelRejectedEvent &event, const om::order_t &order) {
            LOG(0, "OM {C[i1]}. Cancel order rejected. Id={i2} Reason={i3}", getContractName(), order.oid, event.reasonCodeExt)
            LOG(0, "OM {C[i1]}. Before. Target={i2}|{i3} Acked={i4} Flight={i5} Trade={i6} PotentialOverfill={i7} NumFlightout={i8}", getContractName(), _target[0], _target[1], _acknowledged, _flight, _traded, _potentialOverfill, _flightout)

            BOOST_ASSERT_MSG(_om->_venue != hf::VENUE::EUREX, "Rhone will throttle cancels on Eurex, must handle it in the onCriticalError path below as a non-pathological case");

            bool rejectedDueToFill = WasRejectedDueToBeingFilled(event.reasonCodeExt, event.reasonText, _om->_venue);
            if (!rejectedDueToFill) {
                if (hasOrder(order) && !IsRejectDueToMarketClosed(order.venue, event.reasonCodeExt)) {
                    _om->_ime->disableRVDisableMMExternalQuote("Cancel reject not due to fill and has order.");
                }
            }
            om::order_t& mutableOrder = const_cast<om::order_t &>(order);
            mutableOrder.is_cancelling = false;
            LOG(0, "OM {C[i1]}. After. Target={i2}|{i3} Acked={i4} Flight={i5} Trade={i6} PotentialOverfill={i7} NumFlightout={i8}", getContractName(), _target[0], _target[1], _acknowledged, _flight, _traded, _potentialOverfill, _flightout)
        }

        bool isLevelFree() {
            //todo: why is this called so much?
            //todo: remove logging from final build
            //LOG(0, "OM {C[i1]} isLevelFree Level={d1} IsFree={i2}", getContractName(), ToDollars(_price), ((_target == 0) & ((_acknowledged - _traded) == 0) & (_flight == 0) & (_flightout == 0) & _orders.empty()));
            //LOG(0, "OM {C[i1]} isLevelFree Level={d1} _target={i2} _ack={i3} _traded={i4} _flight={i5} _flightout={i6} _oc={i7}",
            //        getContractName(), ToDollars(_price), _target, _acknowledged, _traded, _flight, _flightout, _orders.size());
            //Note technically _flightout == 0 might not be necessary, if we're ok with in-flight fills on free levels

            //The level is free if there is neither stacker target nor hedge target and there is no working order or
            //stacker target is 0 and total working order qty equals to hedge target.
            return (_target[STACKER_ORDER] == 0) & ((_acknowledged - _traded) == _target[HEDGER_ORDER]) & ((_flight + _flightout) == 0);  // no hedge target and order on the level
        }

        void notifyIfLevelFree() {
            if (isLevelFree()) {
                notifyLevelFree();
            }
        }

        void notifyLevelFree() {
            LOG(0, "OM {C[i1]}. Side={i2} Notifying level free. Price={d1}", getContractName(), _om->_side, _dollar)
            _om->notifyLevelFree(_price);
        }

        void onLevelFree() {
            _usedByOtherStack = false;
            LOG(0, "OM {C[i1]}. Side={i2} Level freed. Price={d1}", getContractName(), _om->_side, _dollar)
            if (target() > 0) {
                LOG(0, "OM {C[i1]}. Before. Price={d1} Target={i2}|{i3} Acked={i4} Flight={i5} Trade={i6} PotentialOverfill={i7} NumFlightout={i8}", getContractName(), _dollar, _target[0], _target[1], _acknowledged, _flight, _traded, _potentialOverfill, _flightout)
                synchronizeOrders(_target[HEDGER_ORDER] > 0);
                if (_target[STACKER_ORDER] > 0) {
                    notifyLevelUsed();
                }
                LOG(0, "OM {C[i1]}. After. Price={d1} Target={i2}|{i3} Acked={i4} Flight={i5} Trade={i6} PotentialOverfill={i7} NumFlightout={i8}", getContractName(), _dollar, _target[0], _target[1], _acknowledged, _flight, _traded, _potentialOverfill, _flightout)
            }
        }

        void onLevelUsed() {
            BOOST_ASSERT(isLevelFree() || (_target == 0 && (_flightout || _flight < 0))); //We could have a cancel or modifyPx in flight
            _usedByOtherStack = true;
            LOG(0, "OM {C[i1]}. Side={i2} Level used. Price={d1}", getContractName(), _om->_side, _dollar)
        }

        void removeOrder(const om::order_t& order) {

            //Note it's NOT SAFE to remove the modify tracker here, since we remove the order on a full fill,
            //but we need the modify tracker to handle a potention MR or CR

            Level* level = getTargetLevel(order);

            for (auto it = level->_orders.begin(); it != level->_orders.end(); ++it) {
                if ((*it)->oid == order.oid) {
                    level->_orders.erase(it);
                    break;
                }
            }
        }

        bool hasOrder(const om::order_t &order) {
            Level* level = getTargetLevel(order);

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


        const hf::price_t _price;
        const double _dollar;

        hf::qty_t _target[2]{0, 0};    // 0 for quote; 1 for hedge
        hf::qty_t _acknowledged = 0;
        hf::qty_t _flight = 0;
        hf::qty_t _traded = 0;
        hf::qty_t _potentialOverfill = 0;  //This is *NOT THE OVERFILL AT THIS LEVEL*.  This is used to offset _flight/_acknowledged when an overfill is going to cause issues at the end of a modify chain.
        hf::qty_t _flightout = 0;

        bool _usedByOtherStack = false;

        std::vector<om::order_t*> _orders;
        OrderManager* _om;

        Level* before = nullptr;
        Level* after = nullptr;
    };

    template<typename ObjectType, int size>
    class ContiguousAllocator
    {
    public:

        ContiguousAllocator() : _buffer(static_cast<ObjectType*>(malloc(sizeof(ObjectType[size])))) {}

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
        LevelStealer(Level* begin, const Level* end, const OrderManager<IME_T>* stacker) : _begin(begin), _end(end) {
            //Checks if _begin and _end are in the correct order
            BOOST_ASSERT(([this, stacker]() -> bool {
                if((_begin == nullptr) && (_end == nullptr)) { return true; } //Explicitly checked
                if(_begin == _end) { return true; } //C++ iter convention
                if(!stacker) { return false; } //If _begin/_end have values, then stacker really should too
                return stacker->_side == BID ? _begin->_price >= _end->_price : _begin->_price <= _end->_price;
            })());

            if(stacker) {
                if(BOOST_UNLIKELY(stacker->_overfillProtection.blocked())) {
                    LOG(0, "OM {C[i1]}. Side={i2} Stealer _canIncreaseQuantity=false on ctor due to blocked state. count={i3}", stacker->getContractName(), stacker->_side, stacker->_overfillProtection.getMarkedOrderCountForLogging());
                    _canIncreaseQuantity = false;
                    return;
                }
            }

            if(BOOST_LIKELY(_begin != nullptr)) {
                //Because we're eventually going to cancel the steal-from levels, any cancel that would fail
                //is a decrement that would fail.
                for (; begin != end; begin = begin->after) {
                    for(auto orderIter = begin->_orders.begin(); orderIter != begin->_orders.end(); ++orderIter) {
                        if(BOOST_UNLIKELY(begin->inflightStatePreventsDecrement(**orderIter))) {
                            _canIncreaseQuantity = false;
                            LOG(0, "OM {C[i1]}. Side={i2} Stealer _canIncreaseQuantity=false on ctor due to specific order. oid={i3}", stacker->getContractName(), stacker->_side, (*orderIter)->oid);
                            return;
                        }
                    }
                }
            }
        }

        LevelStealer(const OrderManager<IME_T>* stacker) : LevelStealer(nullptr, nullptr, stacker) {
        }

        enum class Result : uint8_t {
            MODIFIED = 0,
            CAN_NOT_MODIFY = 1,
            NOTHING_DONE = 2,
        };

        Result tryModifyInsteadOfNew(Level* level, hf::qty_t change, bool isHedge) {

            BOOST_ASSERT_MSG(change > 0, "Expected positive quantity");

            if(BOOST_UNLIKELY(change <= 0)) {
                return Result::NOTHING_DONE;
            }

            if (!_begin) {
                return Result::NOTHING_DONE;
            }

            if(!_canIncreaseQuantity) {
                LOG(0, "OM {C[i1]}. Price={d1} cannot steal orders due to overfill protection. IsHedge={i2}", level->getContractName(), level->_dollar, isHedge)
                return Result::CAN_NOT_MODIFY;
            }

            while (_begin != _end) {
                LOG(0, "OM {C[i1]}. Price={d1} try to steal orders from Price={d2} IsHedge={i2}", level->getContractName(), level->_dollar, _begin->_dollar, isHedge)
                hf::qty_t totalOrderQty = _begin->getTotalOrdersQuanity();
                for (auto iter = _begin->_orders.rbegin(); iter != _begin->_orders.rend(); ++iter) {
                    om::order_t& order = **iter;
                    if (BOOST_LIKELY(order.is_active)) {  //Theoretically redundant with "canModifyOrder" check below
                        Level &from = *_begin;
                        BOOST_ASSERT(from._price != level->_price); //We shouldn't be stealing orders from a level we're pricing to
                        if(from.canModifyOrCancelOrder(order)) {
                            hf::qty_t leavesQty = from.getLeavesQty(order);
                            bool hasEnoughSizeLeft = (totalOrderQty - leavesQty) >= from._target[1 - isHedge];
                            if (from._price != level->_price) {
                                BOOST_ASSERT(leavesQty > 0); //Theoretically canModifyOrCancelOrder(...) should catch this
                                if (leavesQty > 0 && hasEnoughSizeLeft) {
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
                                    from._om->modify(&order, level->_price, newTargetQty, isHedge);
                                    return Result::MODIFIED;
                                }
                            }
                            else if (hasEnoughSizeLeft) {
                                //Order, if stolen, would have returned true above
                                from._flight -= leavesQty;
                                from._om->cancel(&order);
                                //LOG(0, "OM {C[i1]}. After decrease. Target={i2} Acked={i3} Flight={i4} Trade={i5} OrderLeaves={i6}", from.getContractName(), _target, _acknowledged, _flight, _traded, leavesQty)
                            }
                        }
                    }
                }
                //Simplest possible (slow?) way to do this
                _begin->updateOnlyOneLevel(0, isHedge ? TargetType::Hedge : TargetType::Stack);
                _begin = _begin->after;
            }

            _begin = nullptr;
            return Result::NOTHING_DONE;
        }

        void cancelAll(TargetType targetType) {
            //Simplest possible (slow?) impl
            if(_begin) {
                while(_begin && (_begin != _end)) {
                    _begin->updateOnlyOneLevel(0, targetType);
                    _begin = _begin->after;
                }
            }
        }

    private:
        Level* _begin;
        const Level* _end;
        bool _canIncreaseQuantity = true;
    };

    class OverfillProtector {
        public:
            hf::price_t _lastPrice;
            hf::qty_t _lastSize;

            //returns if increment was necessary
            bool incrementIfNecessary(const om::order_t& order, OrderManager<IME_T>* om) {
                bool alreadyCounted = TestAndSetBit(order, STACKER_ORDER_FLAGS, SOF_DECREMENT_FAILED);
                if(!alreadyCounted) {
                    increment();
                    LOG(0, "OM {C[i1]}. Side={i2} Overfill protection incremented new={i3}", om->getContractName(), om->_side, getMarkedOrderCountForLogging());
                }
                return alreadyCounted;
            }
            void decrement(OrderManager<IME_T>* om) {
                BOOST_ASSERT(_markedOrders > 0);
                --_markedOrders;
                LOG(0, "OM {C[i1]}. Side={i2} Overfill protection decremented new={i3}", om->getContractName(), om->_side, getMarkedOrderCountForLogging());
            }
            bool blocked() const { BOOST_ASSERT(_markedOrders >= 0); return _markedOrders > 0; }  //<= to try not to block if in bad state
            int getMarkedOrderCountForLogging() const { return _markedOrders; }

        private:
        void increment() { BOOST_ASSERT(_markedOrders >= 0); ++_markedOrders; }
        int _markedOrders = 0;
    };

    void initialize(const ConfigContract& contract, uint32_t side, const StackerConfig& stackerConfig, IME_T* ime, bool iceCancelResting) {
        _internalId = contract._internalId;
        _side = side;
        _tickSize = contract._tickSize;
        _quoteSize = stackerConfig._quoteSize;
        _nLevels = stackerConfig._numberOfLevels;
        _ime = ime;
        _venue = contract._venue;
        _maxSlackSize = stackerConfig._stackSlack;
        _orderType = stackerConfig._useGtc ? om::ORDER_TYPE::GTC : om::ORDER_TYPE::LMT;
        _iceCancelResting = iceCancelResting;
        LOG(0, "OM {C[i1]}. Side={i2} GTC={i3}", getContractName(), 0, (int) stackerConfig._useGtc)
    }

    template<typename T>
    bool testOrderAndDecrementCounter(const om::order_t& order) {
        static_assert(std::is_same<T, om::OMOrderAcceptedEvent>::value || std::is_same<T, om::OMOrderRejectedEvent>::value
                || std::is_same<T, om::OMOrderModifiedEvent>::value || std::is_same<T, om::OMOrderModifyRejectedEvent>::value
                || std::is_same<T, om::OMExecutionEvent>::value || std::is_same<T, om::OMOrderCanceledEvent>::value, "Bad Type");
        BOOST_ASSERT(_venue == hf::VENUE::CME || _venue == hf::VENUE::ICE);
        if(_venue == hf::VENUE::CME) {
            if(std::is_same<T, om::OMOrderAcceptedEvent>::value || std::is_same<T, om::OMOrderRejectedEvent>::value) {
                bool unsetOccurred = TestAndUnsetBit(order, STACKER_ORDER_FLAGS, SOF_DECREMENT_FAILED);
                if(unsetOccurred) {
                    _overfillProtection.decrement(this);
                }
                return unsetOccurred;
            }
        }
        else if(_venue == hf::VENUE::ICE) {
            if(std::is_same<T, om::OMOrderModifiedEvent>::value || std::is_same<T, om::OMOrderModifyRejectedEvent>::value || std::is_same<T, om::OMExecutionEvent>::value
                || std::is_same<T, om::OMOrderCanceledEvent>::value) {
                bool unsetOccurred = TestAndUnsetBit(order, STACKER_ORDER_FLAGS, SOF_DECREMENT_FAILED);
                if(unsetOccurred) {
                    _overfillProtection.decrement(this);
                }
                return unsetOccurred;
            }
        }
        return false;
    }

    template<uint32_t Side>
    void quote(hf::price_t price, hf::qty_t size, bool goThruAllLevels) {
        auto dollarPrice = ToDollars(price);
        LOG(0, "OM {C[i1]}. Set Quote target Side={i2} Price={d1} Size={i3}", getContractName(), Side, dollarPrice, size)
        if (BOOST_UNLIKELY(size < 0)) {
            //This should theoretically never happen.  But in practice it does.
            size = 0;
        }
        _overfillProtection._lastPrice = price;
        _overfillProtection._lastSize = size;

        if (BOOST_UNLIKELY(_topLevel == nullptr)) {
            createFirstStack<Side>(price, size, false);
        } else if (BOOST_UNLIKELY(_bottomLevel == nullptr)) {
            // hedge was called first, _bottomLevel has not been set yet
            _topLevel = findLevel<Side>(price);
            if (_topLevel == nullptr) {
                handleInvalidTickSize(price);
                return;
            }
            auto& gtc = getGTCs(_topLevel->_price);
            _topLevel->updateOnlyOneLevel(size, gtc, false);
            Level* level = _topLevel->after;
            for (uint32_t i = 0; i < _nLevels; ++i) {
                auto& gtc = getGTCs(_topLevel->_price);
                level->updateOnlyOneLevel(_quoteSize, gtc, false);
                level = level->after;
            }
            _bottomLevel = level->before;
            for (uint32_t i = 0; i < _maxSlackSize; ++i) {
                auto &gtc = getGTCs(level->_price);
                bool hasGtc = !gtc.empty();
                if (hasGtc) {
                    level->updateOnlyOneLevel(_quoteSize, gtc, false);
                    _bottomLevel = level;
                }
                level = level->after;
            }
        } else if (_topLevel->_price == price) {
            const bool updated = _topLevel->updateOnlyOneLevel(size, TargetType::Quote);
            if (updated | goThruAllLevels) {
                if (_nLevels > 0) {
                    Level* level = _topLevel->after;
                    while ((level->updateOnlyOneLevel(_quoteSize, TargetType::Stack) | goThruAllLevels) & (level != _bottomLevel)) {
                        level = level->after;
                    }
                }
            }
        } else if (IsPriceMovingAgainst<Side>(price, _topLevel->_price)) {

            BOOST_ASSERT(_topLevel->_price != price);
            Level* lastLevel = _topLevel;
            while(lastLevel && IsBetterPrice<Side>(lastLevel->_price, price)) {
                lastLevel = lastLevel->after;
            }

            if (lastLevel == nullptr) {
                handleInvalidTickSize(price);
                return;
            }

            LevelStealer stealer{_topLevel, lastLevel, this}; //C++ iter conventions

            _topLevel = lastLevel;
            _topLevel->updateOnlyOneLevel(size, TargetType::Quote, &stealer);

            //Move to furthers level of stack, then walk back, to try to encourage stealing
            Level* stackerLevel = _topLevel;
            for(uint32_t stackLevel = 0; stackLevel < _nLevels; ++stackLevel) {
                Level* nextLevel = stackerLevel->after;
                if (BOOST_UNLIKELY(nextLevel == nullptr)) {
                    nextLevel = new Level(this, GetPriorPrice<Side>(stackerLevel->_price, _tickSize));
                    nextLevel->before = stackerLevel;
                    stackerLevel->after = nextLevel;
                }
                stackerLevel = nextLevel;
            }

            if (EqualOrBelow<Side>(stackerLevel->_price, _bottomLevel->_price)) {
                _bottomLevel = stackerLevel;
            }

            for(uint32_t stackLevel = 0; stackLevel < _nLevels; ++stackLevel) {
                stackerLevel->updateOnlyOneLevel(_quoteSize, TargetType::Stack, &stealer);
                stackerLevel = stackerLevel->before;
            }

            stealer.cancelAll(TargetType::Stack);
        } else {
            Level* lastLevel = _topLevel;
            while (lastLevel && IsBetterPrice<Side>(price, lastLevel->_price)) {
                lastLevel = lastLevel->before;
            }

            if (lastLevel == nullptr) {
                handleInvalidTickSize(price);
                return;
            }

            if (BOOST_UNLIKELY(lastLevel->_price != price)) {
                hf::price_t levelPrice = GetNextPrice<Side>(lastLevel->_price, _tickSize);
                for (; EqualOrBelow<Side>(levelPrice, price);) {
                    Level* level = new Level(this, levelPrice);
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
                    return {beginLevel, _bottomLevel->after, this}; //C++ iter convention
                }
                return {this};
            }();

            _topLevel->updateOnlyOneLevel(size, TargetType::Quote, &stealer);

            LOG(0, "OM {C[i1]}. Price={d1} Side={i2}. Will update {i3} levels.", getContractName(), dollarPrice, _side, _nLevels)

            //Update Stack
            Level* stackerLevel = _topLevel;
            for (uint32_t stackLevel = 0; stackLevel < _nLevels; ++stackLevel) {
                stackerLevel = stackerLevel->after;
                stackerLevel->updateOnlyOneLevel(_quoteSize, TargetType::Stack, &stealer);
            }

            Level* newBottomLevel = stackerLevel;

            //Discover if there is slack
            if (_maxSlackSize > 0) {
                uint32_t numberOfSlackLevels = 0;
                while (true) {
                    Level* nextLevel = stackerLevel->after;
                    if ((numberOfSlackLevels < _maxSlackSize) & (nextLevel->_target[0] == _quoteSize)) {
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

                stealer.cancelAll(TargetType::Stack);
                //Theoretically unneeded, hence BOOST_VERIFY below
                if (stackerLevel != _bottomLevel) {
                    stackerLevel = newBottomLevel->after;
                    while (stackerLevel != _bottomLevel) {
                        BOOST_VERIFY(!stackerLevel->updateOnlyOneLevel(0, TargetType::Stack));
                        stackerLevel = stackerLevel->after;
                    }
                    BOOST_VERIFY(!_bottomLevel->updateOnlyOneLevel(0, TargetType::Stack));
                }
            } else {
                stackerLevel = newBottomLevel->after;

                stealer.cancelAll(TargetType::Stack);
                //Theoretically unneeded, hence BOOST_VERIFY below
                while (stackerLevel && stackerLevel != _bottomLevel) {
                    BOOST_VERIFY(!stackerLevel->updateOnlyOneLevel(0, TargetType::Stack));
                    stackerLevel = stackerLevel->after;
                }
                BOOST_VERIFY(!_bottomLevel->updateOnlyOneLevel(0, TargetType::Stack));
            }
            _bottomLevel = newBottomLevel;
        }
        _inMarket = true;
    }

    template<uint32_t Side>
    void hedge(hf::price_t price, hf::qty_t size) {
        LOG(0, "OM {C[i1]}. Set Hedge target to Price={d1} Size={i2}", getContractName(), ToDollars(price), size)

        if (size != 0) {
            if (BOOST_UNLIKELY(_topLevel == nullptr)) {
                createFirstStack<Side>(price, size, true);
                _hedge = _topLevel;
                return;
            }

            if (_hedge == nullptr) {
                _hedge = findLevel<Side>(price);
                if (_hedge == nullptr) {
                    handleInvalidTickSize(price);
                    return;
                }
            }

            if (_hedge->_price == price) {
                _hedge->updateOnlyOneLevel(size, TargetType::Hedge);
            } else {
                LevelStealer stealer{_hedge, _hedge->after, this}; //C++ iter conventions
                Level* newHedge = findLevel<Side>(price);
                if (newHedge == nullptr) {
                    handleInvalidTickSize(price);
                    return;
                }
                _hedge = newHedge;
                _hedge->updateOnlyOneLevel(size, TargetType::Hedge, &stealer);
                stealer.cancelAll(TargetType::Hedge);
            }
        } else if (BOOST_LIKELY(_hedge != nullptr)) {
            _hedgeRetryCount = 0;
            _hedge->updateOnlyOneLevel(size, TargetType::Hedge);
            _hedge = nullptr;
        }
    }

    template<uint32_t Side>
    Level* findLevel(hf::price_t price) {
        BOOST_ASSERT(_topLevel != nullptr);
        Level *level = _topLevel;
        if (IsBetter(price, level->_price, _side)) {
            while (IsBetter(price, level->_price, _side)) {
                if (level->before == nullptr) {
                    level->before = new Level(this, GetNextPrice<Side>(level->_price, _tickSize));
                    level->before->after = level;
                }
                level = level->before;
            }
        } else {
            while (IsBetter(level->_price, price, _side)) {
                if (level->after == nullptr) {
                    level->after = new Level(this, GetPriorPrice<Side>(level->_price, _tickSize));
                    level->after->before = level;
                }
                level = level->after;
            }
        }
        return level->_price == price ? level : nullptr;
    }

    void quoteAfterPotentialOverfillConditionCleared() {
        if(BOOST_LIKELY(_inMarket)) {
            if (_side == BID) {
                quote<BID>(_overfillProtection._lastPrice, _overfillProtection._lastSize, true);
            } else {
                quote<ASK>(_overfillProtection._lastPrice, _overfillProtection._lastSize, true);
            }
        }
    }

    template<uint32_t Side>
    void createFirstStack(hf::price_t price, hf::qty_t size, bool isHedge) {
        if (_stackerStart == nullptr) {
            const uint32_t totalNumberOfLevel = _nLevels + 2 * NUMBER_OF_PRE_CREATED_LEVELS + 1;
            ContiguousAllocator<Level, 100 + 2 * NUMBER_OF_PRE_CREATED_LEVELS + 1> allocator;

            Level* level = allocator.createObject(this, price + GetFirstPriceOffset<Side>(_tickSize));
            _stackerStart = level;

            for (uint32_t levelIndex = 1; levelIndex < totalNumberOfLevel; ++levelIndex) {
                bool setBottomLevel = false;
                Level* nextLevel = allocator.createObject(this, GetPriorPrice<Side>(level->_price, _tickSize));
                level->after = nextLevel;
                nextLevel->before = level;
                if (_usedPrices.count(nextLevel->_price)) {
                    nextLevel->onLevelUsed();
                }
                if (levelIndex < NUMBER_OF_PRE_CREATED_LEVELS) {
                    // nothing just pre-created
                }
                if (levelIndex == NUMBER_OF_PRE_CREATED_LEVELS) {
                    _topLevel = nextLevel;
                    auto& gtc = getGTCs(nextLevel->_price);
                    _topLevel->updateOnlyOneLevel(size, gtc, isHedge);
                    setBottomLevel = !isHedge;
                }

                if (!isHedge) {
                    if (levelIndex > NUMBER_OF_PRE_CREATED_LEVELS &&
                        levelIndex <= NUMBER_OF_PRE_CREATED_LEVELS + _nLevels) {
                        auto& gtc = getGTCs(nextLevel->_price);
                        nextLevel->updateOnlyOneLevel(_quoteSize, gtc, false);
                        setBottomLevel = true;
                    }
                    if (levelIndex > NUMBER_OF_PRE_CREATED_LEVELS + _nLevels &&
                        levelIndex <= NUMBER_OF_PRE_CREATED_LEVELS + _nLevels + _maxSlackSize) {
                        auto &gtc = getGTCs(nextLevel->_price);
                        bool hasGtc = !gtc.empty();
                        if (hasGtc) {
                            nextLevel->updateOnlyOneLevel(_quoteSize, gtc, false);
                        }
                        setBottomLevel = hasGtc;
                    }
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
        LOG(0, "OM {C[i1]}. Cancel order. Id={i2}", getContractName(), order->oid)
        _ime->_executor->cancelOrder(_internalId, order);
    }

    void modify(om::order_t* order, hf::price_t price, hf::qty_t size, bool isHedge) {
        LOG(0, "OM {C[i1]}. Modify order. Id={i2} Price={d1} Size={i3}", getContractName(), order->oid, ToDollars(price), size)
        SetQuantity(*order, isHedge, IS_HEDGE);
        _ime->_executor->modifyOrder(_internalId, order, price, size);
    }

    om::order_t* newOrder(Level* level, hf::price_t price, hf::qty_t size, bool isHedge) {
        om::order_t* order = _ime->_executor->newLimitOrder(_internalId, _side, price, size, _orderType, isHedge);
        LOG(0, "OM {C[i1]}. New order. Id={i2} Price={d1} Size={i3}", getContractName(), order->oid, ToDollars(price), size)
        //TODO: Will segfault if order == nullptr
        order->debug.i[LEVEL_POINTER] = reinterpret_cast<uint64_t>(level);
        //TODO: If templated on _venue this only should be done for ICE.  For CME it's a no-op
        order->debug.i[PRIOR_LEVEL] = reinterpret_cast<uint64_t>(level);
        order->debug.i[IS_HEDGE] = isHedge;
        order->leavesQty = size;
        level->setNewOrderSize(order, size);
        SetQuantity(*order, 0, IS_ACKED);
        return order;
    }

    void onOrderAccepted(const om::OMOrderAcceptedEvent& event, const om::order_t& order) {
        bool requiresRequote = testOrderAndDecrementCounter<om::OMOrderAcceptedEvent>(order);
        Level* level = getLevelForNewAcknowledgement(order);
        level->onOrderAccepted(event, order);
        if(requiresRequote) {
            quoteAfterPotentialOverfillConditionCleared();
        }
    }

    void onOrderReplaced(const om::OMOrderModifiedEvent& event, const om::order_t& order) {
        bool requiresRequote = testOrderAndDecrementCounter<om::OMOrderModifiedEvent>(order);
        Level* level = getLevelForModifyAcknowledgement(order);
        level->onOrderReplaced(event, order, getRestingLevel(order));
        if(requiresRequote) {
            quoteAfterPotentialOverfillConditionCleared();
        }
    }

    void onOrderExecution(const om::OMExecutionEvent &event, const om::order_t &order) {
        bool requiresRequote = (!order.is_active) && testOrderAndDecrementCounter<om::OMExecutionEvent>(order);
        Level* level = getLevelForExchangeAction(order);
        level->onOrderExecution(event, order);
        if(requiresRequote) {
            //We really don't want to do anything here since the IME doesn't update _target until after this method returns
        }
    }

    void onOrderCanceled(const om::OMOrderCanceledEvent& event, const om::order_t& order) {
        testOrderAndDecrementCounter<om::OMOrderCanceledEvent>(order);
        Level* level = getLevelForCancel(order);
        level->onOrderCanceled(event, order);

        if(IsSMPUnsolicitedCancel(order.venue, event.reasonCodeExt, event.reasonText)) {
            hf::price_t price = level->_price;
            if (_hedge && (_hedge == level || _hedge == level->getTargetLevel(order))) {
                if (_hedge->_target[HEDGER_ORDER] > 0 && event.externalGWSequenceNo >  (_hedgeRetrySeqNum + 1)) {
                    ++_hedgeRetryCount;
                    if (_hedgeRetryCount == 20) {
                        _hedgeRetryCount = 0;
                        LOG(0, "OM {C[i1]}. Reached hedge retry limit on unsolicited cancel. Disabling.", getContractName())
                        _ime->_executor->sendErrorMessage(boost::str(
                                boost::format("OM - hedge retry limit on unsolicited cancel. Contract=%1%") %
                                _ime->_name), ErrorType::Critical);
                        _ime->_executor->disableMarketMakerAndRV(_internalId, "Reached hedger retry limit on unsolicited cancel");
                    } else {
                        _hedge->synchronizeOrders(true);
                    }
                }
                _hedgeRetrySeqNum = event.externalGWSequenceNo;
                _ime->handleSMPUnsolicitedCancel(order, _side, price, event.externalGWSequenceNo, true);
            }
            else {
                _ime->handleSMPUnsolicitedCancel(order, _side, price, event.externalGWSequenceNo, false);
            }
        }
    }

    void onOrderRejected(const om::OMOrderRejectedEvent& event, const om::order_t& order) {
        bool requiresRequote = testOrderAndDecrementCounter<om::OMOrderRejectedEvent>(order);
        Level* level = getLevelForNewAcknowledgement(order);
        level->onOrderRejected(event, order);
        if(requiresRequote) {
            //Think carefully about this, we really don't want to requote if the IME is going to do something there.
            //quoteAfterPotentialOverfillConditionCleared();
        }

        if (GetQuantity(order, IS_HEDGE)) {
            // in-flight hedge order rejected
            if (_hedge && _hedge->_target[HEDGER_ORDER] > 0 && level == _hedge) {
                // hedge has not moved to other level or not set to 0 yet
                // immediately put hedge order back in
                ++_hedgeRetryCount;
                if (_hedgeRetryCount == 20) {
                    _hedgeRetryCount = 0;
                    LOG(0, "OM {C[i1]}. Reached hedge retry limit on new order reject. Disabling.", getContractName())
                    _ime->_executor->sendErrorMessage(boost::str(boost::format("OM - hedge retry limit on new order reject. Contract=%1%") % _ime->_name), ErrorType::Critical);
                    _ime->_executor->disableMarketMakerAndRV(_internalId, "Reached hedger retry limit on new order reject");
                }
                else {
                    level->synchronizeOrders(true);
                }
            }
            else {
                // hedge moved to other level or removed. Do nothing
            }
        }
        else {
            std::string codeString = GetGatewayRejectCodeString(event.reasonCodeExt);
            _ime->handleNewOrderReject(_side, order.pr, codeString.empty() ? event.reasonText : codeString);
        }
    }

    void onOrderReplaceRejected(const om::OMOrderModifyRejectedEvent& event, const om::order_t& order) {
        bool requiresRequote = testOrderAndDecrementCounter<om::OMOrderModifyRejectedEvent>(order);
        Level* level = getRestingLevel(order);
        level->onOrderReplaceRejected(event, order, getLevelForModifyAcknowledgement(order));
        if(requiresRequote) {
            //Think carefully about this, we really don't want to requote if the IME is going to do something here.
            //quoteAfterPotentialOverfillConditionCleared();
        }
    }

    void onOrderCancelRejected(const om::OMOrderCancelRejectedEvent &event, const om::order_t &order) {
        Level* level = getLevelForCancel(order);
        level->onOrderCancelRejected(event, order);
    }

    Level* getLevelForNewAcknowledgement(const om::order_t& order) {
        Level* level;
        switch(_venue) {
            case hf::VENUE::CME:
                level = _mtt.getModifyTracker(order)->getInFlightLevel();
                break;
            case hf::VENUE::ICE:
                level = reinterpret_cast<Level*>(GetQuantity(order, PRIOR_LEVEL));
                break;
            default:
                throw std::runtime_error("unrecognized venue");
        }
        return level;
    }

    Level* getLevelForModifyAcknowledgement(const om::order_t& order) {
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
                level = reinterpret_cast<Level*>(GetQuantity(order, PRIOR_LEVEL));
                break;
            default:
                throw std::runtime_error("unrecognized venue");
        }
        return level;
    }

    Level* getLevelForCancel(const om::order_t& order) {
        //A cancel, solicited or unsolicited, resembles an exchange action in that it can't change the price, i.e.
        //it doesn't touch the modify trackers
        return getLevelForExchangeAction(order);
    }

    void sendErrorMessage(const std::string& message) {
        _ime->_executor->sendErrorMessage(message, ErrorType::Critical);
    }

    void onCriticalError(const std::string& message) {
        _ime->_executor->onCriticalError(message);
    }

    void getAllOutOfMarket(bool force = false) {
        getHedgeOutOfMarket(force);
        getQuoteOutOfMarket(force);
        LOG(0, "OM {C[i1]} Side={i2}. Got all out of market.", getContractName(), _side)
    }

    void getQuoteOutOfMarket(bool force = false) {
        if (_inMarket) {
            if (!useGTC() | force) {
                _inMarket = false;
                Level* level = _topLevel;
                while (level && level != _bottomLevel) {
                    level->updateOnlyOneLevel(0, TargetType::Stack);
                    level = level->after;
                }
                if (_bottomLevel) {
                    _bottomLevel->updateOnlyOneLevel(0, TargetType::Stack);
                }
                LOG(0, "OM {C[i1]} Side={i2}. Got quote out of market.", getContractName(), _side)
            }
        }
    }

    void getHedgeOutOfMarket(bool force = false) {
        if (_hedge && _hedge->_target[1] != 0) {
            if (!useGTC() | force) {
                _hedge->updateOnlyOneLevel(0, TargetType::Hedge);
                LOG(0, "OM {C[i1]} Side={i2}. Got hedge out of market.", getContractName(), _side)
                _hedgeRetryCount = 0;
                _hedge = nullptr;
            }
        }
    }

    hf::qty_t getRestingSize() const noexcept {
        if (!_inMarket) {
            return 0;
        } else {
            return _topLevel->target() - _topLevel->_traded;
        }
    }

    hf::price_t getPrice() const noexcept {
        if (!_inMarket) {
            return hf::NULL_PRICE;
        } else {
            return _topLevel->_price;
        }
    }

    void updateQuoteSize(hf::qty_t size, bool enabled) {
        _quoteSize = size;
        if (_nLevels > 0) {
            if (_topLevel != nullptr && _bottomLevel != nullptr) {
                Level* level = _topLevel->after;
                while (level != _bottomLevel) {
                    if (enabled) {
                        level->updateOnlyOneLevel(_quoteSize, TargetType::Stack);
                    } else {
                        level->_target[0] = _quoteSize;
                    }
                    level = level->after;
                }
                if (enabled) {
                    _bottomLevel->updateOnlyOneLevel(_quoteSize, TargetType::Stack);
                } else {
                    _bottomLevel->_target[0] = _quoteSize;
                }
            }
        }
    }

    template<uint32_t Side>
    void updateMaxSlackSize(uint32_t slackSize) {
        LOG(0, "OM {C[i1]} Side={i2}. Update number slack size. N={i3}", getContractName(), _side, slackSize)
        if (slackSize < _maxSlackSize) {
            if (_topLevel != nullptr && _bottomLevel != nullptr) {
                Level* stackerLevel = _topLevel;
                for (uint32_t n = 0; n < _nLevels; ++n) {
                    stackerLevel = stackerLevel->after;
                }
                Level* newBottom = stackerLevel;

                while (stackerLevel != _bottomLevel) {
                    stackerLevel->updateOnlyOneLevel(0, TargetType::Stack);
                    stackerLevel = stackerLevel->after;
                }
                _bottomLevel->updateOnlyOneLevel(0, TargetType::Stack);

                _bottomLevel = newBottom;
                LOG(0, "OM {C[i1]} Side={i2}. BottomPrice={d1}", getContractName(), _side, _bottomLevel->_dollar)
            }
        }
        _maxSlackSize = slackSize;
    }

    template<uint32_t Side>
    void updateNumberOfLevels(uint32_t nLevels, bool enabled) {
        LOG(0, "OM {C[i1]} Side={i2}. Update number of levels. N={i3}", getContractName(), _side, nLevels)
        if (nLevels != _nLevels) {
            if (_topLevel != nullptr && _bottomLevel != nullptr) {
                Level* stackLevel = _topLevel;
                for (uint64_t i = 0; i < nLevels; ++i) {
                    stackLevel = stackLevel->after;
                    if (enabled) {
                        stackLevel->updateOnlyOneLevel(_quoteSize, TargetType::Stack);
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
                            stackLevel->updateOnlyOneLevel(0, TargetType::Stack);
                        } else {
                            stackLevel->_target[0] = 0;
                        }
                        stackLevel = stackLevel->after;
                    }
                    if (enabled) {
                        _bottomLevel->updateOnlyOneLevel(0, TargetType::Stack);
                    } else {
                        _bottomLevel->_target[0] = 0;
                    }

                    _bottomLevel = newBottomLevel;
                }

                LOG(0, "OM {C[i1]} Side={i2}. BottomPrice={d1}", getContractName(), _side, _bottomLevel->_dollar)
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
                LOG(0, "OM {C[i1]} Side={i2}. onLevelFree. Did not find level. Price={d1}", getContractName(), _side, ToDollars(price))
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
                LOG(0, "OM {C[i1]}. Side={i2} onLevelUsed. Did not find level. Price={d1}", getContractName(), _side, ToDollars(price))
                BOOST_ASSERT(false);
            }
        } else {
            LOG(0, "OM {C[i1]}. Side={i2} onLevelUsed. Does not have top level. Storing. Price={d1}", getContractName(), _side, ToDollars(price))
            _usedPrices.insert(price);
        }
    }

    void disableMarketMakerAfterStackerRejects(const std::string& reason) {
        _ime->_executor->disableRVDisableMMExternalQuote(_internalId, reason);
    }

    void handleSMPModifyReject(hf::price_t price, bool isHedge) {
        _ime->handleSMPModifyReject(_side, price, isHedge, SMPModifyRejectReason::VALUE);
    }

    std::vector<const om::order_t*>& getGTCs(hf::price_t price) {
        return _ime->getGTCs(price, _side);
    }

    bool useGTC() {
        return _orderType == om::ORDER_TYPE::GTC;
    }

    template<uint32_t Side>
    void updateUseGtc(bool useGtc, bool onlyDeleteHedge, bool onlyDeleteQuote = false) {
        auto orderType = useGtc ? om::ORDER_TYPE::GTC : om::ORDER_TYPE::LMT;
        if (_orderType != orderType) {
            _orderType = orderType;
            LOG(0, "OM {C[i1]} Side={i2}. Update useGTC use={i3} onlyDelete={i4}", getContractName(), _side, (int)useGtc, (int)onlyDeleteHedge)

            if (_inMarket) {
                hf::price_t price = _topLevel->_price;
                hf::qty_t size = _topLevel->_target[0];
                getQuoteOutOfMarket(true);
                if (!onlyDeleteQuote) {
                    quote<Side>(price, size, true);
                }
            }

            if (_hedge) {
                hf::qty_t hedgeTarget = _hedge->_target[1];

                // _hedge will be set to null in getHedgeOutOfMarket
                Level* orgHedge = _hedge;
                getHedgeOutOfMarket(true);
                _hedge = orgHedge;
                if (!onlyDeleteHedge) {
                    hedge<Side>(_hedge->_price, hedgeTarget);
                }
            }
        }
    }

    void forceDeleteHedgeOrders(bool force = false) {
        if (_hedge && (!useGTC() | force)) {
            _hedge->updateOnlyOneLevel(0, TargetType::Hedge);
        }
    }

    static ModifyTracker* getAvailableModifyTracker() {
        return _mtt.getAvailableModifyTracker();
    }

    static ModifyTracker* getModifyTracker(const om::order_t& order) {
        return _mtt.getModifyTracker(order);
    }

    void setPendingHedge(bool isPayUp) noexcept {
        LOG(0, "OM {C[i1]}. Side={i2} Set pending hedge PayUp={i3}", getContractName(), _side, isPayUp)
        _hasPendingHedge = true;
        _pendingHedgePayUp = isPayUp;
    }

    template<uint32_t Side>
    void applyPendingHedgeUpdate(hf::price_t price, hf::qty_t size) {
        if (_hasPendingHedge) {
            hedge<Side>(price, size);
            _pendingHedgePayUp = false;
            _hasPendingHedge = false;
        }
    }

    void handleInvalidTickSize(hf::price_t price) {
        if (_inMarket) {
            double dollar = ToDollars(price);
            LOG(0, "OM {C[i1]}. Could not find level at {d1}. It most likely caused by invalid tick size.", getContractName(), dollar)
            std::string msg = boost::str(
                    boost::format("OM - Could not find level at %1%. It most likely caused by invalid tick size. Contract=%2%") % dollar % _ime->_name);
            _ime->_executor->disableMarketMakerAndRV(_internalId, msg);
        }
    }

#ifdef IS_DEV
    std::string getContractName() const {
        return _ime->_name;
    }
#else
    uint32_t getContractName() const {
        return _internalId;
    }
#endif

    std::set<hf::price_t> _usedPrices;

    uint32_t _internalId;
    uint32_t _side;  //Any reason not in template?
    hf::price_t _tickSize;
    hf::qty_t _quoteSize;
    uint32_t _nLevels;
    uint32_t _maxSlackSize;
    uint32_t _hedgeRetryCount = 0;
    uint64_t _hedgeRetrySeqNum = 0;

    bool _hasPendingHedge = false;
    bool _pendingHedgePayUp = false;
    bool _inMarket = false;
    bool _iceCancelResting = true;

    Level* _stackerStart = nullptr;
    Level* _topLevel = nullptr;
    Level* _bottomLevel = nullptr;
    Level* _hedge = nullptr;

    hf::VENUE _venue;

    IME_T* _ime;

    OverfillProtector _overfillProtection;

    om::ORDER_TYPE _orderType = om::ORDER_TYPE::LMT;
    static ModifyTrackerTracker _mtt;
    static std::unordered_map<OrderManager<IME_T> *, bool> _isDisablingDueToChangeViolation;
};

template<typename ExecutorT>
typename OrderManager<ExecutorT>::ModifyTrackerTracker OrderManager<ExecutorT>::_mtt;

template<typename ExecutorT>
std::unordered_map<OrderManager<ExecutorT> *, bool>  OrderManager<ExecutorT>::_isDisablingDueToChangeViolation;

}

#ifdef REENABLE_BOOST_ASSERTS
#define BOOST_DISABLE_ASSERTS=1
#endif

#endif //VIPER_STACKER_H
