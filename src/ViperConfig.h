#ifndef VIPER_VIPER_CONFIG_H
#define VIPER_VIPER_CONFIG_H

#include <boost/filesystem.hpp>
#include <boost/tokenizer.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/lexical_cast.hpp>
#include <fstream>
#include <md/sym/Symbols.h>
#include "messages/python.pb.h"
#include "MathStructures.h"
#include "Common.h"
#include "UpdateSelector.h"
#include <boost/algorithm/string.hpp>
#include <boost/format.hpp>
#include <iomanip>
#include <experimental/filesystem>
#include <unordered_map>
#include <unordered_set>

namespace Python {

struct ManualFill {
    uint32_t _contract;
    int32_t _size;
    uint32_t _type;
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
    uint32_t _stackSlack;
    int32_t _maxTicksOffTopLevel;
    bool _useGtc;
    bool _allowMultipleNewHedge;
};

struct GlobalConfig {
    std::string _key;
    std::string _value;
};

struct PNLConfig {
    std::string _strategy;
    uint32_t _limit;
    uint32_t _drawdown;
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
    hf::price_t _tickValue;
    hf::VENUE _venue;
    uint32_t _internalId;
    std::vector<ConfigLeg> _legs;
    uint32_t _marketMakeIndex;
    bool _toMarketMake = false;
    bool* _useGTC;
};

#define INVALID_INTERNAL_ID std::numeric_limits<uint32_t>::max()

struct VectorWithContract {
    std::string _contract;
    std::vector<int> _values;
};

struct PricingConfig {
    std::string _contract;
    double _weight;
    bool _useWeightedMid;
    uint32_t _establishedLevel;
    double _pricingMultiplier;
    double _quantityMultiplier;
    uint32_t _sweepTicks;
    uint32_t _sweepQuantity;
    int32_t _maxTicksWide;
};

struct VectorConfig {
    std::string _vectorName;
    double _range;
    uint32_t _maxPos;
    bool _useEMA;
    uint32_t _EMALength;
    bool _setToMarket;
    double _longTermFair;
    uint32_t _chartNumberOfDecimalPlaces;
    bool _hedged;
};

struct RelativeValueConfig {
    std::string _contract;
    double _hedgeTolerance;
    double _commission;
    double _width;
    double _pctStfMove;
    double _maxTicksThroughMarket;
    uint32_t _quoteSize;
    bool _quoteExternal;
    bool _staticQuote;
};

struct MarketMakerConfig {
    std::string _contract;
    uint32_t _quoteSize;
    double _quotePctTob;
    int32_t _maxTicksWide;
    double _widthPercentage[toUInt(Shifts::__current_marker__) + 1];
    uint32_t _joinSize;
    uint32_t _leaveSize;
    uint32_t _payUpQty;
    uint32_t _payUpOrders;
    double _payUpMultiple;
    uint32_t _maxPosInternal;
    uint32_t _maxPosExternal;
    double _commission;
    double _minInternalPriceChange;
    double _hedgeOffset;
    uint32_t _establishedLevel;
    int32_t _internalPosition;
    int32_t _externalPosition;
    bool _quoteExternal;
    bool _enabled;
    double _worsenPayupCondPercent;
};

const std::string QUOTE_BANNER_PERIOD = "quoteBannerMics";
const std::string FILL_BANNER_PERIOD = "fillBannerMics";
const std::string HEDGE_BANNER_MICS = "hedgeBannerMics";
const std::string NEGATIVE_SCALP_CLIPS = "negScalpClips";
const std::string NEGATIVE_SCALP_DOLLARS = "negScalpDollars";
const std::string NEGATIVE_SCALP_PERIOD = "negScalpPeriodMillis";
const std::string FILL_RATE_CLIPS = "fillRateClips";
const std::string FILL_RATE_SUM_TOTAL = "fillRateSumTotalQty";
const std::string FILL_RATE_PERIOD = "fillRatePeriodMillis";
const std::string ORDER_RATE_CLIPS = "orderRateClips";
const std::string ORDER_RATE_SUM_TOTAL = "orderRateSumTotalQty";
const std::string ORDER_RATE_PERIOD = "orderRatePeriodMillis";
const std::string PERCENT_MSG_HEDGES = "percentMsgHedges";
const std::string PERCENT_MSG_REDUCE_QUOTE = "percentMsgReduceQuote";
const std::string MSG_HEDGES_INTERVAL_MICS = "msgHedgesIntervalMics";
const std::string MSG_REDUCE_QUOTE_MICS = "msgReduceQuoteMics";
const std::string SELF_MATCH_BANNER_MICS = "selfMatchBannerMics";
const std::string RV_WIDTH_MULTIPLIER = "RVWidthMultiplier";

struct ViperConfig {

    const std::string PRICING_MATRIX = "PricingMatrix";
    const std::string VECTOR_CONFIGS = "FactorConfigs";
    const std::string RV_CONFIGS = "RVConfigs";
    const std::string MM_CONFIGS = "MMConfigs";
    const std::string STACKER_CONFIGS = "StackerConfigs";
    const std::string LEAN_MATRIX = "LeanMatrix";
    const std::string PRICING_CONFIGS = "PricingConfigs";
    const std::string GLOBAL_CONFIGS = "GlobalConfigs";
    const std::string PNL_CONFIGS = "PNLConfigs";

