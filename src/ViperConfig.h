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
#include "Logger.h"
#include <boost/algorithm/string.hpp>
#include <boost/format.hpp>

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

struct PricingRow {
    std::string _vectorName;
    std::vector<double> _values;
};

struct StackerConfig {
    std::string _contract;
    uint32_t _quoteSize;
    uint32_t _numberOfLevels;
    bool _enabled;
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
    double _commission;
    int32_t _position;
    double _width;
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
    bool _enabled;
};

struct ExecutionConfig {
    std::string _contract;
    uint32_t _quoteBanner;
    uint32_t _fillBaner;
};

struct ViperConfig {

    const std::string PRICING_MATRIX = "PricingMatrix";
    const std::string VECTOR_CONFIGS = "VectorConfigs";
    const std::string RV_CONFIGS = "RVConfigs";
    const std::string MM_CONFIGS = "MMConfigs";
    const std::string STACKER_CONFIGS = "StackerConfigs";
    const std::string EXECUTION_CONFIGS = "ExecutionConfigs";
    const std::string LEAN_MATRIX = "LeanMatrix";
    const std::string PRICING_DEFINITIONS = "PricingDefinitions";
    const std::string PRICING_CONFIGS = "PricingConfigs";

    ViperConfig()
    :
        _updateSelector(*this)
    {}

    template <typename SymbolsT>
    bool initialize(const std::string& fileName, SymbolsT& symbols) {
        _fileName = fileName;

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

        _logger.logInfo("Config file successfully loaded.");

        return good;
    }

    typedef const boost::tokenizer<boost::char_separator<char>>::iterator TokenizerItT;

    bool loadLeanMatrix(std::ifstream& input) {
        _logger.logInfo("Loading Lean Matrix");

        bool good = parseLeanMatrixHeader(input);

        if (!good) {
            return false;
        }

        return parseLeanMatrixBody(input);
    }

    bool parseLeanMatrixHeader(std::ifstream& input) {
        std::vector<std::string> expectedColumns;
        return parseHeader(input, expectedColumns, LEAN_MATRIX, false);
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
        _logger.logInfo("Loading Pricing Definitions");

        bool good = parsePricingDefinitionsHeader(input);

        if (!good) {
            return false;
        }

        return parseLPricingDefinitionsBody(input);
    }

    bool parsePricingDefinitionsHeader(std::ifstream& input) {
        std::vector<std::string> expectedColumns;
        return parseHeader(input, expectedColumns, PRICING_DEFINITIONS, false);
    }

    bool parseLPricingDefinitionsBody(std::ifstream& input) {
        return parseContractMatrix(input, _pricingDefinitions);
    }

    bool loadPricingConfigs(std::ifstream& input) {
        _logger.logInfo("Loading Pricing Config");

        bool good = parsePricingConfigsHeader(input);

        if (!good) {
            return false;
        }

        return parsePricingConfigBody(input);
    }

    bool parsePricingConfigsHeader(std::ifstream& input) {
        return parseHeader(input, _pricingConfigsHeader, PRICING_CONFIGS);
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
        _logger.logInfo("Loading Execution Config");

        bool good = parseExecutionConfigsHeader(input);

        if (!good) {
            return false;
        }

        return parseExecutionConfigBody(input);
    }

    bool parseExecutionConfigsHeader(std::ifstream& input) {
        return parseHeader(input, _executionConfigHeader, EXECUTION_CONFIGS);
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
        _logger.logInfo("Loading Stacker Config");

        bool good = parseStackerConfigsHeader(input);

        if (!good) {
            return false;
        }

        return parseStackerConfigBody(input);
    }

    bool parseStackerConfigsHeader(std::ifstream& input) {
        return parseHeader(input, _stackerConfigHeader, STACKER_CONFIGS);
    }

    bool parseStackerConfigBody(std::ifstream& input) {
        std::function<void(std::vector<std::string>& values)> setter = [&configs = _stackerConfigs](std::vector<std::string>& values) {
            StackerConfig config;
            config._contract = values[0];
            config._enabled = values[1] == "TRUE";
            config._quoteSize = boost::lexical_cast<uint32_t>(values[2]);
            config._numberOfLevels = boost::lexical_cast<uint32_t>(values[3]);
            configs.push_back(config);
        };
        return parseBody(input, setter);
    }

    bool loadMarketMakersConfig(std::ifstream& input) {
        _logger.logInfo("Loading Market Makers Config");

        bool good = parseMarketMakersHeader(input);

        if (!good) {
            return false;
        }

        return parseMarketMakersBody(input);
    }

