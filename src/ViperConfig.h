#ifndef VIPER_VIPER_CONFIG_H
#define VIPER_VIPER_CONFIG_H

#include <boost/filesystem.hpp>
#include <boost/tokenizer.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/lexical_cast.hpp>
#include <fstream>

#include "messages/omsproto.pb.h"
#include "MathStructures.h"

namespace Python {

struct Contract {
    std::string _name;
    uint64_t _gid;
};

struct ViperConfig {

    bool initialize(const std::string& fileName) {
        std::ifstream input(fileName.c_str(), std::ifstream::in);

        if (!input.good()) {
            return false;
        }

        loadPricingMatrix(input);

    }

    bool loadPricingMatrix(std::ifstream& input) {
        bool good = parsePricingHeader(input);

        if (!good) {
            return false;
        }

        return parsePricingBody(input);
    }

    bool parsePricingHeader(std::ifstream& input) {
        const boost::char_separator<char> sep(",", "", boost::keep_empty_tokens);
        std::string line;

        std::getline(input, line);

        if (!input) {
            false;
        }

        boost::algorithm::trim(line);
        if (line.empty()) {
            false;
        }

        const boost::tokenizer<boost::char_separator<char> > tok(line, sep);

        auto it = tok.begin();

        if (it == tok.end()) {
            return false;
        }

        const std::string type = boost::algorithm::trim_copy(*it++);

        if (type != "pricing") {
            return false;
        }

        for (; it != tok.end(); ++it) {
            const std::string contractName = boost::algorithm::trim_copy(*it++);
            _contracts.push_back({contractName, 0});
        }

        return _contracts.size() == 0;
    }

    bool parsePricingBody(std::ifstream& input) {
        const boost::char_separator<char> sep(",", "", boost::keep_empty_tokens);
        std::string line;

        uint32_t rowIndex = 0;

        while (input) {
            std::getline(input, line);

            if (!input)
                break;

            boost::algorithm::trim(line);
            if (line.empty()) {
                false;
            }

            const boost::tokenizer<boost::char_separator<char> > tok(line, sep);

            auto it = tok.begin();

            if (it == tok.end()) {
                return false;
            }

            const auto vectorName = boost::algorithm::trim_copy(*it++);

            uint32_t columnIndex = 0;
            for (; it != tok.end(); ++it) {
                double value = boost::lexical_cast<double>(*it++);
                _pricingMatrix.setValue(rowIndex, columnIndex, value);
                ++columnIndex;
            }

            ++rowIndex;
        }

        return true;
    }

    void applyChange(const proto::MarketMakerConfigChange& change) {
    }

    void applyChange(const proto::RelativeValueConfigChange& change) {
    }

    void writeToFile() {
    }

private:
    Matrix _pricingMatrix;

    enum class VectorConfigs {Range, MaxPos, Width, Step, EMA, EMALength, SetToMarket};
    Matrix _vectorConfigs;

    uint32_t _numberOfVectors;

    enum class RelativeValueConfig {QuoteExternal, MinLeanBuffer};
    Matrix _relativeValueConfig;

    enum class MarketMakerConfig {QuoteExternal, QuoteSize, Width, JoinSize, LeaveSize, PayUpQty, PayUpOrders, PayUpMultiple, MaxPosInternal, MaxPosExternal};
    Matrix _marketMakerConfig;

    std::vector<Contract> _contracts;
};

};

#endif //VIPER_VIPER_CONFIG_H