    ViperConfig() : _updateSelector(*this) {}

    static std::string formatStringVector(const std::vector<std::string>& values) {
        std::stringstream ss;
        bool first = 0;
        for(auto& s : values) {
            if (!first) ss << ", ";
            ss << s;
            first = false;
        }
        return ss.str();
    }

    template <typename SymbolsT>
    bool initialize(const std::string& modelFilename, const std::string& rvPosFilename, SymbolsT& symbols) {
        _fileName = modelFilename;
        _rvPosFileName = rvPosFilename;

        if(!tryGetInitialRVPos(rvPosFilename, _unallocatedRVPositionsFromFile)) {
            return false;
        }

        std::ifstream input(modelFilename.c_str(), std::ifstream::in);

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

        good = loadGlobalConfigs(input);

        if (!good) {
            return false;
        }

        good = loadPNLConfigs(input);

        if (!good) {
            return false;
        }

        good = loadLeanMatrix(input);

        if (!good) {
            return false;
        }

        good = loadPricingConfigs(input);

        if (!good) {
            return false;
        }

        good = loadAndCullInitialRVPositions(_unallocatedRVPositionsFromFile);

        if(!good) {
            return false;
        }

        input.close();

        validateInputs();

        createContracts(symbols);

        logging::GetLogger().logInfo("Config file successfully loaded.");

        return good;
    }

    bool tryGetInitialRVPos(const std::string& rvPosFilename, std::unordered_map<std::string, int>& initialContractToRVPos) {

        if(!std::experimental::filesystem::exists(rvPosFilename)) {
            logging::GetLogger().logError("RV Position file does not exist.  This version requires said file.  filename=" + rvPosFilename);
            return false;
        }

        std::ifstream input(rvPosFilename.c_str(), std::ifstream::in);

        if (!input.good()) {
            logging::GetLogger().logError("Unexpected error when opening RV Position file.  filename=" + rvPosFilename);
            return false;
        }

        const boost::char_separator<char> sep(",", "", boost::keep_empty_tokens);
        std::string line;
        std::getline(input, line);

        if (!input) {
            logging::GetLogger().logError("RV Position file doesn't appear to contain a header.  filename=" + rvPosFilename);
            return false;
        }

        boost::algorithm::trim(line);
        if (line.empty()) {
            logging::GetLogger().logError("RV Position file contains empty header.  filename=" + rvPosFilename);
            return false;
        }

        const boost::tokenizer<boost::char_separator<char> > tok(line, sep);
        auto it = tok.begin();
        if (it == tok.end()) {
            logging::GetLogger().logError("Could not tokenize header in RV Position file.  header=" + line);
            return false;
        }

        //Expected header: RVContract,position
        auto unexpectedColError = [this, &it](int colNum) {
            logging::GetLogger().logError(std::string("Unexpected column in RV Position file.  expected=") + _rvPositionHeader[colNum] + " discovered=" + *it);
        };
        if(*it != _rvPositionHeader[0]) {
            unexpectedColError(0);
            return false;
        }

        if(++it == tok.end()) {
            logging::GetLogger().logError("Only discovered 1 column in RV Position file header, expected 2");
            return false;
        }

        if(*it != _rvPositionHeader[1]) {
            unexpectedColError(1);
            return false;
        }

        std::function<void(std::vector<std::string>& values)> setter = [&initialContractToRVPos](std::vector<std::string>& values) {
            if(initialContractToRVPos.count(values[0])) {
                throw std::runtime_error("Duplicate key in RV price config");
            }
            if(values.size() != 2) {
                throw std::runtime_error("Wrong number of cols in RV Position file row.");
            }
            initialContractToRVPos[values[0]] = boost::lexical_cast<int>(values[1]);
        };

        return parseBody(input, setter);
    }

    typedef const boost::tokenizer<boost::char_separator<char>>::iterator TokenizerItT;

    bool loadLeanMatrix(std::ifstream& input) {
        logging::GetLogger().logInfo("Loading Lean Matrix");

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

    bool loadPricingConfigs(std::ifstream& input) {
        logging::GetLogger().logInfo("Loading Pricing Config");

        bool good = parsePricingConfigsHeader(input);

        if (!good) {
            return false;
        }

        return parsePricingConfigBody(input);
    }

    bool loadAndCullInitialRVPositions(std::unordered_map<std::string, int>& initialRVPositionsFromFile) {
        logging::GetLogger().logInfo("Loading initial RV positions");
        if(_pricingConfigs.empty()) {
            return false;
        }

        //Note _numberOfPricingContracts isn't init'd yet or we'd use it for size
        _rvPosition.resize(_pricingConfigs.size());

        for(unsigned i = 0; i < _pricingConfigs.size(); ++i) {
            const auto& contract = _pricingConfigs[i];
            auto iter = initialRVPositionsFromFile.find(contract._contract);
            if(iter != initialRVPositionsFromFile.end()) {
                auto& pos = _rvPosition[i];
                if(pos) {
                    logging::GetLogger().logError("Duplicate contract found in initial RV positions.  contract=" + iter->first);
                    return false;
                }
                _rvPosition[i] = iter->second;
                initialRVPositionsFromFile.erase(iter);
            }
        }

        return true;
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
            config._pricingMultiplier = boost::lexical_cast<double>(values[4]);
            config._quantityMultiplier = boost::lexical_cast<double>(values[5]);
            config._sweepTicks = boost::lexical_cast<uint32_t>(values[6]);
            config._sweepQuantity = boost::lexical_cast<uint32_t>(values[7]);
            config._maxTicksWide = boost::lexical_cast<int32_t>(values[8]);
            configs.push_back(config);
        };
        return parseBody(input, setter);
    }

