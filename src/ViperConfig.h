#ifndef VIPER_VIPER_CONFIG_H
#define VIPER_VIPER_CONFIG_H

#include <boost/filesystem.hpp>
#include <boost/tokenizer.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/lexical_cast.hpp>
#include <fstream>
#include <md/sym/Symbols.h>
#include "messages/omsproto.pb.h"
#include "MathStructures.h"
#include "Common.h"
#include "UpdateSelector.h"

namespace Python {

enum class VectorParametersColumns {Position = 0, Mkt = 1, STF = 2, LTF = 3, MaxPos = 4, Range = 5, Width = 6, Step = 7, EMA = 8, EMA_ON = 9, EMA_Time = 10, SetToMarket = 11};
enum class ContractParameterRVColumns {Position = 0, Lean = 1, BidPrice = 2, AskPrice = 3, BidQty = 4, AskQty = 5, STF = 6, Mkt = 7, QuoteExternal = 8, MaxHedgeBuffer = 9, Commission = 10};
enum class PricingConfigsColumns {Weight = 0, UseWeighted = 1, EstablishedLevel = 2};

struct ConfigLeg {
    uint32_t _index;
    int32_t _ratio;
};

struct ConfigContract {
    std::string _name;
    uint64_t _gid;
    hf::symbol_id_t _sid;
    hf::price_t _tickSize;
    hf::VENUE _venue;
    std::vector<ConfigLeg> _legs;
};

struct VectorWithContract {
    std::string _contract;
    std::vector<int> _values;
};

struct PricingConfig {
    std::string _contract;
    double _weight;
    bool _useWeightedMid;
    uint32_t _establishedLevel;
};

struct VectorConfig {
    std::string _vectorName;
    double _range;
    uint32_t _maxPos;
    double _width;
    double _step;
    bool _useEMA;
    uint32_t _EMALength;
    bool _setToMarket;
    double _longTermFair;
    uint32_t _position;
};

struct RelativeValueConfig {
    std::string _contract;
    bool _quoteExternal;
    double _MinLeanBuffer;
    double _commision;
    uint32_t _position;
};

struct ViperConfig {

    ViperConfig()
    :
        _updateSelector(*this)
    {}

    bool initialize(const std::string& fileName, md::sym::Symbols& symbols) {
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

        good = loadStackerConfigs(input);

        if (!good) {
            return false;
        }

        good = loadExecutionConfigs(input);

        if (!good) {
            return false;
        }

        good = loadPricingConfigs(input);

        if (!good) {
            return false;
        }

        good = loadLeanMatrix(input);

        if (!good) {
            return false;
        }

        good = loadPricingDefinitions(input);

        input.close();

        validateInputs();

        createContracts(symbols);

        return good;
    }

    typedef const boost::tokenizer<boost::char_separator<char>>::iterator TokenizerItT;

    bool loadLeanMatrix(std::ifstream& input) {
        bool good = parseLeanMatrixHeader(input);

        if (!good) {
            return false;
        }

        return parseLeanMatrixBody(input);
    }

    bool parseLeanMatrixHeader(std::ifstream& input) {
        std::string expectedColumns[] = {};
        return parseHeader(input, expectedColumns, "LeanMatrix", false);
    }

    bool parseLeanMatrixBody(std::ifstream& input) {
        return parseContractMatrix(input, _leanMatrix);
    }

    bool parseContractMatrix(std::ifstream& input, std::vector<VectorWithContract>& matrix) {
        const boost::char_separator<char> sep(",", "", boost::keep_empty_tokens);
        std::string line;

        while (input) {
            std::getline(input, line);

            if (!input)
                break;

            boost::algorithm::trim(line);
            if (line.empty()) {
                return false;
            }

            const boost::tokenizer<boost::char_separator<char> > tok(line, sep);

            auto it = tok.begin();

            if (it == tok.end()) {
                return false;
            }

            VectorWithContract row;

            row._contract = boost::algorithm::trim_copy(*it++);

            for (; it != tok.end(); ++it) {
                double value = boost::lexical_cast<double>(*it);
                row._values.push_back(value);
            }
        }

        return true;
    }

