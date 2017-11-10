//
// Created by pkusch on 12/26/16.
//

#include <hf/ResourceManager.h>
#include <hf/ConfigManager.h>
#include <md/queue/MDQueueResource.h>
#include "SpreaderFrontSide.h"

LOG_DECLARE();

#include "SpreaderFrontSide.hh"

namespace oms { namespace spreader {

static hf::ResourceRegistrator <SpreaderFrontSide<strat::Context, SpreaderClientsAcceptor, md::sym::SymbolsResource>> s_registrator("spreader::Frontend");

}}