    bool loadGlobalConfigs(std::ifstream& input) {
        logging::GetLogger().logInfo("Loading Global Config");

        bool good = parseHeader(input, _globalConfigHeader, GLOBAL_CONFIGS);

        if (!good) {
            return false;
        }

        return parseGlobalConfigBody(input);
    }

    bool parseGlobalConfigBody(std::ifstream& input) {
        std::function<void(std::vector<std::string>& values)> setter = [&configs = _globalConfigs](std::vector<std::string>& values) {

            if(!std::any_of(std::begin(ValidGlobalParameters), std::end(ValidGlobalParameters), [&values](const char* str) { return !values[0].compare(str); })) {
                throw std::runtime_error(values[0] + " as global parameter not supported by this version of Viper.");
            }

            GlobalConfig config;
            config._key = values[0];
            config._value = values[1];
            configs.push_back(config);
        };
        return parseBody(input, setter);
    }

    std::string getGlobal(const std::string& key) const {
        for (auto& pair : _globalConfigs) {
            if (pair._key == key) {
                return pair._value;
            }
        }
        throw std::runtime_error("could not find global key: " + key);
    }

    bool loadPNLConfigs(std::ifstream& input) {
        logging::GetLogger().logInfo("Loading PNL Config");

        bool good = parseHeader(input, _pnlConfigHeader, PNL_CONFIGS);

        if (!good) {
            return false;
        }

        return parsePnlConfigBody(input);
    }

    bool parsePnlConfigBody(std::ifstream& input) {
        std::function<void(std::vector<std::string>& values)> setter = [&configs = _pnlConfigs](std::vector<std::string>& values) {
            PNLConfig config;
            config._strategy = values[0];
            config._limit = boost::lexical_cast<uint32_t>(values[1]);
            config._drawdown = boost::lexical_cast<uint32_t>(values[2]);
            configs.push_back(config);
        };
        return parseBody(input, setter);
    }

    bool loadStackerConfigs(std::ifstream& input) {
        logging::GetLogger().logInfo("Loading Stacker Config");

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
        std::function<void(std::vector<std::string>& values)> setter = [&configs = _stackerConfigs, this](std::vector<std::string>& values) {
            StackerConfig config;
            config._contract = values[0];
            config._enabled = values[1] == "TRUE";
            config._quoteSize = boost::lexical_cast<uint32_t>(values[2]);
            config._numberOfLevels = boost::lexical_cast<uint32_t>(values[3]);
            config._stackSlack = boost::lexical_cast<uint32_t>(values[4]);
            config._maxTicksOffTopLevel = boost::lexical_cast<int32_t>(values[5]);
            config._useGtc = values[6] == "TRUE";
            config._allowMultipleNewHedge = values[7] == "TRUE";
            if((!_allowGTCs) && config._useGtc) {
                throw std::runtime_error("Refusing to enable GTCs in Viper config if not enabled (allowGTCs=true) in Rhone config.");
            }
            configs.push_back(config);
        };
        return parseBody(input, setter);
    }

    bool loadMarketMakersConfig(std::ifstream& input) {
        logging::GetLogger().logInfo("Loading Market Makers Config");

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
        std::function<void(std::vector<std::string>& values)> setter = [this, &configs = _marketMakerConfigs](std::vector<std::string>& values) {
            MarketMakerConfig config;
            uint32_t i = 0;
            config._contract = values[i++];
            config._enabled = values[i++] == "TRUE";
            config._quoteExternal = values[i++] == "TRUE";
            config._quoteSize = boost::lexical_cast<uint32_t>(values[i++]);
            config._quotePctTob = boost::lexical_cast<double>(values[i++]);
            for (uint32_t j = 0; j < toUInt(Shifts::__current_marker__); ++j) {
                config._widthPercentage[j] = boost::lexical_cast<double>(values[i++]);
            }
            config._widthPercentage[toUInt(Shifts::__current_marker__)] = config._widthPercentage[toUInt(_currentShift)];
            config._hedgeOffset = boost::lexical_cast<double>(values[i++]);
            config._joinSize = boost::lexical_cast<uint32_t>(values[i++]);
            config._leaveSize = boost::lexical_cast<uint32_t>(values[i++]);
            config._payUpQty = boost::lexical_cast<uint32_t>(values[i++]);
            config._payUpOrders = boost::lexical_cast<uint32_t>(values[i++]);
            config._payUpMultiple = boost::lexical_cast<double>(values[i++]);
            config._maxPosInternal = boost::lexical_cast<uint32_t>(values[i++]);
            config._maxPosExternal = boost::lexical_cast<uint32_t>(values[i++]);
            config._commission = boost::lexical_cast<double>(values[i++]);
            config._minInternalPriceChange = boost::lexical_cast<double>(values[i++]);
            config._establishedLevel = boost::lexical_cast<uint32_t>(values[i++]);
            config._maxTicksWide = boost::lexical_cast<int32_t>(values[i++]);
            config._worsenPayupCondPercent = boost::lexical_cast<double>(values[i++]);
            config._internalPosition = boost::lexical_cast<int32_t>(values[i++]);
            config._externalPosition = boost::lexical_cast<int32_t>(values[i++]);
            configs.push_back(config);
        };
        return parseBody(input, setter);
    }

