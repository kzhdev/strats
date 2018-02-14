#ifndef VIPER_GUI_THINGS_H
#define VIPER_GUI_THINGS_H

#include <hf/util/TimeUtils.h>
#include "messages/omsproto.pb.h"
#include "ViperConfig.h"
#include "RelativeValue.h"
#include "TimeUtils.h"
#include "NegativeScalpChecker.h"

namespace Python {

class GUIThings {

public:
    static constexpr const char* NO_VALUE_ID = "-1";

    void init(ViperConfig* config, std::unordered_map<std::string, uint32_t>* symbolToInterId) {
        _config = config;
        _symbolToInterId = symbolToInterId;
    }

    template <typename T>
    void populateMessageCountsUpdate(proto::GridUpdate& message, T* executor) {
        message.set_gridid(toUInt(WindowIds::MessagesCounts));
        auto cells = message.mutable_cells();
        for (uint32_t contractIndex = 0; contractIndex < executor->_contractStates.size(); ++contractIndex) {
            auto& state = executor->_contractStates[contractIndex];
            if (state._marketMaker._toMarketMake) {
                setCell(cells, contractIndex, MessageCountsColumns::Inserts, std::to_string(state._contractExecutor._messages._inserts));
                setCell(cells, contractIndex, MessageCountsColumns::Modifies, std::to_string(state._contractExecutor._messages._modifies));
                setCell(cells, contractIndex, MessageCountsColumns::Cancels, std::to_string(state._contractExecutor._messages._cancels));
            }
        }
    }

    void populatePnlUpdate(proto::GridUpdate& message, AggregatedPnl& pnl, StrategyId id) {
        message.set_gridid(toUInt(WindowIds::PNL));
        auto cells = message.mutable_cells();
        switch (id) {
            case StrategyId::RelativeValue: {
                setCell(cells, toUInt(PNLRows::RelativeValue), PNLColumns::Open, std::to_string(pnl._relativeValue._open));
                setCell(cells, toUInt(PNLRows::RelativeValue), PNLColumns::Closed, std::to_string(pnl._relativeValue._closed));
                setCell(cells, toUInt(PNLRows::RelativeValue), PNLColumns::Drawdown, std::to_string(pnl._relativeValue._drawdown));
                setCell(cells, toUInt(PNLRows::RelativeValue), PNLColumns::Total, std::to_string(pnl._relativeValue.getTotal()));
                break;
            }
            case StrategyId::MarketMaker: {
                setCell(cells, toUInt(PNLRows::MarketMakers), PNLColumns::Open, std::to_string(pnl._marketMakers._open));
                setCell(cells, toUInt(PNLRows::MarketMakers), PNLColumns::Closed, std::to_string(pnl._marketMakers._closed));
                setCell(cells, toUInt(PNLRows::MarketMakers), PNLColumns::Drawdown, std::to_string(pnl._marketMakers._drawdown));
                setCell(cells, toUInt(PNLRows::MarketMakers), PNLColumns::Total, std::to_string(pnl._marketMakers.getTotal()));
                break;
            }
            case StrategyId::Execution: {
                setCell(cells, toUInt(PNLRows::Execution), PNLColumns::Open, std::to_string(pnl._execution._open));
                setCell(cells, toUInt(PNLRows::Execution), PNLColumns::Closed, std::to_string(pnl._execution._closed));
                setCell(cells, toUInt(PNLRows::Execution), PNLColumns::Drawdown, std::to_string(pnl._execution._drawdown));
                setCell(cells, toUInt(PNLRows::Execution), PNLColumns::Total, std::to_string(pnl._execution.getTotal()));
                break;
            }
        }
        setCell(cells, toUInt(PNLRows::Total), PNLColumns::Open, std::to_string(pnl._total._open));
        setCell(cells, toUInt(PNLRows::Total), PNLColumns::Closed, std::to_string(pnl._total._closed));
        setCell(cells, toUInt(PNLRows::Total), PNLColumns::Drawdown, std::to_string(pnl._total._drawdown));
        setCell(cells, toUInt(PNLRows::Total), PNLColumns::Total, std::to_string(pnl._total.getTotal()));
    }

    void populatePnlDetailUpdate(proto::GridUpdate& message, Pnl& pnl, StrategyId id, uint32_t contractIndex) {
        message.set_gridid(toUInt(WindowIds::PnlDetail));
        auto cells = message.mutable_cells();
        switch (id) {
            case StrategyId::RelativeValue: {
                setCell(cells, contractIndex, PnlDetailColumns::RVOpen, std::to_string(pnl._open));
                setCell(cells, contractIndex, PnlDetailColumns::RVClosed, std::to_string(pnl._closed));
                break;
            }
            case StrategyId::MarketMaker: {
                setCell(cells, contractIndex, PnlDetailColumns::MMOpen, std::to_string(pnl._open));
                setCell(cells, contractIndex, PnlDetailColumns::MMClosed, std::to_string(pnl._closed));
                break;
            }
            case StrategyId::Execution: {
                setCell(cells, contractIndex, PnlDetailColumns::ExecOpen, std::to_string(pnl._open));
                setCell(cells, contractIndex, PnlDetailColumns::ExecClosed, std::to_string(pnl._closed));
                break;
            }
        }
    }

    void populateErrorMessage(proto::GridUpdate& message, ErrorMessage& m) {
        message.set_gridid(toUInt(WindowIds::Errors));
        auto cells = message.mutable_cells();
        setCell(cells, _errorMessageNumber, ErrorsColumns::Time, TimeUtils::asctime(TimeUtils::clock_gettime()));
        setCell(cells, _errorMessageNumber, ErrorsColumns::Message, std::string(m._message, m._size));
        setCell(cells, _errorMessageNumber, ErrorsColumns::Type, std::to_string(toUInt(m._type)));
        ++_errorMessageNumber;
    }

    void populateFillMessage(FillMessage& fill, proto::GridUpdate& message) {
        message.set_gridid(toUInt(WindowIds::FillsDisplay));
        auto cells = message.mutable_cells();
        auto& contract = _config->_allContracts[fill._contractId];
        setCell(cells, _fillNumber, FillsColumns::Time, TimeUtils::asctime(TimeUtils::clock_gettime()));
        setCell(cells, _fillNumber, FillsColumns::Contract, std::to_string(contract._internalId));
        setCell(cells, _fillNumber, FillsColumns::Side, std::to_string(fill._side));
        setCell(cells, _fillNumber, FillsColumns::Price, std::to_string(fill._price));
        setCell(cells, _fillNumber, FillsColumns::Size, std::to_string(fill._qty));
        setCell(cells, _fillNumber, FillsColumns::Strategy, std::to_string(toUInt(fill._strategy)));
        setCell(cells, _fillNumber, FillsColumns::Type, std::to_string(toUInt(fill._type)));
        setCell(cells, _fillNumber, FillsColumns::Source, std::to_string(toUInt(fill._source)));
        ++_fillNumber;
    }

