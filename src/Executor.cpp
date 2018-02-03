#include <hf/ResourceManager.h>
#include "Executor.h"

namespace Python  {

static hf::ResourceRegistrator<ExecutorT> s_registrator_1("MarketMakers");

}