    bool loadPricingDefinitions(std::ifstream& input) {
        bool good = parsePricingDefinitionsHeader(input);

        if (!good) {
            return false;
        }

        return parseLPricingDefinitionsBody(input);
    }

    bool parsePricingDefinitionsHeader(std::ifstream& input) {
        std::string expectedColumns[] = {};
        return parseHeader(input, expectedColumns, "PricingDefinitions", false);
    }

    bool parseLPricingDefinitionsBody(std::ifstream& input) {
        return parseContractMatrix(input, _pricingDefinitions);
    }

    bool loadPricingConfigs(std::ifstream& input) {
        bool good = parsePricingConfigsHeader(input);

        if (!good) {
            return false;
        }

        return parsePricingConfigBody(input);
    }

    bool parsePricingConfigsHeader(std::ifstream& input) {
        std::string expectedColumns[] = {"weights", "weightedMid", "establishedLevel"};
        return parseHeader(input, expectedColumns, "PricingConfigs");
    }

    bool parsePricingConfigBody(std::ifstream& input) {
        std::function<void(TokenizerItT& it)> setter = [&configs = _pricingConfigs](auto it) {
            PricingConfig config;
            config._weight = boost::lexical_cast<double>(*it++);
            config._useWeightedMid = boost::lexical_cast<bool>(*it++);
            config._establishedLevel = boost::lexical_cast<uint32_t>(*it++);
            configs.push_back(config);
        };
        return parseBody(input, setter);
    }

    bool loadExecutionConfigs(std::ifstream& input) {
        bool good = parseExecutionConfigsHeader(input);

        if (!good) {
            return false;
        }

        return parseExecutionConfigBody(input);
    }

    bool parseExecutionConfigsHeader(std::ifstream& input) {
        std::string expectedColumns[] = {"quoteBanner", "fillBanner"};
        return parseHeader(input, expectedColumns, "ExecutionConfigs");
    }

    bool parseExecutionConfigBody(std::ifstream& input) {
        std::function<void(TokenizerItT& it)> setter = [&configs = _executionConfigs](auto it) {
            ExecutionConfig config;
            config._quoteBanner = boost::lexical_cast<uint32_t>(*it++);
            config._fillBaner = boost::lexical_cast<uint32_t>(*it++);
            configs.push_back(config);
        };
        return parseBody(input, setter);
    }

    bool loadStackerConfigs(std::ifstream& input) {
        bool good = parseStackerConfigsHeader(input);

        if (!good) {
            return false;
        }

        return parseStackerConfigBody(input);
    }

    bool parseStackerConfigsHeader(std::ifstream& input) {
        std::string expectedColumns[] = {"quoteSize", "stackLevels"};
        return parseHeader(input, expectedColumns, "StackerConfigs");
    }

    bool parseStackerConfigBody(std::ifstream& input) {
        std::function<void(TokenizerItT& it)> setter = [&configs = _stackerConfigs](auto it) {
            StackerConfig config;
            config._quoteSize = boost::lexical_cast<uint32_t>(*it++);
            config._numberOfLevels = boost::lexical_cast<uint32_t>(*it++);
            configs.push_back(config);
        };
        return parseBody(input, setter);
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
        std::function<void(TokenizerItT& it)> setter = [&configs = _marketMakerConfigs](auto it) {
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
            configs.push_back(config);
        };
        return parseBody(input, setter);
    }

    bool parseBody(std::ifstream& input, std::function<void(TokenizerItT& it)>& setter) {
        const boost::char_separator<char> sep(",", "", boost::keep_empty_tokens);
        std::string line;

        while (input) {
            std::getline(input, line);

            if (!input)
                break;

            boost::algorithm::trim(line);
            if (line.empty()) {
                return false;
            }

            const boost::tokenizer<boost::char_separator<char> > tok(line, sep);

            auto it = tok.begin();

            if (it == tok.end()) {
                return false;
            }

            const auto contractName = boost::algorithm::trim_copy(*it++);

            setter(it);
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
        std::function<void(TokenizerItT& it)> setter = [&configs = _relativeValueConfigs](auto it) {
            RelativeValueConfig config;
            config._quoteExternal = boost::lexical_cast<bool>(*it++);
            config._MinLeanBuffer = boost::lexical_cast<double>(*it++);
            configs.push_back(config);
        };
        return parseBody(input, setter);
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
            return false;
        }

        boost::algorithm::trim(line);
        if (line.empty()) {
            return false;
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
            _pricingOutrights.push_back(contractName);
        }

        return _pricingOutrights.size() == 0;
    }

