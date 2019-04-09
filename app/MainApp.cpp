#include <hf/main/hfmain.h>
#include <iostream>

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

int main(int argc, char *argv[]) {
    std::cout << "VIPER_VERSION: " << QUOTED_STR(VIPER_VERSION) << std::endl;
    std::cout << "VIPER_GITHASH: " << QUOTED_STR(VIPER_GITHASH) << std::endl;
    std::cout << "RHONEPUBLIC_GITHASH: " << QUOTED_STR(RHONEPUBLIC_GITHASH) << std::endl;

    return hf::main::HFMain::hf_main(argc,argv);
}
