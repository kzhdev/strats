//
// Created by azhitlenok on 3/23/16.
//

#include <hf/main/hfmain.h>
#include <iostream>
#include <map>

#include "../oms/ServerClientMessenger.h"

#include "../oms/messages/omsproto.pb.h"

int main(int argc, char *argv[]) {

#ifdef CLIENT_CONNECTION_TEST

    std::cout << "Start Start" << std::endl;

    oms::ClientsAcceptor ca("corlsd003", 23926 );
    ca.clientConnectionTest();
    return 0;

#else
    return hf::main::HFMain::hf_main(argc,argv);
#endif
}