    bool parsePricingBody(std::ifstream& input) {
        const boost::char_separator<char> sep(",", "", boost::keep_empty_tokens);
        std::string line;

        while (input) {
            std::getline(input, line);

            if (!input)
                break;

            boost::algorithm::trim(line);
            if (line.empty()) {
                return false;
            }

            const boost::tokenizer<boost::char_separator<char> > tok(line, sep);

            auto it = tok.begin();

            if (it == tok.end()) {
                return false;
            }

            PricingRow row;

            row._vectorName = boost::algorithm::trim_copy(*it++);

            for (; it != tok.end(); ++it) {
                double value = boost::lexical_cast<double>(*it);
                row._values.push_back(value);
            }
            _pricingMatrix.push_back(row);
        }

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
        std::function<void(TokenizerItT& it)> setter = [&configs = _vectorConfigs](auto it) {
            VectorConfig config;
            config._range = boost::lexical_cast<uint32_t>(*it++);
            config._maxPos = boost::lexical_cast<uint32_t>(*it++);
            config._width = boost::lexical_cast<double>(*it++);
            config._step = boost::lexical_cast<double>(*it++);
            config._useEMA = boost::lexical_cast<bool>(*it++);
            config._EMALength = boost::lexical_cast<uint32_t>(*it++);
            config._setToMarket = boost::lexical_cast<bool>(*it++);
            configs.push_back(config);
        };
        return parseBody(input, setter);
    }

    bool parseVectorConfigHeader(std::ifstream& input) {
        std::string expectedColumns[] = {"range", "maxPos", "width", "step", "EMA", "EMALength", "SetToMarket"};
        return parseHeader(input, expectedColumns, "vector configs");
    }