    bool parseMarketMakersHeader(std::ifstream& input) {
        return parseHeader(input, _marketMakerHeader, MM_CONFIGS);
    }

    bool parseMarketMakersBody(std::ifstream& input) {
        std::function<void(std::vector<std::string>& values)> setter = [&configs = _marketMakerConfigs](std::vector<std::string>& values) {
            MarketMakerConfig config;
            config._contract = values[0];
            config._enabled = values[1] == "TRUE";
            config._quoteExternal = values[2] == "TRUE";
            config._quoteSize = boost::lexical_cast<uint32_t>(values[3]);
            config._width = boost::lexical_cast<double>(values[4]);
            config._joinSize = boost::lexical_cast<uint32_t>(values[5]);
            config._leaveSize = boost::lexical_cast<uint32_t>(values[6]);
            config._payUpQty = boost::lexical_cast<uint32_t>(values[7]);
            config._payUpOrders = boost::lexical_cast<uint32_t>(values[8]);
            config._payUpMultiple = boost::lexical_cast<double>(values[9]);
            config._maxPosInternal = boost::lexical_cast<uint32_t>(values[10]);
            config._maxPosExternal = boost::lexical_cast<uint32_t>(values[11]);
            config._commission = boost::lexical_cast<double>(values[12]);
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
        _logger.logInfo("Loading Relative Value Config");

        bool good = parseRelativeValueHeader(input);

        if (!good) {
            return false;
        }

        return parseRelativeValueBody(input);
    }

    bool parseRelativeValueHeader(std::ifstream& input) {
        return parseHeader(input, _relativeValueHeader, RV_CONFIGS);
    }

    bool parseRelativeValueBody(std::ifstream& input) {
        std::function<void(std::vector<std::string>& values)> setter = [&configs = _relativeValueConfigs](std::vector<std::string>& values) {
            RelativeValueConfig config;
            config._contract = values[0];
            config._quoteExternal = values[1] == "TRUE";
            config._MinLeanBuffer = boost::lexical_cast<double>(values[2]);
            config._commission = boost::lexical_cast<double>(values[3]);
            config._width = boost::lexical_cast<double>(values[4]);
            config._position = boost::lexical_cast<int32_t>(values[5]);
            configs.push_back(config);
        };
        return parseBody(input, setter);
    }

    bool loadPricingMatrix(std::ifstream& input) {
        _logger.logInfo("Loading Pricing Matrix");

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

        if (type != PRICING_MATRIX) {
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
        _logger.logInfo("Loading Vector Config");

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
            config._step = boost::lexical_cast<double>(values[3]);
            config._longTermFair = boost::lexical_cast<double>(values[4]);
            config._useEMA = values[5] == "TRUE";
            config._EMALength = boost::lexical_cast<uint32_t>(values[6]);
            config._setToMarket = values[7] == "TRUE";
            configs.push_back(config);
        };
        return parseBody(input, setter);
    }

    bool parseVectorConfigHeader(std::ifstream& input) {
        return parseHeader(input, _vectorConfigHeader, VECTOR_CONFIGS);
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
        _numberOfColumns = header.size();

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
            const bool numberOfColumnsGood = columnIndex - 1 == expectedColumns.size();

            if (!numberOfColumnsGood) {
                _logger.logError(boost::str(boost::format("Number of columns in header different than expected. %1% %2%") % (columnIndex - 1) % expectedColumns.size()));
                return false;
            }
        }

        return true;
    }