    void populateLTFmessage(proto::GridUpdate& message, RelativeValue* relativeValue) {
        message.set_gridid(toUInt(WindowIds::VectorParameters));
        auto cells = message.mutable_cells();
        for (uint32_t vectorIndex = 0; vectorIndex < relativeValue->_numberOfVectors; ++vectorIndex) {
            const double ltf = relativeValue->_longTermFairsInVector.getValue(vectorIndex);
            setCell(cells, vectorIndex, VectorParametersColumns::LTF, std::to_string(ltf));
        }
    }

    void populateLTFChartDataPoints(proto::DataSeriesPointSet& message, RelativeValue* relativeValue) {
        auto points = message.mutable_points();
        for (uint32_t vectorIndex = 0; vectorIndex < relativeValue->_numberOfVectors; ++vectorIndex) {
            const double ltf = relativeValue->_longTermFairsInVector.getValue(vectorIndex);
            auto point = points->Add();
            point->set_chartid(vectorIndex);
            point->set_dataseriesid(toUInt(DataSeries::LTF));
            point->set_value(ltf);
            point->set_time(hf::util::TimeUtils::clock_gettime());
        }
    }

    void populateVectorPrices(proto::GridUpdate& message, RelativeValue* relativeValue) {
        message.set_gridid(toUInt(WindowIds::VectorParameters));
        auto cells = message.mutable_cells();
        for (uint32_t vectorIndex = 0; vectorIndex < relativeValue->_numberOfVectors; ++vectorIndex) {
            const double mkt = relativeValue->_newVectorMarketPrices.getValue(vectorIndex);
            setCell(cells, vectorIndex, VectorParametersColumns::Mkt, std::to_string(mkt));
        }
    }

    void populateVectorPricesForChart(proto::DataSeriesPointSet& message, RelativeValue* relativeValue) {
        auto points = message.mutable_points();
        for (uint32_t vectorIndex = 0; vectorIndex < relativeValue->_numberOfVectors; ++vectorIndex) {
            const double mkt = relativeValue->_newVectorMarketPrices.getValue(vectorIndex);
            auto point = points->Add();
            point->set_chartid(vectorIndex);
            point->set_dataseriesid(toUInt(DataSeries::VectorPrice));
            point->set_value(mkt);
            point->set_time(hf::util::TimeUtils::clock_gettime());
        }
    }

    void populateContractPrices(proto::GridUpdate& message, RelativeValue* relativeValue) {
        message.set_gridid(toUInt(WindowIds::ContractParametersRv));
        auto cells = message.mutable_cells();
        for (uint32_t contractIndex = 0; contractIndex < relativeValue->_numberOfAllContracts; ++contractIndex) {
            RvContract& contract = relativeValue->_contracts[contractIndex];
            if (contract._traded) {
                hf::price_t price;
                if (contract._definition._numberOfLegs > 1) {
                    double sum = 0;
                    for (uint32_t i = 0; i < contract._definition._numberOfLegs; ++i) {
                        auto& leg = contract._definition._legs[i];
                        sum += relativeValue->_overdeterminedPrices.getValue(0, leg._index);
                    }
                    price = ToRhonePrice(sum);
                } else {
                    price = ToRhonePrice(relativeValue->_overdeterminedPrices.getValue(0, contractIndex));
                }
                setCell(cells, contract._internalId, ContractParameterRVColumns::Mkt, std::to_string(price));
            }
        }
    }

    void populateContractQuotes(proto::GridUpdate& message, RelativeValue* relativeValue, bool checkForCross) {
        message.set_gridid(toUInt(WindowIds::ContractParametersRv));
        auto cells = message.mutable_cells();
        for (uint32_t contractIndex = 0; contractIndex < relativeValue->_numberOfAllContracts; ++contractIndex) {
            RvContract& contract = relativeValue->_contracts[contractIndex];
            if (contract._traded) {
                Quote& bid = contract._quotes[BID];
                setCell(cells, contract._internalId, ContractParameterRVColumns::BidPrice, std::to_string(bid._bestPrice));
                setCell(cells, contract._internalId, ContractParameterRVColumns::BidQty, std::to_string(bid._bestSize));
                RvContract& bidBestLean = relativeValue->_contracts[bid._bestLeanIndex];
                setCell(cells, contract._internalId, ContractParameterRVColumns::BidLean, std::to_string(bidBestLean._definition._internalId));
                YesNoType bidCrossed = YesNoType::Unknown;
                if (checkForCross && bid._externalPrice != hf::NULL_PRICE && contract._externalMarket[ASK]._price != hf::NULL_PRICE) {
                    bidCrossed = bid._externalPrice >= contract._externalMarket[ASK]._price ? YesNoType::Yes : YesNoType::No;
                }
                setCell(cells, contract._internalId, ContractParameterRVColumns::BidCross, std::to_string(toUInt(bidCrossed)));
                Quote& ask = contract._quotes[ASK];
                setCell(cells, contract._internalId, ContractParameterRVColumns::AskPrice, std::to_string(ask._bestPrice));
                setCell(cells, contract._internalId, ContractParameterRVColumns::AskQty, std::to_string(ask._bestSize));
                RvContract& askBestLean = relativeValue->_contracts[ask._bestLeanIndex];
                setCell(cells, contract._internalId, ContractParameterRVColumns::AskLean, std::to_string(askBestLean._definition._internalId));
                YesNoType askCrossed = YesNoType::Unknown;
                if (checkForCross && ask._externalPrice != hf::NULL_PRICE && contract._externalMarket[BID]._price != hf::NULL_PRICE) {
                    askCrossed = ask._externalPrice <= contract._externalMarket[BID]._price ? YesNoType::Yes : YesNoType::No;
                }
                setCell(cells, contract._internalId, ContractParameterRVColumns::AskCross, std::to_string(toUInt(askCrossed)));
            }
        }
    }

    void populateVectorStf(proto::GridUpdate& vectorMessage, RelativeValue* relativeValue) {
        vectorMessage.set_gridid(toUInt(WindowIds::VectorParameters));
        auto cells = vectorMessage.mutable_cells();
        for (uint32_t vectorIndex = 0; vectorIndex < relativeValue->_numberOfVectors; ++vectorIndex) {
            double stf = relativeValue->_shortTermFairsInVector.getValue(vectorIndex);
            setCell(cells, vectorIndex, VectorParametersColumns::STF, std::to_string(stf));
        }
    }

