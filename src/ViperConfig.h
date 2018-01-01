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
#include <boost/algorithm/string.hpp>

namespace Python {

struct ManualFill {
    static const uint32_t NO_VALUE = std::numeric_limits<uint32_t>::max();

    uint32_t _contract;
    int32_t _size;
    uint32_t _strategyId;

    void clear() {
        _contract = NO_VALUE;
        _size = 0;
        _strategyId = NO_VALUE;
    }

    bool validate() {
        return true;
    }
};

struct ManualFillsCollection {
    static const uint32_t SIZE = 5;

    ManualFillsCollection() {
        _manualFills.reserve(SIZE);
    }

    std::vector<ManualFill> _manualFills;

    bool validate() {
        bool good = true;
        for (auto& fill : _manualFills) {
            good &= fill.validate();
        }
        return good;
    }

    void clear() {
        for (auto& fill : _manualFills) {
            fill.clear();
        }
    }
};

struct StackerConfig {
    std::string _contract;
    uint32_t _quoteSize;
    uint32_t _numberOfLevels;
};

struct ConfigLeg {
    uint32_t _index;
    int32_t _ratio;
};

struct ConfigContract {
    std::string _name;
    hf::symbol_gid_t _gid;
    hf::symbol_id_t _sid;
    hf::price_t _tickSize;
    hf::VENUE _venue;
    uint32_t _internalId;
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
};

struct RelativeValueConfig {
    std::string _contract;
    bool _quoteExternal;
    double _MinLeanBuffer;
    double _commision;
    int32_t _position;
};

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
    double _commission;
};

struct ExecutionConfig {
    std::string _contract;
    uint32_t _quoteBanner;
    uint32_t _fillBaner;
};

struct ViperConfig {

    ViperConfig()
    :
        _updateSelector(*this)
    {}

    template <typename SymbolsT>
    bool initialize(const std::string& fileName, SymbolsT& symbols) {
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

        good = loadLeanMatrix(input);

        if (!good) {
            return false;
        }

        good = loadPricingDefinitions(input);

        if (!good) {
            return false;
        }

        good = loadPricingConfigs(input);

        if (!good) {
            return false;
        }


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
        std::vector<std::string> expectedColumns;
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
                std::string valuesAsString = *it;
                if (valuesAsString == "") {
                    break;
                }
                int value = boost::lexical_cast<int>(valuesAsString);
                row._values.push_back(value);
            }

            if (row._contract == "") {
                return true;
            }

