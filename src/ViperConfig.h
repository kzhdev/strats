#ifndef VIPER_VIPER_CONFIG_H
#define VIPER_VIPER_CONFIG_H

#include "messages/omsproto.pb.h"

namespace Python {

struct ViperConfig {

    void initialize(const std::string& fileName) {
    }

    void applyChange(const proto::MarketMakerConfigChange& change) {
    }

    void applyChange(const proto::RelativeValueConfigChange& change) {
    }

    void writeToFile() {
    }
};

};

#endif //VIPER_VIPER_CONFIG_H
