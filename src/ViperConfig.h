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

        bool good = loadPricingMatrix(input);

        if (!good) {
            return false;
        }

        good = loadVectorConfig(input);

        if (!good) {
            return false;
        }

        good = loadRelativeValueConfig(input);

        if (!good) {
            return false;
        }

        good = loadMarketMakersConfig(input);

        if (!good) {
            return false;
        }


    }

    bool loadMarketMakersConfig(std::ifstream& input) {
        bool good = parseMarketMakersHeader(input);

        if (!good) {
            return false;
        }

        return parseMarketMakersBody(input);
    }

    bool parseMarketMakersHeader(std::ifstream& input) {
        std::string expectedColumns[] = {"quoteExternal", "quoteSize", "width", "joinSize", "leaveSize", "payUpQty", "payUpOrders", "payUpMultiple",
                                         "maxPosInternal", "maxPosExternal"};
        return parseHeader(input, expectedColumns, "mm configs");
    }

    bool parseMarketMakersBody(std::ifstream& input) {
        const boost::char_separator<char> sep(",", "", boost::keep_empty_tokens);
        std::string line;

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

            const auto contractName = boost::algorithm::trim_copy(*it++);

            MarketMakerConfig config;

            config._quoteExternal = boost::lexical_cast<bool>(*it++);
            config._quoteSize = boost::lexical_cast<uint32_t>(*it++);
            config._width = boost::lexical_cast<double>(*it++);
            config._joinSize = boost::lexical_cast<uint32_t>(*it++);
            config._leaveSize = boost::lexical_cast<uint32_t>(*it++);
            config._payUpQty = boost::lexical_cast<uint32_t>(*it++);
            config._payUpOrders = boost::lexical_cast<uint32_t>(*it++);
            config._payUpMultiple = boost::lexical_cast<double>(*it++);
            config._maxPosInternal = boost::lexical_cast<uint32_t>(*it++);
            config._maxPosExternal = boost::lexical_cast<uint32_t>(*it++);

            _marketMakerConfigs.push_back(config);
        }

        return true;
    }

    bool loadRelativeValueConfig(std::ifstream& input) {
        bool good = parseRelativeValueHeader(input);

        if (!good) {
            return false;
        }

        return parseRelativeValueBody(input);
    }

    bool parseRelativeValueHeader(std::ifstream& input) {
        std::string expectedColumns[] = {"quoteExternal", "minLeanBuffer"};
        return parseHeader(input, expectedColumns, "rv configs");
    }

    bool parseRelativeValueBody(std::ifstream& input) {
        const boost::char_separator<char> sep(",", "", boost::keep_empty_tokens);
        std::string line;

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

            const auto contractName = boost::algorithm::trim_copy(*it++);

            RelativeValueConfig config;

            config._quoteExternal = boost::lexical_cast<bool>(*it++);
            config._MinLeanBuffer = boost::lexical_cast<double>(*it++);

            _relativeValueConfigs.push_back(config);
        }

        return true;
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
            const std::string contractName = boost::algorithm::trim_copy(*it);
            _contracts.push_back({contractName, 0});
        }

        return _contracts.size() == 0;
    }

    bool parsePricingBody(std::ifstream& input) {
        const boost::char_separator<char> sep(",", "", boost::keep_empty_tokens);
        std::string line;

        uint32_t vectorIndex = 0;

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
                double value = boost::lexical_cast<double>(*it);
                _pricingMatrix.setValue(vectorIndex, columnIndex, value);
                ++columnIndex;
            }

            ++vectorIndex;
        }

        _numberOfVectors = vectorIndex;

        return true;
    }

    bool loadVectorConfig(std::ifstream& input) {
        bool good = parseVectorConfigHeader(input);

        if (!good) {
            return false;
        }

        return parseVectorConfigValues(input);
    }

    bool parseVectorConfigValues(std::ifstream& input) {
        const boost::char_separator<char> sep(",", "", boost::keep_empty_tokens);
        std::string line;

        uint32_t vectorIndex = 0;

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

            VectorConfig config;

            config._range = boost::lexical_cast<uint32_t>(*it++);
            config._maxPos = boost::lexical_cast<uint32_t>(*it++);
            config._width = boost::lexical_cast<double>(*it++);
            config._step = boost::lexical_cast<double>(*it++);
            config._useEMA = boost::lexical_cast<bool>(*it++);
            config._EMALength = boost::lexical_cast<uint32_t>(*it++);
            config._setToMarket = boost::lexical_cast<bool>(*it++);

            _vectorConfigs.push_back(config);

            ++vectorIndex;
        }

        return _numberOfVectors == vectorIndex;
    }

    bool parseVectorConfigHeader(std::ifstream& input) {
        std::string expectedColumns[] = {"range", "maxPos", "width", "step", "EMA", "EMALength", "SetToMarket"};
        return parseHeader(input, expectedColumns, "vector configs");
    }

    bool parseHeader(std::ifstream& input, std::string expectedColumns[], const std::string& expectedType) {
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

        if (type != expectedType) {
            return false;
        }

        uint32_t columnIndex = 0;
        for (; tok != tok.end(); ++it) {
            const std::string name = boost::algorithm::trim_copy(*it);
            if (name == "") {
                continue;
            }
            if (columnIndex >= expectedColumns->size()) {
                return false;
            }
            if (expectedColumns[columnIndex] != name) {
                return false;
            }
        }
        return columnIndex == expectedColumns->size() - 1;
    }


    void applyChange(const proto::MarketMakerConfigChange& change) {
    }

    void applyChange(const proto::RelativeValueConfigChange& change) {
    }

    void writeToFile() {
    }

private:
    Matrix _pricingMatrix;

    struct VectorConfig {
        uint32_t _range;
        uint32_t _maxPos;
        double _width;
        double _step;
        bool _useEMA;
        uint32_t _EMALength;
        bool _setToMarket;
    };

    std::vector<VectorConfig> _vectorConfigs;

    uint32_t _numberOfVectors;

    struct RelativeValueConfig {
        bool _quoteExternal;
        double _MinLeanBuffer;
    };

    std::vector<RelativeValueConfig> _relativeValueConfigs;

    struct MarketMakerConfig {
        bool _quoteExternal;
        uint32_t _quoteSize;
        double _width;
        uint32_t _joinSize;
        uint32_t _leaveSize;
        uint32_t _payUpQty;
        uint32_t _payUpOrders;
        double _payUpMultiple;
        uint32_t _maxPosInternal;
        uint32_t _maxPosExternal;
    };

    std::vector<MarketMakerConfig> _marketMakerConfigs;

    std::vector<Contract> _contracts;
};

};

#endif //VIPER_VIPER_CONFIG_H