    bool parseBody(std::ifstream& input, std::function<void(std::vector<std::string>&)>& setter) {
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

            try
            {
                setter(values);
            }
            catch(std::exception& ex)
            {
                std::stringstream ss;
                ss << " Caught [" << ex.what() << "] while parsing {" << formatStringVector(values);

                logging::GetLogger().logInfo(ss.str());
                throw;
            }

        }
        return true;
    }

    bool loadRelativeValueConfig(std::ifstream& input) {
        logging::GetLogger().logInfo("Loading Relative Value Config");

        bool good = parseRelativeValueHeader(input);

        if (!good) {
            logging::GetLogger().logInfo("Relative Value config: invalid header");
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
            config._hedgeTolerance = boost::lexical_cast<double>(values[2]);
            config._commission = boost::lexical_cast<double>(values[3]);
            config._width = boost::lexical_cast<double>(values[4]);
            config._pctStfMove = boost::lexical_cast<double>(values[5]);
            config._maxTicksThroughMarket = boost::lexical_cast<double>(values[6]);
            config._staticQuote = values[7] == "TRUE";
            config._quoteSize = boost::lexical_cast<uint32_t>(values[8]);
            configs.push_back(config);
        };
        return parseBody(input, setter);
    }

    bool loadPricingMatrix(std::ifstream& input) {
        logging::GetLogger().logInfo("Loading Pricing Matrix");

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
        logging::GetLogger().logInfo("Loading Vector Config");
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
            config._longTermFair = boost::lexical_cast<double>(values[3]);
            config._useEMA = values[4] == "TRUE";
            config._EMALength = boost::lexical_cast<uint32_t>(values[5]);
            config._setToMarket = values[6] == "TRUE";
            config._chartNumberOfDecimalPlaces = boost::lexical_cast<uint32_t>(values[7]);
            config._hedged = values[8] == "TRUE";
            configs.push_back(config);
        };
        return parseBody(input, setter);
    }

    bool parseVectorConfigHeader(std::ifstream& input) {
        return parseHeader(input, _vectorConfigHeader, VECTOR_CONFIGS);
    }

    bool parseHeader(std::ifstream& input, std::vector<std::string>& expectedColumns, const std::string& expectedType, bool checkColumns = true) {
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
                logging::GetLogger().logError(boost::str(boost::format("Number of columns in header different than expected. %1% %2%") % (columnIndex - 1) % expectedColumns.size()));
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
            contract._tickValue = symbolInfo.tick_value;
            contract._venue = symbolInfo.getVenue();
            contract._internalId = internalId;
            contract._toMarketMake = isContractToMarketMake(symbol, contract._marketMakeIndex);
            contract._useGTC = new bool(false);
            if (symbolInfo.legs.size() > 0) {
                for (auto& symbolInfoleg : symbolInfo.legs) {
                    int32_t legIndex = findLegIndex(symbolInfoleg.gid);
                    if (legIndex == -1) {
                        throw std::runtime_error(boost::str(boost::format("could not find leg. gid=%1% symbol=%2%") % symbolInfoleg.gid % symbol));
                    }
                    ConfigLeg leg;
                    leg._index = static_cast<uint32_t>(legIndex);
                    leg._ratio = ToIntByPoint_1(symbolInfoleg.ratio);
                    contract._legs.push_back(leg);
                }
            }
            _allContracts.push_back(contract);
            _sidToInternalId.emplace(contract._sid, contract._internalId);
            _symbolToInterId.emplace(contract._name, contract._internalId);
        }
        _numberOfPricingContracts = _pricingOutrights.size();
    }

    int32_t findLegIndex(hf::symbol_gid_t gid) const {
        for (uint32_t index = 0; index < _allContracts.size(); ++index) {
            const ConfigContract& contract = _allContracts[index];
            if (contract._gid == gid) {
                return index;
            }
        }
        return -1;
    }

    const ConfigContract* getContract(const std::string& name) const {
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
                case VectorParametersColumns::Hedged: {
                    config._hedged = (1 == atoi(gridCell.value().c_str()));
                    break;
                }
                default:
                    break;
            }
        }
        return true;
    }

    void updateVectorLtf(uint32_t vectorIndex, double value) {
        VectorConfig& config = _vectorConfigs[vectorIndex];
        config._longTermFair = value;
    }

    bool onRvContractParametersUpdate(GridUpdate& update) {
        for (int i = 0; i < update._update.cells_size(); ++i) {
            const proto::GridCell& gridCell = update._update.cells(i);
            uint32_t contractIndex = gridCell.row() - _indexOfFirstTradedContract;
            auto& config = _relativeValueConfigs[contractIndex];
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
                    break;
                }
                case ContractParameterRVColumns::HedgeTolerance: {
                    config._hedgeTolerance = atof(gridCell.value().c_str());
                    break;
                }
                case ContractParameterRVColumns::PCTStfMove: {
                    config._pctStfMove = atof(gridCell.value().c_str());
                    break;
                }
                case ContractParameterRVColumns::MaxTicksThroughMarket: {
                    config._maxTicksThroughMarket = atof(gridCell.value().c_str());
                    break;
                }
                case ContractParameterRVColumns::StaticQuote: {
                    config._staticQuote = (1 == atoi(gridCell.value().c_str()));
                    break;
                }
                case ContractParameterRVColumns::QuoteSize: {
                    config._quoteSize = boost::lexical_cast<uint32_t>(gridCell.value());
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
            uint32_t rowIndex = gridCell.row() - _indexOfFirstTradedContract;
            uint32_t colIndex = gridCell.col() - _indexOfFirstTradedContract;
            _leanMatrix[rowIndex]._values[colIndex] = (1 == atoi(gridCell.value().c_str()));
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
                case PricingConfigsColumns::EstablishedLevel: {
                    config._establishedLevel = atoi(gridCell.value().c_str());
                    break;
                }
                case PricingConfigsColumns::SweepTicks: {
                    config._sweepTicks = atoi(gridCell.value().c_str());
                    break;
                }
                case PricingConfigsColumns::SweepQty: {
                    config._sweepQuantity = atoi(gridCell.value().c_str());
                    break;
                }
                case PricingConfigsColumns::MaxTicksWide: {
                    config._maxTicksWide = atoi(gridCell.value().c_str());
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
        std::unordered_set<uint32_t> updatedWith;
        for (int i = 0; i < update._update.cells_size(); ++i) {
            const proto::GridCell& gridCell = update._update.cells(i);
            uint32_t contractIndex = gridCell.row() - _indexOfFirstTradedContract;
            auto& config = _marketMakerConfigs[contractIndex];
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
                case MarketMakerParametersColumns::QuotePctTob: {
                    config._quotePctTob = atoi(gridCell.value().c_str());
                    break;
                }
                case MarketMakerParametersColumns::HedgeOffset: {
                    config._hedgeOffset = boost::lexical_cast<double>((gridCell.value()));
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
                case MarketMakerParametersColumns::AsiaWidth: {
                    config._widthPercentage[toUInt(Shifts::Asia)] = boost::lexical_cast<double>((gridCell.value()));
                    if (_currentShift == Shifts::Asia) {
                        config._widthPercentage[toUInt(Shifts::__current_marker__)] = config._widthPercentage[toUInt(Shifts::Asia)];
                        updatedWith.emplace(contractIndex);
                    }
                    break;
                }
                case MarketMakerParametersColumns::EuroWidth: {
                    config._widthPercentage[toUInt(Shifts::Europe)] = boost::lexical_cast<double>((gridCell.value()));
                    if (_currentShift == Shifts::Europe) {
                        config._widthPercentage[toUInt(Shifts::__current_marker__)] = config._widthPercentage[toUInt(Shifts::Europe)];
                        updatedWith.emplace(contractIndex);
                    }
                    break;
                }
                case MarketMakerParametersColumns::USWidth: {
                    config._widthPercentage[toUInt(Shifts::US)] = boost::lexical_cast<double>((gridCell.value()));
                    if (_currentShift == Shifts::US) {
                        config._widthPercentage[toUInt(Shifts::__current_marker__)] = config._widthPercentage[toUInt(Shifts::US)];
                        updatedWith.emplace(contractIndex);
                    }
                    break;
                }
                case MarketMakerParametersColumns::Commission: {
                    config._commission = boost::lexical_cast<double>((gridCell.value()));
                    break;
                }
                case MarketMakerParametersColumns::Enabled: {
                    config._enabled = 1 == atoi(gridCell.value().c_str());
                    break;
                }
                case MarketMakerParametersColumns::MinInternalPriceChange: {
                    config._minInternalPriceChange = boost::lexical_cast<double>((gridCell.value()));
                    break;
                }
                case MarketMakerParametersColumns::EstablishedLevel: {
                    config._establishedLevel = atoi(gridCell.value().c_str());
                    break;
                }
                case MarketMakerParametersColumns::MaxTicksWide: {
                    config._maxTicksWide = atoi(gridCell.value().c_str());
                    break;
                }
                case MarketMakerParametersColumns::WorsenPayupCondPercent : {
                    config._worsenPayupCondPercent = boost::lexical_cast<double>(gridCell.value());
                    break;
                }
                default:
                    break;
            }
        }

        if (!updatedWith.empty()) {
            for (auto contractId : updatedWith) {
                auto cell = update._update.add_cells();
                auto& config = _marketMakerConfigs[contractId];
                cell->set_row(contractId + _indexOfFirstTradedContract);
                cell->set_col(toUInt(MarketMakerParametersColumns::Width));
                cell->set_value(std::to_string(config._widthPercentage[toUInt(Shifts::__current_marker__)]));
            }
        }
        return true;
    }

    bool onStackerParametersUpdate(GridUpdate& update) {
        for (int i = 0; i < update._update.cells_size(); ++i) {
            const proto::GridCell& gridCell = update._update.cells(i);
            uint32_t contractIndex = gridCell.row() - _indexOfFirstTradedContract;
            auto& config = _stackerConfigs.at(contractIndex);
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
                case StackerParametersColumns::StackSlack: {
                    config._stackSlack = atoi(gridCell.value().c_str());
                    break;
                }
                case StackerParametersColumns::MaxTicksOffTopLevel: {
                    config._maxTicksOffTopLevel = atoi(gridCell.value().c_str());
                    break;
                }
                case StackerParametersColumns::UseGTC:
                    config._useGtc = 1 == atoi(gridCell.value().c_str());
                    break;
                case StackerParametersColumns::AllowMultipleNewHedge:
                    config._allowMultipleNewHedge = 1 == atoi(gridCell.value().c_str());
                    break;
            }
        }
        return true;
    }

    bool onPnlParametersUpdate(GridUpdate& update) {
        for (int i = 0; i < update._update.cells_size(); ++i) {
            const proto::GridCell& gridCell = update._update.cells(i);
            PNLRows rowType = static_cast<PNLRows>(gridCell.row());
            PNLConfig* config = getPnlConfig(rowType);
            PNLColumns columnType = static_cast<PNLColumns>(gridCell.col());
            switch (columnType) {
                case PNLColumns::PnlLimit: {
                    std::string value = gridCell.value();
                    config->_limit = boost::lexical_cast<uint32_t>(value);
                    break;
                }
                case PNLColumns::DrawdownLimit: {
                    std::string value = gridCell.value();
                    config->_drawdown = boost::lexical_cast<uint32_t>(value);
                    break;
                }
                default:
                    break;
            }
        }
        return true;
    }

    PNLConfig* getPnlConfig(PNLRows rowType) {
        std::string name;
        switch (rowType) {
            case PNLRows::MarketMakers: {
                name = "MM";
                break;
            }
            case PNLRows::RelativeValue: {
                name = "RV";
                break;
            }
            case PNLRows::Total: {
                name = "Total";
                break;
            }
            default:
                break;
        }
        for (PNLConfig& config : _pnlConfigs) {
            if (config._strategy == name) {
                return &config;
            }
        }
        return nullptr;
    }

    bool onGlobalConfigsUpdate(GridUpdate& update) {
        for (int i = 0; i < update._update.cells_size(); ++i) {
            const proto::GridCell& gridCell = update._update.cells(i);
            auto& config = _globalConfigs[gridCell.row()];
            config._value = gridCell.value();
        }
        return true;
    }

    bool isContractToMarketMake(const std::string& name, uint32_t& index) const {
        for (uint32_t i = 0; i < _marketMakerConfigs.size(); ++i) {
            auto& config = _marketMakerConfigs[i];
            if (config._contract == name) {
                index = i;
                return true;
            }
        }
        return false;
    }

    void addPosition(uint32_t contractIndex, hf::qty_t position, StrategyId strategy, bool internal) {
        uint32_t index = contractIndex - _indexOfFirstTradedContract;
        if (strategy == StrategyId::RelativeValue) {
            ConfigContract& contract = _allContracts[contractIndex];
            if(contract._legs.size() > 1) {
                for(unsigned i = 0; i < contract._legs.size(); ++i) {
                    auto& leg = contract._legs[i];
                    _rvPosition[leg._index] += position * leg._ratio;
                }
            }
            else {
                _rvPosition[contractIndex] += position;
            }
        } else {
            if (internal) {
                _marketMakerConfigs[index]._internalPosition += static_cast<int32_t>(position);
            } else {
                _marketMakerConfigs[index]._externalPosition += static_cast<int32_t>(position);
            }
        }
    }

    void writeToFile() {

        auto writeFile = [](const std::string& filename, auto&& gen) {
            std::string tempFilename = filename + ".temp";
            std::ofstream output(tempFilename.c_str(), std::ofstream::out);
            gen(output);
            output.close();
            std::experimental::filesystem::rename(tempFilename, filename);
        };

        writeFile(_fileName, [this](std::ofstream& output) {
            output.precision(12);
            writePricingMatrix(output);
            writeVectorConfig(output);
            writeRelativeValueConfig(output);
            writeMarketMakersConfig(output);
            writeStackerConfigs(output);
            writeGlobalConfigs(output);
            writePnlConfigs(output);
            writeLeanMatrix(output);
            writePricingConfigs(output);
        });

        writeFile(_rvPosFileName, [this](std::ofstream& output) {
            writeRVPos(output);
        });
    }

    void writePnlConfigs(std::ofstream& output) const {
        writeHeader(output, PNL_CONFIGS, _pnlConfigHeader);
        for (auto& config : _pnlConfigs) {
            output << config._strategy;
            output << "," << config._limit;
            output << "," << config._drawdown;
            fillInMissingCommas(output, _pnlConfigHeader.size());
        }
        writeEmptyLine(output);
    }

    void writeGlobalConfigs(std::ofstream& output) const {
        writeHeader(output, GLOBAL_CONFIGS, _globalConfigHeader);
        for (auto& config : _globalConfigs) {
            output << config._key;
            output << "," << config._value;
            fillInMissingCommas(output, _globalConfigHeader.size());
        }
        writeEmptyLine(output);
    }

    void writePricingConfigs(std::ofstream& output) const {
        writeHeader(output, PRICING_CONFIGS, _pricingConfigsHeader);
        for (auto& config : _pricingConfigs) {
            output << config._contract;
            output << "," << config._weight;
            output << "," <<  getWriteBoolValue(config._useWeightedMid);
            output << "," << config._establishedLevel;
            output << "," << config._pricingMultiplier;
            output << "," << config._quantityMultiplier;
            output << "," << config._sweepTicks;
            output << "," << config._sweepQuantity;
            output << "," << config._maxTicksWide;
            fillInMissingCommas(output, _pricingConfigsHeader.size());
        }
        writeEmptyLine(output);
    }

    void writeRVPos(std::ofstream& output) {
        if(_numberOfPricingContracts) {
            output << _rvPositionHeader[0] << "," << _rvPositionHeader[1] << "\n";
            for (unsigned i = 0; i < _numberOfPricingContracts; ++i) {
                output << _pricingConfigs[i]._contract << ',' << _rvPosition[i] << "\n";
            }
        }
    }

    void writeLeanMatrix(std::ofstream& output) {
        writeContractMatrix(output, LEAN_MATRIX, _leanMatrix);
    }

    void writeContractMatrix(std::ofstream& output, const std::string& name, const std::vector<VectorWithContract>& matrix) const {
        std::vector<std::string> contracts;
        for (const auto& config : matrix) {
            contracts.push_back(config._contract);
        }
        writeHeader(output, name, contracts);
        for (const auto& config : matrix) {
            output << config._contract;
            for (auto value : config._values) {
                output << "," << value;
            }
            fillInMissingCommas(output, contracts.size());
        }
        writeEmptyLine(output);
    }

    void writeStackerConfigs(std::ofstream& output) const {
        writeHeader(output, STACKER_CONFIGS, _stackerConfigHeader);
        for (auto& config : _stackerConfigs) {
            output << config._contract;
            output << "," << getWriteBoolValue(config._enabled);
            output << "," << config._quoteSize;
            output << "," << config._numberOfLevels;
            output << "," << config._stackSlack;
            output << "," << config._maxTicksOffTopLevel;
            output << "," << getWriteBoolValue(config._useGtc);
            output << "," << getWriteBoolValue(config._allowMultipleNewHedge);
            fillInMissingCommas(output, _stackerConfigHeader.size());
        }
        writeEmptyLine(output);
    }

    void writePricingMatrix(std::ofstream& output) const {
        writeHeader(output, PRICING_MATRIX, _pricingOutrights);
        std::size_t columnIndex = 0;
        for (auto& row : _pricingMatrix) {
            columnIndex = 0;
            output << row._vectorName << ",";
            for (; columnIndex < row._values.size(); ++columnIndex) {
                output << row._values[columnIndex] << ",";
            }
            fillInMissingCommas(output, columnIndex);
        }
        writeEmptyLine(output);
    }

    void writeVectorConfig(std::ofstream& output) const {
        writeHeader(output, VECTOR_CONFIGS, _vectorConfigHeader);
        for (const auto& config : _vectorConfigs) {
            output << config._vectorName;
            output << "," << config._range;
            output << "," << config._maxPos;
            output << "," << config._longTermFair;
            output << "," << getWriteBoolValue(config._useEMA);
            output << "," << config._EMALength;
            output << "," << getWriteBoolValue(config._setToMarket);
            output << "," << config._chartNumberOfDecimalPlaces;
            output << "," << getWriteBoolValue(config._hedged);
            fillInMissingCommas(output, _vectorConfigs.size());
        }
        writeEmptyLine(output);
    }

    void writeRelativeValueConfig(std::ofstream& output) const {
        writeHeader(output, RV_CONFIGS, _relativeValueHeader);
        for (auto& config : _relativeValueConfigs) {
            output << config._contract;
            output << "," << getWriteBoolValue(config._quoteExternal);
            output << "," << config._hedgeTolerance;
            output << "," << config._commission;
            output << "," << config._width;
            output << "," << config._pctStfMove;
            output << "," << config._maxTicksThroughMarket;
            output << "," << getWriteBoolValue(config._staticQuote);
            output << "," << config._quoteSize;
            fillInMissingCommas(output, _relativeValueHeader.size());
        }
        writeEmptyLine(output);
    }

    void writeMarketMakersConfig(std::ofstream& output) const {
        writeHeader(output, MM_CONFIGS, _marketMakerHeader);
        for (auto& config : _marketMakerConfigs) {
            output << config._contract;
            output << "," << getWriteBoolValue(config._enabled);
            output << "," << getWriteBoolValue(config._quoteExternal);
            output << "," << config._quoteSize;
            output << "," << config._quotePctTob;
            for (uint32_t i = 0; i < toUInt(Shifts::__current_marker__); ++i) {
                output << "," << config._widthPercentage[i];
            }
            output << "," << config._hedgeOffset;
            output << "," << config._joinSize;
            output << "," << config._leaveSize;
            output << "," << config._payUpQty;
            output << "," << config._payUpOrders;
            output << "," << config._payUpMultiple;
            output << "," << config._maxPosInternal;
            output << "," << config._maxPosExternal;
            output << "," << config._commission;
            output << "," << config._minInternalPriceChange;
            output << "," << config._establishedLevel;
            output << "," << config._maxTicksWide;
            output << "," << config._worsenPayupCondPercent;
            output << "," << config._internalPosition;
            output << "," << config._externalPosition;
            fillInMissingCommas(output, _marketMakerHeader.size());
        }
        writeEmptyLine(output);
    }

    static std::string getWriteBoolValue(bool b) {
        return b ? "TRUE" : "FALSE";
    }

    void writeEmptyLine(std::ofstream& output) const {
        for (std::size_t columnIndex = 1; columnIndex < _numberOfColumns; ++columnIndex) {
            output << ",";
        }
        output << std::endl;
    }

    void fillInMissingCommas(std::ofstream& output, std::size_t numberOfColumns) const {
        for (std::size_t columnIndex = numberOfColumns + 1; columnIndex < _numberOfColumns; ++columnIndex) {
            output << ",";
        }
        output << std::endl;
    }

    void writeHeader(std::ofstream& output, const std::string& name, const std::vector<std::string>& columns) const {
        output << name;
        std::size_t columnIndex = 0;
        for (; columnIndex < columns.size(); ++columnIndex) {
            output << "," << columns[columnIndex];
        }
        for (; columnIndex + 1 < _numberOfColumns; ++columnIndex) {
            output << ",";
        }
        output << std::endl;
    }

    const PNLConfig* getPnlConfig(const std::string& strat) const {
        for (auto& config : _pnlConfigs) {
            if (config._strategy == strat) {
                return &config;
            }
        }
        throw std::runtime_error("error getting pnl config");
    }

    uint32_t getInternalId(hf::symbol_id_t sid) const {
        auto it = _sidToInternalId.find(sid);
        return (it != _sidToInternalId.end()) ? it->second : INVALID_INTERNAL_ID;
    }

    uint32_t getInternalId(const std::string& name) {
        auto it = _symbolToInterId.find(name);
        if (it != _symbolToInterId.end()) {
            return it->second;
        }
        throw std::runtime_error("could not map symbol name to internal id. " + name);
    }

    std::vector<PricingRow> _pricingMatrix;
    std::vector<std::string> _pricingOutrights;
    uint32_t _numberOfPricingContracts;

    std::vector<VectorConfig> _vectorConfigs;
    std::vector<std::string> _vectorConfigHeader = {"range", "maxPos", "ltf", "EMA", "EMALength(secs)", "SetToMarket", "ChartNumberOfDecimals", "Hedged"};

    std::vector<RelativeValueConfig> _relativeValueConfigs;
    std::vector<int32_t> _rvPosition;
    std::vector<std::string> _relativeValueHeader  = {"quoteExternal", "hedgeTolerance", "commission", "RVWidth", "%stfMove", "maxTicksThroughMarket", "staticQuote", "quoteSize" };
    static constexpr const char* _rvPositionHeader[] = {"RVContract", "position"};

    std::vector<MarketMakerConfig> _marketMakerConfigs;
    std::vector<std::string> _marketMakerHeader = {
            "enabled",
            "quoteExternal",
            "quoteSize",
            "quotePctTob",
            "AsiaWidthPercent",
            "EuroWidthPercent",
            "USWidthPercent",
            "hedgeOffset",
            "joinSize",
            "leaveSize",
            "payupQty",
            "payupOrders",
            "payupMultiple",
            "maxPosInternal",
            "maxPosExternal",
            "commission",
            "minInternalPriceChange",
            "MMEstablishedLevel",
            "maxTicksWide",
            "MMWorsenPayUpCondPct",
            "internalPosition",
            "externalPosition"
    };

    std::vector<StackerConfig> _stackerConfigs;
    std::vector<std::string> _stackerConfigHeader  = {"enabled", "quoteSize", "stackLevels", "stackSlack", "maxTicksOffTopLevel", "useGTC", "allowMultipleNewHedge"};

    std::vector<GlobalConfig> _globalConfigs;
    std::vector<std::string> _globalConfigHeader  = {"Value"};

    std::vector<PNLConfig> _pnlConfigs;
    std::vector<std::string> _pnlConfigHeader  = {"Limit", "Drawdown"};

    std::vector<VectorWithContract> _leanMatrix;

    std::vector<PricingConfig> _pricingConfigs;
    std::vector<std::string> _pricingConfigsHeader = {"weights", "weightedMid", "establishedLevel", "pricingMultiplier", "quantityMultiplier", "sweepTicks", "sweepQty", "maxTicksWide" };

    static constexpr const char* ValidGlobalParameters[] = {
            "quoteBannerMics", "fillBannerMics", "negScalpClips", "negScalpDollars", "negScalpPeriodMillis", "fillRateClips", "fillRateSumTotalQty", "fillRatePeriodMillis", "percentMsgHedges",
            "percentMsgReduceQuote", "msgHedgesIntervalMics", "msgReduceQuoteMics", "hedgeBannerMics", "MMWorsenPayUpCond%", "selfMatchBannerMics", "RVWidthMultiplier",
            "orderRateClips", "orderRateSumTotalQty", "orderRatePeriodMillis" };

    std::vector<ConfigContract> _allContracts;
    std::unordered_map<hf::symbol_id_t, uint32_t> _sidToInternalId;
    std::unordered_map<std::string, uint32_t> _symbolToInterId;
    uint32_t _indexOfFirstTradedContract;

    UpdateSelector<ViperConfig> _updateSelector;

    std::string _fileName; //model
    std::string _rvPosFileName; //rv position
    std::size_t _numberOfColumns = 0;

    bool _iceCancelResting = true;
    bool _allowGTCs = false;
    bool _allowStatusGrid = false;

    //Slow-path data structures
    std::unordered_map<std::string, int> _unallocatedRVPositionsFromFile;

    Traders _currentTrader = Traders::Steven;
    Shifts _currentShift = Shifts::US;
};

};

#endif //VIPER_VIPER_CONFIG_H
