#ifndef VIPER_GUI_THINGS_H
#define VIPER_GUI_THINGS_H

#include <hf/util/TimeUtils.h>
#include <util/VenueUtils.h>
#include "messages/python.pb.h"
#include "ViperConfig.h"
#include "RelativeValue.h"
#include "TimeUtils.h"
#include "NegativeScalpChecker.h"
#include "Common.h"
#include "MarketMaker.h"
#include "SimpleMarketMaker.h"

namespace Python {

using HeaderCellT = std::tuple<uint32_t, const char*, bool, proto::DataType, const char*, uint32_t, uint32_t>;

template<typename EnumType>
inline HeaderCellT headerCell(EnumType id, const char* label, bool readOnly, proto::DataType type , const char* e = "", uint32_t target = 0, uint32_t delta = 0) {
    return std::make_tuple(toUInt(id), label, readOnly, type, e, target, delta);
}

using RowCellT = std::tuple<uint32_t, std::string, uint32_t>;

template<typename EnumType>
inline RowCellT Cell(EnumType col, std::string&& value, uint32_t colorId = UINT32_MAX) {
    return std::make_tuple(toUInt(col), std::move(value), colorId);
}

class GUIThings {
public:
    enum class RequiresConfirmation : bool { NO = false, YES = true };

    static GUIThings& get() {
        static GUIThings instance;
        return instance;
    }

    static constexpr const char* NO_VALUE_ID = "-1";

    void init(ViperConfig* config) {
        _config = config;
    }

    auto addWindow(::google::protobuf::RepeatedPtrField< ::Python::proto::GridDefinition >* definitions,
                   WindowIds id, const char* name, proto::GridType type, bool isSubscriptionWindow = false) {
        auto definition = definitions->Add();
        definition->set_gridid(toUInt(id));
        definition->set_name(name);
        definition->set_type(type);
        definition->set_refreshonsubscription(isSubscriptionWindow);

        if (isSubscriptionWindow) {
            _subscriptionWindows.emplace(id);
        }

        auto rowHeader = definition->mutable_rowheader();
        auto cells = definition->mutable_cells();

        return std::make_tuple(definition, rowHeader, cells);
    }

    auto addWindowWithSubmitButton(::google::protobuf::RepeatedPtrField< ::Python::proto::GridDefinition >* definitions,
                                   WindowIds id, const char* name, proto::GridType type, bool isSubscriptionWindow = false) {
        auto rt = addWindow(definitions, id, name, type, isSubscriptionWindow);
        addSubmitButton(std::get<0>(rt));
        return rt;
    }

    struct Position {
        hf::qty_t _rv = 0;
        hf::qty_t _as = 0;
        hf::qty_t _mmInternal = 0;
        hf::qty_t _mmExternal = 0;
        hf::qty_t _total = 0;
    };

    Position& getPosition(uint32_t index, std::map<uint32_t, Position>& positions) {
        auto it = positions.find(index);
        if (it == positions.end()) {
            return positions.emplace(index, Position()).first->second;
        }
        return it->second;
    }

    void getPositions(std::map<uint32_t, Position>& positions) {
        uint32_t adj = _config->_indexOfFirstTradedContract;

        auto& rvPositions = _config->_positions[toUInt(StrategyId::RelativeValue)];
        for(uint32_t row = 0; row < rvPositions.size(); ++row) {
            hf::qty_t rv = rvPositions[row];
            ConfigContract& contract = _config->_allContracts[row];
            if(BOOST_UNLIKELY(contract._legs.size() > 1)) {
                for(uint32_t i = 0; i < contract._legs.size(); ++i) {
                    auto& leg = contract._legs[i];
                    getPosition(leg._index, positions)._rv += rv * leg._ratio;
                }
            }
            else {
                getPosition(row, positions)._rv += rv;
            }
        }

        auto& asPositions = _config->_positions[toUInt(StrategyId::ArbieSpreader)];
        for(uint32_t row = 0; row < asPositions.size(); ++row) {
            hf::qty_t as = asPositions[row];
            ConfigContract& contract = _config->_allContracts[row];
            if(BOOST_UNLIKELY(contract._legs.size() > 1)) {
                for(uint32_t i = 0; i < contract._legs.size(); ++i) {
                    auto& leg = contract._legs[i];
                    getPosition(leg._index, positions)._as += as * leg._ratio;
                }
            }
            else {
                getPosition(row, positions)._as += as;
            }
        }

        for (uint32_t row = _config->_indexOfFirstTradedContract; row < _config->_allContracts.size(); ++row) {
            ConfigContract& contract = _config->_allContracts[row];
            if (contract._toMarketMake) {
                hf::qty_t mmInt = _config->_marketMakerConfigs[row - adj]._internalPosition;
                hf::qty_t mmExt = _config->_marketMakerConfigs[row - adj]._externalPosition;
                if (contract._legs.size() > 1) {
                    for (uint32_t i = 0; i < contract._legs.size(); ++i) {
                        auto& leg = contract._legs[i];
                        Position& position = getPosition(leg._index, positions);
                        position._mmInternal += mmInt * leg._ratio;
                        position._mmExternal += mmExt * leg._ratio;
                        position._total = position._rv + position._as + position._mmInternal + position._mmExternal;
                    }
                } else {
                    Position& position = getPosition(row, positions);
                    position._mmInternal += mmInt;
                    position._mmExternal += mmExt;
                    position._total = position._rv + position._as + position._mmInternal + position._mmExternal;
                }
            }
        }
    }

    void updatePositions(proto::GridUpdate& message) {
        message.set_gridid(toUInt(WindowIds::PositionInOutrights));
        message.set_updatetype(proto::GridUpdateType::CellsType);
        auto cells = message.mutable_cells();
        std::map<uint32_t, Position> positions;
        getPositions(positions);
        for (auto& pair : positions) {
            Position& position = pair.second;
            setCell(cells, pair.first, PositionsColumns::RV, std::to_string(position._rv));
            setCell(cells, pair.first, PositionsColumns::AS, std::to_string(position._as));
            setCell(cells, pair.first, PositionsColumns::MMInternal, std::to_string(position._mmInternal));
            setCell(cells, pair.first, PositionsColumns::MMExternal, std::to_string(position._mmExternal));
            setCell(cells, pair.first, PositionsColumns::Total, std::to_string(position._total));
        }
    }

    template <typename T>
    void populateMessageCountsUpdate(proto::GridUpdate& message, T* executor) {
        message.set_gridid(toUInt(WindowIds::MessagesCounts));
        message.set_updatetype(proto::GridUpdateType::CellsType);
        auto cells = message.mutable_cells();
        auto total = _config->_allContracts.size();
        uint32_t inserts = 0;
        uint32_t modifies = 0;
        uint32_t cancels = 0;
        uint32_t cancelRejects = 0;
        uint32_t modifyRejects = 0;
        uint32_t fills = 0;
        hf::VENUE venue = hf::VENUE::UNKNOWN;
        for (uint32_t contractIndex = 0; contractIndex < executor->_contractStates.size(); ++contractIndex) {
            auto& state = executor->_contractStates[contractIndex];
            if (state._contract._toMarketMake) {
                venue = state._contract._venue;
                setCell(cells, contractIndex, MessageCountsColumns::Inserts, std::to_string(state._ime._messages._inserts));
                inserts += state._ime._messages._inserts;
                setCell(cells, contractIndex, MessageCountsColumns::Modifies, std::to_string(state._ime._messages._modifies));
                modifies += state._ime._messages._modifies;
                setCell(cells, contractIndex, MessageCountsColumns::Cancels, std::to_string(state._ime._messages._cancels));
                cancels += state._ime._messages._cancels;
                setCell(cells, contractIndex, MessageCountsColumns::CancelRejects, std::to_string(state._ime._messages._cancelRejects));
                cancelRejects += state._ime._messages._cancelRejects;
                setCell(cells, contractIndex, MessageCountsColumns::ModifyRejects, std::to_string(state._ime._messages._modifyRejects));
                modifyRejects += state._ime._messages._modifyRejects;
                setCell(cells, contractIndex, MessageCountsColumns::Fills, std::to_string(state._ime._messages._fills));
                fills += state._ime._messages._fills;
                double fillsRatio = 0;
                if (state._ime._messages._fills != 0) {
                    switch (state._contract._venue) {
                        case hf::VENUE::EUREX: //todo_eurex
                        case hf::VENUE::ICE: {
                            fillsRatio = static_cast<double>(state._ime._messages._inserts + state._ime._messages._modifies) / state._ime._messages._fills;
                            break;
                        }
                        case hf::VENUE::B3:
                        case hf::VENUE::CME: {
                            fillsRatio = static_cast<double>(state._ime._messages._cancels * 3 + state._ime._messages._modifies) / state._ime._messages._fills;
                            break;
                        }
                        default:
                            throw std::runtime_error("messages-unhandled exchange");
                    }
                }
                setCell(cells, contractIndex, MessageCountsColumns::FillsRatio, std::to_string(fillsRatio));
            }
        }
        setCell(cells, total, MessageCountsColumns::Inserts, std::to_string(inserts));
        setCell(cells, total, MessageCountsColumns::Modifies, std::to_string(modifies));
        setCell(cells, total, MessageCountsColumns::Cancels, std::to_string(cancels));
        setCell(cells, total, MessageCountsColumns::CancelRejects, std::to_string(cancelRejects));
        setCell(cells, total, MessageCountsColumns::ModifyRejects, std::to_string(modifyRejects));
        setCell(cells, total, MessageCountsColumns::Fills, std::to_string(fills));
        double fillsRatio = 0;
        if (fills != 0) {
            switch (venue) {
                //todo_eurex
                case hf::VENUE::EUREX:
                case hf::VENUE::ICE: {
                    fillsRatio = static_cast<double>(inserts + modifies) / fills;
                    break;
                }
                case hf::VENUE::B3:
                case hf::VENUE::CME: {
                    fillsRatio = static_cast<double>(cancels * 3 + modifies) / fills;
                    break;
                }
                default:
                    throw std::runtime_error("messages-unhandled exchange");
            }
        }
        setCell(cells, total, MessageCountsColumns::FillsRatio, std::to_string(fillsRatio));
    }

    void populateMarketMakerState(proto::GridUpdate& message, const MarketMaker* marketMaker) {

    }

    static constexpr PNLRows getPNLRow(StrategyId strategy) noexcept {
        return static_cast<PNLRows>(strategy);
    }