    void populateVectorStfForChart(proto::DataSeriesPointSet& message, RelativeValue* relativeValue) {
        auto points = message.mutable_points();
        for (uint32_t vectorIndex = 0; vectorIndex < relativeValue->_numberOfVectors; ++vectorIndex) {
            double stf = relativeValue->_shortTermFairsInVector.getValue(vectorIndex);
            auto point = points->Add();
            point->set_chartid(vectorIndex);
            point->set_dataseriesid(toUInt(DataSeries::STF));
            point->set_value(stf);
            point->set_time(hf::util::TimeUtils::clock_gettime());
        }
    }

    void populateContractStf(proto::GridUpdate& message, RelativeValue* relativeValue) {
        message.set_gridid(toUInt(WindowIds::ContractParametersRv));
        auto cells = message.mutable_cells();
        for (uint32_t contractIndex = 0; contractIndex < relativeValue->_numberOfAllContracts; ++contractIndex) {
            RvContract& contract = relativeValue->_contracts[contractIndex];
            if (contract._traded) {
                hf::price_t stf = relativeValue->_shortTermFairsForContracts.getValue(contractIndex);
                setCell(cells, contract._internalId, ContractParameterRVColumns::STF, std::to_string(stf));
            }
        }
    }

    void populateMarketMakerEnabled(proto::GridUpdate& message, uint32_t id, bool enabled) {
        message.set_gridid(toUInt(WindowIds::MarketMakerParameters));
        auto cells = message.mutable_cells();
        setCell(cells, id, MarketMakerParametersColumns::Enabled, std::to_string(enabled));
    }

    void populateContractPosition(uint32_t contractIndex, proto::GridUpdate& message, RelativeValue* relativeValue) {
        message.set_gridid(toUInt(WindowIds::ContractParametersRv));
        auto cells = message.mutable_cells();
        int64_t position = (int64_t) relativeValue->_contractPositions.getValue(contractIndex);
        setCell(cells, contractIndex, ContractParameterRVColumns::Position, std::to_string(position));
    }

    void populateVectorPosition(proto::GridUpdate& message, RelativeValue* relativeValue) {
        message.set_gridid(toUInt(WindowIds::VectorParameters));
        auto cells = message.mutable_cells();
        for (uint32_t vectorIndex = 0; vectorIndex < relativeValue->_numberOfVectors; ++vectorIndex) {
            double position = relativeValue->_positionsInVector.getValue(vectorIndex);
            setCell(cells, vectorIndex, VectorParametersColumns::Position, std::to_string(position));
        }

    }

    void populateGUIInitialization(proto::GUIInitialization& message, Matrix& pricingMatrix, Matrix& riskMatrix) {
        auto definitions = message.mutable_definitions();
        populateMainControl(definitions->Add());
        populateErrors(definitions->Add());
        populateMarketMakerParameters(definitions->Add());
        populateContractParameterRVColumns(definitions->Add());
        populateLeanMatrix(definitions->Add());
        populatePricingConfig(definitions->Add());
        populatePricingMatrix(definitions->Add(), pricingMatrix);
        populateRiskMatrix(definitions->Add(), riskMatrix);
        populateVectorParametersDefinition(definitions->Add());
        populateManualFillsEntry(definitions->Add());
        populateFills(definitions->Add());
        populateGatewayStatuses(definitions->Add());
        populateStackerParameters(definitions->Add());
        populatePNL(definitions->Add());
        populatePnlDetail(definitions->Add());
        populateGlobalConfigs(definitions->Add());
        populateMessageCounts(definitions->Add());

        auto enumDefinitions = message.mutable_enumdefinitions();
        addContractsEnum(enumDefinitions->Add());
        addSidesEnum(enumDefinitions->Add());
        addStrategiesEnum(enumDefinitions->Add());
        addFillTypeEnum(enumDefinitions->Add());
        addFillSourceEnum(enumDefinitions->Add());
        addSwitchesEnum(enumDefinitions->Add());
        addErrorTypesEnum(enumDefinitions->Add());
        addYesNoEnum(enumDefinitions->Add());

        addChartDefinitions(message);
    }

    void addChartDefinitions(proto::GUIInitialization& message) {
        auto chartDefinitions = message.mutable_charts();
        for (uint32_t index = 0; index < _config->_vectorConfigs.size(); ++index) {
            auto& config = _config->_vectorConfigs[index];
            addChart(config._vectorName, index, chartDefinitions->Add());
        }
    }

    void addChart(const std::string& name, uint32_t id, proto::Chart* chart) {
        chart->set_name(name);
        chart->set_id(id);
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
        addEnumMember(members, "Hard-disabled", toUInt(proto::SystemState::SystemStateHardShutdown));
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
        addEnumMember(members, "Allocated", toUInt(FillSource::Quote));
        addEnumMember(members, "ReverseHedge", toUInt(FillSource::ReverseHedge));
        addEnumMember(members, "Overfill", toUInt(FillSource::Overfill));
        addEnumMember(members, "Hedge", toUInt(FillSource::Hedge));
    }

    void addStrategiesEnum(proto::EnumDefinition* definition) {
        definition->set_name("strategies");
        auto members = definition->mutable_members();
        addEnumMember(members, "Relative Value", toUInt(StrategyId::RelativeValue));
        addEnumMember(members, "Market Maker", toUInt(StrategyId::MarketMaker));
    }

    void addSidesEnum(proto::EnumDefinition* definition) {
        definition->set_name("sides");
        auto members = definition->mutable_members();
        addEnumMember(members, "BID", 0);
        addEnumMember(members, "ASK", 1);
    }

    void addContractsEnum(proto::EnumDefinition* definition) {
        definition->set_name("contracts");
        auto members = definition->mutable_members();
        for (auto& mmc : _config->_marketMakerConfigs) {
            uint32_t internalId = getInternalId(mmc._contract);
            addEnumMember(members, mmc._contract, internalId);
        }
    }

    template <typename T>
    void addEnumMember(T* members, const std::string& display, uint32_t id) {
        auto member = members->Add();
        member->set_display(display);
        member->set_id(id);
    }

    void populateMessageCounts(proto::GridDefinition* definition) {
        definition->set_gridid(toUInt(WindowIds::MessagesCounts));
        definition->set_name("Message Counts");
        definition->set_type(proto::GridType::Table);

        auto columnHeader = definition->mutable_columnheader();

        addColumnHeaderCell(columnHeader, MessageCountsColumns::Inserts, "Insert", true, proto::DataType::unsigned_type);
        addColumnHeaderCell(columnHeader, MessageCountsColumns::Modifies, "Modify", true, proto::DataType::unsigned_type);
        addColumnHeaderCell(columnHeader, MessageCountsColumns::Cancels, "Cancel", true, proto::DataType::unsigned_type);

        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t contractIndex = 0; contractIndex < _config->_marketMakerConfigs.size(); ++contractIndex) {
            MarketMakerConfig& config = _config->_marketMakerConfigs.at(contractIndex);
            uint32_t internalId = getInternalId(config._contract);
            addRowHeaderCell(rowHeader,internalId, config._contract);
        }