            matrix.push_back(row);
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
        std::vector<std::string> expectedColumns;
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
        std::vector<std::string> expectedColumns = {"weights", "weightedMid", "establishedLevel"};
        return parseHeader(input, expectedColumns, "PricingConfigs");
    }

    bool parsePricingConfigBody(std::ifstream& input) {
        std::function<void(std::vector<std::string>& values)> setter = [&configs = _pricingConfigs](std::vector<std::string>& values) {
            PricingConfig config;
            config._contract = values[0];
            config._weight = boost::lexical_cast<double>(values[1]);
            config._useWeightedMid = values[2] == "TRUE";
            config._establishedLevel = boost::lexical_cast<uint32_t>(values[3]);
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
        std::vector<std::string> expectedColumns  = {"quoteBanner", "fillBanner"};
        return parseHeader(input, expectedColumns, "ExecutionConfigs");
    }

    bool parseExecutionConfigBody(std::ifstream& input) {
        std::function<void(std::vector<std::string>& values)> setter = [&configs = _executionConfigs](std::vector<std::string>& values) {
            ExecutionConfig config;
            config._contract = values[0];
            config._quoteBanner = boost::lexical_cast<uint32_t>(values[1]);
            config._fillBaner = boost::lexical_cast<uint32_t>(values[2]);
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
        std::vector<std::string> expectedColumns  = {"quoteSize", "stackLevels"};
        return parseHeader(input, expectedColumns, "StackerConfigs");
    }

    bool parseStackerConfigBody(std::ifstream& input) {
        std::function<void(std::vector<std::string>& values)> setter = [&configs = _stackerConfigs](std::vector<std::string>& values) {
            StackerConfig config;
            config._contract = values[0];
            config._quoteSize = boost::lexical_cast<uint32_t>(values[1]);
            config._numberOfLevels = boost::lexical_cast<uint32_t>(values[2]);
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
        std::vector<std::string> expectedColumns  = {"quoteExternal", "quoteSize", "width", "joinSize", "leaveSize", "payupQty", "payupOrders", "payupMultiple",
                                                      "maxPosInternal", "maxPosExternal", "commission"};
        return parseHeader(input, expectedColumns, "MMConfigs");
    }

    bool parseMarketMakersBody(std::ifstream& input) {
        std::function<void(std::vector<std::string>& values)> setter = [&configs = _marketMakerConfigs](std::vector<std::string>& values) {
            MarketMakerConfig config;
            config._contract = values[0];
            config._quoteExternal = values[1] == "TRUE";
            config._quoteSize = boost::lexical_cast<uint32_t>(values[2]);
            config._width = boost::lexical_cast<double>(values[3]);
            config._joinSize = boost::lexical_cast<uint32_t>(values[4]);
            config._leaveSize = boost::lexical_cast<uint32_t>(values[5]);
            config._payUpQty = boost::lexical_cast<uint32_t>(values[6]);
            config._payUpOrders = boost::lexical_cast<uint32_t>(values[7]);
            config._payUpMultiple = boost::lexical_cast<double>(values[8]);
            config._maxPosInternal = boost::lexical_cast<uint32_t>(values[9]);
            config._maxPosExternal = boost::lexical_cast<uint32_t>(values[10]);
            config._commission = boost::lexical_cast<double>(values[11]);
            configs.push_back(config);
        };
        return parseBody(input, setter);
    }

    bool parseBody(std::ifstream& input, std::function<void(std::vector<std::string>&)>& setter) {
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

            std::vector<std::string> values;
            boost::split(values, line, boost::is_any_of(","));

            if (values.empty()) {
                return false;
            }

            if (values[0] == "") {
                return true;
            }

            setter(values);
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
        std::vector<std::string> expectedColumns  = {"quoteExternal", "maxHedgeBuffer", "commission", "position"};
        return parseHeader(input, expectedColumns, "RVConfigs");
    }

    bool parseRelativeValueBody(std::ifstream& input) {
        std::function<void(std::vector<std::string>& values)> setter = [&configs = _relativeValueConfigs](std::vector<std::string>& values) {
            RelativeValueConfig config;
            config._contract = values[0];
            config._quoteExternal = values[1] == "TRUE";
            config._MinLeanBuffer = boost::lexical_cast<double>(values[2]);
            config._commision = boost::lexical_cast<double>(values[3]);
            config._position = boost::lexical_cast<int32_t>(values[4]);
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

        if (type != "PricingMatrix") {
            return false;
        }

        for (; it != tok.end(); ++it) {
            const std::string contractName = boost::algorithm::trim_copy(*it);
            if (contractName != "") {
                _pricingOutrights.push_back(contractName);
            }
        }

        return _pricingOutrights.size() != 0;
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

            if (row._vectorName == "") {
                return true;
            }

            for (; it != tok.end(); ++it) {
                const std::string valueAsString = boost::algorithm::trim_copy(*it);
                if (valueAsString != "") {
                    double value = boost::lexical_cast<double>(valueAsString);
                    row._values.push_back(value);
                }
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
        std::function<void(std::vector<std::string>& values)> setter = [&configs = _vectorConfigs](std::vector<std::string>& values) {
            VectorConfig config;
            config._vectorName = values[0];
            config._range = boost::lexical_cast<double>(values[1]);
            config._maxPos = boost::lexical_cast<uint32_t>(values[2]);
            config._width = boost::lexical_cast<double>(values[3]);
            config._step = boost::lexical_cast<double>(values[4]);
            config._longTermFair = boost::lexical_cast<double>(values[5]);
            config._useEMA = values[6] == "TRUE";
            config._EMALength = boost::lexical_cast<uint32_t>(values[7]);
            config._setToMarket = values[8] == "TRUE";
            configs.push_back(config);
        };
        return parseBody(input, setter);
    }

    bool parseVectorConfigHeader(std::ifstream& input) {
        std::vector<std::string> expectedColumns = {"range", "maxPos", "width", "step", "ltf", "EMA", "EMALength(secs)", "SetToMarket"};
        return parseHeader(input, expectedColumns, "VectorConfigs");
    }

    bool parseHeader(std::ifstream& input, std::vector<std::string>& expectedColumns, const std::string& expectedType, bool checkColumns = true) {
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

        std::vector<std::string> header;
        boost::split(header, line, boost::is_any_of(","));

        if (header.empty())
            return false;

        if (header[0] != expectedType) {
            return false;
        }

        if (checkColumns) {
            uint32_t columnIndex = 1;
            for (; columnIndex < header.size(); ++columnIndex) {
                const std::string name = header[columnIndex];
                if (name == "") {
                    break;
                }
                if (columnIndex > expectedColumns.size()) {
                    return false;
                }
                std::string& expected = expectedColumns[columnIndex - 1];
                if (expected != name) {
                    return false;
                }
            }
            return columnIndex - 1 == expectedColumns.size();
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

    template <typename T>
    void createContracts(T& symbols) {
        for (uint32_t internalId = 0; internalId < _pricingConfigs.size(); ++internalId) {
            auto& pc = _pricingConfigs[internalId];
            std::string symbol = pc._contract;
            hf::symbol_id_t sid = symbols.findSymbol(symbol);
            const hf::symbol_info_t& symbolInfo = symbols.getSymbol(sid);
            ConfigContract contract;
            contract._gid = symbolInfo.gid;
            contract._sid = symbolInfo.sid;
            contract._name = symbol;
            contract._tickSize = symbolInfo.tick_size;
            contract._venue = symbolInfo.getVenue();
            contract._internalId = internalId;
            if (symbolInfo.legs.size() > 0) {
                for (auto& symbolInfoleg : symbolInfo.legs) {
                    int32_t legIndex = findLegIndex(symbolInfoleg.gid);
                    if (legIndex == -1) {
                        throw std::runtime_error("could not find leg");
                    }
                    ConfigLeg leg;
                    leg._index = static_cast<uint32_t>(legIndex);
                    leg._ratio = ToIntByPoint_1(symbolInfoleg.ratio);
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

    ConfigContract* getContract(const std::string& name) {
        for (uint32_t i = 0; i < _allContracts.size(); ++i) {
            auto& contract = _allContracts[i];
            if (contract._name == name) {
                return &contract;
            }
        }
        return nullptr;
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
        for (int i = 0; i < update._update.cells_size(); ++i) {
            const proto::GridCell& gridCell = update._update.cells(i);
            auto& config = _pricingConfigs[gridCell.row()];
            PricingConfigsColumns columnType = static_cast<PricingConfigsColumns>(gridCell.col());

            switch (columnType) {
                case PricingConfigsColumns::UseWeighted: {
                    config._useWeightedMid = (1 == atoi(gridCell.value().c_str()));
                    break;
                }
                case PricingConfigsColumns::Weight: {
                    config._weight = (atof(gridCell.value().c_str()));
                    break;
                }
                default:
                    break;
            }
        }
        return true;
    }

    bool onManualFills(GridUpdate& update) {
        return true;
    }

    bool onMarketMakerParametersUpdate(GridUpdate& update) {
        for (int i = 0; i < update._update.cells_size(); ++i) {
            const proto::GridCell& gridCell = update._update.cells(i);
            auto& config = _marketMakerConfigs[gridCell.row()];
            MarketMakerParametersColumns columnType = static_cast<MarketMakerParametersColumns>(gridCell.col());

            switch (columnType) {
                case MarketMakerParametersColumns::QuoteExternal: {
                    config._quoteExternal = 1 == atoi(gridCell.value().c_str());
                    break;
                }
                case MarketMakerParametersColumns::QuoteSize: {
                    config._quoteSize = atoi(gridCell.value().c_str());
                    break;
                }
                case MarketMakerParametersColumns::JoinSize: {
                    config._joinSize = atoi(gridCell.value().c_str());
                    break;
                }
                case MarketMakerParametersColumns::LeaveSize: {
                    config._leaveSize = atoi(gridCell.value().c_str());
                    break;
                }
                case MarketMakerParametersColumns::MaxPosInterrnal: {
                    config._maxPosInternal = atoi(gridCell.value().c_str());
                    break;
                }
                case MarketMakerParametersColumns::MaxPosExternal: {
                    config._maxPosExternal = atoi(gridCell.value().c_str());
                    break;
                }
                case MarketMakerParametersColumns::PayUpOrders: {
                    config._payUpOrders = atoi(gridCell.value().c_str());
                    break;
                }
                case MarketMakerParametersColumns::PayUpQty: {
                    config._payUpQty = atoi(gridCell.value().c_str());
                    break;
                }
                case MarketMakerParametersColumns::PayUpMultiple: {
                    config._payUpMultiple = atof(gridCell.value().c_str());
                }
                case MarketMakerParametersColumns::Width: {
                    config._width = atoi(gridCell.value().c_str());
                    break;
                }
                case MarketMakerParametersColumns::Commission: {
                    config._commission = atoi(gridCell.value().c_str());
                    break;
                }
            }
        }
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
    std::vector<MarketMakerConfig> _marketMakerConfigs;
    std::vector<StackerConfig> _stackerConfigs;
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
