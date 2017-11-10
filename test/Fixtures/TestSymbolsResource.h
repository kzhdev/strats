//
// Created by pkusch on 3/7/17.
//

#ifndef RHONEOMSTEST_TESTSYMBOLSRESOURCE_H
#define RHONEOMSTEST_TESTSYMBOLSRESOURCE_H

#include <hf/hf.h>

namespace oms { namespace spreader {

class TestSymbolsResource {
public:
    const hf::symbol_info_t& getSymbol(hf::symbol_id_t id) const {
        static hf::symbol_info_t info;
        info.gid = id;
        return info;
    }
};

}}

#endif //RHONEOMSTEST_TESTSYMBOLSRESOURCE_H