    void populatePnlUpdate(proto::GridUpdate& message, StrategyId strategy, const Pnl& pnl, const Pnl& totalPnl) {
        message.set_gridid(toUInt(WindowIds::PNL));
        message.set_updatetype(proto::GridUpdateType::CellsType);
        auto cells = message.mutable_cells();
        auto row = getPNLRow(strategy);

        if (BOOST_LIKELY(row != PNLRows::__end_marker__)) {
            setCell(cells, toUInt(row), PNLColumns::Open, std::to_string(pnl._open));
            setCell(cells, toUInt(row), PNLColumns::Closed, std::to_string(pnl._closed));
            setCell(cells, toUInt(row), PNLColumns::Drawdown, std::to_string(pnl._drawdown));
            setCell(cells, toUInt(row), PNLColumns::Total, std::to_string(pnl.getTotal()));
        }
        setCell(cells, toUInt(PNLRows::Total), PNLColumns::Open, std::to_string(totalPnl._open));
        setCell(cells, toUInt(PNLRows::Total), PNLColumns::Closed, std::to_string(totalPnl._closed));
        setCell(cells, toUInt(PNLRows::Total), PNLColumns::Drawdown, std::to_string(totalPnl._drawdown));
        setCell(cells, toUInt(PNLRows::Total), PNLColumns::Total, std::to_string(totalPnl.getTotal()));
    }

    void populatePnlDetailUpdate(proto::GridUpdate& message, StrategyId id, uint32_t contractIndex, Pnl& pnl, hf::price_t totalPnl) {
        message.set_gridid(toUInt(WindowIds::PnlDetail));
        message.set_updatetype(proto::GridUpdateType::CellsType);
        auto cells = message.mutable_cells();
        switch (id) {
            case StrategyId::RelativeValue: {
                setCell(cells, contractIndex, PnlDetailColumns::RVOpen, std::to_string(pnl._open));
                setCell(cells, contractIndex, PnlDetailColumns::RVClosed, std::to_string(pnl._closed));
                setCell(cells, contractIndex, PnlDetailColumns::RVTotal, std::to_string(pnl.getTotal()));
                break;
            }
            case StrategyId::MarketMaker: {
                setCell(cells, contractIndex, PnlDetailColumns::MMOpen, std::to_string(pnl._open));
                setCell(cells, contractIndex, PnlDetailColumns::MMClosed, std::to_string(pnl._closed));
                setCell(cells, contractIndex, PnlDetailColumns::MMTotal, std::to_string(pnl.getTotal()));
                break;
            }
            case StrategyId::ArbieSpreader: {
                setCell(cells, contractIndex, PnlDetailColumns::ASOpen, std::to_string(pnl._open));
                setCell(cells, contractIndex, PnlDetailColumns::ASClosed, std::to_string(pnl._closed));
                setCell(cells, contractIndex, PnlDetailColumns::ASTotal, std::to_string(pnl.getTotal()));
                break;
            }
            case StrategyId::MAX:
                break;
        }
        setCell(cells, contractIndex, PnlDetailColumns::Total, std::to_string(totalPnl));
    }

    void populateErrorMessage(proto::GridUpdate& message, ErrorMessage& m) {
        message.set_gridid(toUInt(WindowIds::Errors));
        message.set_updatetype(proto::GridUpdateType::CellsType);
        auto cells = message.mutable_cells();
        setCell(cells, _errorMessageNumber, ErrorsColumns::Time, std::to_string(TimeUtils::clock_gettime()));
        setCell(cells, _errorMessageNumber, ErrorsColumns::Message, std::string(m._message, m._size));
        setCell(cells, _errorMessageNumber, ErrorsColumns::Type, std::to_string(toUInt(m._type)));
        ++_errorMessageNumber;
    }

    void populateAuditMessage(uint32_t gridId, proto::GridUpdate& message, GridUpdate& update, uint32_t trader,
                              const char* const user) {
        message.set_gridid(toUInt(WindowIds::Audit));
        message.set_updatetype(proto::GridUpdateType::CellsType);
        auto cells = message.mutable_cells();
        for (int i = 0; i < update._update.cells_size(); ++i) {
            proto::GridCell& gridCell = *update._update.mutable_cells(i);
            setCell(cells, _auditMessageNumber, AuditColumns::Time, std::to_string(TimeUtils::clock_gettime()));
            setCell(cells, _auditMessageNumber, AuditColumns::User, user ? user : "unknown");
            setCell(cells, _auditMessageNumber, AuditColumns::Trader, std::to_string(trader));
            setCell(cells, _auditMessageNumber, AuditColumns::Grid, std::to_string(toUInt(gridId)));
            setCell(cells, _auditMessageNumber, AuditColumns::Row, getRowLabel(gridId, gridCell.row()));
            setCell(cells, _auditMessageNumber, AuditColumns::Column, getColumnLabel(gridId, gridCell.col()));
            setCell(cells, _auditMessageNumber, AuditColumns::OldValue, getOldValue(update, gridCell.row(), gridCell.col()));
            setCell(cells, _auditMessageNumber, AuditColumns::NewValue, getNewValue(gridId, gridCell.col(), gridCell.value()));
            ++_auditMessageNumber;
        }
    }

    void populateManualFilAuditMessage(proto::GridUpdate& message, uint32_t contractId, hf::qty_t size, uint32_t trader, StrategyId strategy) {
        message.set_gridid(toUInt(WindowIds::Audit));
        message.set_updatetype(proto::GridUpdateType::CellsType);
        auto cells = message.mutable_cells();
        setCell(cells, _auditMessageNumber, AuditColumns::Time, std::to_string(TimeUtils::clock_gettime()));
        setCell(cells, _auditMessageNumber, AuditColumns::Trader, std::to_string(trader));
        setCell(cells, _auditMessageNumber, AuditColumns::Grid, std::to_string(toUInt(WindowIds::ManualFillsEntry)));
        std::string value = createManualFillString(contractId, size, strategy);
        setCell(cells, _auditMessageNumber, AuditColumns::NewValue, value);
        ++_auditMessageNumber;
    }
    
    std::string createManualFillString(uint32_t contractId, hf::qty_t size, StrategyId strategy) {
        std::string fillType = toString(strategy);
        auto& contract = _config->_allContracts[contractId];
        std::string value;
        value.append(contract._name).append(": ").append(std::to_string(size)).append(": ").append(fillType);
        return value;
    }

    auto populateFillMessage(FillMessage& fill, proto::GridUpdate& message, RelativeValue* relativeValue, MarketMaker* marketMaker) {
        message.set_gridid(toUInt(WindowIds::FillsDisplay));
        message.set_updatetype(proto::GridUpdateType::CellsType);
        auto cells = message.mutable_cells();
        auto& contract = _config->_allContracts[fill._contractId];
        auto color = static_cast<uint32_t>(fill._side == BID ? Color::TRANSPARENT_BID_BLUE : Color::TRANSPARENT_OFFER_RED);
        hf::nanos_t time = TimeUtils::clock_gettime();
        setCell(cells, _fillNumber, FillsColumns::Number, std::to_string(_fillNumber), color);
        setCell(cells, _fillNumber, FillsColumns::Time, std::to_string(time), color);
        setCell(cells, _fillNumber, FillsColumns::Contract, std::to_string(contract._internalId), color);
        setCell(cells, _fillNumber, FillsColumns::Side, std::to_string(fill._side), color);
        setCell(cells, _fillNumber, FillsColumns::Price, std::to_string(fill._price), color);
        setCell(cells, _fillNumber, FillsColumns::Size, std::to_string(fill._qty), color);
        setCell(cells, _fillNumber, FillsColumns::Strategy, std::to_string(toUInt(fill._strategy)), color);
        setCell(cells, _fillNumber, FillsColumns::Type, std::to_string(toUInt(fill._type)), color);
        setCell(cells, _fillNumber, FillsColumns::Source, std::to_string(toUInt(fill._source)), color);
        double edgeStf = 0;
        double width = 0;
        if (fill._strategy == StrategyId::RelativeValue) {
            if (fill._rvStf != hf::NULL_PRICE) {
                width = ToDollars(relativeValue->_contractWidths.getValue(fill._contractId));
                if (width != 0) {
                    edgeStf = ToDollars(fill._side == BID ? fill._rvStf - fill._price : fill._price - fill._rvStf) / width;
                }
            }
        } else {
            auto& mm = marketMaker->_marketMakers[fill._contractId];
            if (mm._contract._toMarketMake) {
                width = ToDollars(mm._width);
            }
        }
        RvContract& rvContract = relativeValue->_contracts[fill._contractId];
        const bool useWightedMid = relativeValue->_useWeightedMid.getValue(fill._contractId);
        double mkt = -1;
        if ((rvContract.externalMarket<BID>().getPrice() != hf::NULL_PRICE) & (rvContract.externalMarket<ASK>().getPrice() != hf::NULL_PRICE)) {
            if (useWightedMid) {
                mkt = relativeValue->calculateWeightedMidpoint(rvContract.externalMarket<BID>(), rvContract.externalMarket<ASK>(), rvContract._establishedLevel);
            } else {
                mkt = relativeValue->calculateSimpleAverage(rvContract.externalMarket<BID>(), rvContract.externalMarket<ASK>());
            }
        }
        double edgeMkt = 0;
        if ((width != 0) & (mkt != -1)) {
            edgeMkt = (fill._side == BID ? mkt - ToDollars(fill._price) : ToDollars(fill._price) - mkt) / width;
        }
        if (fill._strategy == StrategyId::MarketMaker) {
            edgeStf = edgeMkt;
        }
        setCell(cells, _fillNumber, FillsColumns::EdgeStf, std::to_string(edgeStf), color);
        setCell(cells, _fillNumber, FillsColumns::EdgeMkt, std::to_string(edgeMkt), color);
        ++_fillNumber;

        return std::make_tuple(time, edgeStf, edgeMkt);
    }

    bool populateChartDataPoints(proto::DataSeriesPointSets& message, RelativeValue* relativeValue) {
        auto sets = message.mutable_sets();
        for (uint32_t vectorIndex = 0; vectorIndex < relativeValue->_numberOfVectors; ++vectorIndex) {
            auto set = sets->Add();
            auto points = set->mutable_points();
            {
                const double ltf = relativeValue->_longTermFairsInVector.getValue(vectorIndex);
                auto point = points->Add();
                point->set_dataseriesid(toUInt(DataSeries::LTF));
                point->set_value(ltf);
            }
            {
                const double mkt = relativeValue->_currentVectorMarketPrices->getValue(vectorIndex);
                auto point = points->Add();
                point->set_dataseriesid(toUInt(DataSeries::VectorPrice));
                point->set_value(mkt);
            }
            {
                double stf = relativeValue->_shortTermFairsInVector.getValue(vectorIndex);
                    auto point = points->Add();
                    point->set_dataseriesid(toUInt(DataSeries::STF));
                    point->set_value(stf);
            }
            set->set_chartid(vectorIndex);
            set->set_time(hf::util::TimeUtils::clock_gettime());
        }
        return sets->size() > 0;
    }

