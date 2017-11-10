//
// Created by pkusch on 3/2/17.
//

#include <strat/Context.hpp>
#include "AlgoManager.h"
#include "../test/Fixtures/TestStrategy.h"

LOG_DECLARE();

namespace oms { namespace spreader {

template class AlgoManager<strat::Context>;
template class AlgoManager<TestStrategy::ContextT>;

template <typename ContextT>
void AlgoManager<ContextT>::log(const std::string& s) {
    LOG_INFO(s);
}

}}