    void populatePricingMatrix(Matrix& matrix) {
        std::size_t rows = _pricingMatrix.size();
        std::size_t columns = _pricingMatrix[0]._values.size();
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
                    config._commission = atof(gridCell.value().c_str());
                    break;
                }
                case ContractParameterRVColumns::Width: {
                    config._width = atof(gridCell.value().c_str());
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
                    break;
                }
                case MarketMakerParametersColumns::Width: {
                    config._width = atoi(gridCell.value().c_str());
                    break;
                }
                case MarketMakerParametersColumns::Commission: {
                    config._commission = atoi(gridCell.value().c_str());
                    break;
                }
                case MarketMakerParametersColumns::Enabled: {
                    config._enabled = 1 == atoi(gridCell.value().c_str());
                    break;
                }
            }
        }
        return true;
    }

    bool onStackerParametersUpdate(GridUpdate& update) {
        for (int i = 0; i < update._update.cells_size(); ++i) {
            const proto::GridCell& gridCell = update._update.cells(i);
            auto& config = _stackerConfigs[gridCell.row()];
            StackerParametersColumns columnType = static_cast<StackerParametersColumns>(gridCell.col());

            switch (columnType) {
                case StackerParametersColumns::Enabled: {
                    config._enabled = 1 == atoi(gridCell.value().c_str());
                    break;
                }
                case StackerParametersColumns::QuoteSize: {
                    config._quoteSize = atoi(gridCell.value().c_str());
                    break;
                }
                case StackerParametersColumns::StackLevels: {
                    config._numberOfLevels = atoi(gridCell.value().c_str());
                    break;
                }
            }
        }
        return true;
    }

    bool onExecutionParametersUpdate(GridUpdate& update) {
        for (int i = 0; i < update._update.cells_size(); ++i) {
            const proto::GridCell& gridCell = update._update.cells(i);
            auto& config = _executionConfigs[gridCell.row()];
            ExecutionParametersColumns columnType = static_cast<ExecutionParametersColumns>(gridCell.col());

            switch (columnType) {
                case ExecutionParametersColumns::FillBanner: {
                    config._fillBaner = 1 == atoi(gridCell.value().c_str());
                    break;
                }
                case ExecutionParametersColumns::QuoteBanner: {
                    config._quoteBanner = atoi(gridCell.value().c_str());
                    break;
                }
            }
        }
        return true;
    }

    bool isContractToMarketMake(std::string& name, uint32_t& index) {
        for (uint32_t i = 0; i < _marketMakerConfigs.size(); ++i) {
            auto& config = _marketMakerConfigs[i];
            if (config._contract == name) {
                index = i;
                return true;
            }
        }
        return false;
    }

    void writeToFile() {
        std::ofstream output(_fileName.c_str(), std::ofstream::out);
        writePricingMatrix(output);
        writeVectorConfig(output);
        writeRelativeValueConfig(output);
        writeMarketMakersConfig(output);
        writeStackerConfigs(output);
        writeExecutionConfigs(output);
        writeLeanMatrix(output);
        writePricingDefinitions(output);
        writePricingConfigs(output);
    }

    void writePricingConfigs(std::ofstream& output) {
        writeHeader(output, PRICING_CONFIGS, _pricingConfigsHeader);
    }

    void writePricingDefinitions(std::ofstream& output) {
        writeContractMatrix(output, PRICING_DEFINITIONS, _pricingDefinitions);
    }

    void writeLeanMatrix(std::ofstream& output) {
        writeContractMatrix(output, LEAN_MATRIX, _leanMatrix);
    }

    void writeContractMatrix(std::ofstream& output, const std::string& name, std::vector<VectorWithContract>& matrix) {
        std::vector<std::string> contracts;
        for (auto& config : matrix) {
            contracts.push_back(config._contract);
        }
        writeHeader(output, LEAN_MATRIX, contracts);
        for (auto& config : matrix) {
            output << "," << config._contract;
            for (auto value : config._values) {
                output << "," << value;
            }
            fillInMissingCommas(output, contracts.size() + 1);
        }
    }

    void writeStackerConfigs(std::ofstream& output) {
        writeHeader(output, STACKER_CONFIGS, _stackerConfigHeader);
        for (auto& config : _stackerConfigs) {
            output << "," << config._contract;
            output << "," << config._quoteSize;
            output << "," << config._numberOfLevels;
            output << "," << config._contract;
            output << "," << getWriteBoolValue(config._enabled);
            fillInMissingCommas(output, _stackerConfigHeader.size() + 1);
        }
    }

    void writeExecutionConfigs(std::ofstream& output) {
        writeHeader(output, EXECUTION_CONFIGS, _executionConfigHeader);
        for (auto& config : _executionConfigs) {
            output << "," << config._contract;
            output << "," << config._fillBaner;
            output << "," << config._quoteBanner;
            fillInMissingCommas(output, _executionConfigHeader.size() + 1);
        }
    }

    void writePricingMatrix(std::ofstream& output) {
        writeHeader(output, PRICING_MATRIX, _pricingOutrights);
        std::size_t columnIndex = 0;
        for (auto& row : _pricingMatrix) {
            columnIndex = 0;
            output << row._vectorName << ",";
            for (; columnIndex < row._values.size(); ++columnIndex) {
                output << row._values[columnIndex];
            }
            fillInMissingCommas(output, columnIndex + 1);
        }
        writeEmptyLine(output);
    }

    void writeVectorConfig(std::ofstream& output) {
        writeHeader(output, VECTOR_CONFIGS, _vectorConfigHeader);
        for (auto& config : _vectorConfigs) {
            output << "," << config._vectorName;
            output << "," << config._range;
            output << "," << config._maxPos;
            output << "," << config._step;
            output << "," << getWriteBoolValue(config._useEMA);
            output << "," << config._EMALength;
            output << "," << getWriteBoolValue(config._setToMarket);
            output << "," << config._longTermFair;
            fillInMissingCommas(output, _vectorConfigs.size() + 1);
        }
    }

    void writeRelativeValueConfig(std::ofstream& output) {
        writeHeader(output, RV_CONFIGS, _relativeValueHeader);
        for (auto& config : _relativeValueConfigs) {
            output << "," << config._contract;
            output << "," << getWriteBoolValue(config._quoteExternal);
            output << "," << config._MinLeanBuffer;
            output << "," << config._commission;
            output << "," << config._position;
            output << "," << config._width;
            fillInMissingCommas(output, _relativeValueHeader.size() + 1);
        }
    }

    void writeMarketMakersConfig(std::ofstream& output) {
        writeHeader(output, MM_CONFIGS, _marketMakerHeader);
        for (auto& config : _marketMakerConfigs) {
            output << "," << config._contract;
            output << "," << getWriteBoolValue(config._quoteExternal);
            output << "," << config._quoteSize;
            output << "," << config._width;
            output << "," << config._joinSize;
            output << "," << config._leaveSize;
            output << "," << config._payUpQty;
            output << "," << config._payUpOrders;
            output << "," << config._payUpMultiple;
            output << "," << config._maxPosInternal;
            output << "," << config._maxPosExternal;
            output << "," << config._commission;
            output << "," << getWriteBoolValue(config._enabled);
            fillInMissingCommas(output, _marketMakerHeader.size() + 1);
        }
    }

    std::string getWriteBoolValue(bool b) {
        return b ? "TRUE" : "FALSE";
    }

    void writeEmptyLine(std::ofstream& output) {
        for (std::size_t columnIndex = 0; columnIndex < _numberOfColumns; ++columnIndex) {
            output << ",";
        }
    }

    void fillInMissingCommas(std::ofstream& output, std::size_t numberOfColumns) {
        for (std::size_t columnIndex = 0; columnIndex < numberOfColumns; ++columnIndex) {
            output << ",";
        }
    }

    void writeHeader(std::ofstream& output, const std::string& name, std::vector<std::string>& columns) {
        output << name;
        std::size_t columnIndex = 0;
        for (; columnIndex < _pricingOutrights.size(); ++columnIndex) {
            output << "," << _pricingOutrights[columnIndex];
        }
        for (; columnIndex < _numberOfColumns; ++columnIndex) {
            output << ",";
        }
    }

    std::vector<PricingRow> _pricingMatrix;
    std::vector<std::string> _pricingOutrights;

    std::vector<VectorConfig> _vectorConfigs;
    std::vector<std::string> _vectorConfigHeader = {"range", "maxPos", "step", "ltf", "EMA", "EMALength(secs)", "SetToMarket"};

    std::vector<RelativeValueConfig> _relativeValueConfigs;
    std::vector<std::string> _relativeValueHeader  = {"quoteExternal", "maxHedgeBuffer", "commission", "width", "position"};

    std::vector<MarketMakerConfig> _marketMakerConfigs;
    std::vector<std::string> _marketMakerHeader  = {"enabled", "quoteExternal", "quoteSize", "width", "joinSize", "leaveSize", "payupQty", "payupOrders", "payupMultiple",
                                                 "maxPosInternal", "maxPosExternal", "commission"};

    std::vector<StackerConfig> _stackerConfigs;
    std::vector<std::string> _stackerConfigHeader  = {"enabled", "quoteSize", "stackLevels"};

    std::vector<ExecutionConfig> _executionConfigs;
    std::vector<std::string> _executionConfigHeader  = {"quoteBanner", "fillBanner"};

    std::vector<VectorWithContract> _leanMatrix;
    std::vector<VectorWithContract> _pricingDefinitions;

    std::vector<PricingConfig> _pricingConfigs;
    std::vector<std::string> _pricingConfigsHeader = {"weights", "weightedMid", "establishedLevel"};

    std::vector<ConfigContract> _allContracts;
    uint32_t _numberOfOutrights = 0;

    UpdateSelector<ViperConfig> _updateSelector;

    std::string _fileName;
    std::size_t _numberOfColumns = 0;

    Logger _logger;
};

};

#endif //VIPER_VIPER_CONFIG_H
