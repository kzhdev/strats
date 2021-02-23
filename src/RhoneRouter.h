//
// Created by cmilutinovic on 2/22/21.
//

#ifndef RHONEDS_RHONEROUTE_H
#define RHONEDS_RHONEROUTE_H

#include <orderrouter/RoutingPolicy.h>
#include <vector>

namespace Python {

    namespace RhoneRouterUtil {

        //Size is the number of messages per time unit, i.e. (3 messages/ms) => Size=3
        template<uint64_t Size>
        class RoundRobinContainer {

        public:

            void insertEvent(uint64_t ts) {
                BOOST_ASSERT(nextInsertIndex < Size);
                _timestamps[_nextIndex] = ts;
                incrementNextInsertIndex();
            }

            uint64_t oldestReleventEvent() const {
                return _timestamps[firstKnownEventIndex()];
            }

            uint64_t newestRelevantEvent() const {
                return _timestamps[lastKnownEventIndex()];
            }

        private:

            auto firstKnownEventIndex() const {
                return _nextIndex;
            }

            auto lastKnownEventIndex() const {
                return (_nextIndex == 0) ? Size - 1 : _nextIndex - 1;
            }

            void incrementNextInsertIndex() {
                _nextIndex = (_nextIndex + 1) % Size;
            }

            //TODO: Figure out optimal order here
            uint8_t _nextIndex = 0; //Note that the _nextIndex will be equal to the first known event
            std::array<uint64_t, Size> _timestamps = {};

            static_assert(std::numeric_limits<decltype(_nextIndex)>::max() > Size, "Size too large for index");
        };

        struct RoundRobinContainerReferenceHolder {
            RoundRobinContainer<3>& _roundRobinContainer;
            orderrouter::RoutingPolicy _routingPolicy;
        };

        //For b3, we wish to limit our rate to 3msg/ms, and a heartbeat every 500ms if no messages sent
        template<hf::VENUE B3_VENUE = hf::VENUE::B3>
        class B3Router {
        public:
            template<typename ContextT>
            COLD void init(hf::symbol_id_t sid, std::vector<RhoneRouterUtil::RoundRobinContainer<3>>& rrc, ContextT& context) {
                orderrouter::RoutingSpec spec(B3_VENUE, sid, orderrouter::ROUTE_OPTIONS::NO_OPTIONS_SPECIFIED);
                std::vector<strat::om_queue_info_t *> connections = context.getViableExchangeConnections(spec);
                if(connections.size() != rrc.size()) {
                    throw std::runtime_error("Size of rrc and number of connections do not match.  rrc_size=" + std::to_string(rrc.size()) + " connections.size=" + std::to_string(connections.size()));
                }
                for(int i = 0; i < static_cast<int>(connections.size()); ++i) {
                    auto* connection = connections[i];
                    std::vector<strat::om_queue_info_t *> piece(1, connection);
                    orderrouter::RoutingPolicy policy = context.getRoutingPolicyForGivenConnections(spec, piece);
                    if(!policy.hasConnections()) {
                        throw std::runtime_error("No connections for B3 route");
                    }
                    _routes.emplace_back(RoundRobinContainerReferenceHolder{._roundRobinContainer=rrc[i], ._routingPolicy=policy});
                }
            }

            //The way the API works, we expect the client to call getRouteForNextNewOrder(...) then immedietly getRoutingPolicyForNextNewOrder(...), then setNextRoute(...)
            strat::om_queue_info_t* getRouteForNextNewOrder() {
                return &_routes[_currentIndex]._routingPolicy.getNextConnection().get();
            }

            orderrouter::RoutingPolicy& getRoutingPolicyForNextNewOrder() {
                return _routes[_currentIndex]._routingPolicy;
            }

            void setNextRoute(uint64_t ts, int ticksPerMillisecond) {
                _routes[_currentIndex]._roundRobinContainer.insertEvent(ts);
                //We should increment (size() - 1) times and default to that as the newest index if no unthrottled con found.
                //Note arguably we should increment once no matter what since the throttle is so small
                for(int i = 0; i < static_cast<int>(_routes.size()) - 1; ++i) {
                    if(_routes[_currentIndex]._roundRobinContainer.oldestReleventEvent() + ticksPerMillisecond < ts) {
                        return;
                    }
                    incrementIndex();
                }
            }

            void orderCancelled(uint64_t ts, const om::order_t& order) {
                insertEventBasedOnQueue(ts, order.route);
            }

            void orderModified(uint64_t ts, const om::order_t& order) {
                insertEventBasedOnQueue(ts, order.route);
            }

            bool hasConnections() const {
                return !_routes.empty();
            }

            std::size_t numRoutes() const {
                return _routes.size();
            }

            COLD
            const strat::om_queue_info_t* getQueueAtIndex(int index) {
                if(index >= static_cast<int>(_routes.size())) {
                    return nullptr;
                }
                return &_routes[index]._routingPolicy.getNextConnection().get();
            }

            COLD
            bool sameOMQueueRoundRobinBehaviour(B3Router& other) {
                if(other._routes.size() != _routes.size()) {
                    return false;
                }

                for(unsigned i = 0; i < _routes.size(); ++i) {
                    std::reference_wrapper<strat::om_queue_info_t> lhs = _routes[i]._routingPolicy.getNextConnection();
                    std::reference_wrapper<strat::om_queue_info_t> rhs = other._routes[i]._routingPolicy.getNextConnection();

                    if(&lhs.get() != &rhs.get()) {
                        return false;
                    }
                }

                return true;
            }