    void populateMarketMakerEnabled(proto::GridUpdate& message, uint32_t id, bool enabled) {
        message.set_gridid(toUInt(WindowIds::MarketMakerParameters));
        message.set_updatetype(proto::GridUpdateType::CellsType);
        auto cells = message.mutable_cells();
        setCell(cells, id, MarketMakerParametersColumns::Enabled, std::to_string(enabled));
    }

    template<typename ExecutorT>
    void populateGUIInitialization(proto::GUIInitialization& message, std::vector<ViperStrategyBase*>& strategies,
                                   ExecutorT* executor) {
        auto definitions = message.mutable_definitions();
        populateMainControl(definitions->Add(), executor);
        populateErrors(definitions->Add());
        populateTraderSwitch(definitions->Add());
        for (auto strategy : strategies) {
            strategy->populateGUIInitialization(definitions);
        }
        populateLeanMatrix(definitions->Add());
        populatePricingConfig(definitions->Add());
        populateManualFillsEntry(definitions->Add());
        populateFills(definitions->Add());
        populateGatewayStatuses(definitions->Add());
        populateStackerParameters(definitions->Add());
        populatePNL(definitions);
        populatePnlDetail(definitions);
        populateGlobalConfigs(definitions->Add());
        populateMessageCounts(definitions);
        populatePositionInOutrights(definitions);
        populateAudit(definitions->Add());
        tryPopulateGTCGridColumns(message);
        tryPopulateStatusDebugGridColumns(message);
        populateLoggingLevel(definitions->Add());
        populateNegativeScalpCheckerConfig(definitions->Add());
        populateEconomicNumbers(definitions->Add(), executor);

        auto enumDefinitions = message.mutable_enumdefinitions();
        addContractsEnum(enumDefinitions->Add());
        addFactorsEnum(enumDefinitions->Add());
        addFillSidesEnum(enumDefinitions->Add());
        addStrategiesEnum(enumDefinitions->Add());
        addFillTypeEnum(enumDefinitions->Add());
        addFillSourceEnum(enumDefinitions->Add());
        addSwitchesEnum(enumDefinitions->Add());
        addErrorTypesEnum(enumDefinitions->Add());
        addYesNoEnum(enumDefinitions->Add());
        addLeanType(enumDefinitions->Add());
        addManualFillTypes(enumDefinitions->Add());
        addTradersEnum(enumDefinitions->Add());
        addShiftsEnum(enumDefinitions->Add());
        addGridsEnum(enumDefinitions->Add());
        addStatusDebugEnum(enumDefinitions->Add());
        addAllContractsEnum(enumDefinitions->Add());
        addEMAOnTimeEnum(enumDefinitions->Add());
        addEMAOffTimeEnum(enumDefinitions->Add());
        addMMQueueImportanceEnum(enumDefinitions->Add());
        addLoggingLevelEnum(enumDefinitions->Add());
        addEconomicNumberColorEnum(enumDefinitions->Add());
        addColorDefinitions(message);
        addChartDefinitions(message);
    }

    void addChartDefinitions(proto::GUIInitialization& message) {
        auto chartDefinitions = message.mutable_charts();
        for (uint32_t index = 0; index < _config->_vectorConfigs.size(); ++index) {
            auto& config = _config->_vectorConfigs[index];
            addChart(config._vectorName, config._chartNumberOfDecimalPlaces, index, chartDefinitions->Add());
        }
    }

    void addColorDefinitions(proto::GUIInitialization& message) {
        auto* colorDefinition = message.mutable_colors();

        auto addColor = [&colorDefinition](Color color) {
            auto* colorDef = colorDefinition->Add();
            colorDef->set_id(static_cast<uint32_t>(color));
            colorDef->set_argb(ColorDefinition(color));
        };

        addColor(Color::TRANSPARENT_RED);
        addColor(Color::TRANSPARENT_GREEN);
        addColor(Color::TRANSPARENT);
        addColor(Color::TRANSPARENT_OFFER_RED);
        addColor(Color::TRANSPARENT_BID_BLUE);
        addColor(Color::TRANSPARENT_SWITCH_ENABLED);
        addColor(Color::TRANSPARENT_SWITCH_DISABLED);
    }

    void addChart(const std::string& name, uint32_t precision, uint32_t id, proto::Chart* chart) {
        chart->set_name(name);
        chart->set_id(id);
        chart->set_numberofdecimalplaces(precision);
        auto series = chart->mutable_series();
        addDataSeries(DataSeries::VectorPrice, "VectorPrice", series->Add());
        addDataSeries(DataSeries::LTF, "LTF", series->Add());
        addDataSeries(DataSeries::STF, "STF", series->Add());
    }

    void addDataSeries(DataSeries id, const std::string& name, proto::DataSeries* series) {
        series->set_id(toUInt(id));
        series->set_name(name);
    }

    void addYesNoEnum(proto::EnumDefinition* definition) {
        definition->set_name("YesNo");
        auto members = definition->mutable_members();
        addEnumMember(members, "Yes", toUInt(YesNoType::Yes));
        addEnumMember(members, "No", toUInt(YesNoType::No));
        addEnumMember(members, "-", toUInt(YesNoType::Unknown));
    }

    void addLeanType(proto::EnumDefinition* definition) {
        definition->set_name("LeanType");
        auto members = definition->mutable_members();
        addEnumMember(members, "External", toUInt(LeanType::External));
        addEnumMember(members, "MM_Quote", toUInt(LeanType::MM_Quote));
        addEnumMember(members, "MM_Hedge", toUInt(LeanType::MM_Hedge));
        addEnumMember(members, "RV_Quote", toUInt(LeanType::RV_Quote));
        addEnumMember(members, "AS_Quote", toUInt(LeanType::AS_Quote));
        addEnumMember(members, "", toUInt(LeanType::None));
    }

    void addErrorTypesEnum(proto::EnumDefinition* definition) {
        definition->set_name("errorTypes");
        auto members = definition->mutable_members();
        addEnumMember(members, "Critical", toUInt(ErrorType::Critical));
        addEnumMember(members, "Normal", toUInt(ErrorType::Normal));
    }

    void addSwitchesEnum(proto::EnumDefinition* definition) {
        definition->set_name("switches");
        auto members = definition->mutable_members();
        addEnumMember(members, "Enabled", toUInt(proto::SystemState::SystemStateEnabled));
        addEnumMember(members, "Disabled", toUInt(proto::SystemState::SystemStateDisabled));
        addEnumMember(members, "Shutdown", toUInt(proto::SystemState::SystemStateHardShutdown));
    }

    void addFillTypeEnum(proto::EnumDefinition* definition) {
        definition->set_name("fillTypes");
        auto members = definition->mutable_members();
        addEnumMember(members, "Internal", toUInt(FillType::Internal));
        addEnumMember(members, "External", toUInt(FillType::External));
    }

    void addFillSourceEnum(proto::EnumDefinition* definition) {
        definition->set_name("fillSource");
        auto members = definition->mutable_members();
        addEnumMember(members, "Quote", toUInt(FillSource::Quote));
        addEnumMember(members, "Missed Hedge", toUInt(FillSource::MissedHedge));
        addEnumMember(members, "Overfill", toUInt(FillSource::Overfill));
        addEnumMember(members, "Hedge", toUInt(FillSource::Hedge));
        addEnumMember(members, "SelfMatch", toUInt(FillSource::SelfMatch));
        addEnumMember(members, "RV Disable", toUInt(FillSource::RvDisable));
        addEnumMember(members, "RV HedgeReject", toUInt(FillSource::RvHedgeReject));
        addEnumMember(members, "Viper Off", toUInt(FillSource::ViperOff));
        addEnumMember(members, "Viper Start", toUInt(FillSource::ViperStart));
        addEnumMember(members, "Manual", toUInt(FillSource::Manual));
        addEnumMember(members, "AS Disable", toUInt(FillSource::AsDisable));
        addEnumMember(members, "AS HedgeReject", toUInt(FillSource::AsHedgeReject));
        addEnumMember(members, "AS UnableToHedge", toUInt(FillSource::ASUnableToHedge));
        addEnumMember(members, "AS To RV", toUInt(FillSource::AsToRv));
    }

    void addStrategiesEnum(proto::EnumDefinition* definition) {
        definition->set_name("strategies");
        auto members = definition->mutable_members();
        addEnumMember(members, "Relative Value", toUInt(StrategyId::RelativeValue));
        addEnumMember(members, "Market Maker", toUInt(StrategyId::MarketMaker));
        addEnumMember(members, "Arbie Spreader", toUInt(StrategyId::ArbieSpreader));
    }

    void addManualFillTypes(proto::EnumDefinition* definition) {
        definition->set_name("manual_fill_types");
        auto members = definition->mutable_members();
        addEnumMember(members, "Market Maker", toUInt(ManualFillType::MarketMaker));
        addEnumMember(members, "Relative Value", toUInt(ManualFillType::RelativeValue));
        addEnumMember(members, "Arbie Spreader", toUInt(ManualFillType::ArbieSpreader));
    }

    void addFillSidesEnum(proto::EnumDefinition* definition) {
        definition->set_name("fillSides");
        auto members = definition->mutable_members();
        addEnumMember(members, "BUY", 0);
        addEnumMember(members, "SELL", 1);
    }

    void addContractsEnum(proto::EnumDefinition* definition) {
        definition->set_name("contracts");
        auto members = definition->mutable_members();
        for (auto& mmc : _config->_marketMakerConfigs) {
            uint32_t internalId = _config->getInternalId(mmc._contract);
            addEnumMember(members, mmc._contract, internalId);
        }
    }

    void addFactorsEnum(proto::EnumDefinition* definition) {
        definition->set_name("factors");
        auto members = definition->mutable_members();
        for (unsigned i = 0; i < _config->_vectorConfigs.size(); ++i) {
            addEnumMember(members, _config->_vectorConfigs[i]._vectorName, i);
        }
    }

    void addTradersEnum(proto::EnumDefinition* definition) {
        definition->set_name("traders");
        auto members = definition->mutable_members();
        for (size_t i = 0; i < _config->_traders.size(); ++i) {
            auto& trader = _config->_traders[i];
            addEnumMember(members, trader, i);
        }
    }

