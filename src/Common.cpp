#include "Common.h"
#include <hf/log.h>
#include <boost/format.hpp>

LOG_DECLARE();

namespace Python {

void ViperPool::create() {
    std::string id = "spreader_pool";
    uint64_t memsize = 1000000000;
    if (!ViperPool::createPool(id, memsize)) {
        LOG_FATAL_AND_THROW(boost::str(boost::format("onCreate() FAILED on createPool('%1%')") % id));
    }
}

void ViperQueue::create() {
    auto capacity = 10000000;
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

}