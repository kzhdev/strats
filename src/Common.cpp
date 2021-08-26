#include "Common.h"
#include <util/log.h>
#include <boost/format.hpp>

#define STR_EXPAND(tok) #tok
#define QUOTED_STR(tok) STR_EXPAND(tok)
#ifndef VIPER_VERSION
#define VIPER_VERSION "Unset"
#endif
#ifndef VIPER_GITHASH
#define VIPER_GITHASH "Unset"
#endif
#ifndef RHONEPUBLIC_GITHASH
#define RHONEPUBLIC_GITHASH "Unset"
#endif

LOG_DECLARE();


namespace Python {

const std::string ViperVersion = QUOTED_STR(VIPER_VERSION);
const std::string ViperGitHash = QUOTED_STR(VIPER_GITHASH);
const std::string RhonePublicGitHash = QUOTED_STR(RHONEPUBLIC_GITHASH);

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