    void addShiftsEnum(proto::EnumDefinition* definition) {
        definition->set_name("shifts");
        auto members = definition->mutable_members();
        addEnumMember(members, "Asia", toUInt(Shifts::Asia));
        addEnumMember(members, "Europe", toUInt(Shifts::Europe));
        addEnumMember(members, "US", toUInt(Shifts::US));
        addEnumMember(members, "SafeMode", toUInt(Shifts::SafeMode));
    }

    void addGridsEnum(proto::EnumDefinition* definition) {
        definition->set_name("grids");
        auto members = definition->mutable_members();
        addEnumMember(members, "Audit Trail", toUInt(WindowIds::Audit));
        addEnumMember(members, "Commissions", toUInt(WindowIds::Commissions));
        addEnumMember(members, "Relative Value Parameters", toUInt(WindowIds::ContractParametersRv));
        addEnumMember(members, "Errors", toUInt(WindowIds::Errors));
        addEnumMember(members, "Factor Hedged", toUInt(WindowIds::FactorHedged));
        addEnumMember(members, "Fills", toUInt(WindowIds::FillsDisplay));
        addEnumMember(members, "Gateway Status", toUInt(WindowIds::GatewayStatuses));
        addEnumMember(members, "Hedge Ratios", toUInt(WindowIds::HedgeRatios));
        addEnumMember(members, "Global Configs", toUInt(WindowIds::GlobalConfigs));
        addEnumMember(members, "Lean Matrix", toUInt(WindowIds::LeanMatrix));
        addEnumMember(members, "PNL", toUInt(WindowIds::PNL));
        addEnumMember(members, "Stacker Parameters", toUInt(WindowIds::StackerParameters));
        addEnumMember(members, "Trader Login", toUInt(WindowIds::TraderSwitch));
        addEnumMember(members, "Risk Matrix", toUInt(WindowIds::RiskMatrix));
        addEnumMember(members, "Quote Sizes", toUInt(WindowIds::QuoteSizes));
        addEnumMember(members, "Pricing Matrix", toUInt(WindowIds::PricingMatrix));
        addEnumMember(members, "Pricing Parameters", toUInt(WindowIds::PricingConfig));
        addEnumMember(members, "PNL Detail", toUInt(WindowIds::PnlDetail));
        addEnumMember(members, "Market Maker Parameters", toUInt(WindowIds::MarketMakerParameters));
        addEnumMember(members, "Manual Fills Entry", toUInt(WindowIds::ManualFillsEntry));
        addEnumMember(members, "Main Control", toUInt(WindowIds::MainControl));
        addEnumMember(members, "Positions", toUInt(WindowIds::PositionInOutrights));
        addEnumMember(members, "Messages", toUInt(WindowIds::MessagesCounts));
        addEnumMember(members, "Factor Parameters", toUInt(WindowIds::VectorParameters));
        if(allowGTCs()) {
            addEnumMember(members, "GTC Management", toUInt(WindowIds::GTCManagement));
        }
    }

    COLD
    void addEMAOnTimeEnum(proto::EnumDefinition* definition) {
        definition->set_name("ema_on_time");
        auto members = definition->mutable_members();
#ifdef IS_DEV
        addTimeEnum(members, 100, 1700);
#else
        addTimeEnum(members, 100, 815);
#endif
    }

    COLD
    void addEMAOffTimeEnum(proto::EnumDefinition* definition) {
        definition->set_name("ema_off_time");
        auto members = definition->mutable_members();
#ifdef IS_DEV
        addTimeEnum(members, 600, 1730);
#else
        addTimeEnum(members, 1030, 1415);
#endif
    }

    template<typename T>
    COLD void addTimeEnum(T* members, uint32_t begin, uint32_t end) {
        uint32_t min = begin % 100;
        uint32_t i = begin;
        begin /= 100;
        for (; i < end;) {
            std::stringstream ss;
            ss << std::setw(2) << std::setfill('0') << begin << ":" << std::setw(2) << std::setfill('0') << min;
            addEnumMember(members, ss.str(), i);
#ifdef IS_DEV
            min += 1;
#else
            min += 15;
#endif
            if (min == 60) {
                begin += 1;
                i = begin * 100;
                min = 0;
            } else {
#ifdef IS_DEV
                i += 1;
#else
                i += 15;
#endif
            }
        }
    }

    COLD
    void addMMQueueImportanceEnum(proto::EnumDefinition* definition) {
        definition->set_name("queue_importance");
        auto members = definition->mutable_members();
        for (uint32_t i = 1; i < 11; ++i) {
            addEnumMember(members, std::to_string(i), i);
        }
    }

    template <typename T>
    void addEnumMember(T* members, const std::string& display, uint32_t id) {
        auto member = members->Add();
        member->set_display(display);
        member->set_id(id);
    }

    void populatePositionInOutrights(::google::protobuf::RepeatedPtrField< ::Python::proto::GridDefinition >* definitions) {
        auto [definition, rowHeader, cells] = 
                addWindow(definitions, WindowIds::PositionInOutrights, "Positions", proto::GridType::Table);

        addHeader(definition, {
                headerCell(PositionsColumns::RV, "RV", true, proto::DataType::signed_type),
                headerCell(PositionsColumns::AS, "AS", true, proto::DataType::signed_type),
                headerCell(PositionsColumns::MMInternal, "MMInternal", true, proto::DataType::signed_type),
                headerCell(PositionsColumns::MMExternal, "MMExternal", true, proto::DataType::signed_type),
                headerCell(PositionsColumns::Total, "Total", true, proto::DataType::signed_type),
        });
        
        std::map<uint32_t, Position> positions;
        getPositions(positions);

        for (auto& pair : positions) {
            ConfigContract& contract = _config->_allContracts[pair.first];
            addRowHeaderCell(WindowIds::PositionInOutrights, rowHeader, pair.first, contract._name);
            Position& position = pair.second;
            addCells(cells, pair.first, {
                    Cell(PositionsColumns::RV, std::to_string(position._rv)),
                    Cell(PositionsColumns::AS, std::to_string(position._as)),
                    Cell(PositionsColumns::MMInternal, std::to_string(position._mmInternal)),
                    Cell(PositionsColumns::MMExternal, std::to_string(position._mmExternal)),
                    Cell(PositionsColumns::Total, std::to_string(position._total)),
            });
        }
    }

    void populateAudit(proto::GridDefinition* definition) {
        definition->set_gridid(toUInt(WindowIds::Audit));
        definition->set_name("Audit Trail");
        definition->set_type(proto::GridType::List);
        auto columnHeader = definition->mutable_columnheader();
        addColumnHeaderCell(WindowIds::Audit, columnHeader, AuditColumns::Time, "Time", true, proto::DataType::rhone_time);
        addColumnHeaderCell(WindowIds::Audit, columnHeader, AuditColumns::User, "User", true, proto::DataType::string_type);
        addColumnHeaderCell(WindowIds::Audit, columnHeader, AuditColumns::Trader, "Trader", true, proto::DataType::enum_type, "traders");
        addColumnHeaderCell(WindowIds::Audit, columnHeader, AuditColumns::Grid, "Grid", true, proto::DataType::enum_type, "grids");
        addColumnHeaderCell(WindowIds::Audit, columnHeader, AuditColumns::Row, "Row", true, proto::DataType::string_type);
        addColumnHeaderCell(WindowIds::Audit, columnHeader, AuditColumns::Column, "Column", true, proto::DataType::string_type);
        addColumnHeaderCell(WindowIds::Audit, columnHeader, AuditColumns::OldValue, "OldValue", true, proto::DataType::string_type);
        addColumnHeaderCell(WindowIds::Audit, columnHeader, AuditColumns::NewValue, "NewValue", true, proto::DataType::string_type);
    }

    void populateTraderSwitch(proto::GridDefinition* definition) {
        definition->set_gridid(toUInt(WindowIds::TraderSwitch));
        definition->set_name("Trader Login");
        definition->set_type(proto::GridType::Table);

        auto columnHeader = definition->mutable_columnheader();

        addColumnHeaderCell(WindowIds::TraderSwitch, columnHeader, TraderSwitchColumns::Trader, "Trader", false, proto::DataType::enum_type, "traders");
        addColumnHeaderCell(WindowIds::TraderSwitch, columnHeader, TraderSwitchColumns::Shift, "Shift", false, proto::DataType::enum_type, "shifts");

        auto rowHeader = definition->mutable_rowheader();
        addRowHeaderCell(WindowIds::TraderSwitch, rowHeader, 0, "Trader/Shift: ");

        auto cells = definition->mutable_cells();
        setCell(cells, 0, TraderSwitchColumns::Trader, std::to_string(_config->_currentTrader));
        setCell(cells, 0, TraderSwitchColumns::Shift, std::to_string(toUInt(Shifts::US)));

        addSubmitButton(definition);
    }

    void populateMessageCounts(::google::protobuf::RepeatedPtrField< ::Python::proto::GridDefinition >* definitions) {
        auto [definition, rowHeader, cells] = addWindow(definitions, WindowIds::MessagesCounts, "Message Counts", proto::GridType::Table);
        addHeader(definition, {
                headerCell(MessageCountsColumns::Inserts, "Insert", true, proto::DataType::unsigned_type),
                headerCell(MessageCountsColumns::Modifies, "Modify", true, proto::DataType::unsigned_type),
                headerCell(MessageCountsColumns::Cancels, "Cancel", true, proto::DataType::unsigned_type),
                headerCell(MessageCountsColumns::CancelRejects, "CancelRejects", true, proto::DataType::unsigned_type),
                headerCell(MessageCountsColumns::ModifyRejects, "ModifyRejects", true, proto::DataType::unsigned_type),
                headerCell(MessageCountsColumns::Fills, "Fills", true, proto::DataType::unsigned_type),
                headerCell(MessageCountsColumns::FillsRatio, "FillsRatio", true, proto::DataType::double_type),
        });

        for (auto& config : _config->_marketMakerConfigs) {
            uint32_t internalId = _config->getInternalId(config._contract);
            addRowHeaderCell(WindowIds::MessagesCounts, rowHeader,internalId, config._contract);
            addCells(cells, internalId, {
                    Cell(MessageCountsColumns::Inserts, "0"),
                    Cell(MessageCountsColumns::Modifies, "0"),
                    Cell(MessageCountsColumns::Cancels, "0"),
                    Cell(MessageCountsColumns::CancelRejects, "0"),
                    Cell(MessageCountsColumns::ModifyRejects, "0"),
                    Cell(MessageCountsColumns::Fills, "0"),
                    Cell(MessageCountsColumns::FillsRatio, "0"),
            });
        }

        auto total = _config->_allContracts.size();
        addRowHeaderCell(WindowIds::MessagesCounts, rowHeader,total, "Total");
        setCell(cells, total, MessageCountsColumns::Inserts, "0");
        setCell(cells, total, MessageCountsColumns::Modifies, "0");
        setCell(cells, total, MessageCountsColumns::Cancels, "0");
        setCell(cells, total, MessageCountsColumns::CancelRejects, "0");
        setCell(cells, total, MessageCountsColumns::ModifyRejects, "0");
        setCell(cells, total, MessageCountsColumns::Fills, "0");
        setCell(cells, total, MessageCountsColumns::FillsRatio, "0");
    }