    bool parseHeader(std::ifstream& input, std::string expectedColumns[], const std::string& expectedType, bool checkColumns = true) {
        const boost::char_separator<char> sep(",", "", boost::keep_empty_tokens);
        std::string line;

        std::getline(input, line);

        if (!input) {
            return false;
        }

        boost::algorithm::trim(line);
        if (line.empty()) {
            return false;
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

        if (checkColumns) {
            uint32_t columnIndex = 0;
            for (; it != tok.end(); ++it) {
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

        return true;
    }

    void populatePricingMatrix(Matrix& matrix) {
        uint32_t rows = _pricingMatrix.size();
        uint32_t columns = _pricingMatrix[0]._values.size();
        matrix.initialize(rows, columns);
        for (uint32_t row = 0; row < rows; ++row) {
            for (uint32_t column = 0; column < columns; ++column) {
                const double value = _pricingMatrix[row]._values[column];
                matrix.setValue(row, column, value);
            }
        }
    }

    void validateInputs() {
    }

    void createContracts(md::sym::Symbols& symbols) {
        for (auto& pc : _pricingConfigs) {
            std::string symbol = pc._contract;
            hf::symbol_id_t sid = symbols.findSymbol(symbol);
            const hf::symbol_info_t& symbolInfo = symbols.getSymbol(sid);
            ConfigContract contract;
            contract._gid = symbolInfo.gid;
            contract._sid = symbolInfo.sid;
            contract._name = symbol;
            contract._tickSize = symbolInfo.tick_size;
            contract._venue = symbolInfo.getVenue();
            if (symbolInfo.legs.size() > 0) {
                for (auto& symbolInfoleg : symbolInfo.legs) {
                    int32_t legIndex = findLegIndex(symbolInfoleg.gid);
                    if (legIndex == -1) {
                        throw std::runtime_error("could not find leg");
                    }
                    ConfigLeg leg;
                    leg._index = static_cast<uint32_t>(legIndex);
                    leg._ratio = leg._ratio;
                    contract._legs.push_back(leg);
                }
            } else {
                ++_numberOfOutrights;
            }
            _allContracts.push_back(contract);
        }
    }

    int32_t findLegIndex(hf::symbol_gid_t gid) {
        for (uint32_t index = 0; index < _allContracts.size(); ++index) {
            ConfigContract& contract = _allContracts[index];
            if (contract._gid == gid) {
                return index;
            }
        }
        return -1;
    }

    void applyGridUpdate(GridUpdate& update) {
        _updateSelector.processUpdate(update);
        writeToFile();
    }

    bool onRvVectorParametersUpdate(GridUpdate& update) {
        for (int i = 0; i < update._update.cells_size(); ++i) {
            const proto::GridCell& gridCell = update._update.cells(i);
            VectorConfig& config = _vectorConfigs[gridCell.row()];
            VectorParametersColumns columnType = static_cast<VectorParametersColumns>(gridCell.col());
            switch (columnType) {
                case VectorParametersColumns::Step: {
                    config._step = atof(gridCell.value().c_str());
                    break;
                }
                case VectorParametersColumns::Range: {
                    config._range = atof(gridCell.value().c_str());
                    break;
                }
                case VectorParametersColumns::MaxPos: {
                    config._maxPos = atoi(gridCell.value().c_str());
                    break;
                }
                case VectorParametersColumns::Width: {
                    config._width = atof(gridCell.value().c_str());
                    break;
                }
                case VectorParametersColumns::EMA_ON: {
                    config._useEMA = (1 == atoi(gridCell.value().c_str()));
                    break;
                }
                case VectorParametersColumns::EMA_Time: {
                    config._EMALength = atoi(gridCell.value().c_str());
                    break;
                }
                case VectorParametersColumns::LTF: {
                    config._longTermFair = atof(gridCell.value().c_str());
                    break;
                }
                case VectorParametersColumns::SetToMarket: {
                    config._setToMarket = (1 == atoi(gridCell.value().c_str()));
                    break;
                }
                default:
                    break;
            }
        }
        return true;
    }

    bool onRvContractParametersUpdate(GridUpdate& update) {
        for (int i = 0; i < update._update.cells_size(); ++i) {
            const proto::GridCell& gridCell = update._update.cells(i);
            auto& config = _relativeValueConfigs[gridCell.row()];
            ContractParameterRVColumns columnType = static_cast<ContractParameterRVColumns>(gridCell.col());

            switch (columnType) {
                case ContractParameterRVColumns::QuoteExternal: {
                    config._quoteExternal = (1 == atoi(gridCell.value().c_str()));
                    break;
                }
                case ContractParameterRVColumns::Commission: {
                    config._commision = atof(gridCell.value().c_str());
                    break;
                }
                default:
                    break;
            }
        }
        return true;
    }

    bool onRvLeanMatrixUpdate(GridUpdate& update) {
        for (int i = 0; i < update._update.cells_size(); ++i) {
            const proto::GridCell& gridCell = update._update.cells(i);
            _leanMatrix[gridCell.row()]._values[gridCell.col()] = (1 == atoi(gridCell.value().c_str()));
        }
        return true;
    }

    bool onRvPricingConfigUpdate(GridUpdate& update) {
        return true;
    }

    bool onManualFills(GridUpdate& update) {
        return true;
    }

    void writeToFile() {
    }

    struct PricingRow {
        std::string _vectorName;
        std::vector<double> _values;
    };

    std::vector<PricingRow> _pricingMatrix;
    std::vector<std::string> _pricingOutrights;
    std::vector<VectorConfig> _vectorConfigs;
    std::vector<RelativeValueConfig> _relativeValueConfigs;

    struct MarketMakerConfig {
        std::string _contract;
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

    struct StackerConfig {
        std::string _contract;
        uint32_t _quoteSize;
        uint32_t _numberOfLevels;
    };

    std::vector<StackerConfig> _stackerConfigs;

    struct ExecutionConfig {
        std::string _contract;
        uint32_t _quoteBanner;
        uint32_t _fillBaner;
    };

    std::vector<ExecutionConfig> _executionConfigs;

    std::vector<VectorWithContract> _leanMatrix;
    std::vector<VectorWithContract> _pricingDefinitions;
    std::vector<PricingConfig> _pricingConfigs;

    std::vector<ConfigContract> _allContracts;
    uint32_t _numberOfOutrights = 0;

    UpdateSelector<ViperConfig> _updateSelector;
};

};

#endif //VIPER_VIPER_CONFIG_H