        auto cells = definition->mutable_cells();

        for (uint32_t contractIndex = 0; contractIndex < _config->_marketMakerConfigs.size(); ++contractIndex) {
            MarketMakerConfig& config = _config->_marketMakerConfigs.at(contractIndex);
            uint32_t internalId = getInternalId(config._contract);
            setCell(cells, internalId, MessageCountsColumns::Inserts, "0");
            setCell(cells, internalId, MessageCountsColumns::Modifies, "0");
            setCell(cells, internalId, MessageCountsColumns::Cancels, "0");
        }
    }

    void populatePnlDetail(proto::GridDefinition* definition) {
        definition->set_gridid(toUInt(WindowIds::PnlDetail));
        definition->set_name("PNL Detail");
        definition->set_type(proto::GridType::Table);

        auto columnHeader = definition->mutable_columnheader();

        addColumnHeaderCell(columnHeader, PnlDetailColumns::MMOpen, "MM Open", true, proto::DataType::rhone_price);
        addColumnHeaderCell(columnHeader, PnlDetailColumns::MMClosed, "MM Closed", true, proto::DataType::rhone_price);
        addColumnHeaderCell(columnHeader, PnlDetailColumns::RVOpen, "RV Open", true, proto::DataType::rhone_price);
        addColumnHeaderCell(columnHeader, PnlDetailColumns::RVClosed, "RV Closed", true, proto::DataType::rhone_price);
        addColumnHeaderCell(columnHeader, PnlDetailColumns::ExecOpen, "Exec Open", true, proto::DataType::rhone_price);
        addColumnHeaderCell(columnHeader, PnlDetailColumns::ExecClosed, "Exec Closed", true, proto::DataType::rhone_price);

        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t contractIndex = 0; contractIndex < _config->_marketMakerConfigs.size(); ++contractIndex) {
            MarketMakerConfig& config = _config->_marketMakerConfigs.at(contractIndex);
            uint32_t internalId = getInternalId(config._contract);
            addRowHeaderCell(rowHeader,internalId, config._contract);
        }

        auto cells = definition->mutable_cells();