    void populatePnlDetail(::google::protobuf::RepeatedPtrField< ::Python::proto::GridDefinition >* definitions) {
        auto [definition, rowHeader, cells] = addWindow(definitions, WindowIds::PnlDetail, "PNL Detail", proto::GridType::Table);

        addHeader(definition, {
                headerCell(PnlDetailColumns::MMOpen, "MM Open", true, proto::DataType::rhone_price),
                headerCell(PnlDetailColumns::MMClosed, "MM Closed", true, proto::DataType::rhone_price),
                headerCell(PnlDetailColumns::MMTotal, "MM Total", true, proto::DataType::rhone_price),
                headerCell(PnlDetailColumns::RVOpen, "RV Open", true, proto::DataType::rhone_price),
                headerCell(PnlDetailColumns::RVClosed, "RV Closed", true, proto::DataType::rhone_price),
                headerCell(PnlDetailColumns::RVTotal, "RV Total", true, proto::DataType::rhone_price),
                headerCell(PnlDetailColumns::ASOpen, "AS Open", true, proto::DataType::rhone_price),
                headerCell(PnlDetailColumns::ASClosed, "AS Closed", true, proto::DataType::rhone_price),
                headerCell(PnlDetailColumns::ASTotal, "AS Total", true, proto::DataType::rhone_price),
                headerCell(PnlDetailColumns::Total, "Total", true, proto::DataType::rhone_price),
        });

        for (auto& config : _config->_marketMakerConfigs) {
            uint32_t internalId = _config->getInternalId(config._contract);
            addRowHeaderCell(WindowIds::PnlDetail, rowHeader,internalId, config._contract);
            addCells(cells, internalId, {
                    Cell(PnlDetailColumns::MMOpen, "0"),
                    Cell(PnlDetailColumns::MMClosed, "0"),
                    Cell(PnlDetailColumns::RVOpen, "0"),
                    Cell(PnlDetailColumns::RVClosed, "0"),
                    Cell(PnlDetailColumns::ASOpen, "0"),
                    Cell(PnlDetailColumns::ASClosed, "0"),
                    Cell(PnlDetailColumns::MMTotal, "0"),
                    Cell(PnlDetailColumns::RVTotal, "0"),
                    Cell(PnlDetailColumns::ASTotal, "0"),
                    Cell(PnlDetailColumns::Total, "0"),
            });
        }
    }

    void populateGlobalConfigs(proto::GridDefinition* definition) {
        definition->set_gridid(toUInt(WindowIds::GlobalConfigs));
        definition->set_name("Global Configs");
        definition->set_type(proto::GridType::Table);

        auto columnHeader = definition->mutable_columnheader();
        addColumnHeaderCell(WindowIds::GlobalConfigs, columnHeader, GlobalConfigsColumns::Value, "Value", true, proto::DataType::string_type);
        for (uint32_t i = 0; i < toUInt(Shifts::__current_marker__); ++i) {
            addColumnHeaderCell(WindowIds::GlobalConfigs, columnHeader, static_cast<GlobalConfigsColumns>(i + 1), to_string((Shifts)i), false, proto::DataType::string_type);
        }

        auto rowHeader = definition->mutable_rowheader();
        auto cells = definition->mutable_cells();

        for (uint32_t i = 0; i < _config->_globalConfigs.size(); ++i) {
            auto& config = _config->_globalConfigs[i];
            addRowHeaderCell(WindowIds::GlobalConfigs, rowHeader, i, config._key);
            setCell(cells, i, GlobalConfigsColumns::Value, config._value);
            setCell(cells, i, GlobalConfigsColumns::Asia, config._shifts[toUInt(Shifts::Asia)]);
            setCell(cells, i, GlobalConfigsColumns::Europe, config._shifts[toUInt(Shifts::Europe)]);
            setCell(cells, i, GlobalConfigsColumns::US, config._shifts[toUInt(Shifts::US)]);
            setCell(cells, i, GlobalConfigsColumns::Release, config._shifts[toUInt(Shifts::SafeMode)]);
        }

        auto buttons = definition->mutable_buttons();
        addButton(buttons, toUInt(0), "Submit", proto::ButtonFrontBehavior_CommitValues, proto::ButtonBackBehavior_Send, RequiresConfirmation::NO);
    }

    void populatePNL(::google::protobuf::RepeatedPtrField< ::Python::proto::GridDefinition >* definitions) {
        auto [definition, rowHeader, cells] = addWindowWithSubmitButton(definitions, WindowIds::PNL, "PNL", proto::GridType::Table);
        addHeader(definition, {
                headerCell(PNLColumns::Open, "Open", true, proto::DataType::rhone_price),
                headerCell(PNLColumns::Closed, "Closed", true, proto::DataType::rhone_price),
                headerCell(PNLColumns::Total, "Total", true, proto::DataType::rhone_price),
                headerCell(PNLColumns::Drawdown, "Drawdown", true, proto::DataType::rhone_price),
                headerCell(PNLColumns::PnlLimit, "PnlLimit", true, proto::DataType::unsigned_type),
                headerCell(PNLColumns::DrawdownLimit, "DrawdownLimit", true, proto::DataType::unsigned_type),
                headerCell(PNLColumns::AsiaPnlLimit, "AsiaPnlLimit", false, proto::DataType::unsigned_type),
                headerCell(PNLColumns::AsiaDrawdownLimit, "AsiaDrawdownLimit", false, proto::DataType::unsigned_type),
                headerCell(PNLColumns::EuropePnlLimit, "EuropePnlLimit", false, proto::DataType::unsigned_type),
                headerCell(PNLColumns::EuropeDrawdownLimit, "EuropeDrawdownLimit", false, proto::DataType::unsigned_type),
                headerCell(PNLColumns::USPnlLimit, "USPnlLimit", false, proto::DataType::unsigned_type),
                headerCell(PNLColumns::USDrawdownLimit, "USDrawdownLimit", false, proto::DataType::unsigned_type),
                headerCell(PNLColumns::SafeModePnlLimit, "SafeModePnlLimit", false, proto::DataType::unsigned_type),
                headerCell(PNLColumns::SafeModeDrawdownLimit, "SafeModeDrawdownLimit", false, proto::DataType::unsigned_type),
                headerCell(PNLColumns::ResetDrawdown, "ResetDrawdown", false, proto::DataType::button),
        });

        addRowHeaderCell(WindowIds::PNL, rowHeader, PNLRows::RelativeValue, "Relative Value");
        addRowHeaderCell(WindowIds::PNL, rowHeader, PNLRows::MarketMakers, "Market Maker");
        addRowHeaderCell(WindowIds::PNL, rowHeader, PNLRows::ArbieSpreader, "Arbie Spreader");
        addRowHeaderCell(WindowIds::PNL, rowHeader, PNLRows::Total, "Total");

        std::vector<PNLColumns> shiftColumns = {
                PNLColumns::AsiaPnlLimit,
                PNLColumns::AsiaDrawdownLimit,
                PNLColumns::EuropePnlLimit,
                PNLColumns::EuropeDrawdownLimit,
                PNLColumns::USPnlLimit,
                PNLColumns::USDrawdownLimit,
                PNLColumns::SafeModePnlLimit,
                PNLColumns::SafeModeDrawdownLimit,
        };

        for (uint32_t i = 0; i < toUInt(PNLRows::__end_marker__); ++i) {
            setCell(cells, i, PNLColumns::Open, "0");
            setCell(cells, i, PNLColumns::Closed, "0");
            setCell(cells, i, PNLColumns::Total, "0");
            setCell(cells, i, PNLColumns::Drawdown, "0");
            uint32_t pnlLimit = 0;
            uint32_t drawdownLimit = 0;
            std::vector<uint32_t> shiftLimits(shiftColumns.size(), 0);
            const PNLConfig& config = _config->_pnlConfigs[i];
            pnlLimit = config._limit;
            drawdownLimit = config._drawdown;
            for (uint32_t ii = 0, j = 0; ii < toUInt(Shifts::__current_marker__); ++ii) {
                shiftLimits[j++] = config._shiftLimit[ii];
                shiftLimits[j++] = config._shiftDrawdown[ii];
            }
            setCell(cells, i, PNLColumns::PnlLimit, std::to_string(pnlLimit));
            setCell(cells, i, PNLColumns::DrawdownLimit, std::to_string(drawdownLimit));

            for (uint32_t ii = 0; ii < shiftLimits.size(); ++ii) {
                setCell(cells, i, shiftColumns[ii], std::to_string(shiftLimits[ii]));
            }
            setCell(cells, i, PNLColumns::ResetDrawdown, "Reset Drawdown", toUInt(Color::TRANSPARENT));
        }
    }

