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

struct ViperStrategyBase;

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
    uint32_t _quoteGap;
    uint32_t _maxQuote;
    uint32_t _numberOfLevels;
    uint32_t _stackSlack;
    int32_t _maxTicksOffTopLevel;
    bool _useGtc;
    bool _allowMultipleNewHedge;
    bool _enableQuoteSprayer;
    bool _reactToRedNumber;
    bool _reactToDarkRedNumber;
};

struct GlobalConfig {
    std::string _key;
    std::string _value;
    std::string _shifts[toUInt(Shifts::__current_marker__)];
};

struct PNLConfig {
    std::string _strategy;
    uint32_t _limit;
    uint32_t _drawdown;
    uint32_t _shiftLimit[toUInt(Shifts::__current_marker__)];
    uint32_t _shiftDrawdown[toUInt(Shifts::__current_marker__)];
    PNLRows _row;
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
    int32_t _orderIncrementQty;
    hf::price_t _settlement = hf::NULL_PRICE;
    bool _toMarketMake = false;
    bool* _useGTC;

    hf::qty_t toValidQty(hf::qty_t qty) const noexcept{
        return _orderIncrementQty == 1 ? qty : qty - (qty % _orderIncrementQty);
    }
};

#define INVALID_INTERNAL_ID std::numeric_limits<uint32_t>::max()

struct VectorWithContract {
    std::string _contract;
    std::vector<int> _values;
};

struct PricingConfig {
    std::string _contract;
    double _weight;
    double _pricingMultiplier;
    double _quantityMultiplier;
    double _asExtCommission;
    uint32_t _establishedLevel;
    uint32_t _sweepTicks;
    uint32_t _sweepQuantity;
    int32_t _maxTicksWide;
    bool _useWeightedMid;
};

struct VectorConfig {
    std::string _vectorName;
    double _range;
    uint32_t _maxPosLong;
    uint32_t _maxPosShort;
    bool _useEMA;
    uint32_t _EMALength;
    uint32_t _EMAOn;
    uint32_t _EMAOff;
    bool _setToMarket;
    double _longTermFair;
    uint32_t _chartNumberOfDecimalPlaces;
    bool _hedged;
    double _ltfDelta;
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
struct ArbieSpreaderPath {
    double _commission;
    std::string _name;
    std::string _config;
    bool _enabled;
    struct Contract {
        std::string _contract;
        int32_t _multiplier;
        Contract(std::string contract, int32_t multiplier) : _contract(std::move(contract)), _multiplier(multiplier) {}
    };
    std::vector<Contract> _contracts;
};

struct ArbieSpreaderConfig {
    std::string _contract;
    std::string _header;
    hf::qty_t _quoteSize;
    double _edge;
    double _hedgeOffset;
    double _maxTicksThroughMarket;
    bool _enabled;
    bool _quoteExternal;
    std::vector<ArbieSpreaderPath> _paths;
};

struct MarketMakerConfig {
    std::string _contract;
    double _quotePctTob;
    uint32_t _quoteSize;
    uint32_t _quoteGap;
    int32_t _maxTicksWideStopExtQuoting;
    int32_t _maxTicksWideDisable;
    int32_t _maxPayUpTicks;
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
    double _estLevelPctEMA;
    uint32_t _establishedLevel;
    uint32_t _estLevelEMASeconds;
    int32_t _internalPosition;
    int32_t _externalPosition;
    uint32_t _queueImportance;
    bool _quoteExternal;
    bool _enabled;
    double _worsenPayupCondPercent;
};

struct NegativeScalpCheckerConfig {
    std::string _contract;
    int64_t _period;
    hf::price_t _pnlLimit;
    hf::qty_t _countLimit;
};

struct EconomicNumber {
    uint64_t _beginTime;
    uint64_t _mmEndTime = std::numeric_limits<uint64_t>::max();
    uint64_t _rvEndTime = std::numeric_limits<uint64_t>::max();
    uint64_t _asEndTime = std::numeric_limits<uint64_t>::max();
    time_t _datetime = std::numeric_limits<uint64_t>::max();
    uint32_t _id;
    uint32_t _mmDuration;
    uint32_t _rvDuration;
    uint32_t _asDuration;
    enum Color {
        DarkRed,
        Red,
        Orange,
        __end__,
    } _color;

    std::string _datetimeString;
    std::string _tradeName;
    std::string _description;

    std::vector<ViperStrategyBase*> _strategyDisabled;
    std::vector<ViperStrategyBase*> _strategyQuoteExtDisabled;
    Shifts _currentShift;

