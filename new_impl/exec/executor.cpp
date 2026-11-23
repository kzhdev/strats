#include "executor.hpp"

// The executor is a template on the mailbox synchronisation mode, so its body lives in the
// header. Those two explicit instantiations are the compile gate: both modes are built in every
// configuration, so a mode-specific defect cannot hide behind the default choice.
namespace tachyon::trade::exec {

template class basic_executor<sync_mode::single>;
template class basic_executor<sync_mode::spsc>;

}  // namespace tachyon::trade::exec