    void populateStackerParameters(proto::GridDefinition* definition) {
        definition->set_gridid(toUInt(WindowIds::StackerParameters));
        definition->set_name("Stacker Parameters");
        definition->set_type(proto::GridType::Table);

        auto columnHeader = definition->mutable_columnheader();

        addColumnHeaderCell(WindowIds::StackerParameters, columnHeader, StackerParametersColumns::QuoteSize, "QuoteSize", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::StackerParameters, columnHeader, StackerParametersColumns::QuoteGap, "QuoteGap", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::StackerParameters, columnHeader, StackerParametersColumns::MaxQuote, "MaxQuote", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::StackerParameters, columnHeader, StackerParametersColumns::StackLevels, "StackLevels", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::StackerParameters, columnHeader, StackerParametersColumns::StackSlack, "StackSlack", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::StackerParameters, columnHeader, StackerParametersColumns::MaxTicksOffTopLevel, "MaxTicksOffTopLevel", false, proto::DataType::signed_type);
        addColumnHeaderCell(WindowIds::StackerParameters, columnHeader, StackerParametersColumns::UseGTC, "UseGTC", !allowGTCs(), proto::DataType::bool_type);
        addColumnHeaderCell(WindowIds::StackerParameters, columnHeader, StackerParametersColumns::AllowMultipleNewHedge, "AllowMultipleNewHedge", false, proto::DataType::bool_type);
        addColumnHeaderCell(WindowIds::StackerParameters, columnHeader, StackerParametersColumns::EnableQuoteSprayer, "EnableQuoteSprayer", false, proto::DataType::bool_type);
        addColumnHeaderCell(WindowIds::StackerParameters, columnHeader, StackerParametersColumns::RedNumberPull, "RedNumberPull", false, proto::DataType::bool_type);
        addColumnHeaderCell(WindowIds::StackerParameters, columnHeader, StackerParametersColumns::DarkRedNumberPull, "DarkRedNumberPull", false, proto::DataType::bool_type);

        auto rowHeader = definition->mutable_rowheader();
        auto cells = definition->mutable_cells();

        for (uint32_t contractIndex = 0; contractIndex < _config->_stackerConfigs.size(); ++contractIndex) {
            StackerConfig& config = _config->_stackerConfigs.at(contractIndex);
            uint32_t internalId = _config->getInternalId(config._contract);
            addRowHeaderCell(WindowIds::StackerParameters, rowHeader,internalId, config._contract);

            setCell(cells, internalId, StackerParametersColumns::QuoteSize, std::to_string(config._quoteSize));
            setCell(cells, internalId, StackerParametersColumns::QuoteGap, std::to_string(config._quoteGap));
            setCell(cells, internalId, StackerParametersColumns::MaxQuote, std::to_string(config._maxQuote));
            setCell(cells, internalId, StackerParametersColumns::StackLevels, std::to_string(config._numberOfLevels));
            setCell(cells, internalId, StackerParametersColumns::StackSlack, std::to_string(config._stackSlack));
            setCell(cells, internalId, StackerParametersColumns::MaxTicksOffTopLevel, std::to_string(config._maxTicksOffTopLevel));
            setCell(cells, internalId, StackerParametersColumns::UseGTC, std::to_string(config._useGtc));
            setCell(cells, internalId, StackerParametersColumns::AllowMultipleNewHedge, std::to_string(config._allowMultipleNewHedge));
            setCell(cells, internalId, StackerParametersColumns::EnableQuoteSprayer, std::to_string(config._enableQuoteSprayer));
            setCell(cells, internalId, StackerParametersColumns::RedNumberPull, std::to_string(config._reactToRedNumber));
            setCell(cells, internalId, StackerParametersColumns::DarkRedNumberPull, std::to_string(config._reactToDarkRedNumber));
        }

        auto buttons = definition->mutable_buttons();
        addButton(buttons, toUInt(0), "Submit", proto::ButtonFrontBehavior_CommitValues, proto::ButtonBackBehavior_Send, RequiresConfirmation::NO);
    }

    void populateGatewayStatuses(proto::GridDefinition* definition) {
        definition->set_gridid(toUInt(WindowIds::GatewayStatuses));
        definition->set_name("Gateway Status");
        definition->set_type(proto::GridType::Table);

        auto columnHeader = definition->mutable_columnheader();

        addColumnHeaderCell(WindowIds::GatewayStatuses, columnHeader, GatewayStatusColumns::Exchange, "Exchange", true, proto::DataType::string_type);
        addColumnHeaderCell(WindowIds::GatewayStatuses, columnHeader, GatewayStatusColumns::Status, "Status", true, proto::DataType::bool_type);

        auto rowHeader = definition->mutable_rowheader();

        addRowHeaderCell(WindowIds::GatewayStatuses, rowHeader, 1, "");

        auto cells = definition->mutable_cells();

        setCell(cells, 1, GatewayStatusColumns::Exchange, "ICE");
        setCell(cells, 1, GatewayStatusColumns::Status, "1");
    }

    void populateGatewayStatusUpdate(proto::GridUpdate& message, bool active) {
        message.set_gridid(toUInt(WindowIds::GatewayStatuses));
        message.set_updatetype(proto::GridUpdateType::CellsType);
        auto cells = message.mutable_cells();
        uint32_t value = active ? 1 : 0;
        setCell(cells, 1, GatewayStatusColumns::Status, std::to_string(value));
    }

    void populateErrors(proto::GridDefinition* definition) {
        definition->set_gridid(toUInt(WindowIds::Errors));
        definition->set_name("Messages");
        definition->set_type(proto::GridType::List);

        auto columnHeader = definition->mutable_columnheader();

        addColumnHeaderCell(WindowIds::Errors, columnHeader, ErrorsColumns::Time, "Time", true, proto::DataType::rhone_time);
        addColumnHeaderCell(WindowIds::Errors, columnHeader, ErrorsColumns::Message, "Message", true, proto::DataType::string_type);
        addColumnHeaderCell(WindowIds::Errors, columnHeader, ErrorsColumns::Type, "Type", true, proto::DataType::enum_type, "errorTypes");
    }

    void populateFills(proto::GridDefinition* definition) {
        definition->set_gridid(toUInt(WindowIds::FillsDisplay));
        definition->set_name("Fills");
        definition->set_type(proto::GridType::List);

        auto columnHeader = definition->mutable_columnheader();

        addColumnHeaderCell(WindowIds::FillsDisplay, columnHeader, FillsColumns::Number, "", true, proto::DataType::string_type);
        addColumnHeaderCell(WindowIds::FillsDisplay, columnHeader, FillsColumns::Time, "Time", true, proto::DataType::rhone_time);
        addColumnHeaderCell(WindowIds::FillsDisplay, columnHeader, FillsColumns::Contract, "Contract", true, proto::DataType::enum_type, "contracts");
        addColumnHeaderCell(WindowIds::FillsDisplay, columnHeader, FillsColumns::Side, "Side", true, proto::DataType::enum_type, "fillSides");
        addColumnHeaderCell(WindowIds::FillsDisplay, columnHeader, FillsColumns::Price, "Price", true, proto::DataType::rhone_price);
        addColumnHeaderCell(WindowIds::FillsDisplay, columnHeader, FillsColumns::Size, "Size", true, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::FillsDisplay, columnHeader, FillsColumns::Strategy, "Strategy", true, proto::DataType::enum_type, "strategies");
        addColumnHeaderCell(WindowIds::FillsDisplay, columnHeader, FillsColumns::Type, "Type", true, proto::DataType::enum_type, "fillTypes");
        addColumnHeaderCell(WindowIds::FillsDisplay, columnHeader, FillsColumns::Source, "Source", true, proto::DataType::enum_type, "fillSource");
        addColumnHeaderCell(WindowIds::FillsDisplay, columnHeader, FillsColumns::EdgeStf, "%EdgeSTF", true, proto::DataType::double_type);
        addColumnHeaderCell(WindowIds::FillsDisplay, columnHeader, FillsColumns::EdgeMkt, "%EdgeMkt", true, proto::DataType::double_type);
    }

    template<typename ExecutorT>
    void populateMainControl(proto::GridDefinition* definition, ExecutorT* executor) {
        definition->set_gridid(toUInt(WindowIds::MainControl));
        definition->set_name("Main Control");
        definition->set_type(proto::GridType::MainControl);

        auto columnHeader = definition->mutable_columnheader();

        addColumnHeaderCell(WindowIds::MainControl, columnHeader, MainControlAction::Enabled, "Enabled", true, proto::DataType::enum_type, "switches");

        auto rowHeader = definition->mutable_rowheader();

        auto cells = definition->mutable_cells();
        uint32_t headerOffset = 0;

        // Strategy Enable
        for (; headerOffset < toUInt(StrategyId::MAX); ++headerOffset) {
            addRowHeaderCell(WindowIds::MainControl, rowHeader, headerOffset, strategyFullName((StrategyId)headerOffset));
            auto strategy = executor->getStrategy(static_cast<StrategyId>(headerOffset));
            setCell(cells, headerOffset, MainControlAction::Enabled, std::to_string(strategy->isEnabled() ? proto::SystemState::SystemStateEnabled : proto::SystemState::SystemStateDisabled),
                    toUInt(strategy->isEnabled() ? Color::TRANSPARENT_SWITCH_ENABLED : Color::TRANSPARENT_SWITCH_DISABLED));
        }

        // Strategy Quote External
        for (; headerOffset < toUInt(StrategyId::MAX) * 2; ++headerOffset) {
            auto strategy = executor->getStrategy(static_cast<StrategyId>(headerOffset - toUInt(StrategyId::MAX)));
            addRowHeaderCell(WindowIds::MainControl, rowHeader, headerOffset,
                             boost::str(boost::format("%1% QuoteExternal") % toString(strategy->strategyId())));
            setCell(cells, headerOffset, MainControlAction::Enabled, std::to_string(strategy->canQuoteExternally() ? proto::SystemState::SystemStateEnabled : proto::SystemState::SystemStateDisabled),
                    toUInt(strategy->canQuoteExternally() ? Color::TRANSPARENT_SWITCH_ENABLED : Color::TRANSPARENT_SWITCH_DISABLED));
        }

        // Exchange ExtQuote Control
        auto extQuote = executor->s_systemState._exchQuoteExternally.load(std::memory_order_relaxed);
        for (auto iter = executor->_usedVenues.begin(); iter != executor->_usedVenues.end(); ++iter) {
            addRowHeaderCell(WindowIds::MainControl, rowHeader, headerOffset, boost::str(boost::format("%1% QuoteExternal") % ven::VenueUtils::getName(*iter)));
            setCell(cells, headerOffset, MainControlAction::Enabled, std::to_string(extQuote.test(toUInt(*iter)) ? proto::SystemState::SystemStateEnabled : proto::SystemState::SystemStateDisabled),
                    toUInt(extQuote.test(toUInt(*iter)) ? Color::TRANSPARENT_SWITCH_ENABLED : Color::TRANSPARENT_SWITCH_DISABLED));
            _mapRowIndexVenue.emplace(headerOffset, static_cast<uint8_t>(*iter));
            _mapVenueRowIndex.emplace(static_cast<uint8_t>(*iter), headerOffset++);
        }

        auto buttons = definition->mutable_buttons();
        addButton(buttons, toUInt(MainControlButtons::HardShutdown), "Shutdown", proto::ButtonFrontBehavior_Nothing, proto::ButtonBackBehavior_Nothing, RequiresConfirmation::YES);
        addButton(buttons, toUInt(MainControlButtons::CheckQuotes), "Check Quotes", proto::ButtonFrontBehavior_Nothing, proto::ButtonBackBehavior_Nothing, RequiresConfirmation::NO);
        addButton(buttons, toUInt(MainControlButtons::CancelGTC), "Cancel All Orders", proto::ButtonFrontBehavior_Nothing, proto::ButtonBackBehavior_Nothing, RequiresConfirmation::YES);
    }