            static inline uint64_t HeartbeatThreshold(int oneMillisecond) {
                return 500 * oneMillisecond;
            }

        protected:
            void insertEventBasedOnQueue(uint64_t ts, const strat::om_queue_info_t* route) {
                //TODO: If Size is over some threshold, then linear search becomes bad
                //TODO: Might be a faster impl if using port_gid instead of ptr, which is in the order object
                for(int i = 0; i < static_cast<int>(_routes.size()); ++i) {
                    auto& policyRoute = _routes[i];
                    if(&policyRoute._routingPolicy.getNextConnection().get() == route) {
                        policyRoute._roundRobinContainer.insertEvent(ts);
                        if(i == _currentIndex) {  //Our throttle is so small very likely we max it out, so just get off this index
                            incrementIndex();
                        }
                        return;
                    }
                }
                BOOST_ASSERT_MSG(false, "Could not find RoundRobin route corresponding to route");
            }

            void incrementIndex() {
                if(_currentIndex == _routes.size() - 1) {
                    _currentIndex = 0;
                }
                else {
                    ++_currentIndex;
                }
            }

            std::vector<RoundRobinContainerReferenceHolder> _routes;  //Note that _routes._roundRobinContainer is the same for all RhoneRouters, really code should be refactored to take this into account
            uint8_t _currentIndex = 0;
        };
    }

    //Venue only exists to facilitate testing, i.e. to use B3 round robin logic with CME for example
    template<hf::VENUE B3_VENUE = hf::VENUE::B3>
    class RhoneRouter {

    public:

        template<typename ContextT>
        COLD void init(hf::VENUE venue, bool forceUseSimpleRouter, hf::symbol_id_t sid, double frequency, std::vector<RhoneRouterUtil::RoundRobinContainer<3>>& rrc, ContextT& context) {
            if(venue != B3_VENUE || forceUseSimpleRouter) {
                orderrouter::RoutingSpec spec(venue, sid, orderrouter::ROUTE_OPTIONS::NO_OPTIONS_SPECIFIED);
                _simpleRoundRobinRoute = context.getRoutingPolicy(spec);
            }
            else {
                _b3Router = std::make_unique<RhoneRouterUtil::B3Router<B3_VENUE>>();
                _b3Router->init(sid, rrc, context);
            }
        }

        bool hasAssignedB3Router() const {
            return static_cast<bool>(_b3Router);
        }

        //Note for all the below, the original implementation to try to help the optimizer was just (venue != B3_VENUE). However the check
        //_b3Router != nullptr is more correct since the ability was added to disable this feature but cannot be fully optimized.
        bool useSimpleRoute(hf::VENUE venue) const {
            return !hasAssignedB3Router();
        }

        strat::om_queue_info_t* getRouteForNextNewOrder(hf::VENUE venue) {
            if(useSimpleRoute(venue)) {
                return _simpleRoundRobinRoute.getAllConnections()[_simpleRoundRobinRoute.getNextConnectionIndex()];
            }
            return _b3Router->getRouteForNextNewOrder();
        }

        orderrouter::RoutingPolicy& getRoutingPolicyForNextNewOrder(hf::VENUE venue) {
            if(useSimpleRoute(venue)) {
                return _simpleRoundRobinRoute;
            }
            return _b3Router->getRoutingPolicyForNextNewOrder();
        }

        void setNextRoute(hf::VENUE venue, uint64_t ts, int ticksPerMillisecond) {
            if(!useSimpleRoute(venue)) {
                _b3Router->setNextRoute(ts, ticksPerMillisecond);
            }
        }

        void orderCancelled(hf::VENUE venue, uint64_t now, const om::order_t& order) {
            if(!useSimpleRoute(venue)) {
                _b3Router->orderCancelled(now, order);
            }
        }

        void orderModified(hf::VENUE venue, uint64_t now, const om::order_t& order) {
            if(!useSimpleRoute(venue)) {
                _b3Router->orderModified(now, order);
            }
        }

        bool hasConnections(hf::VENUE venue) const {
            if(useSimpleRoute(venue)) {
                return _simpleRoundRobinRoute.hasConnections();
            }
            return _b3Router->hasConnections();
        }

        std::size_t numRoutes() const {
            if(_b3Router) {
                return _b3Router->numRoutes();
            }
            else {
                return _simpleRoundRobinRoute.getNumberOfConnections();
            }
        }

        COLD
        bool sameOMQueueRoundRobinBehaviour(RhoneRouter<B3_VENUE>& other) {
            if(_b3Router.get() == nullptr) {
                return other._b3Router.get() == nullptr;
            }
            if(other._b3Router.get() == nullptr) {
                return false;
            }
            return _b3Router->sameOMQueueRoundRobinBehaviour(*other._b3Router);
        }

        COLD
        bool validateSameOMQueueAtIndex(int index, const strat::om_queue_info_t* queue) {
            if(_b3Router) {
                const strat::om_queue_info_t* otherQueue = _b3Router->getQueueAtIndex(index);
                if(otherQueue) {
                    return otherQueue == queue;
                }
            }
            return false;
        }

            protected:
        //TODO: Ideally a union but tons of asserts in Rhone violated due to becoming a non-default-ctor object
        //TODO: Ideally we could share the router with all instances, but part of the routing policy is the sid
        orderrouter::RoutingPolicy _simpleRoundRobinRoute;
        std::unique_ptr<RhoneRouterUtil::B3Router<B3_VENUE>> _b3Router;
    };
}

#endif //RHONEDS_RHONEROUTE_H