        for (uint32_t contractIndex = 0; contractIndex < _config->_marketMakerConfigs.size(); ++contractIndex) {
            MarketMakerConfig& config = _config->_marketMakerConfigs.at(contractIndex);
            uint32_t internalId = getInternalId(config._contract);
            setCell(cells, internalId, PnlDetailColumns::MMOpen, "0");
            setCell(cells, internalId, PnlDetailColumns::MMClosed, "0");
            setCell(cells, internalId, PnlDetailColumns::RVOpen, "0");
            setCell(cells, internalId, PnlDetailColumns::RVClosed, "0");
            setCell(cells, internalId, PnlDetailColumns::ExecOpen, "0");
            setCell(cells, internalId, PnlDetailColumns::ExecClosed, "0");
        }
    }

    void populateGlobalConfigs(proto::GridDefinition* definition) {
        definition->set_gridid(toUInt(WindowIds::GlobalConfigs));
        definition->set_name("Global Configs");
        definition->set_type(proto::GridType::Table);

        auto columnHeader = definition->mutable_columnheader();
        addColumnHeaderCell(columnHeader, GlobalConfigsColumns::Key, "Key", true, proto::DataType::string_type);
        addColumnHeaderCell(columnHeader, GlobalConfigsColumns::Value, "Value", true, proto::DataType::string_type);

        auto rowHeader = definition->mutable_rowheader();
        auto cells = definition->mutable_cells();

        for (uint32_t i = 0; i < _config->_globalConfigs.size(); ++i) {
            auto& config = _config->_globalConfigs[i];
            addRowHeaderCell(rowHeader, i, "");
            setCell(cells, i, GlobalConfigsColumns::Value, config._value);
            setCell(cells, i, GlobalConfigsColumns::Key, config._key);
        }
    }

    void populatePNL(proto::GridDefinition* definition) {
        definition->set_gridid(toUInt(WindowIds::PNL));
        definition->set_name("PNL");
        definition->set_type(proto::GridType::Table);

        auto columnHeader = definition->mutable_columnheader();

        addColumnHeaderCell(columnHeader, PNLColumns::Open, "Open", true, proto::DataType::rhone_price);
        addColumnHeaderCell(columnHeader, PNLColumns::Closed, "Closed", true, proto::DataType::rhone_price);
        addColumnHeaderCell(columnHeader, PNLColumns::Total, "Total", true, proto::DataType::rhone_price);
        addColumnHeaderCell(columnHeader, PNLColumns::Drawdown, "Drawdown", false, proto::DataType::rhone_price);
        addColumnHeaderCell(columnHeader, PNLColumns::PnlLimit, "PnlLimit", false, proto::DataType::rhone_price);
        addColumnHeaderCell(columnHeader, PNLColumns::DrawdownLimit, "DrawdownLimit", false, proto::DataType::rhone_price);

        auto rowHeader = definition->mutable_rowheader();

        addRowHeaderCell(rowHeader, PNLRows::RelativeValue, "Relative Value");
        addRowHeaderCell(rowHeader, PNLRows::MarketMakers, "Market Maker");
        addRowHeaderCell(rowHeader, PNLRows::Execution, "Execution");
        addRowHeaderCell(rowHeader, PNLRows::Total, "Total");

        auto cells = definition->mutable_cells();

        for (uint32_t i = toUInt(PNLRows::__start_marker__) + 1; i < toUInt(PNLRows::__end_marker__); ++i) {
            setCell(cells, i, PNLColumns::Open, "0");
            setCell(cells, i, PNLColumns::Closed, "0");
            setCell(cells, i, PNLColumns::Total, "0");
            setCell(cells, i, PNLColumns::Drawdown, "0");
            uint32_t pnlLimit = 0;
            uint32_t drawdownLimit = 0;
            if (i == toUInt(PNLRows::MarketMakers)) {
                PNLConfig* config = _config->getPnlConfig("MM");
                pnlLimit = config->_limit;
                drawdownLimit = config->_drawdown;
            } else if (i == toUInt(PNLRows::RelativeValue)) {
                PNLConfig* config = _config->getPnlConfig("RV");
                pnlLimit = config->_limit;
                drawdownLimit = config->_drawdown;
            } else if (i == toUInt(PNLRows::Total)) {
                PNLConfig* config = _config->getPnlConfig("Total");
                pnlLimit = config->_limit;
                drawdownLimit = config->_drawdown;
            }
            setCell(cells, i, PNLColumns::PnlLimit, std::to_string(ToRhonePrice(pnlLimit)));
            setCell(cells, i, PNLColumns::DrawdownLimit, std::to_string(ToRhonePrice(drawdownLimit)));
        }
    }

    void populateStackerParameters(proto::GridDefinition* definition) {
        definition->set_gridid(toUInt(WindowIds::StackerParameters));
        definition->set_name("Stacker Parameters");
        definition->set_type(proto::GridType::Table);

        auto columnHeader = definition->mutable_columnheader();

        addColumnHeaderCell(columnHeader, StackerParametersColumns::QuoteSize, "QuoteSize", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(columnHeader, StackerParametersColumns::Enabled, "Enabled", false, proto::DataType::bool_type);
        addColumnHeaderCell(columnHeader, StackerParametersColumns::StackLevels, "StackLevels", false, proto::DataType::unsigned_type);

        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t contractIndex = 0; contractIndex < _config->_stackerConfigs.size(); ++contractIndex) {
            StackerConfig& config = _config->_stackerConfigs.at(contractIndex);
            uint32_t internalId = getInternalId(config._contract);
            addRowHeaderCell(rowHeader,internalId, config._contract);
        }

        auto cells = definition->mutable_cells();

        for (uint32_t contractIndex = 0; contractIndex < _config->_stackerConfigs.size(); ++contractIndex) {
            StackerConfig& config = _config->_stackerConfigs.at(contractIndex);
            uint32_t internalId = getInternalId(config._contract);
            setCell(cells, internalId, StackerParametersColumns::Enabled, std::to_string(config._enabled));
            setCell(cells, internalId, StackerParametersColumns::QuoteSize, std::to_string(config._quoteSize));
            setCell(cells, internalId, StackerParametersColumns::StackLevels, std::to_string(config._numberOfLevels));
        }
    }

    void populateMarketMakerParameters(proto::GridDefinition* definition) {
        definition->set_gridid(toUInt(WindowIds::MarketMakerParameters));
        definition->set_name("Market Maker Parameters");
        definition->set_type(proto::GridType::Table);

        auto columnHeader = definition->mutable_columnheader();

        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::QuoteExternal, "QuoteExternal", false, proto::DataType::bool_type);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::QuoteSize, "QuoteSize", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::Width, "Width", false, proto::DataType::double_type);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::JoinSize, "JoinSize", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::LeaveSize, "LeaveSize", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::PayUpQty, "PayUpQty", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::PayUpOrders, "PayUpOrders", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::PayUpMultiple, "PayUpMultiple", false, proto::DataType::double_type);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::MaxPosInterrnal, "MaxPosInternal", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::MaxPosExternal, "MaxPosExternal", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::Commission, "Commission", false, proto::DataType::double_type);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::Enabled, "Enabled", false, proto::DataType::bool_type);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::Position, "Position", true, proto::DataType::signed_type);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::BidPrice, "BidPrice", true, proto::DataType::rhone_price);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::AskPrice, "AskPrice", true, proto::DataType::rhone_price);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::BidQty, "BidQty", true, proto::DataType::unsigned_type);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::AskQty, "AskQty", true, proto::DataType::unsigned_type);

        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t contractIndex = 0; contractIndex < _config->_marketMakerConfigs.size(); ++contractIndex) {
            MarketMakerConfig& config = _config->_marketMakerConfigs.at(contractIndex);
            uint32_t internalId = getInternalId(config._contract);
            addRowHeaderCell(rowHeader,internalId, config._contract);
        }

        auto cells = definition->mutable_cells();

        for (uint32_t contractIndex = 0; contractIndex < _config->_marketMakerConfigs.size(); ++contractIndex) {
            MarketMakerConfig& config = _config->_marketMakerConfigs.at(contractIndex);
            uint32_t internalId = getInternalId(config._contract);
            setCell(cells, internalId, MarketMakerParametersColumns::Enabled, std::to_string(config._enabled));
            setCell(cells, internalId, MarketMakerParametersColumns::QuoteExternal, std::to_string(config._quoteExternal));
            setCell(cells, internalId, MarketMakerParametersColumns::QuoteSize, std::to_string(config._quoteSize));
            setCell(cells, internalId, MarketMakerParametersColumns::Width, std::to_string(config._width));
            setCell(cells, internalId, MarketMakerParametersColumns::JoinSize, std::to_string(config._joinSize));
            setCell(cells, internalId, MarketMakerParametersColumns::LeaveSize, std::to_string(config._leaveSize));
            setCell(cells, internalId, MarketMakerParametersColumns::PayUpQty, std::to_string(config._payUpQty));
            setCell(cells, internalId, MarketMakerParametersColumns::PayUpOrders, std::to_string(config._payUpOrders));
            setCell(cells, internalId, MarketMakerParametersColumns::PayUpMultiple, std::to_string(config._payUpMultiple));
            setCell(cells, internalId, MarketMakerParametersColumns::MaxPosInterrnal, std::to_string(config._maxPosInternal));
            setCell(cells, internalId, MarketMakerParametersColumns::MaxPosExternal, std::to_string(config._maxPosExternal));
            setCell(cells, internalId, MarketMakerParametersColumns::Commission, std::to_string(config._commission));
            setCell(cells, internalId, MarketMakerParametersColumns::Position, "0");
            setCell(cells, internalId, MarketMakerParametersColumns::BidPrice, "0");
            setCell(cells, internalId, MarketMakerParametersColumns::AskPrice, "0");
            setCell(cells, internalId, MarketMakerParametersColumns::BidQty, "0");
            setCell(cells, internalId, MarketMakerParametersColumns::AskQty, "0");
        }
    }

    void populateGatewayStatuses(proto::GridDefinition* definition) {
        definition->set_gridid(toUInt(WindowIds::GatewayStatuses));
        definition->set_name("Gateway Status");
        definition->set_type(proto::GridType::Table);

        auto columnHeader = definition->mutable_columnheader();

        addColumnHeaderCell(columnHeader, GatewayStatusColumns::Exchange, "Exchange", true, proto::DataType::string_type);
        addColumnHeaderCell(columnHeader, GatewayStatusColumns::Status, "Status", true, proto::DataType::bool_type);

        auto rowHeader = definition->mutable_rowheader();

        addRowHeaderCell(rowHeader, 1, "");

        auto cells = definition->mutable_cells();

        setCell(cells, 1, GatewayStatusColumns::Exchange, "ICE");
        setCell(cells, 1, GatewayStatusColumns::Status, "1");
    }

    void populateGatewayStatusUpdate(proto::GridUpdate& message, bool active) {
        message.set_gridid(toUInt(WindowIds::GatewayStatuses));
        auto cells = message.mutable_cells();
        uint32_t value = active ? 1 : 0;
        setCell(cells, 1, GatewayStatusColumns::Status, std::to_string(value));
    }

    void populateErrors(proto::GridDefinition* definition) {
        definition->set_gridid(toUInt(WindowIds::Errors));
        definition->set_name("Errors");
        definition->set_type(proto::GridType::List);

        auto columnHeader = definition->mutable_columnheader();

        addColumnHeaderCell(columnHeader, ErrorsColumns::Time, "Time", true, proto::DataType::string_type);
        addColumnHeaderCell(columnHeader, ErrorsColumns::Message, "Message", true, proto::DataType::string_type);
        addColumnHeaderCell(columnHeader, ErrorsColumns::Type, "Type", true, proto::DataType::enum_type, "errorTypes");
    }

    void populateFills(proto::GridDefinition* definition) {
        definition->set_gridid(toUInt(WindowIds::FillsDisplay));
        definition->set_name("Fills");
        definition->set_type(proto::GridType::List);

        auto columnHeader = definition->mutable_columnheader();

        addColumnHeaderCell(columnHeader, FillsColumns::Time, "Time", true, proto::DataType::string_type);
        addColumnHeaderCell(columnHeader, FillsColumns::Contract, "Contract", true, proto::DataType::enum_type, "contracts");
        addColumnHeaderCell(columnHeader, FillsColumns::Side, "Side", true, proto::DataType::enum_type, "sides");
        addColumnHeaderCell(columnHeader, FillsColumns::Price, "Price", true, proto::DataType::rhone_price);
        addColumnHeaderCell(columnHeader, FillsColumns::Size, "Size", true, proto::DataType::unsigned_type);
        addColumnHeaderCell(columnHeader, FillsColumns::Strategy, "Strategy", true, proto::DataType::enum_type, "strategies");
        addColumnHeaderCell(columnHeader, FillsColumns::Type, "Type", true, proto::DataType::enum_type, "fillTypes");
        addColumnHeaderCell(columnHeader, FillsColumns::Source, "Source", true, proto::DataType::enum_type, "fillSource");
    }

    void populateMainControl(proto::GridDefinition* definition) {
        definition->set_gridid(toUInt(WindowIds::MainControl));
        definition->set_name("Main Control");
        definition->set_type(proto::GridType::MainControl);

        auto columnHeader = definition->mutable_columnheader();

        addColumnHeaderCell(columnHeader, MainControlAction::Enabled, "Enabled", true, proto::DataType::enum_type, "switches");
        addColumnHeaderCell(columnHeader, MainControlAction::Quoting, "Quoting", true, proto::DataType::enum_type, "switches");

        auto rowHeader = definition->mutable_rowheader();

        addRowHeaderCell(rowHeader, StrategyId::MarketMaker, "MarketMaker");
        addRowHeaderCell(rowHeader, StrategyId::RelativeValue, "RelativeValue");

        auto cells = definition->mutable_cells();

        setCell(cells, toUInt(StrategyId::MarketMaker), MainControlAction::Enabled, std::to_string(proto::SystemState::SystemStateDisabled));
        setCell(cells, toUInt(StrategyId::MarketMaker), MainControlAction::Quoting, std::to_string(proto::SystemState::SystemStateEnabled));
        setCell(cells, toUInt(StrategyId::RelativeValue), MainControlAction::Enabled, std::to_string(proto::SystemState::SystemStateDisabled));
        setCell(cells, toUInt(StrategyId::RelativeValue), MainControlAction::Quoting, std::to_string(proto::SystemState::SystemStateEnabled));

        auto buttons = definition->mutable_buttons();
        addButton(buttons, toUInt(MainControlButtons::Enable), "Enable All", proto::ButtonFrontBehavior_Nothing, proto::ButtonBackBehavior_Nothing);
        addButton(buttons, toUInt(MainControlButtons::Disable), "Disable All", proto::ButtonFrontBehavior_Nothing, proto::ButtonBackBehavior_Nothing);
        addButton(buttons, toUInt(MainControlButtons::HardShutdown), "Shutdown", proto::ButtonFrontBehavior_Nothing, proto::ButtonBackBehavior_Nothing);
        addButton(buttons, toUInt(MainControlButtons::CheckQuotes), "Check Quotes", proto::ButtonFrontBehavior_Nothing, proto::ButtonBackBehavior_Nothing);
    }

    template <typename ButtonsT>
    void addButton(ButtonsT buttons, uint32_t id, const std::string& label, proto::ButtonFrontBehavior front, proto::ButtonBackBehavior back) {
        proto::Button* button = buttons->Add();
        button->set_id(id);
        button->set_name(label);
        button->set_back(back);
        button->set_front(front);
    }

    template <typename HeaderT, typename EnumValue>
    void addColumnHeaderCell(HeaderT* header, EnumValue id, const std::string& label, bool readyOnly, proto::DataType type, const std::string& e = "") {
        proto::ColumnHeaderCell* headerCell = header->Add();
        headerCell->set_id(toUInt(id));
        headerCell->set_label(label);
        headerCell->set_type(type);
        if (e != "") {
            headerCell->set_enumerationname(e);
        }
        headerCell->set_readonly(readyOnly);
    }

    template <typename HeaderT, typename EnumValue>
    void addRowHeaderCell(HeaderT* header, EnumValue id, const std::string& label) {
        auto headerCell = header->Add();
        headerCell->set_id(toUInt(id));
        headerCell->set_label(label);
    }

    void populateManualFillsEntry(proto::GridDefinition* definition) {
        definition->set_gridid(toUInt(WindowIds::ManualFillsEntry));
        definition->set_name("Manual Fills Entry");
        definition->set_type(proto::GridType::Table);

        auto columnHeader = definition->mutable_columnheader();

        addColumnHeaderCell(columnHeader, ManualFillsEntryColumns::Contract, "Contract", false, proto::DataType::enum_type, "contracts");
        addColumnHeaderCell(columnHeader, ManualFillsEntryColumns::Size, "Size", false, proto::DataType::signed_type);
        addColumnHeaderCell(columnHeader, ManualFillsEntryColumns::Strategy, "Strategy", false, proto::DataType::enum_type, "strategies");

        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t index = 0; index < 6; ++index) {
            std::string name;
            name.append(std::to_string(index)).append(". ");
            addRowHeaderCell(rowHeader, index, name);
        }

        auto cells = definition->mutable_cells();

        for (uint32_t index = 0; index < 6; ++index) {
            setCell(cells, index, ManualFillsEntryColumns::Contract, NO_VALUE_ID);
            setCell(cells, index, ManualFillsEntryColumns::Size, "0");
            setCell(cells, index, ManualFillsEntryColumns::Strategy, NO_VALUE_ID);
        }
    }

    void populateRiskMatrix(proto::GridDefinition* definition, Matrix& riskMatrix) {
        definition->set_gridid(toUInt(WindowIds::RiskMatrix));
        definition->set_name("Risk Matrix");
        definition->set_type(proto::GridType::Matrix);

        auto columnHeader = definition->mutable_columnheader();
        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t index = 0; index < _config->_numberOfOutrights; ++index) {
            ConfigContract& contract = _config->_allContracts[index];
            addColumnHeaderCell(columnHeader, index, contract._name, true, proto::DataType::double_type);
        }

        for (uint32_t index = 0; index < _config->_vectorConfigs.size(); ++index) {
            VectorConfig& config = _config->_vectorConfigs[index];
            addRowHeaderCell(rowHeader, index, config._vectorName);
        }

        auto cells = definition->mutable_cells();

        for (uint32_t row = 0; row < riskMatrix._numberOfRows; ++row) {
            for (uint32_t col = 0; col < riskMatrix._numberOfColumns; ++col) {
                setCell(cells, row, col, std::to_string(riskMatrix.getValue(row, col)));
            }
        }
    }

    void populatePricingMatrix(proto::GridDefinition* definition, Matrix& pricingMatrix) {
        definition->set_gridid(toUInt(WindowIds::PricingMatrix));
        definition->set_name("Pricing Matrix");
        definition->set_type(proto::GridType::Matrix);

        auto columnHeader = definition->mutable_columnheader();
        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t index = 0; index < _config->_numberOfOutrights; ++index) {
            ConfigContract& contract = _config->_allContracts[index];
            addColumnHeaderCell(columnHeader, index, contract._name, true, proto::DataType::double_type);
        }

        for (uint32_t index = 0; index < _config->_vectorConfigs.size(); ++index) {
            VectorConfig& config = _config->_vectorConfigs[index];
            addRowHeaderCell(rowHeader, index, config._vectorName);
        }

        auto cells = definition->mutable_cells();

        for (uint32_t row = 0; row < pricingMatrix._numberOfRows; ++row) {
            for (uint32_t col = 0; col < pricingMatrix._numberOfColumns; ++col) {
                setCell(cells, row, col, std::to_string(pricingMatrix.getValue(row, col)));
            }
        }
    }

    void populatePricingConfig(proto::GridDefinition* definition) {
        definition->set_gridid(toUInt(WindowIds::PricingConfig));
        definition->set_name("Pricing Parameters");
        definition->set_type(proto::GridType::Table);

        auto columnHeader = definition->mutable_columnheader();

        addColumnHeaderCell(columnHeader, PricingConfigsColumns::Weight, "Weight", false, proto::DataType::double_type);
        addColumnHeaderCell(columnHeader, PricingConfigsColumns::UseWeighted, "UseWeighted", false, proto::DataType::bool_type);
        addColumnHeaderCell(columnHeader, PricingConfigsColumns::EstablishedLevel, "EstablishedLevel", false, proto::DataType::unsigned_type);

        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t contractIndex = 0; contractIndex < _config->_pricingConfigs.size(); ++contractIndex) {
            PricingConfig& config = _config->_pricingConfigs.at(contractIndex);
            uint32_t internalId = getInternalId(config._contract);
            addRowHeaderCell(rowHeader,internalId, config._contract);
        }

        auto cells = definition->mutable_cells();

        for (uint32_t contractIndex = 0; contractIndex < _config->_pricingConfigs.size(); ++contractIndex) {
            PricingConfig& config = _config->_pricingConfigs.at(contractIndex);
            setCell(cells, contractIndex, PricingConfigsColumns::Weight, std::to_string(config._weight));
            setCell(cells, contractIndex, PricingConfigsColumns::UseWeighted, std::to_string(config._useWeightedMid));
            setCell(cells, contractIndex, PricingConfigsColumns::EstablishedLevel, std::to_string(config._establishedLevel));
        }
    }

    void populateLeanMatrix(proto::GridDefinition* definition) {
        definition->set_gridid(toUInt(WindowIds::LeanMatrix));
        definition->set_name("Lean Matrix");
        definition->set_type(proto::GridType::Matrix);

        auto columnHeader = definition->mutable_columnheader();
        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t index = 0; index < _config->_leanMatrix.size(); ++index) {
            VectorWithContract& config = _config->_leanMatrix[index];
            uint32_t internalId = getInternalId(config._contract);
            addColumnHeaderCell(columnHeader, internalId, config._contract, false, proto::DataType::bool_type);
            addRowHeaderCell(rowHeader, internalId, config._contract);
        }

        auto cells = definition->mutable_cells();

        for (uint32_t row = 0; row < _config->_leanMatrix.size(); ++row) {
            VectorWithContract& rowConfig = _config->_leanMatrix[row];
            uint32_t rowInternalId = getInternalId(rowConfig._contract);
            for (uint32_t col = 0; col < _config->_leanMatrix.size(); ++col) {
                VectorWithContract& colConfig = _config->_leanMatrix[col];
                uint32_t colInternalId = getInternalId(colConfig._contract);
                int value = _config->_leanMatrix[row]._values[col];
                setCell(cells, rowInternalId, colInternalId, std::to_string(value));
            }
        }
    }

    void populateContractParameterRVColumns(proto::GridDefinition* definition) {
        definition->set_gridid(toUInt(WindowIds::ContractParametersRv));
        definition->set_name("Relative Value Parameters");
        definition->set_type(proto::GridType::Table);

        auto columnHeader = definition->mutable_columnheader();

        addColumnHeaderCell(columnHeader, ContractParameterRVColumns::Position, "Position", true, proto::DataType::signed_type);
        addColumnHeaderCell(columnHeader, ContractParameterRVColumns::BidLean, "BidLean", true, proto::DataType::enum_type, "contracts");
        addColumnHeaderCell(columnHeader, ContractParameterRVColumns::AskLean, "AskLean", true, proto::DataType::enum_type, "contracts");
        addColumnHeaderCell(columnHeader, ContractParameterRVColumns::BidPrice, "BidPrice", true, proto::DataType::rhone_price);
        addColumnHeaderCell(columnHeader, ContractParameterRVColumns::AskPrice, "AskPrice", true, proto::DataType::rhone_price);
        addColumnHeaderCell(columnHeader, ContractParameterRVColumns::BidCross, "BidCross", true, proto::DataType::enum_type, "YesNo");
        addColumnHeaderCell(columnHeader, ContractParameterRVColumns::AskCross, "AskCross", true, proto::DataType::enum_type, "YesNo");
        addColumnHeaderCell(columnHeader, ContractParameterRVColumns::BidQty, "BidQty", true, proto::DataType::unsigned_type);
        addColumnHeaderCell(columnHeader, ContractParameterRVColumns::AskQty, "AskQty", true, proto::DataType::unsigned_type);
        addColumnHeaderCell(columnHeader, ContractParameterRVColumns::STF, "STF", true, proto::DataType::rhone_price);
        addColumnHeaderCell(columnHeader, ContractParameterRVColumns::Mkt, "Mkt", true, proto::DataType::rhone_price);
        addColumnHeaderCell(columnHeader, ContractParameterRVColumns::QuoteExternal, "QuoteExternal", false, proto::DataType::bool_type);
        addColumnHeaderCell(columnHeader, ContractParameterRVColumns::HedgeTolerance, "HedgeTolerance", false, proto::DataType::double_type);
        addColumnHeaderCell(columnHeader, ContractParameterRVColumns::Width, "Width", false, proto::DataType::double_type);
        addColumnHeaderCell(columnHeader, ContractParameterRVColumns::Commission, "Commission", false, proto::DataType::double_type);

        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t index = 0; index < _config->_relativeValueConfigs.size(); ++index) {
            RelativeValueConfig& config = _config->_relativeValueConfigs[index];
            uint32_t internalId = getInternalId(config._contract);
            addRowHeaderCell(rowHeader, internalId, config._contract);
        }

        auto cells = definition->mutable_cells();

        for (uint32_t index = 0; index < _config->_relativeValueConfigs.size(); ++index) {
            RelativeValueConfig& config = _config->_relativeValueConfigs[index];
            uint32_t internalId = getInternalId(config._contract);
            setCell(cells, internalId, ContractParameterRVColumns::Position, std::to_string(config._position));
            setCell(cells, internalId, ContractParameterRVColumns::BidLean, NO_VALUE_ID);
            setCell(cells, internalId, ContractParameterRVColumns::AskLean, NO_VALUE_ID);
            setCell(cells, internalId, ContractParameterRVColumns::BidPrice, "0");
            setCell(cells, internalId, ContractParameterRVColumns::AskPrice, "0");
            setCell(cells, internalId, ContractParameterRVColumns::BidQty, "0");
            setCell(cells, internalId, ContractParameterRVColumns::AskQty, "0");
            setCell(cells, internalId, ContractParameterRVColumns::STF, "0");
            setCell(cells, internalId, ContractParameterRVColumns::Mkt, "0");
            setCell(cells, internalId, ContractParameterRVColumns::QuoteExternal, std::to_string(config._quoteExternal));
            setCell(cells, internalId, ContractParameterRVColumns::HedgeTolerance, std::to_string(config._hedgeTolerance));
            setCell(cells, internalId, ContractParameterRVColumns::Width, std::to_string(config._width));
            setCell(cells, internalId, ContractParameterRVColumns::Commission, std::to_string(config._commission));
            setCell(cells, internalId, ContractParameterRVColumns::BidCross, std::to_string(toUInt(YesNoType::Unknown)));
            setCell(cells, internalId, ContractParameterRVColumns::AskCross, std::to_string(toUInt(YesNoType::Unknown)));
        }
    }

    uint32_t getInternalId(const std::string& name) {
        auto it = _symbolToInterId->find(name);
        if (it != _symbolToInterId->end()) {
            return it->second;
        }
        throw std::runtime_error("could not map symbol name to internal id. " + name);
    }

    void populateVectorParametersDefinition(proto::GridDefinition* definition) {
        definition->set_gridid(toUInt(WindowIds::VectorParameters));
        definition->set_name("Vector Parameters");
        definition->set_type(proto::GridType::Table);

        auto columnHeader = definition->mutable_columnheader();

        addColumnHeaderCell(columnHeader, VectorParametersColumns::Position, "Position", true, proto::DataType::double_type);
        addColumnHeaderCell(columnHeader, VectorParametersColumns::Mkt, "Mkt", true, proto::DataType::double_type);
        addColumnHeaderCell(columnHeader, VectorParametersColumns::STF, "STF", true, proto::DataType::double_type);
        addColumnHeaderCell(columnHeader, VectorParametersColumns::LTF, "LTF", true, proto::DataType::double_type);
        addColumnHeaderCell(columnHeader, VectorParametersColumns::MaxPos, "MaxPos", false, proto::DataType::signed_type);
        addColumnHeaderCell(columnHeader, VectorParametersColumns::Range, "Range", false, proto::DataType::double_type);
        addColumnHeaderCell(columnHeader, VectorParametersColumns::Step, "Step", false, proto::DataType::double_type);
        addColumnHeaderCell(columnHeader, VectorParametersColumns::EMA_ON, "EMA_ON", false, proto::DataType::bool_type);
        addColumnHeaderCell(columnHeader, VectorParametersColumns::EMA_Time, "EMA_Time", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(columnHeader, VectorParametersColumns::SetToMarket, "SetToMarket", false, proto::DataType::bool_type);

        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t vectorIndex = 0; vectorIndex < _config->_vectorConfigs.size(); ++vectorIndex) {
            VectorConfig& config = _config->_vectorConfigs[vectorIndex];
            addRowHeaderCell(rowHeader, vectorIndex, config._vectorName);
        }

        auto cells = definition->mutable_cells();

        for (uint32_t vectorIndex = 0; vectorIndex < _config->_vectorConfigs.size(); ++vectorIndex) {
            VectorConfig& config = _config->_vectorConfigs[vectorIndex];
            setCell(cells, vectorIndex, VectorParametersColumns::Position, "0");
            setCell(cells, vectorIndex, VectorParametersColumns::Mkt, "0");
            setCell(cells, vectorIndex, VectorParametersColumns::STF, "0");
            setCell(cells, vectorIndex, VectorParametersColumns::LTF, std::to_string(config._longTermFair));
            setCell(cells, vectorIndex, VectorParametersColumns::MaxPos, std::to_string(config._maxPos));
            setCell(cells, vectorIndex, VectorParametersColumns::Range, std::to_string(config._range));
            setCell(cells, vectorIndex, VectorParametersColumns::Step, std::to_string(config._step));
            setCell(cells, vectorIndex, VectorParametersColumns::EMA_ON, std::to_string(config._useEMA));
            setCell(cells, vectorIndex, VectorParametersColumns::EMA_Time, std::to_string(config._EMALength));
            setCell(cells, vectorIndex, VectorParametersColumns::SetToMarket, std::to_string(config._setToMarket));
        }
    }

    template <typename CellsT, typename ColT>
     void setCell(CellsT cells, uint32_t row, ColT col, const std::string& value) {
        auto cell = cells->Add();
        cell->set_row(row);
        cell->set_col(toUInt(col));
        cell->set_value(value);
    }

private:
    ViperConfig* _config;
    std::unordered_map<std::string, uint32_t>* _symbolToInterId;
    uint32_t _fillNumber = 0;
    uint32_t _errorMessageNumber = 0;
};

};

#endif //VIPER_VIPER_CONFIG_H
