//
// Created by pkusch on 1/5/17.
//

#include "SpreaderQueues.h"
#include <hf/log.h>
LOG_DECLARE();

namespace oms { namespace spreader  {

void SpreaderPool::create() {
    std::string id = "spreader_pool";
    uint64_t memsize = 100000000;
    if (!DataPool::createPool(id, memsize)) {
        LOG_FATAL_AND_THROW(boost::str(boost::format("onCreate() FAILED on createPool('%1%')") % id));
    }
}

void SpreaderQueue::create() {
    auto capacity = 1000000;
    const size_t memsize = sizeof(my_queue_t) + hf::queue::U64Queue::size(capacity);

    auto id = "some";

    if (!hf::mem::DataBlock::createBlock(id, memsize, false, "")) {
        LOG_FATAL_AND_THROW(boost::str(boost::format("onCreate() FAILED on createQueue('%1%')") % id));
    }

    auto *ptr = static_cast<my_queue_t *> (hf::mem::DataBlock::getP());
    if (!ptr) {
        LOG_FATAL_AND_THROW(boost::str(boost::format("onCreate() FAILED on createQueue('%1%')") % id));
    }

    if (!hf::queue::U64Queue::init(id, ptr->data, capacity)) {
        LOG_FATAL_AND_THROW(boost::str(boost::format("onCreate() FAILED on createQueue('%1%')") % id));
    }

    ptr->memsize = memsize;
}

}}