    template <typename ButtonsT>
    void addButton(ButtonsT buttons, uint32_t id, const std::string& label, proto::ButtonFrontBehavior front, proto::ButtonBackBehavior back, RequiresConfirmation requiresConfirmationValue) {
        proto::Button* button = buttons->Add();
        button->set_id(id);
        button->set_name(label);
        button->set_back(back);
        button->set_front(front);
        button->set_confirmation(static_cast<bool>(requiresConfirmationValue));
    }

    void addSubmitButton(proto::GridDefinition* definition) {
        auto buttons = definition->mutable_buttons();
        addButton(buttons, toUInt(0), "Submit", proto::ButtonFrontBehavior_CommitValues, proto::ButtonBackBehavior_Send, RequiresConfirmation::NO);
    }

    void addHeader(proto::GridDefinition* definition, std::vector<HeaderCellT>&& cells) {
        auto columnHeader = definition->mutable_columnheader();
        for (const auto& [id, label, readOnly, type, e, target, delta] : cells) {
            addColumnHeaderCell((WindowIds)definition->gridid(), columnHeader, id, label, readOnly, type, e, target, delta);
        }
    }

    void addHeaderCell(proto::GridDefinition* definition, HeaderCellT&& cell) {
        auto columnHeader = definition->mutable_columnheader();
        auto& [id, label, readOnly, type, e, target, delta] = cell;
        addColumnHeaderCell((WindowIds)definition->gridid(), columnHeader, id, label, readOnly, type, e, target, delta);
    }
    
    template <typename HeaderT, typename EnumValue>
    proto::ColumnHeaderCell* addColumnHeaderCell(WindowIds gridId, HeaderT* header, EnumValue id, const std::string& label, bool readyOnly, proto::DataType type, const std::string& e = "", uint32_t targetColumn = 0, uint32_t deltaColumn = 0) {
        proto::ColumnHeaderCell* headerCell = header->Add();
        headerCell->set_id(toUInt(id));
        headerCell->set_label(label);
        headerCell->set_type(type);
        if (e != "") {
            headerCell->set_enumerationname(e);
        }
        headerCell->set_readonly(readyOnly);
        auto& columns = _columns[toUInt(gridId)];
        columns[toUInt(id)] = label;
        if (type == proto::DataType::value_increment_button || type == proto::DataType::value_decrement_button) {
            headerCell->set_target_col(targetColumn);
            headerCell->set_delta_col(deltaColumn);
        }
        return headerCell;
    }

    std::string getRowLabel(uint32_t gridId, uint32_t rowId) {
        return _rows[gridId][rowId];
    }

    std::string getColumnLabel(uint32_t gridId, uint32_t id) {
        return _columns[gridId][id];
    }

    template<typename CellsT>
    void addCells(CellsT cells, uint32_t row, std::vector<RowCellT>&& values) {
        for (const auto& [col, value, colorId] : values) {
            setCell(cells, row, col, value, colorId);
        }
    }

    template <typename HeaderT, typename EnumValue>
    void addRowHeaderCell(WindowIds gridId, HeaderT* header, EnumValue id, const std::string& label) {
        auto headerCell = header->Add();
        headerCell->set_id(toUInt(id));
        headerCell->set_label(label);
        auto& rows = _rows[toUInt(gridId)];
        rows[toUInt(id)] = label;
    }

    std::string getOldValue(GridUpdate& update, uint32_t row, uint32_t column) {
        for (auto& value : update._oldValues) {
            if (value.row() == row && value.col() == column) {
                return value.value();
            }
        }
        return "";
    }

    std::string getNewValue(uint32_t gridId, uint32_t column, const std::string& value) {
        if (gridId == toUInt(WindowIds::TraderSwitch)) {
            if (column == toUInt(TraderSwitchColumns::Trader)) {
                uint32_t trader = atoi(value.c_str());
                assert(trader >= 0 && trader < _config->_traders.size());
                return _config->_traders[trader];
            } else if (column == toUInt(TraderSwitchColumns::Shift)) {
                return to_string(static_cast<Shifts>(atoi(value.c_str())));
            }
        }
        return value;
    }

    void populateManualFillsEntry(proto::GridDefinition* definition) {
        definition->set_gridid(toUInt(WindowIds::ManualFillsEntry));
        definition->set_name("Manual Fills Entry");
        definition->set_type(proto::GridType::EntryTable);

        auto columnHeader = definition->mutable_columnheader();

        addColumnHeaderCell(WindowIds::ManualFillsEntry, columnHeader, ManualFillsEntryColumns::Contract, "Contract", false, proto::DataType::enum_type, "contracts");
        addColumnHeaderCell(WindowIds::ManualFillsEntry, columnHeader, ManualFillsEntryColumns::Size, "Size", false, proto::DataType::signed_type);
        addColumnHeaderCell(WindowIds::ManualFillsEntry, columnHeader, ManualFillsEntryColumns::Type, "Type", false, proto::DataType::enum_type, "manual_fill_types");

        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t index = 0; index < 5; ++index) {
            std::string name;
            name.append(std::to_string(index)).append(". ");
            addRowHeaderCell(WindowIds::ManualFillsEntry, rowHeader, index, name);
        }

        auto cells = definition->mutable_cells();

        for (uint32_t index = 0; index < 5; ++index) {
            setCell(cells, index, ManualFillsEntryColumns::Contract, NO_VALUE_ID);
            setCell(cells, index, ManualFillsEntryColumns::Size, "0");
            setCell(cells, index, ManualFillsEntryColumns::Type, NO_VALUE_ID);
        }

        auto buttons = definition->mutable_buttons();
        addButton(buttons, toUInt(0), "Submit", proto::ButtonFrontBehavior_Clear, proto::ButtonBackBehavior_Send, RequiresConfirmation::NO);
        addButton(buttons, toUInt(1), "Clear", proto::ButtonFrontBehavior_Clear, proto::ButtonBackBehavior_Nothing, RequiresConfirmation::NO);
    }

    void populatePricingConfig(proto::GridDefinition* definition) {
        definition->set_gridid(toUInt(WindowIds::PricingConfig));
        definition->set_name("Pricing Parameters");
        definition->set_type(proto::GridType::Table);

        auto columnHeader = definition->mutable_columnheader();

        addColumnHeaderCell(WindowIds::PricingConfig, columnHeader, PricingConfigsColumns::Weight, "Weight", false, proto::DataType::double_type);
        addColumnHeaderCell(WindowIds::PricingConfig, columnHeader, PricingConfigsColumns::UseWeighted, "UseWeighted", false, proto::DataType::bool_type);
        addColumnHeaderCell(WindowIds::PricingConfig, columnHeader, PricingConfigsColumns::EstablishedLevel, "EstablishedLevel", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::PricingConfig, columnHeader, PricingConfigsColumns::PricingMultiplier, "PricingMultiplier", true, proto::DataType::double_type);
        auto qtyMultiplierHeader = addColumnHeaderCell(WindowIds::PricingConfig, columnHeader, PricingConfigsColumns::QuantityMultiplier, "QuantityMultiplier", true, proto::DataType::double_type);
        addColumnHeaderCell(WindowIds::PricingConfig, columnHeader, PricingConfigsColumns::SweepTicks, "SweepTicks", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::PricingConfig, columnHeader, PricingConfigsColumns::SweepQty, "SweepQty", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::PricingConfig, columnHeader, PricingConfigsColumns::MaxTicksWide, "MaxTicksWide", false, proto::DataType::signed_type);
        addColumnHeaderCell(WindowIds::PricingConfig, columnHeader, PricingConfigsColumns::ASExtCommission, "ASExtCommission", false, proto::DataType::double_type);

        auto rowHeader = definition->mutable_rowheader();
        auto cells = definition->mutable_cells();

        for (uint32_t contractIndex = 0; contractIndex < _config->_pricingConfigs.size(); ++contractIndex) {
            PricingConfig& config = _config->_pricingConfigs.at(contractIndex);
            uint32_t internalId = _config->getInternalId(config._contract);
            addRowHeaderCell(WindowIds::PricingConfig, rowHeader,internalId, config._contract);

            setCell(cells, contractIndex, PricingConfigsColumns::Weight, std::to_string(config._weight));
            setCell(cells, contractIndex, PricingConfigsColumns::UseWeighted, std::to_string(config._useWeightedMid));
            setCell(cells, contractIndex, PricingConfigsColumns::EstablishedLevel, std::to_string(config._establishedLevel));
            setCell(cells, contractIndex, PricingConfigsColumns::PricingMultiplier, std::to_string(config._pricingMultiplier));

            auto contract = _config->getContract(config._contract);
            if (contract &&
#ifndef IS_DEV
                contract->_venue == hf::VENUE::B3 &&
#endif
                qtyMultiplierHeader->readonly()) {
                qtyMultiplierHeader->set_readonly(false);
            }
            setCell(cells, contractIndex, PricingConfigsColumns::QuantityMultiplier, std::to_string(config._quantityMultiplier));
            setCell(cells, contractIndex, PricingConfigsColumns::SweepTicks, std::to_string(config._sweepTicks));
            setCell(cells, contractIndex, PricingConfigsColumns::SweepQty, std::to_string(config._sweepQuantity));
            setCell(cells, contractIndex, PricingConfigsColumns::MaxTicksWide, std::to_string(config._maxTicksWide));
            setCell(cells, contractIndex, PricingConfigsColumns::ASExtCommission, std::to_string(config._asExtCommission));
        }

        addSubmitButton(definition);
    }

    void populateLeanMatrix(proto::GridDefinition* definition) {
        definition->set_gridid(toUInt(WindowIds::LeanMatrix));
        definition->set_name("Lean Matrix");
        definition->set_type(proto::GridType::Matrix);

        auto columnHeader = definition->mutable_columnheader();
        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t index = 0; index < _config->_leanMatrix.size(); ++index) {
            VectorWithContract& config = _config->_leanMatrix[index];
            uint32_t internalId = _config->getInternalId(config._contract);
            addColumnHeaderCell(WindowIds::LeanMatrix, columnHeader, internalId, config._contract, false, proto::DataType::bool_type);
            addRowHeaderCell(WindowIds::LeanMatrix, rowHeader, internalId, config._contract);
        }

        auto cells = definition->mutable_cells();

