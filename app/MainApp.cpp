#include <hf/main/hfmain.h>
#include "../src/Common.h"
#include <iostream>

int main(int argc, char *argv[]) {
    if (argc == 1) {
        std::cout << "VIPER_VERSION:        " << Python::ViperVersion << std::endl;
        std::cout << "VIPER_GITHASH:        " << Python::ViperGitHash << std::endl;
        std::cout << "RHONEPUBLIC_GITHASH:  " << Python::RhonePublicGitHash << std::endl;
        return 0;
    }

    return hf::main::HFMain::hf_main(argc,argv);
}