    uint64_t nextCheckPoint() const noexcept {
        return std::min<uint64_t>(std::min<uint64_t>(std::min<uint64_t>(_asEndTime, _rvEndTime), _mmEndTime), _beginTime);
    }
};

inline const char* to_string(EconomicNumber::Color color) {
    static const char* str[] = { "DarkRed", "Red", "Orange" };
    static_assert(sizeof(str) / sizeof(const char*) == EconomicNumber::Color::__end__, "EconomicNumber::Color changed");
    return str[toUInt(color)];
}

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
const std::string AS_EDGE_MULTIPLIER = "ASEdgeMultiplier";
const std::string AUTO_SHUTDOWN_TIME = "autoShutdownTime";
const std::string AUTO_RV_DISABLE_TIME = "autoRVDisableTime";
const std::string AUTO_MM_DISABLE_TIME = "autoMMDisableTime";
const std::string AUTO_AS_DISABLE_TIME = "autoASDisableTime";

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
    const std::string NEGATIVE_SCALP_CHECKER_CONFIGS = "NegativeScalpCheckerConfigs";
    const std::string AS_CONFIGS = "ASConfigs";

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
    bool initialize(const std::string& modelFilename, const std::string& rvPosFilename, const std::string& asPosFilename,
                    const std::string& mmLastFillPriceFillName, const std::string& economicNumbersFileName,
                    const std::string& asPathFilename, SymbolsT& symbols) {
        _fileName = modelFilename;
        _posFileName[toUInt(StrategyId::RelativeValue)] = rvPosFilename;
        _posFileName[toUInt(StrategyId::ArbieSpreader)] = asPosFilename;
        _economicNumbersFileName = economicNumbersFileName;
        _mmLastFillPriceFileName = mmLastFillPriceFillName;
        _asPathFileName = asPathFilename;

        for (uint32_t i = 1; i < toUInt(StrategyId::MAX); ++i) {
            if(!tryGetInitialPos(i)) {
                return false;
            }
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

        good = loadArbieSpreaderConfigs(input);
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

        good = loadNegativeScalpCheckerConfigs(input);

        if (!good) {
            return false;
        }

        for (uint32_t i = 1; i < toUInt(StrategyId::MAX); ++i) {
            good = loadAndCullInitialPositions(i);
            if(!good) {
                return false;
            }
        }

        input.close();

        validateInputs();

        createContracts(symbols);

        good = loadMMLastFillPrices();
        if (!good) {
            return false;
        }

        good = loadArbieSpreaderPaths(asPathFilename);
        if (!good) {
            return false;
        }

        good = loadEconomicNumbers();
        if (!good) {
            return false;
        }

        logging::GetLogger().logInfo("Config file successfully loaded.");

        return good;
    }

    bool tryGetInitialPos(uint32_t strategy) {

        if(!std::experimental::filesystem::exists(_posFileName[strategy])) {
            logging::GetLogger().logError(std::string(toString((StrategyId)strategy)) + " Position file does not exist.  This version requires said file.  filename=" + _posFileName[strategy]);
            return false;
        }

        std::ifstream input(_posFileName[strategy].c_str(), std::ifstream::in);

        if (!input.good()) {
            logging::GetLogger().logError("Unexpected error when opening " + std::string(toString((StrategyId)strategy)) + " Position file.  filename=" + _posFileName[strategy]);
            return false;
        }

        const boost::char_separator<char> sep(",", "", boost::keep_empty_tokens);
        std::string line;
        std::getline(input, line);

        if (!input) {
            logging::GetLogger().logError(std::string(toString((StrategyId)strategy)) + " Position file doesn't appear to contain a header.  filename=" + _posFileName[strategy]);
            return false;
        }

        boost::algorithm::trim(line);
        if (line.empty()) {
            logging::GetLogger().logError(std::string(toString((StrategyId)strategy)) + " Position file contains empty header.  filename=" + _posFileName[strategy]);
            return false;
        }

        const boost::tokenizer<boost::char_separator<char> > tok(line, sep);
        auto it = tok.begin();
        if (it == tok.end()) {
            logging::GetLogger().logError("Could not tokenize header in " + std::string(toString((StrategyId)strategy)) + " Position file.  header=" + line);
            return false;
        }

        //Expected header: RVContract,position
        auto unexpectedColError = [strategy, &it](int colNum) {
            logging::GetLogger().logError("Unexpected column in " + std::string(toString((StrategyId)strategy)) + " Position file.  expected=" + _positionHeader[colNum] + " discovered=" + *it);
        };
        if(*it != (std::string(toString((StrategyId)strategy)) + _positionHeader[0])) {
            unexpectedColError(0);
            return false;
        }

        if(++it == tok.end()) {
            logging::GetLogger().logError("Only discovered 1 column in RV Position file header, expected 2");
            return false;
        }

        if(*it != _positionHeader[1]) {
            unexpectedColError(1);
            return false;
        }

        auto& initialContractToRVPos = _unallocatedPositionsFromFile[strategy];
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

    bool loadMMLastFillPrices() {
        if(!std::experimental::filesystem::exists(_mmLastFillPriceFileName)) {
            logging::GetLogger().logError("MM LastFillPrice file does not exist.  This version requires said file.  filename=" + _mmLastFillPriceFileName);
            return false;
        }

        std::ifstream input(_mmLastFillPriceFileName.c_str(), std::ifstream::in);

        if (!input.good()) {
            logging::GetLogger().logError("Unexpected error when opening MM LastFillPrice file.  filename=" + _mmLastFillPriceFileName);
            return false;
        }

        const boost::char_separator<char> sep(",", "", boost::keep_empty_tokens);
        std::string line;
        std::getline(input, line);

        if (!input) {
            logging::GetLogger().logError("MM LastFillPrice file doesn't appear to contain a header.  filename=" + _mmLastFillPriceFileName);
            return false;
        }

        boost::algorithm::trim(line);
        if (line.empty()) {
            logging::GetLogger().logError("MM LastFillPrice file contains empty header.  filename=" + _mmLastFillPriceFileName);
            return false;
        }

        const boost::tokenizer<boost::char_separator<char> > tok(line, sep);
        auto it = tok.begin();
        if (it == tok.end()) {
            logging::GetLogger().logError("Could not tokenize header in MM LastFillPrice file.  header=" + line);
            return false;
        }

        //Expected header: Contract,LastFillPrice
        auto unexpectedColError = [this, &it](int colNum) {
            logging::GetLogger().logError(std::string("Unexpected column in MM LastFillPrice file.  expected=") + _mmLastFillPriceHeader[colNum] + " discovered=" + *it);
        };
        if(*it != _mmLastFillPriceHeader[0]) {
            unexpectedColError(0);
            return false;
        }

        if(++it == tok.end()) {
            logging::GetLogger().logError("Only discovered 1 column in MM LastFillPrice file header, expected 2");
            return false;
        }

        if(*it != _mmLastFillPriceHeader[1]) {
            unexpectedColError(1);
            return false;
        }

        std::function<void(std::vector<std::string>& values)> setter = [this](std::vector<std::string>& values) {
            if(values.size() != 2) {
                throw std::runtime_error("Wrong number of cols in RV Position file row.");
            }
            const auto* contract = getContract(values[0]);
            if (contract) {
                if (contract->_toMarketMake) {
                    _mmLastFillPrices[contract->_marketMakeIndex] = boost::lexical_cast<hf::price_t>(values[1]);
                } else {
                    logging::GetLogger().logInfo("MM LastFillPrice file contains invalid contract: " + values[0]);
                }
            } else {
                logging::GetLogger().logInfo("MM LastFillPrice file contains invalid contract: " + values[0]);
            }
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

    bool loadNegativeScalpCheckerConfigs(std::ifstream& input) {
        logging::GetLogger().logInfo("Loading NegativeScalpChecker Config");

        bool good = parseHeader(input, _negativeScalpCheckerConfigsHeader, NEGATIVE_SCALP_CHECKER_CONFIGS);

        if (!good) {
            return false;
        }

        std::function<void(std::vector<std::string>& values)> setter = [&configs = _negativeScalpCheckerConfigs](std::vector<std::string>& values) {
            NegativeScalpCheckerConfig config;
            size_t i = 0;
            config._contract = values[i++];
            config._period = boost::lexical_cast<uint32_t>(values[i++]);
            config._countLimit = boost::lexical_cast<uint32_t>(values[i++]);
            config._pnlLimit = boost::lexical_cast<hf::price_t>(values[i++]);
            configs.emplace_back(std::move(config));
        };
        return parseBody(input, setter);
    }

    bool loadAndCullInitialPositions(uint32_t strategy) {
        logging::GetLogger().logInfo("Loading initial " + std::string(toString((StrategyId)strategy)) + " positions");
        if(_pricingConfigs.empty()) {
            return false;
        }

        //Note _numberOfPricingContracts isn't init'd yet or we'd use it for size
        _positions[strategy].resize(_pricingConfigs.size());

        for(unsigned i = 0; i < _pricingConfigs.size(); ++i) {
            const auto& contract = _pricingConfigs[i];
            auto iter = _unallocatedPositionsFromFile[strategy].find(contract._contract);
            if(iter != _unallocatedPositionsFromFile[strategy].end()) {
                auto& pos = _positions[strategy][i];
                if(pos) {
                    logging::GetLogger().logError("Duplicate contract found in initial " +
                        std::string(toString((StrategyId)strategy)) + " positions.  contract=" + iter->first);
                    return false;
                }
                _positions[strategy][i] = iter->second;
                _unallocatedPositionsFromFile[strategy].erase(iter);
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
            config._asExtCommission = boost::lexical_cast<double>(values[9]);
            configs.push_back(config);
        };
        return parseBody(input, setter);
    }

    bool loadGlobalConfigs(std::ifstream& input) {
        assert(_globalConfigHeader.size() == (toUInt(Shifts::__current_marker__) + 1) && "Update globalConfigHeader and GlobalConfigsColumns");

        logging::GetLogger().logInfo("Loading Global Config");

        bool good = parseHeader(input, _globalConfigHeader, GLOBAL_CONFIGS, false);

        if (!good) {
            return false;
        }

        return parseGlobalConfigBody(input);
    }

    bool parseGlobalConfigBody(std::ifstream& input) {
        std::function<void(std::vector<std::string>& values)> setter = [this, &configs = _globalConfigs](std::vector<std::string>& values) {

            if(!std::any_of(std::begin(ValidGlobalParameters), std::end(ValidGlobalParameters), [&values](const char* str) { return !values[0].compare(str); })) {
                throw std::runtime_error(values[0] + " as global parameter not supported by this version of Viper.");
            }

            GlobalConfig config;
            uint32_t i = 0;
            config._key = values[i++];
            config._value = values[i++];
            while (i < values.size() && !values[i].empty()) {
                for (uint32_t ii = 0; ii < toUInt(Shifts::__current_marker__); ++ii) {
                    config._shifts[ii] = values[i++];
                }
            }

            while (i < (_globalConfigHeader.size() + 1)) {
                // global config has added a column for each shift, add the current value to missing
                // column
                config._shifts[i++ - 2] = config._value;
            }
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

    std::string getGlobalByShift(const std::string& key, uint32_t shift) const {
        for (auto& pair : _globalConfigs) {
            if (pair._key == key) {
                return pair._shifts[shift];
            }
        }
        throw std::runtime_error("could not find global key: " + key);
    }

    bool loadPNLConfigs(std::ifstream& input) {
        assert(_pnlConfigHeader.size() == (toUInt(Shifts::__current_marker__) * 2 + 2) && "Update PNLConfigHeader and PNLConfigsColumns");

        logging::GetLogger().logInfo("Loading PNL Config");

        bool good = parseHeader(input, _pnlConfigHeader, PNL_CONFIGS, false);

        if (!good) {
            return false;
        }

        return parsePnlConfigBody(input);
    }

    bool parsePnlConfigBody(std::ifstream& input) {
        _pnlConfigs.resize(toUInt(PNLRows::__end_marker__));
        std::function<void(std::vector<std::string>& values)> setter = [this, &configs = _pnlConfigs](std::vector<std::string>& values) {
            uint32_t i = 0;
            auto strategy = values[i++];
            auto row = getPnlRow(strategy);
            if (row == PNLRows::__end_marker__) {
                throw std::runtime_error("Invalid PNL name " + strategy);
            }
            auto& config = configs[toUInt(row)];
            config._strategy = strategy;
            config._row = row;
            config._limit = boost::lexical_cast<uint32_t>(values[i++]);
            config._drawdown = boost::lexical_cast<uint32_t>(values[i++]);

            // TODO: when all trades' model converted the if check and else case be removed
            if (i < values.size() && !values[i].empty()) {
                for (uint32_t ii = 0; ii < toUInt(Shifts::__current_marker__); ++ii) {
                    config._shiftLimit[ii] = boost::lexical_cast<uint32_t>(values[i++]);
                    config._shiftDrawdown[ii] = boost::lexical_cast<uint32_t>(values[i++]);
                }
            } else {
                for (uint32_t ii = 0; ii < toUInt(Shifts::__current_marker__); ++ii) {
                    config._shiftLimit[ii] = config._limit;
                    config._shiftDrawdown[ii] = config._drawdown;
                }
            }
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
            size_t i = 0;
            config._contract = values[i++];
            config._quoteSize = boost::lexical_cast<uint32_t>(values[i++]);
            config._quoteGap = boost::lexical_cast<uint32_t>(values[i++]);
            config._maxQuote = boost::lexical_cast<uint32_t>(values[i++]);
            config._numberOfLevels = boost::lexical_cast<uint32_t>(values[i++]);
            config._stackSlack = boost::lexical_cast<uint32_t>(values[i++]);
            config._maxTicksOffTopLevel = boost::lexical_cast<int32_t>(values[i++]);
            config._useGtc = values[i++] == "TRUE";
            config._allowMultipleNewHedge = values[i++] == "TRUE";
            config._enableQuoteSprayer = values[i++] == "TRUE";
            config._reactToRedNumber = values[i++] == "TRUE";
            config._reactToDarkRedNumber = values[i++] == "TRUE";
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
            config._quoteGap = boost::lexical_cast<uint32_t>(values[i++]);
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
            config._estLevelEMASeconds = boost::lexical_cast<uint32_t>(values[i++]);
            config._estLevelPctEMA = boost::lexical_cast<double>(values[i++]);
            config._maxTicksWideStopExtQuoting = boost::lexical_cast<int32_t>(values[i++]);
            config._maxTicksWideDisable = boost::lexical_cast<int32_t>(values[i++]);
            config._maxPayUpTicks = boost::lexical_cast<int32_t>(values[i++]);
            if (config._maxPayUpTicks <= 0) {
                throw std::runtime_error(config._contract + " maxPayUpTicks must be a positive value");
            }
            config._worsenPayupCondPercent = boost::lexical_cast<double>(values[i++]);
            if (BOOST_UNLIKELY(config._worsenPayupCondPercent < 0 || IsZero(config._worsenPayupCondPercent))) {
                throw std::runtime_error(config._contract + " _worsenPayupCondPercent must be > 0");
            }
            config._queueImportance = boost::lexical_cast<uint32_t>(values[i++]);
            if (config._queueImportance > 10) {
                config._queueImportance = 0;
            }
            config._internalPosition = boost::lexical_cast<int32_t>(values[i++]);
            config._externalPosition = boost::lexical_cast<int32_t>(values[i++]);
            configs.push_back(config);
        };
        return parseBody(input, setter);
    }

    bool loadArbieSpreaderConfigs(std::ifstream& input) {
        logging::GetLogger().logInfo("Loading Arbie Spreader Config");

        bool good = parseHeader(input, _arbieSpreaderConfigHeader, AS_CONFIGS);
        if (!good) {
            return false;
        }

        std::function<void(std::vector<std::string>& values)> setter = [&configs = _arbieSpreaderConfigs](std::vector<std::string>& values) {
            ArbieSpreaderConfig config;
            uint32_t i = 0;
            config._contract = values[i++];
            config._enabled = values[i++] == "TRUE";
            config._quoteExternal = values[i++] == "TRUE";
            config._quoteSize = boost::lexical_cast<hf::qty_t>(values[i++]);
            config._edge = boost::lexical_cast<double>(values[i++]);
            config._hedgeOffset = boost::lexical_cast<double>(values[i++]);
            config._maxTicksThroughMarket = boost::lexical_cast<double>(values[i++]);
            configs.emplace_back(std::move(config));
        };
        return parseBody(input, setter);
    }

    bool loadArbieSpreaderPaths(const std::string& asPathFilename) {
        logging::GetLogger().logInfo("Loading Arbie Spreader Paths");
        std::ifstream input(asPathFilename.c_str(), std::ifstream::in);
        if (!input.good()) {
            return false;
        }

        auto trimEnd = [](std::vector<std::string>& v) {
            while (!v.empty()) {
                auto it = std::prev(v.end());
                if (!(*it).empty()) {
                    break;
                }
                v.erase(it);
            }
        };

        while (!input.eof()) {
            std::string line;
            std::getline(input, line);

            if (!input) {
                break;
            }

            boost::algorithm::trim(line);
            if (line.empty()) {
                break;
            }

            // parse header
            std::vector<std::string> header;
            boost::split(header, line, boost::is_any_of(","));
            trimEnd(header);

            if (header.empty()) {
                logging::GetLogger().logError("No header");
                return false;
            }

            auto& contract = header[0];
            auto iter = std::find_if(_arbieSpreaderConfigs.begin(), _arbieSpreaderConfigs.end(), [&contract](const ArbieSpreaderConfig& config) {
                return config._contract == contract;
            });
            if (iter == _arbieSpreaderConfigs.end()) {
                logging::GetLogger().logError("ArbieSpreader config not found for contract " + contract);
                return false;
            }

            auto& asConfig = *iter;
            asConfig._header = line;

            // parse body
            while (true) {
                std::getline(input, line);
                if (!input) {
                    break;
                }

                boost::algorithm::trim(line);
                if (line.empty()) {
                    return false;
                }

                std::vector<std::string> values;
                boost::split(values, line, boost::is_any_of(","));
                trimEnd(values);

                if (values.empty()) {
                    // body end
                    break;
                }

                if (values.size() != header.size()) {
                    logging::GetLogger().logError("ArbieSpreader path body and header have different size. contract=" + contract);
                    return false;
                }

                ArbieSpreaderPath path;
                path._name = values[0];
                path._config = values[0];
                uint32_t i = 1;
                for (; i < header.size() - 2; ++i) {    // - 2 exclude Commission, and enabled
                    path._config.append(",").append(values[i]);
                    try {
                        auto multiplier = boost::lexical_cast<int32_t>(values[i]);
                        if (multiplier != 0) {
                            path._contracts.emplace_back(header[i], multiplier);
                        }
                    } catch (const std::exception& e) {
                        logging::GetLogger().logError(boost::str(boost::format("ArbieSpreader path %1% contract %2% invalid multiplier %3%") % i % header[i] % values[i]));
                        return false;
                    }
                }

                if (header[i] != "Commission") {
                    logging::GetLogger().logError("ArbieSpreader path. Expected Commission got " + header[i]);
                    return false;
                }
                path._commission = boost::lexical_cast<double>(values[i++]);
                if (header[i] != "Enabled") {
                    logging::GetLogger().logError("ArbieSpreader path. Expected enabled got " + header[i]);
                    return false;
                }
                path._enabled = values[i] == "TRUE";
                asConfig._paths.emplace_back(std::move(path));
            }
        }
        return true;
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
        bool good = parseHeader(input, _vectorConfigHeader, VECTOR_CONFIGS);

        if (!good) {
            return false;
        }

        return parseVectorConfigValues(input);
    }

    bool parseVectorConfigValues(std::ifstream& input) {
        std::function<void(std::vector<std::string>& values)> setter = [&configs = _vectorConfigs](std::vector<std::string>& values) {
            VectorConfig config;
            size_t i = 0;
            config._vectorName = values[i++];
            config._range = boost::lexical_cast<double>(values[i++]);
            config._maxPosLong = boost::lexical_cast<uint32_t>(values[i++]);
            config._maxPosShort = boost::lexical_cast<uint32_t>(values[i++]);
            config._longTermFair = boost::lexical_cast<double>(values[i++]);
            config._useEMA = values[i++] == "TRUE";
            config._EMALength = boost::lexical_cast<uint32_t>(values[i++]);
            config._EMAOn = values[i++].empty() ? -1 : boost::lexical_cast<uint32_t>(values[i - 1]);
            config._EMAOff = values[i++].empty() ? -1 : boost::lexical_cast<uint32_t>(values[i - 1]);
            config._setToMarket = values[i++] == "TRUE";
            config._chartNumberOfDecimalPlaces = boost::lexical_cast<uint32_t>(values[i++]);
            config._hedged = values[i++] == "TRUE";
            config._ltfDelta = boost::lexical_cast<double>(values[i++]);
            configs.push_back(config);
        };
        return parseBody(input, setter);
    }

    bool loadEconomicNumbers() {
        logging::GetLogger().logInfo("Loading Economic Numbers");

        if(!std::experimental::filesystem::exists(_economicNumbersFileName)) {
            logging::GetLogger().logError("Economic number file does not exist. filename=" + _economicNumbersFileName);
            return false;
        }

        std::ifstream input(_economicNumbersFileName.c_str(), std::ifstream::in);

        if (!input.good()) {
            logging::GetLogger().logError("Unexpected error when opening economic number file.  filename=" + _economicNumbersFileName);
            return false;
        }

        const boost::char_separator<char> sep(",", "", boost::keep_empty_tokens);
        std::string line;
        std::getline(input, line);

        if (!input) {
            logging::GetLogger().logError("Economic number file doesn't appear to contain a header.  filename=" + _economicNumbersFileName);
            return false;
        }

        boost::algorithm::trim(line);
        if (line.empty()) {
            logging::GetLogger().logError("Economic number file contains empty header.  filename=" + _economicNumbersFileName);
            return false;
        }

        const boost::tokenizer<boost::char_separator<char> > tok(line, sep);
        auto it = tok.begin();
        if (it == tok.end()) {
            logging::GetLogger().logError("Could not tokenize header in economic number file.  header=" + line);
            return false;
        }

        auto unexpectedColError = [this, &it](int colNum) {
            logging::GetLogger().logError(std::string("Unexpected column in economic number file.  expected=") + _economicNumberHeader[colNum] + " discovered=" + *it);
        };

        uint32_t i = 0;
        for (auto& header : _economicNumberHeader) {
            if(*it != header) {
                unexpectedColError(i);
                return false;
            }
            ++i;
            if (++it == tok.end()) {
                break;
            }
        }

        if(i != _economicNumberHeader.size()) {
            logging::GetLogger().logError("Only discovered " + std::to_string(i)
                + " column in economic number file header, expected "
                + std::to_string(_economicNumbersFileName.size()));
            return false;
        }

        uint32_t id = 0;
        std::function<void(std::vector<std::string>& values)> setter = [this, &id](std::vector<std::string>& values) {
            EconomicNumber economicNumber;
            uint32_t i = 0;
            if (values[i].size() != 19) {
                logging::GetLogger().logErrorAndThrow("Datetime in economic number must be in \"YYYY-MM-DD HH:MM:SS\" format. value=" + values[i]);
            }
            time_t datetime;
            if (!parseDateTime(values[i], datetime)) {
                logging::GetLogger().logErrorAndThrow("Datetime in economic number must be in \"YYYY-MM-DD HH:MM:SS\" format. value=" + values[i]);
            }
            economicNumber._datetimeString = values[i++];
            economicNumber._mmDuration = boost::lexical_cast<uint32_t>(values[i++]);
            economicNumber._rvDuration = boost::lexical_cast<uint32_t>(values[i++]);
            economicNumber._asDuration = boost::lexical_cast<uint32_t>(values[i++]);
            economicNumber._beginTime = std::numeric_limits<uint64_t>::max();
            economicNumber._mmEndTime = std::numeric_limits<uint64_t>::max();
            economicNumber._rvEndTime = std::numeric_limits<uint64_t>::max();
            economicNumber._asEndTime = std::numeric_limits<uint64_t>::max();
            economicNumber._datetime = datetime;
            economicNumber._id = id++;
            economicNumber._tradeName = values[i++];
            std::transform(values[i].begin(), values[i].end(), values[i].begin(), [](unsigned char c){ return std::tolower(c); });
            if (values[i] != "red" && values[i] != "darkred" && values[i] != "orange") {
                logging::GetLogger().logErrorAndThrow("Only DarkRed, Red, Orange are supported in economic number color");
            }
            auto& color = values[i++];
            if (color == "darkred") {
                economicNumber._color = EconomicNumber::DarkRed;
            } else if (color == "red") {
                economicNumber._color = EconomicNumber::Red;
            } else {
                economicNumber._color = EconomicNumber::Orange;
            }
            economicNumber._description = values[i];
            _mapIdEconomicNumber.emplace(economicNumber._id, std::move(economicNumber));
        };

        return parseBody(input, setter);
    }

    bool loadTraders(std::ifstream& input) {
        logging::GetLogger().logInfo("Loading Traders Config");
        bool good = parseHeader(input, _relativeValueHeader, RV_CONFIGS);

        if (!good) {
            return false;
        }

        return parseVectorConfigValues(input);
    }

    COLD
    bool parseHeader(std::ifstream& input, const std::vector<std::string>& expectedColumns, const std::string& expectedType, bool checkColumns = true) {
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

        if (header.empty()) {
            logging::GetLogger().logError("No header");
            return false;
        }

        if (header[0] != expectedType) {
            logging::GetLogger().logError("Unexpected type. Expecting " + expectedType + " got " + header[0] + " instead.");
            return false;
        }

        if (checkColumns) {

            std::set<std::string> expectedSet{ std::begin(expectedColumns), std::end(expectedColumns) };
            std::set<std::string> foundSet{ std::begin(header), std::end(header) };
            foundSet.erase(header[0]); // Description of section isn't in expected set
            foundSet.erase("");
            expectedSet.erase("");

            auto compareSets = [this, &expectedColumns](const std::set<std::string>& lhs, const std::set<std::string>& rhs, const std::string& errorHeader) -> bool {
                auto resultSetSize = std::max(lhs.size(), rhs.size());
                std::vector<std::string> setDifferenceResult{resultSetSize};
                auto it = std::set_difference(std::begin(lhs), std::end(lhs), std::begin(rhs), std::end(rhs), std::begin(setDifferenceResult));
                setDifferenceResult.resize(it - std::begin(setDifferenceResult));

                if(setDifferenceResult.size()) {

                    std::string result = errorHeader;
                    result += " missing={";
                    for(const auto unexpected : setDifferenceResult) {
                        result += unexpected + ",";
                    }
                    result += "} expectedHeader=";
                    for(const auto elem : expectedColumns) {
                        result += elem + ",";
                    }
                    result += "}";
                    logging::GetLogger().logError(result);
                    return true;
                }

                //No early return
                return false;
            };

            bool error = compareSets(expectedSet, foundSet, "Missing columns.");
            error |= compareSets(foundSet, expectedSet, "Unexpected columns.");

            if(error) {
                return false;
            }

            uint32_t columnIndex = 1;
            for (; columnIndex < header.size(); ++columnIndex) {
                const std::string name = header[columnIndex];
                if (name == "") {
                    break;
                }
                if (columnIndex > expectedColumns.size()) {
                    return false;
                }
                const std::string& expected = expectedColumns[columnIndex - 1];
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
            if(!symbolInfo.enabled) {
                logging::GetLogger().logErrorAndThrow("Symbol not enabled by rhone.  Not in secdb?  sym=" + symbol);
            }
            ConfigContract contract;
            contract._gid = symbolInfo.gid;
            contract._sid = symbolInfo.sid;
            contract._name = symbol;
            contract._tickSize = symbolInfo.tick_size;
            contract._tickValue = symbolInfo.tick_value;
            contract._venue = symbolInfo.getVenue();
            contract._internalId = internalId;
            contract._orderIncrementQty = symbolInfo.order_increment_qty;
            contract._toMarketMake = isContractToMarketMake(symbol, contract._marketMakeIndex);
            if (contract._toMarketMake) {
                _mmLastFillPrices.emplace_back(hf::NULL_PRICE);
            }
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
                case VectorParametersColumns::MaxPosLong: {
                    config._maxPosLong = atoi(gridCell.value().c_str());
                    break;
                }
                case VectorParametersColumns::MaxPosShort: {
                    config._maxPosShort = atoi(gridCell.value().c_str());
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
                case VectorParametersColumns::EMA_On_Time: {
                    config._EMAOn = atoi(gridCell.value().c_str());
                    break;
                }
                case VectorParametersColumns::EMA_Off_Time: {
                    config._EMAOff = atoi(gridCell.value().c_str());
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
                case VectorParametersColumns::LTF_Delta: {
                    config._ltfDelta = atof(gridCell.value().c_str());
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
                case PricingConfigsColumns::ASExtCommission: {
                    config._asExtCommission = boost::lexical_cast<double>(gridCell.value());
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
                case MarketMakerParametersColumns::QuoteGap: {
                    config._quoteGap = boost::lexical_cast<uint32_t>(gridCell.value());
                    break;
                }
                case MarketMakerParametersColumns::QuotePctTob: {
                    config._quotePctTob = boost::lexical_cast<double>(gridCell.value());
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
                case MarketMakerParametersColumns::SafeModeWidth: {
                    config._widthPercentage[toUInt(Shifts::SafeMode)] = boost::lexical_cast<double>(gridCell.value());
                    if(_currentShift == Shifts::SafeMode) {
                        config._widthPercentage[toUInt(Shifts::__current_marker__)] = config._widthPercentage[toUInt(Shifts::SafeMode)];
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
                    config._minInternalPriceChange = boost::lexical_cast<double>(gridCell.value());
                    break;
                }
                case MarketMakerParametersColumns::EstLevelEMASeconds: {
                    config._estLevelEMASeconds = atoi(gridCell.value().c_str());
                    break;
                }
                case MarketMakerParametersColumns::EstLevelPctEMA: {
                    config._estLevelPctEMA = boost::lexical_cast<double>(gridCell.value());
                    break;
                }
                case MarketMakerParametersColumns::MaxTicksWideStopExtQuoting: {
                    config._maxTicksWideStopExtQuoting = atoi(gridCell.value().c_str());
                    break;
                }
                case MarketMakerParametersColumns::MaxTicksWideDisable: {
                    config._maxTicksWideDisable = atoi(gridCell.value().c_str());
                    break;
                }
                case MarketMakerParametersColumns::MaxPayUpTicks: {
                    config._maxPayUpTicks = atoi(gridCell.value().c_str());
                    break;
                }
                case MarketMakerParametersColumns::WorsenPayupCondPercent : {
                    config._worsenPayupCondPercent = boost::lexical_cast<double>(gridCell.value());
                    break;
                }
                case MarketMakerParametersColumns::QueueImportance: {
                    config._queueImportance = boost::lexical_cast<uint32_t>(gridCell.value());
                    if (config._queueImportance > 10) {
                        config._queueImportance = 0;
                    }
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

    bool onArbieSpreaderParametersUpdate(GridUpdate& update) {
        for (int i = 0; i < update._update.cells_size(); ++i) {
            const proto::GridCell& gridCell = update._update.cells(i);
            uint32_t internalId = gridCell.row();
            auto& contract = _allContracts[internalId];
            auto iter = std::find_if(_arbieSpreaderConfigs.begin(), _arbieSpreaderConfigs.end(), [&contract](const ArbieSpreaderConfig& config) {
                return config._contract == contract._name;
            });
            if (iter != _arbieSpreaderConfigs.end()) {
                auto& config = *iter;
                auto columnType = static_cast<ArbieSpreaderParameterColumns>(gridCell.col());

                switch (columnType) {
                    case ArbieSpreaderParameterColumns::Enabled:
                        config._enabled = boost::lexical_cast<bool>(gridCell.value());
                        break;
                    case ArbieSpreaderParameterColumns::QuoteExternal:
                        config._quoteExternal = boost::lexical_cast<bool>(gridCell.value());
                        break;
                    case ArbieSpreaderParameterColumns::Edge:
                        config._edge = boost::lexical_cast<double>(gridCell.value());
                        break;
                    case ArbieSpreaderParameterColumns::QuoteSize:
                        config._quoteSize = boost::lexical_cast<hf::qty_t>(gridCell.value());
                        break;
                    case ArbieSpreaderParameterColumns::HedgeOffset:
                        config._hedgeOffset = boost::lexical_cast<double>(gridCell.value());
                        break;
                    case ArbieSpreaderParameterColumns::MaxTicksThroughMarket:
                        config._maxTicksThroughMarket = boost::lexical_cast<double>(gridCell.value());
                        break;
                    default:
                        break;
                }
            }
        }
        return true;
    }

    bool onArbieSpreaderPathsUpdate(GridUpdate& update) {
        return true;
    }

    bool onStackerParametersUpdate(GridUpdate& update) {
        for (int i = 0; i < update._update.cells_size(); ++i) {
            const proto::GridCell& gridCell = update._update.cells(i);
            uint32_t contractIndex = gridCell.row() - _indexOfFirstTradedContract;
            auto& config = _stackerConfigs.at(contractIndex);
            StackerParametersColumns columnType = static_cast<StackerParametersColumns>(gridCell.col());

            switch (columnType) {
                case StackerParametersColumns::QuoteSize: {
                    config._quoteSize = atoi(gridCell.value().c_str());
                    break;
                }
                case StackerParametersColumns::QuoteGap: {
                    config._quoteGap = atoi(gridCell.value().c_str());
                    break;
                }
                case StackerParametersColumns::MaxQuote: {
                    config._maxQuote = atoi(gridCell.value().c_str());
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
                case StackerParametersColumns::EnableQuoteSprayer:
                    config._enableQuoteSprayer = 1 == atoi(gridCell.value().c_str());
                    break;
                case StackerParametersColumns::RedNumberPull:
                    config._reactToRedNumber = 1 == atoi(gridCell.value().c_str());
                    break;
                case StackerParametersColumns::DarkRedNumberPull:
                    config._reactToDarkRedNumber = 1 == atoi(gridCell.value().c_str());
                    break;
            }
        }
        return true;
    }

    bool onPnlParametersUpdate(GridUpdate& update) {
        auto updateLimit = [this](PNLConfig& config, const std::string& value, Shifts shift) {
            config._shiftLimit[toUInt(shift)] = boost::lexical_cast<int64_t>(value);
            LOG(0, "Shift {i1} Pnl limit changed to {i2}", toUInt(shift), config._shiftLimit[toUInt(shift)])
            if (_currentShift == shift) {
                config._limit = config._shiftLimit[toUInt(shift)];
            }
        };

        auto updateDrawdown = [this](PNLConfig& config, const std::string& value, Shifts shift) {
            config._shiftDrawdown[toUInt(shift)] = boost::lexical_cast<int64_t>(value);
            LOG(0, "Shift {i1} Dawdown limit changed to {i2}", toUInt(shift), config._shiftDrawdown[toUInt(shift)])
            if (_currentShift == shift) {
                config._drawdown = config._shiftDrawdown[toUInt(shift)];
            }
        };

        for (int i = 0; i < update._update.cells_size(); ++i) {
            const proto::GridCell& gridCell = update._update.cells(i);
            PNLConfig& config = _pnlConfigs[gridCell.row()];
            PNLColumns columnType = static_cast<PNLColumns>(gridCell.col());
            switch (columnType) {
                case PNLColumns::AsiaPnlLimit: {
                    updateLimit(config, gridCell.value(), Shifts::Asia);
                    break;
                }
                case PNLColumns::AsiaDrawdownLimit: {
                    updateDrawdown(config, gridCell.value(), Shifts::Asia);
                    break;
                }

                case PNLColumns::EuropePnlLimit: {
                    updateLimit(config, gridCell.value(), Shifts::Europe);
                    break;
                }
                case PNLColumns::EuropeDrawdownLimit: {
                    updateDrawdown(config, gridCell.value(), Shifts::Europe);
                    break;
                }

                case PNLColumns::USPnlLimit: {
                    updateLimit(config, gridCell.value(), Shifts::US);
                    break;
                }
                case PNLColumns::USDrawdownLimit: {
                    updateDrawdown(config, gridCell.value(), Shifts::US);
                    break;
                }

                case PNLColumns::SafeModePnlLimit: {
                    updateLimit(config, gridCell.value(), Shifts::SafeMode);
                    break;
                }
                case PNLColumns::SafeModeDrawdownLimit: {
                    updateDrawdown(config, gridCell.value(), Shifts::SafeMode);
                    break;
                }

                default:
                    break;
            }
        }
        return true;
    }

    static PNLRows getPnlRow(const std::string& strategy) noexcept {
        if (strategy == "Total") {
            return PNLRows::Total;
        }

        for (uint32_t i = toUInt(StrategyId::MarketMaker); i < toUInt(StrategyId::MAX); ++i) {
            if (strcmp(toString((StrategyId)i), strategy.c_str()) == 0) {
                return static_cast<PNLRows>(i);
            }
        }
        assert(false);
        return PNLRows::__end_marker__;
    }

    bool onGlobalConfigsUpdate(GridUpdate& update) {
        for (int i = 0; i < update._update.cells_size(); ++i) {
            const proto::GridCell& gridCell = update._update.cells(i);
            auto& config = _globalConfigs[gridCell.row()];
            auto shift = static_cast<Shifts>(gridCell.col() - 1);
            config._shifts[toUInt(shift)] = gridCell.value();
            if (_currentShift == shift) {
                config._value = gridCell.value();
            }
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
        if (strategy != StrategyId::MarketMaker) {
            ConfigContract& contract = _allContracts[contractIndex];
            if(contract._legs.size() > 1) {
                for(unsigned i = 0; i < contract._legs.size(); ++i) {
                    auto& leg = contract._legs[i];
#ifdef IS_DEV
                    if (strategy == StrategyId::ArbieSpreader) {
                        LOG(0, "{C[i1]}. Add AS Leg Position. position={i2} posAdded={i3}", _allContracts[leg._index]._name, position, (position * leg._ratio))
                    }
#endif
                    _positions[toUInt(strategy)][leg._index] += position * leg._ratio;
                }
            }
            else {
#ifdef IS_DEV
                if (strategy == StrategyId::ArbieSpreader) {
                    LOG(0, "{C[i1]}. Add AS Position. position={i2}", contract._name, position)
                }
#endif
                _positions[toUInt(strategy)][contractIndex] += position;
            }
        } else {
            if (internal) {
                _marketMakerConfigs[index]._internalPosition += static_cast<int32_t>(position);
            } else {
                _marketMakerConfigs[index]._externalPosition += static_cast<int32_t>(position);
            }
        }
    }

    void writeToFile(const std::string& filename, std::function<void (std::ofstream&)>&& gen) {
        std::string tempFilename = filename + ".temp";
        std::ofstream output(tempFilename.c_str(), std::ofstream::out);
        gen(output);
        output.close();
        std::experimental::filesystem::rename(tempFilename, filename);
    }

    void writeToFile() {
        writeToFile(_fileName, [this](std::ofstream& output) {
            output.precision(12);
            writePricingMatrix(output);
            writeVectorConfig(output);
            writeRelativeValueConfig(output);
            writeMarketMakersConfig(output);
            writeArbieSpreaderConfig(output);
            writeStackerConfigs(output);
            writeGlobalConfigs(output);
            writePnlConfigs(output);
            writeLeanMatrix(output);
            writePricingConfigs(output);
            writeNegativeScalpCheckerConfigs(output);
        });

        for (uint32_t i = 1; i < toUInt(StrategyId::MAX); ++i) {
            writeToFile(_posFileName[i], [this, i](std::ofstream& output) {
                writePos(i,output);
            });
        }

        writeToFile(_asPathFileName, [this](std::ofstream& output){
            writeArbieSpreaderPath(output);
        });
    }

    void recordMMLastFillPrice(uint32_t contractId, hf::price_t lastFillPrice) {
        auto& contract = _allContracts[contractId];
        if (_mmLastFillPrices[contract._marketMakeIndex] != lastFillPrice) {
            _mmLastFillPrices[contract._marketMakeIndex] = lastFillPrice;
            writeToFile(_mmLastFillPriceFileName, [this](std::ofstream& output) {
                output << _mmLastFillPriceHeader[0] << "," << _mmLastFillPriceHeader[1] << std::endl;
                for (const auto& contract : _allContracts) {
                    if (contract._toMarketMake) {
                        output << contract._name << "," << _mmLastFillPrices[contract._marketMakeIndex] << std::endl;
                    }
                }
            });
        }
    }

    void writePnlConfigs(std::ofstream& output) const {
        writeHeader(output, PNL_CONFIGS, _pnlConfigHeader);
        for (auto& config : _pnlConfigs) {
            output << config._strategy;
            output << "," << config._limit;
            output << "," << config._drawdown;
            for (uint32_t i = 0; i < toUInt(Shifts::__current_marker__); ++i) {
                output << "," << config._shiftLimit[i];
                output << "," << config._shiftDrawdown[i];
            }
            fillInMissingCommas(output, _pnlConfigHeader.size());
        }
        writeEmptyLine(output);
    }

    void writeGlobalConfigs(std::ofstream& output) const {
        writeHeader(output, GLOBAL_CONFIGS, _globalConfigHeader);
        for (auto& config : _globalConfigs) {
            output << config._key;
            output << "," << config._value;
            for (uint32_t i = 0; i < toUInt(Shifts::__current_marker__); ++i) {
                output << "," << config._shifts[i];
            }
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
            output << "," << config._asExtCommission;
            fillInMissingCommas(output, _pricingConfigsHeader.size());
        }
        writeEmptyLine(output);
    }

    void writeNegativeScalpCheckerConfigs(std::ofstream& output) const {
        writeHeader(output, NEGATIVE_SCALP_CHECKER_CONFIGS, _negativeScalpCheckerConfigsHeader);
        for (auto& config : _negativeScalpCheckerConfigs) {
            output << config._contract;
            output << "," << config._period;
            output << "," << config._countLimit;
            output << "," << config._pnlLimit;
            fillInMissingCommas(output, _negativeScalpCheckerConfigsHeader.size());
        }
        writeEmptyLine(output);
    }

    void writePos(uint32_t strategy, std::ofstream& output) const {
        if(_numberOfPricingContracts) {
            output << toString((StrategyId)strategy) << _positionHeader[0] << "," << _positionHeader[1] << "\n";
            for (unsigned i = 0; i < _numberOfPricingContracts; ++i) {
                output << _pricingConfigs[i]._contract << ',' << _positions[strategy][i] << "\n";
            }
        }
    }

    void writeLeanMatrix(std::ofstream& output) const {
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
            output << "," << config._quoteSize;
            output << "," << config._quoteGap;
            output << "," << config._maxQuote;
            output << "," << config._numberOfLevels;
            output << "," << config._stackSlack;
            output << "," << config._maxTicksOffTopLevel;
            output << "," << getWriteBoolValue(config._useGtc);
            output << "," << getWriteBoolValue(config._allowMultipleNewHedge);
            output << "," << getWriteBoolValue(config._enableQuoteSprayer);
            output << "," << getWriteBoolValue(config._reactToRedNumber);
            output << "," << getWriteBoolValue(config._reactToDarkRedNumber);
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
            output << "," << config._maxPosLong;
            output << "," << config._maxPosShort;
            output << "," << config._longTermFair;
            output << "," << getWriteBoolValue(config._useEMA);
            output << "," << config._EMALength;
            output << ",";
            if (config._EMAOn != (uint32_t) -1) {
                output << config._EMAOn;
            } else {
                output << -1;
            }

            output << ",";
            if (config._EMAOff != (uint32_t) -1) {
                output << config._EMAOff;
            } else {
                output << -1;
            }
            output << "," << getWriteBoolValue(config._setToMarket);
            output << "," << config._chartNumberOfDecimalPlaces;
            output << "," << getWriteBoolValue(config._hedged);
            output << "," << config._ltfDelta;
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
            output << "," << config._quoteGap;
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
            output << "," << config._estLevelEMASeconds;
            output << "," << config._estLevelPctEMA;
            output << "," << config._maxTicksWideStopExtQuoting;
            output << "," << config._maxTicksWideDisable;
            output << "," << config._maxPayUpTicks;
            output << "," << config._worsenPayupCondPercent;
            output << "," << config._queueImportance;
            output << "," << config._internalPosition;
            output << "," << config._externalPosition;
            fillInMissingCommas(output, _marketMakerHeader.size());
        }
        writeEmptyLine(output);
    }

    void writeArbieSpreaderConfig(std::ofstream& output) const {
        writeHeader(output, AS_CONFIGS, _arbieSpreaderConfigHeader);
        for (auto& config : _arbieSpreaderConfigs) {
            output << config._contract;
            output << "," << getWriteBoolValue(config._enabled);
            output << "," << getWriteBoolValue(config._quoteExternal);
            output << "," << config._quoteSize;
            output << "," << config._edge;
            output << "," << config._hedgeOffset;
            output << "," << config._maxTicksThroughMarket;
            fillInMissingCommas(output, _arbieSpreaderConfigHeader.size());
        }
        writeEmptyLine(output);
    }

    void writeArbieSpreaderPath(std::ofstream& output) {
        for (auto& config : _arbieSpreaderConfigs) {
            output << config._header << std::endl;
            for (auto& path : config._paths) {
                output << path._config;
                output << "," << path._commission;
                output << "," << getWriteBoolValue(path._enabled);
                output << std::endl;
            }
            writeEmptyLine(output);
        }
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
    std::vector<std::string> _vectorConfigHeader = {"range", "maxPosLong", "maxPosShort", "ltf", "EMA", "EMALength(secs)", "EMAOnTime", "EMAOffTime", "SetToMarket", "ChartNumberOfDecimals", "Hedged", "LTFDelta"};

    std::vector<RelativeValueConfig> _relativeValueConfigs;
    std::vector<int32_t> _positions[toUInt(StrategyId::MAX)];
    std::vector<std::string> _relativeValueHeader  = {"quoteExternal", "hedgeTolerance", "commission", "RVWidth", "%stfMove", "maxTicksThroughMarket", "staticQuote", "quoteSize" };
    static constexpr const char* _positionHeader[] = {"Contract", "position"};

    std::vector<ArbieSpreaderConfig> _arbieSpreaderConfigs;
    std::vector<std::string> _arbieSpreaderConfigHeader = {"enabled", "quoteExternal", "quoteSize", "edge", "hedgeOffset", "maxTicksThroughMarket"};

    std::vector<MarketMakerConfig> _marketMakerConfigs;
    std::vector<std::string> _marketMakerHeader = {
            "enabled",
            "quoteExternal",
            "quoteSize",
            "quoteGap",
            "quotePctTob",
            "AsiaWidthPercent",
            "EuroWidthPercent",
            "USWidthPercent",
            "SafeModeWidthPercent",
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
            "EstLevelEMA",
            "EstLevelEMASeconds",
            "EstLevel%EMA",
            "maxTicksWideStopExtQuoting",
            "maxTicksWideDisable",
            "maxPayUpTicks",
            "MMWorsenPayUpCondPct",
            "queueImportance",
            "internalPosition",
            "externalPosition",
    };
    std::vector<hf::price_t> _mmLastFillPrices;
    static constexpr const char* _mmLastFillPriceHeader[] = {"MMContract", "LastFillPrice"};

    std::vector<StackerConfig> _stackerConfigs;
    std::vector<std::string> _stackerConfigHeader  = {"quoteSize", "quoteGap", "maxQuote", "stackLevels", "stackSlack", "maxTicksOffTopLevel", "useGTC", "allowMultipleNewHedge", "enableQuoteSprayer", "redNumberPull", "darkRedNumberPull"};

    std::vector<GlobalConfig> _globalConfigs;
    std::vector<std::string> _globalConfigHeader = {"Value", "Asia", "Europe", "US", "SafeMode"};

    std::vector<PNLConfig> _pnlConfigs;
    std::vector<std::string> _pnlConfigHeader  = {"Limit", "Drawdown", "AsiaLimit", "AsiaDrawdown", "EuropeLimit", "EuropeDrawdown", "USLimit", "USDrawdown", "SafeModeLimit", "SafeModeDrawdown"};

    std::vector<VectorWithContract> _leanMatrix;

    std::vector<PricingConfig> _pricingConfigs;
    std::vector<std::string> _pricingConfigsHeader = {"weights", "weightedMid", "establishedLevel", "pricingMultiplier", "quantityMultiplier", "sweepTicks", "sweepQty", "maxTicksWide", "asExtCommission" };

    std::vector<NegativeScalpCheckerConfig> _negativeScalpCheckerConfigs;
    std::vector<std::string> _negativeScalpCheckerConfigsHeader = {"periodMillis", "clips", "dollars" };

    static constexpr const char* ValidGlobalParameters[] = {
            "quoteBannerMics",
            "fillBannerMics",
            "negScalpClips",
            "negScalpDollars",
            "negScalpPeriodMillis",
            "fillRateClips",
            "fillRateSumTotalQty",
            "fillRatePeriodMillis",
            "percentMsgHedges",
            "percentMsgReduceQuote",
            "msgHedgesIntervalMics",
            "msgReduceQuoteMics",
            "hedgeBannerMics",
            "MMWorsenPayUpCond%",
            "selfMatchBannerMics",
            "RVWidthMultiplier",
            "ASEdgeMultiplier",
            "orderRateClips",
            "orderRateSumTotalQty",
            "orderRatePeriodMillis",
            "autoShutdownTime",
            "autoRVDisableTime",
            "autoMMDisableTime",
            "autoASDisableTime"
    };


    std::vector<std::string> _economicNumberHeader = {"datetime", "mmDuration", "rvDuration", "asDuration", "trade", "color", "description"};
    std::unordered_map<uint32_t, EconomicNumber> _mapIdEconomicNumber;

    std::vector<ConfigContract> _allContracts;
    std::unordered_map<hf::symbol_id_t, uint32_t> _sidToInternalId;
    std::unordered_map<std::string, uint32_t> _symbolToInterId;
    uint32_t _indexOfFirstTradedContract;

    UpdateSelector<ViperConfig> _updateSelector;

    std::string _fileName; //model
    std::string _posFileName[toUInt(StrategyId::MAX)]; //none-MM position
    std::string _economicNumbersFileName;
    std::string _mmLastFillPriceFileName;
    std::string _asPathFileName;
    std::size_t _numberOfColumns = 0;

    bool _iceCancelResting = true;
    bool _allowGTCs = false;
    bool _allowStatusGrid = false;

    //Slow-path data structures
    std::unordered_map<std::string, int> _unallocatedPositionsFromFile[toUInt(StrategyId::MAX)];

    std::vector<std::string> _traders;
    size_t _currentTrader = 0;
    Shifts _currentShift = Shifts::US;
};

};

#endif //VIPER_VIPER_CONFIG_H