        for (uint32_t row = 0; row < _config->_leanMatrix.size(); ++row) {
            VectorWithContract& rowConfig = _config->_leanMatrix[row];
            uint32_t rowInternalId = _config->getInternalId(rowConfig._contract);
            for (uint32_t col = 0; col < _config->_leanMatrix.size(); ++col) {
                VectorWithContract& colConfig = _config->_leanMatrix[col];
                uint32_t colInternalId = _config->getInternalId(colConfig._contract);
                int value = _config->_leanMatrix[row]._values[col];
                setCell(cells, rowInternalId, colInternalId, std::to_string(value));
            }
        }

        addSubmitButton(definition);
    }

    void populateLoggingLevel(proto::GridDefinition* definition) {
        definition->set_gridid(toUInt(WindowIds::LoggingLevel));
        definition->set_name("LoggingLevel");
        definition->set_type(proto::GridType::Table);

        auto columnHeader = definition->mutable_columnheader();
        addColumnHeaderCell(WindowIds::LoggingLevel, columnHeader, LoggingLevelColumns::LoggingLevel, "Level", false, proto::DataType::enum_type, "logging_level");

        auto rowHeader = definition->mutable_rowheader();
        addRowHeaderCell(WindowIds::LoggingLevel, rowHeader, 0, "LoggingLevel: ");

        auto cells = definition->mutable_cells();

#ifdef IS_DEV
        setCell(cells, 0, LoggingLevelColumns::LoggingLevel, std::to_string(toUInt(logging::LOG_LEVEL::TRACE)));
#else
        setCell(cells, 0, LoggingLevelColumns::LoggingLevel, std::to_string(toUInt(logging::LOG_LEVEL::INFO)));
#endif

        addSubmitButton(definition);
    }

    void addLoggingLevelEnum(proto::EnumDefinition* definition) {
        definition->set_name("logging_level");
        auto members = definition->mutable_members();
        addEnumMember(members, "Error", toUInt(logging::LOG_LEVEL::ERROR));
        addEnumMember(members, "Warning", toUInt(logging::LOG_LEVEL::WARNING));
        addEnumMember(members, "Info", toUInt(logging::LOG_LEVEL::INFO));
        addEnumMember(members, "Debug", toUInt(logging::LOG_LEVEL::DEBUG));
        addEnumMember(members, "Trace", toUInt(logging::LOG_LEVEL::TRACE));
    }

    void addEconomicNumberColorEnum(proto::EnumDefinition* definition) {
        definition->set_name("economic_number_color");
        auto members = definition->mutable_members();
        addEnumMember(members, "DarkRed", toUInt(EconomicNumber::DarkRed));
        addEnumMember(members, "Red", toUInt(EconomicNumber::Red));
        addEnumMember(members, "Orange", toUInt(EconomicNumber::Orange));
    }

    void populateNegativeScalpCheckerConfig(proto::GridDefinition* definition) {
        definition->set_gridid(toUInt(WindowIds::NegativeScalpChecker));
        definition->set_name("Negative Scalp Checker");
        definition->set_type(proto::GridType::Table);
        auto columnHeader = definition->mutable_columnheader();
        addColumnHeaderCell(WindowIds::NegativeScalpChecker, columnHeader, NegativeScalpCheckerColumns::Period, "periodMillis", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::NegativeScalpChecker, columnHeader, NegativeScalpCheckerColumns::ClipCount, "clips", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::NegativeScalpChecker, columnHeader, NegativeScalpCheckerColumns::Pnl, "dollars", false, proto::DataType::unsigned_type);

        auto rowHeader = definition->mutable_rowheader();
        auto cells = definition->mutable_cells();

        for (uint32_t contractIndex = 0; contractIndex < _config->_negativeScalpCheckerConfigs.size(); ++contractIndex) {
            auto& config = _config->_negativeScalpCheckerConfigs.at(contractIndex);
            addRowHeaderCell(WindowIds::NegativeScalpChecker, rowHeader, contractIndex, config._contract);

            setCell(cells, contractIndex, NegativeScalpCheckerColumns::Period, std::to_string(config._period));
            setCell(cells, contractIndex, NegativeScalpCheckerColumns::ClipCount, std::to_string(config._countLimit));
            setCell(cells, contractIndex, NegativeScalpCheckerColumns::Pnl, std::to_string(config._pnlLimit));
        }

        addSubmitButton(definition);
    }

    template<typename ExecutorT>
    void populateEconomicNumbers(proto::GridDefinition* definition, ExecutorT* executor) {
        definition->set_gridid(toUInt(WindowIds::EconomicNumbers));
        definition->set_name("Economic Numbers");
        definition->set_type(proto::GridType::Table);
        auto columnHeader = definition->mutable_columnheader();
        addColumnHeaderCell(WindowIds::EconomicNumbers, columnHeader, EconomicNumberColumns::Time, "Time (HH::MM::SS)", false, proto::DataType::string_type);
        addColumnHeaderCell(WindowIds::EconomicNumbers, columnHeader, EconomicNumberColumns::MMDuration, "MM duration (sec)", false, proto::DataType::string_type);
        addColumnHeaderCell(WindowIds::EconomicNumbers, columnHeader, EconomicNumberColumns::RVDuration, "RV duration (sec)", false, proto::DataType::string_type);
        addColumnHeaderCell(WindowIds::EconomicNumbers, columnHeader, EconomicNumberColumns::ASDuration, "AS duration (sec)", false, proto::DataType::string_type);
        addColumnHeaderCell(WindowIds::EconomicNumbers, columnHeader, EconomicNumberColumns::Color, "color", false, proto::DataType::enum_type, "economic_number_color");
        addColumnHeaderCell(WindowIds::EconomicNumbers, columnHeader, EconomicNumberColumns::Description, "description", false, proto::DataType::string_type);

        auto rowHeader = definition->mutable_rowheader();
        auto cells = definition->mutable_cells();

        for (auto* en : executor->_economicNumbers) {
            addRowHeaderCell(WindowIds::EconomicNumbers, rowHeader, en->_id, std::to_string(en->_id));

            setCell(cells, en->_id, EconomicNumberColumns::Time, en->_datetimeString.substr(11));
            setCell(cells, en->_id, EconomicNumberColumns::MMDuration, std::to_string(en->_mmDuration));
            setCell(cells, en->_id, EconomicNumberColumns::RVDuration, std::to_string(en->_rvDuration));
            setCell(cells, en->_id, EconomicNumberColumns::ASDuration, std::to_string(en->_asDuration));
            setCell(cells, en->_id, EconomicNumberColumns::Color, std::to_string(toUInt(en->_color)));
            setCell(cells, en->_id, EconomicNumberColumns::Description, en->_description);
        }

        // add extra 5 empty line
        for (uint32_t i = _config->_mapIdEconomicNumber.size(), len= i + 5; i < len; i++) {
            addRowHeaderCell(WindowIds::EconomicNumbers, rowHeader, i, std::to_string(i));

            setCell(cells, i, EconomicNumberColumns::Time, "");
            setCell(cells, i, EconomicNumberColumns::MMDuration, "");
            setCell(cells, i, EconomicNumberColumns::RVDuration, "");
            setCell(cells, i, EconomicNumberColumns::ASDuration, "");
            setCell(cells, i, EconomicNumberColumns::Color, "");
            setCell(cells, i, EconomicNumberColumns::Description, "");
        }

        auto buttons = definition->mutable_buttons();
        addButton(buttons, toUInt(0), "Submit", proto::ButtonFrontBehavior_CommitValues,
                  proto::ButtonBackBehavior_Send, RequiresConfirmation::NO);
    }

    //GTCGrid Methods
    COLD void tryPopulateGTCGridColumns(Python::proto::GUIInitialization& message);
    bool tryPopulateIOPUpdate(const IOPUpdate& iopUpdate, proto::GridUpdate& gridUpdate);
    bool tryPopulateResetGTCGrid(proto::GridUpdate& gridUpdate);
    bool tryPopulateAuctionMDUpdate(const AuctionMDUpdate& update, proto::GridUpdate& gridUpdate);
    bool tryPopulateNumGTCUpdate(const GenericMessage& update, proto::GridUpdate& gridUpdate);
    template<typename GridCellContainerType>
    bool tryPopulateResetGridCells(GridCellContainerType& cells);

    //Status debug grid
    COLD void tryPopulateStatusDebugGridColumns(Python::proto::GUIInitialization& message);
    COLD void addStatusDebugEnum(proto::EnumDefinition* definition);
    COLD void addAllContractsEnum(proto::EnumDefinition* definition);

    template <typename CellsT, typename ColT>
    void setCell(CellsT cells, uint32_t row, ColT col, const std::string& value, uint32_t colorId = UINT32_MAX) const {
        auto cell = cells->Add();
        cell->set_row(row);
        cell->set_col(toUInt(col));
        cell->set_value(value);
        if(colorId != UINT32_MAX) {
            cell->set_colorid(colorId);
        }
    }

    uint8_t getVenueFromMainControlRowIndex(uint8_t rowIndex) const {
        auto iter = _mapRowIndexVenue.find(rowIndex);
        if (iter != _mapRowIndexVenue.end()) {
            return iter->second;
        }
        return static_cast<uint8_t>(hf::VENUE::UNKNOWN);
    }

    uint8_t venueToMainControlRowIndex(uint8_t venue) const {
        auto iter = _mapVenueRowIndex.find(venue);
        if (iter != _mapVenueRowIndex.end()) {
            return iter->second;
        }
        BOOST_ASSERT(false);
        return std::numeric_limits<uint8_t>::max();
    }

    const std::unordered_set<WindowIds>& subscriptionWindows() const noexcept {
        return _subscriptionWindows;
    }

private:
    GUIThings() = default;
    ~GUIThings() = default;
    bool allowGTCs() { return _config->_allowGTCs; }
    bool allowStatusGrid() { return _config->_allowStatusGrid; }

    ViperConfig* _config;
    uint32_t _fillNumber = 1;
    uint32_t _errorMessageNumber = 0;
    uint32_t _auditMessageNumber = 0;
    std::unordered_map<uint32_t, std::unordered_map<uint32_t, std::string>> _columns;
    std::unordered_map<uint32_t, std::unordered_map<uint32_t, std::string>> _rows;
    std::unordered_map<uint8_t, uint8_t> _mapRowIndexVenue;
    std::unordered_map<uint8_t, uint8_t> _mapVenueRowIndex;
    std::unordered_set<WindowIds> _subscriptionWindows;
};

};

#endif //VIPER_VIPER_CONFIG_H
