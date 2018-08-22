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

    void populateQuoteSizes(proto::GridUpdate& message, Matrix& quoteSizes) {
        message.set_gridid(toUInt(WindowIds::QuoteSizes));
        auto cells = message.mutable_cells();
        for (uint32_t row = _config->_indexOfFirstTradedContract; row < _config->_allContracts.size(); ++row) {
            for (uint32_t col = _config->_indexOfFirstTradedContract; col < _config->_allContracts.size(); ++col) {
                setCell(cells, row, col, std::to_string(quoteSizes.getValue(row, col)));
            }
        }
    }

    void updateHedgeRatios(proto::GridUpdate& message, SimpleMatrix<double>& hedgeRatios) {
        message.set_gridid(toUInt(WindowIds::HedgeRatios));
        auto cells = message.mutable_cells();
        for (uint32_t row = _config->_indexOfFirstTradedContract; row < _config->_allContracts.size(); ++row) {
            for (uint32_t col = _config->_indexOfFirstTradedContract; col < _config->_allContracts.size(); ++col) {
                setCell(cells, row, col, std::to_string(hedgeRatios.getValue(row, col)));
            }
        }
    }

    template <typename T>
    void populateMessageCountsUpdate(proto::GridUpdate& message, T* executor) {
        message.set_gridid(toUInt(WindowIds::MessagesCounts));
        auto cells = message.mutable_cells();
        uint32_t inserts = 0;
        uint32_t modifies = 0;
        uint32_t cancels = 0;
        uint32_t cancelRejects = 0;
        uint32_t modifyRejects = 0;
        uint32_t fills = 0;
        hf::VENUE venue = hf::VENUE::UNKNOWN;
        for (uint32_t contractIndex = 0; contractIndex < executor->_contractStates.size(); ++contractIndex) {
            auto& state = executor->_contractStates[contractIndex];
            if (state._marketMaker._toMarketMake) {
                venue = state._venue;
                setCell(cells, contractIndex, MessageCountsColumns::Inserts, std::to_string(state._contractExecutor._messages._inserts));
                inserts += state._contractExecutor._messages._inserts;
                setCell(cells, contractIndex, MessageCountsColumns::Modifies, std::to_string(state._contractExecutor._messages._modifies));
                modifies += state._contractExecutor._messages._modifies;
                setCell(cells, contractIndex, MessageCountsColumns::Cancels, std::to_string(state._contractExecutor._messages._cancels));
                cancels += state._contractExecutor._messages._cancels;
                setCell(cells, contractIndex, MessageCountsColumns::CancelRejects, std::to_string(state._contractExecutor._messages._cancelRejects));
                cancelRejects += state._contractExecutor._messages._cancelRejects;
                setCell(cells, contractIndex, MessageCountsColumns::ModifyRejects, std::to_string(state._contractExecutor._messages._modifyRejects));
                modifyRejects += state._contractExecutor._messages._modifyRejects;
                setCell(cells, contractIndex, MessageCountsColumns::Fills, std::to_string(state._contractExecutor._messages._fills));
                fills += state._contractExecutor._messages._fills;
                double fillsRatio = 0;
                if (state._contractExecutor._messages._fills != 0) {
                    switch (state._venue) {
                        case hf::VENUE::ICE: {
                            fillsRatio = static_cast<double>(state._contractExecutor._messages._inserts + state._contractExecutor._messages._modifies) / state._contractExecutor._messages._fills;
                            break;
                        }
                        case hf::VENUE::CME: {
                            fillsRatio = static_cast<double>(state._contractExecutor._messages._cancels * 3 + state._contractExecutor._messages._modifies) / state._contractExecutor._messages._fills;
                            break;
                        }
                        default:
                            throw std::runtime_error("messages-unhandled exchange");
                    }
                }
                setCell(cells, contractIndex, MessageCountsColumns::FillsRatio, std::to_string(fillsRatio));
            }
        }
        setCell(cells, 1000, MessageCountsColumns::Inserts, std::to_string(inserts));
        setCell(cells, 1000, MessageCountsColumns::Modifies, std::to_string(modifies));
        setCell(cells, 1000, MessageCountsColumns::Cancels, std::to_string(cancels));
        setCell(cells, 1000, MessageCountsColumns::CancelRejects, std::to_string(cancelRejects));
        setCell(cells, 1000, MessageCountsColumns::ModifyRejects, std::to_string(modifyRejects));
        setCell(cells, 1000, MessageCountsColumns::Fills, std::to_string(fills));
        double fillsRatio = 0;
        if (fills != 0) {
            switch (venue) {
                case hf::VENUE::ICE: {
                    fillsRatio = static_cast<double>(inserts + modifies) / fills;
                    break;
                }
                case hf::VENUE::CME: {
                    fillsRatio = static_cast<double>(cancels * 3 + modifies) / fills;
                    break;
                }
                default:
                    throw std::runtime_error("messages-unhandled exchange");
            }
        }
        setCell(cells, 1000, MessageCountsColumns::FillsRatio, std::to_string(fillsRatio));
    }

    template <typename T>
    void populateMarketMakerState(proto::GridUpdate& message, T* executor) {
        message.set_gridid(toUInt(WindowIds::MarketMakerParameters));
        auto cells = message.mutable_cells();
        for (uint32_t contractIndex = 0; contractIndex < executor->_contractStates.size(); ++contractIndex) {
            auto& state = executor->_contractStates[contractIndex];
            if (state._marketMaker._toMarketMake) {
                setCell(cells, contractIndex, MarketMakerParametersColumns::MidPoint, std::to_string(state._marketMaker._midPoint));
                setCell(cells, contractIndex, MarketMakerParametersColumns::InternalPosition, std::to_string(state._marketMaker._internallyInitiatedPosition));
                setCell(cells, contractIndex, MarketMakerParametersColumns::ExternalPosition, std::to_string(state._marketMaker._externallyInitiatedPosition));
                auto& bidQuote = state._marketMaker._quotes[BID];
                setCell(cells, contractIndex, MarketMakerParametersColumns::BidInternalPrice, std::to_string(bidQuote._internalPrice));
                setCell(cells, contractIndex, MarketMakerParametersColumns::BidInternalQty, std::to_string(bidQuote._internalSize));
                setCell(cells, contractIndex, MarketMakerParametersColumns::BidExternalPrice, std::to_string(bidQuote._externalPrice));
                setCell(cells, contractIndex, MarketMakerParametersColumns::BidExternalQty, std::to_string(bidQuote._externalSize));
                auto& askQuote = state._marketMaker._quotes[ASK];
                setCell(cells, contractIndex, MarketMakerParametersColumns::AskInternalPrice, std::to_string(askQuote._internalPrice));
                setCell(cells, contractIndex, MarketMakerParametersColumns::AskInternalQty, std::to_string(askQuote._internalSize));
                setCell(cells, contractIndex, MarketMakerParametersColumns::AskExternalPrice, std::to_string(askQuote._externalPrice));
                setCell(cells, contractIndex, MarketMakerParametersColumns::AskExternalQty, std::to_string(askQuote._externalSize));
                auto& bidHedge = state._marketMaker._hedges[BID];
                setCell(cells, contractIndex, MarketMakerParametersColumns::BidHedgeInternalPrice, std::to_string(bidHedge._internalPrice));
                setCell(cells, contractIndex, MarketMakerParametersColumns::BidHedgeExternalPrice, std::to_string(bidHedge._externalPrice));
                setCell(cells, contractIndex, MarketMakerParametersColumns::BidHedgeSize, std::to_string(bidHedge._sizeToQuote));
                auto& askHedge = state._marketMaker._hedges[ASK];
                setCell(cells, contractIndex, MarketMakerParametersColumns::AskHedgeInternalPrice, std::to_string(askHedge._internalPrice));
                setCell(cells, contractIndex, MarketMakerParametersColumns::AskHedgeExternalPrice, std::to_string(askHedge._externalPrice));
                setCell(cells, contractIndex, MarketMakerParametersColumns::AskHedgeSize, std::to_string(askHedge._sizeToQuote));
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

    void populatePnlDetailUpdate(proto::GridUpdate& message, Pnl& rvPnl, Pnl& mmPnl, StrategyId id, uint32_t contractIndex) {
        message.set_gridid(toUInt(WindowIds::PnlDetail));
        auto cells = message.mutable_cells();
        switch (id) {
            case StrategyId::RelativeValue: {
                setCell(cells, contractIndex, PnlDetailColumns::RVOpen, std::to_string(rvPnl._open));
                setCell(cells, contractIndex, PnlDetailColumns::RVClosed, std::to_string(rvPnl._closed));
                setCell(cells, contractIndex, PnlDetailColumns::RVTotal, std::to_string(rvPnl.getTotal()));
                break;
            }
            case StrategyId::MarketMaker: {
                setCell(cells, contractIndex, PnlDetailColumns::MMOpen, std::to_string(mmPnl._open));
                setCell(cells, contractIndex, PnlDetailColumns::MMClosed, std::to_string(mmPnl._closed));
                setCell(cells, contractIndex, PnlDetailColumns::MMTotal, std::to_string(mmPnl.getTotal()));
                break;
            }
            case StrategyId::Execution: {
                break;
            }
        }
        setCell(cells, contractIndex, PnlDetailColumns::Total, std::to_string(mmPnl.getTotal() + rvPnl.getTotal()));
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
        setCell(cells, _fillNumber, FillsColumns::Number, std::to_string(_fillNumber));
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

    void populateVectorGridUpdate(proto::GridUpdate& message, RelativeValue* relativeValue) {
        message.set_gridid(toUInt(WindowIds::VectorParameters));
        auto cells = message.mutable_cells();
        for (uint32_t vectorIndex = 0; vectorIndex < relativeValue->_numberOfVectors; ++vectorIndex) {
            const double ltf = relativeValue->_longTermFairsInVector.getValue(vectorIndex);
            setCell(cells, vectorIndex, VectorParametersColumns::LTF, std::to_string(ltf));
            const double mkt = relativeValue->_currentVectorMarketPrices->getValue(vectorIndex);
            setCell(cells, vectorIndex, VectorParametersColumns::Mkt, std::to_string(mkt));
            double stf = relativeValue->_shortTermFairsInVector.getValue(vectorIndex);
            setCell(cells, vectorIndex, VectorParametersColumns::STF, std::to_string(stf));
            double position = relativeValue->_positionsInVector.getValue(vectorIndex);
            setCell(cells, vectorIndex, VectorParametersColumns::Position, std::to_string(position));
        }
    }

    void populateChartDataPoints(proto::DataSeriesPointSet& message, RelativeValue* relativeValue) {
        auto points = message.mutable_points();
        for (uint32_t vectorIndex = 0; vectorIndex < relativeValue->_numberOfVectors; ++vectorIndex) {
            {
                const double ltf = relativeValue->_longTermFairsInVector.getValue(vectorIndex);
                auto point = points->Add();
                point->set_chartid(vectorIndex);
                point->set_dataseriesid(toUInt(DataSeries::LTF));
                point->set_value(ltf);
                point->set_time(hf::util::TimeUtils::clock_gettime());
            }
            {
                const double mkt = relativeValue->_currentVectorMarketPrices->getValue(vectorIndex);
                auto point = points->Add();
                point->set_chartid(vectorIndex);
                point->set_dataseriesid(toUInt(DataSeries::VectorPrice));
                point->set_value(mkt);
                point->set_time(hf::util::TimeUtils::clock_gettime());
            }
            {
                double stf = relativeValue->_shortTermFairsInVector.getValue(vectorIndex);
                auto point = points->Add();
                point->set_chartid(vectorIndex);
                point->set_dataseriesid(toUInt(DataSeries::STF));
                point->set_value(stf);
                point->set_time(hf::util::TimeUtils::clock_gettime());
            }
        }
    }


    void populateRelativeValueGridUpdate(proto::GridUpdate& message, RelativeValue* relativeValue, bool checkForCross) {
        message.set_gridid(toUInt(WindowIds::ContractParametersRv));
        auto cells = message.mutable_cells();
        for (uint32_t contractIndex = 0; contractIndex < relativeValue->_numberOfAllContracts; ++contractIndex) {
            RvContract& contract = relativeValue->_contracts[contractIndex];
            if (contract._traded) {
                Quote& bid = contract._quotes[BID];
                if (bid._bestPrice != hf::NULL_PRICE) {
                    setCell(cells, contract._internalId, ContractParameterRVColumns::BidInternalPrice, std::to_string(bid._bestPrice));
                    setCell(cells, contract._internalId, ContractParameterRVColumns::BidInternalQty, std::to_string(bid._bestSize));
                    setCell(cells, contract._internalId, ContractParameterRVColumns::BidExternalPrice, std::to_string(bid._externalPrice));
                    setCell(cells, contract._internalId, ContractParameterRVColumns::BidExternalQty, std::to_string(bid._externalSize));
                    RvContract& bidBestLean = relativeValue->_contracts[bid._bestLeanIndex];
                    setCell(cells, contract._internalId, ContractParameterRVColumns::BidLean, std::to_string(bidBestLean._definition._internalId));
                    LeanType leanType = LeanType::None;
                    if (bid._leans.getNumberOfLeans() > 0) {
                        Lean& bidLean = bid._leans._leans[0];
                        if (!bidLean._isExternal) {
                            leanType = bidLean._marketIndex == toUInt(OrderIds::ORDER_ID_MM_HEDGE) ? LeanType::MM_Hedge : LeanType::MM_Quote;
                        } else {
                            leanType = LeanType::External;
                        }
                    }
                    setCell(cells, contract._internalId, ContractParameterRVColumns::BidLeanType, std::to_string(toUInt(leanType)));
                    YesNoType bidCrossed = YesNoType::Unknown;
                    if (checkForCross && bid._externalPrice != hf::NULL_PRICE && contract._externalMarket[ASK].getPrice() != hf::NULL_PRICE) {
                        bidCrossed = bid._externalPrice >= contract._externalMarket[ASK].getPrice() ? YesNoType::Yes : YesNoType::No;
                    }
                    setCell(cells, contract._internalId, ContractParameterRVColumns::BidCross, std::to_string(toUInt(bidCrossed)));
                }
                Quote& ask = contract._quotes[ASK];
                if (ask._bestPrice != hf::NULL_PRICE) {
                    setCell(cells, contract._internalId, ContractParameterRVColumns::AskInternalPrice, std::to_string(ask._bestPrice));
                    setCell(cells, contract._internalId, ContractParameterRVColumns::AskInternalQty, std::to_string(ask._bestSize));
                    setCell(cells, contract._internalId, ContractParameterRVColumns::AskExternalPrice, std::to_string(ask._externalPrice));
                    setCell(cells, contract._internalId, ContractParameterRVColumns::AskExternalQty, std::to_string(ask._externalSize));
                    RvContract& askBestLean = relativeValue->_contracts[ask._bestLeanIndex];
                    setCell(cells, contract._internalId, ContractParameterRVColumns::AskLean, std::to_string(askBestLean._definition._internalId));
                    LeanType leanType = LeanType::None;
                    if (ask._leans.getNumberOfLeans() > 0) {
                        Lean& askLean = ask._leans._leans[0];
                        if (!askLean._isExternal) {
                            leanType = askLean._marketIndex == toUInt(OrderIds::ORDER_ID_MM_HEDGE) ? LeanType::MM_Hedge : LeanType::MM_Quote;
                        } else {
                            leanType = LeanType::External;
                        }
                    }
                    setCell(cells, contract._internalId, ContractParameterRVColumns::AskLeanType, std::to_string(toUInt(leanType)));
                    YesNoType askCrossed = YesNoType::Unknown;
                    if (checkForCross && ask._externalPrice != hf::NULL_PRICE && contract._externalMarket[BID].getPrice() != hf::NULL_PRICE) {
                        askCrossed = ask._externalPrice <= contract._externalMarket[BID].getPrice() ? YesNoType::Yes : YesNoType::No;
                    }
                    setCell(cells, contract._internalId, ContractParameterRVColumns::AskCross, std::to_string(toUInt(askCrossed)));
                }
                {
                    hf::price_t price = 0;
                    if (contract._definition._numberOfLegs > 1) {
                        double sum = 0;
                        for (uint32_t i = 0; i < contract._definition._numberOfLegs; ++i) {
                            auto& leg = contract._definition._legs[i];
                            sum += relativeValue->_overdeterminedPrices.getValue(0, leg._index) * leg._size;
                        }
                        price = ToRhonePrice(sum);
                    } else {
                        price = ToRhonePrice(relativeValue->_overdeterminedPrices.getValue(0, contractIndex));
                    }
                    setCell(cells, contract._internalId, ContractParameterRVColumns::Mkt, std::to_string(price));
                }
                {
                    hf::price_t stf = relativeValue->_shortTermFairsForContracts.getValue(contractIndex);
                    setCell(cells, contract._internalId, ContractParameterRVColumns::STF, std::to_string(stf));
                }
                {
                    int64_t position = (int64_t) relativeValue->_contractPositions.getValue(contractIndex);
                    setCell(cells, contractIndex, ContractParameterRVColumns::Position, std::to_string(position));
                }
            }
        }
    }

    void populateMarketMakerEnabled(proto::GridUpdate& message, uint32_t id, bool enabled) {
        message.set_gridid(toUInt(WindowIds::MarketMakerParameters));
        auto cells = message.mutable_cells();
        setCell(cells, id, MarketMakerParametersColumns::Enabled, std::to_string(enabled));
    }

    void populateGUIInitialization(proto::GUIInitialization& message, Matrix& pricingMatrix, Matrix& riskMatrix, SimpleMatrix<double>& hedgeRatios,
                                   Matrix& quoteSizes, SimpleMatrix<hf::price_t>& commissions) {
        auto definitions = message.mutable_definitions();
        populateMainControl(definitions->Add());
        populateErrors(definitions->Add());
        populateMarketMakerParameters(definitions->Add());
        populateContractParameterRVColumns(definitions->Add());
        populateLeanMatrix(definitions->Add());
        populatePricingConfig(definitions->Add());
        populatePricingMatrix(definitions->Add(), pricingMatrix);
        populateRiskMatrix(definitions->Add(), riskMatrix);
        populateHedgeRatios(definitions->Add(), hedgeRatios);
        populateQuoteSizes(definitions->Add(), quoteSizes);
        populateCommissions(definitions->Add(), commissions);
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
        addFillSidesEnum(enumDefinitions->Add());
        addStrategiesEnum(enumDefinitions->Add());
        addFillTypeEnum(enumDefinitions->Add());
        addFillSourceEnum(enumDefinitions->Add());
        addSwitchesEnum(enumDefinitions->Add());
        addErrorTypesEnum(enumDefinitions->Add());
        addYesNoEnum(enumDefinitions->Add());
        addLeanType(enumDefinitions->Add());
        addManualFillTypes(enumDefinitions->Add());

        addChartDefinitions(message);
    }

    void addChartDefinitions(proto::GUIInitialization& message) {
        auto chartDefinitions = message.mutable_charts();
        for (uint32_t index = 0; index < _config->_vectorConfigs.size(); ++index) {
            auto& config = _config->_vectorConfigs[index];
            addChart(config._vectorName, config._chartNumberOfDecimalPlaces, index, chartDefinitions->Add());
        }
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
    }

    void addStrategiesEnum(proto::EnumDefinition* definition) {
        definition->set_name("strategies");
        auto members = definition->mutable_members();
        addEnumMember(members, "Relative Value", toUInt(StrategyId::RelativeValue));
        addEnumMember(members, "Market Maker", toUInt(StrategyId::MarketMaker));
    }

    void addManualFillTypes(proto::EnumDefinition* definition) {
        definition->set_name("manual_fill_types");
        auto members = definition->mutable_members();
        addEnumMember(members, "Relative Value", toUInt(ManualFillType::RelativeValue));
        addEnumMember(members, "Market Maker", toUInt(ManualFillType::MarketMaker));
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
        addColumnHeaderCell(columnHeader, MessageCountsColumns::CancelRejects, "CancelRejects", true, proto::DataType::unsigned_type);
        addColumnHeaderCell(columnHeader, MessageCountsColumns::ModifyRejects, "ModifyRejects", true, proto::DataType::unsigned_type);
        addColumnHeaderCell(columnHeader, MessageCountsColumns::Fills, "Fills", true, proto::DataType::unsigned_type);
        addColumnHeaderCell(columnHeader, MessageCountsColumns::FillsRatio, "FillsRatio", true, proto::DataType::double_type);

        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t contractIndex = 0; contractIndex < _config->_marketMakerConfigs.size(); ++contractIndex) {
            MarketMakerConfig& config = _config->_marketMakerConfigs.at(contractIndex);
            uint32_t internalId = getInternalId(config._contract);
            addRowHeaderCell(rowHeader,internalId, config._contract);
        }
        addRowHeaderCell(rowHeader,1000, "Total");

        auto cells = definition->mutable_cells();

        for (uint32_t contractIndex = 0; contractIndex < _config->_marketMakerConfigs.size(); ++contractIndex) {
            MarketMakerConfig& config = _config->_marketMakerConfigs.at(contractIndex);
            uint32_t internalId = getInternalId(config._contract);
            setCell(cells, internalId, MessageCountsColumns::Inserts, "0");
            setCell(cells, internalId, MessageCountsColumns::Modifies, "0");
            setCell(cells, internalId, MessageCountsColumns::Cancels, "0");
            setCell(cells, internalId, MessageCountsColumns::CancelRejects, "0");
            setCell(cells, internalId, MessageCountsColumns::ModifyRejects, "0");
            setCell(cells, internalId, MessageCountsColumns::Fills, "0");
            setCell(cells, internalId, MessageCountsColumns::FillsRatio, "0");
        }
        setCell(cells, 1000, MessageCountsColumns::Inserts, "0");
        setCell(cells, 1000, MessageCountsColumns::Modifies, "0");
        setCell(cells, 1000, MessageCountsColumns::Cancels, "0");
        setCell(cells, 1000, MessageCountsColumns::CancelRejects, "0");
        setCell(cells, 1000, MessageCountsColumns::ModifyRejects, "0");
        setCell(cells, 1000, MessageCountsColumns::Fills, "0");
        setCell(cells, 1000, MessageCountsColumns::FillsRatio, "0");
    }

    void populatePnlDetail(proto::GridDefinition* definition) {
        definition->set_gridid(toUInt(WindowIds::PnlDetail));
        definition->set_name("PNL Detail");
        definition->set_type(proto::GridType::Table);

        auto columnHeader = definition->mutable_columnheader();

        addColumnHeaderCell(columnHeader, PnlDetailColumns::MMOpen, "MM Open", true, proto::DataType::rhone_price);
        addColumnHeaderCell(columnHeader, PnlDetailColumns::MMClosed, "MM Closed", true, proto::DataType::rhone_price);
        addColumnHeaderCell(columnHeader, PnlDetailColumns::MMTotal, "MM Total", true, proto::DataType::rhone_price);
        addColumnHeaderCell(columnHeader, PnlDetailColumns::RVOpen, "RV Open", true, proto::DataType::rhone_price);
        addColumnHeaderCell(columnHeader, PnlDetailColumns::RVClosed, "RV Closed", true, proto::DataType::rhone_price);
        addColumnHeaderCell(columnHeader, PnlDetailColumns::RVTotal, "RV Total", true, proto::DataType::rhone_price);
        addColumnHeaderCell(columnHeader, PnlDetailColumns::Total, "Total", true, proto::DataType::rhone_price);

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
            setCell(cells, internalId, PnlDetailColumns::MMTotal, "0");
            setCell(cells, internalId, PnlDetailColumns::RVTotal, "0");
            setCell(cells, internalId, PnlDetailColumns::Total, "0");
        }
    }

    void populateGlobalConfigs(proto::GridDefinition* definition) {
        definition->set_gridid(toUInt(WindowIds::GlobalConfigs));
        definition->set_name("Global Configs");
        definition->set_type(proto::GridType::Table);

        auto columnHeader = definition->mutable_columnheader();
        addColumnHeaderCell(columnHeader, GlobalConfigsColumns::Key, "Key", true, proto::DataType::string_type);
        addColumnHeaderCell(columnHeader, GlobalConfigsColumns::Value, "Value", false, proto::DataType::string_type);

        auto rowHeader = definition->mutable_rowheader();
        auto cells = definition->mutable_cells();

        for (uint32_t i = 0; i < _config->_globalConfigs.size(); ++i) {
            auto& config = _config->_globalConfigs[i];
            addRowHeaderCell(rowHeader, i, "");
            setCell(cells, i, GlobalConfigsColumns::Value, config._value);
            setCell(cells, i, GlobalConfigsColumns::Key, config._key);
        }

        auto buttons = definition->mutable_buttons();
        addButton(buttons, toUInt(0), "Submit", proto::ButtonFrontBehavior_CommitValues, proto::ButtonBackBehavior_Send);
    }

    void populatePNL(proto::GridDefinition* definition) {
        definition->set_gridid(toUInt(WindowIds::PNL));
        definition->set_name("PNL");
        definition->set_type(proto::GridType::Table);

        auto columnHeader = definition->mutable_columnheader();

        addColumnHeaderCell(columnHeader, PNLColumns::Open, "Open", true, proto::DataType::rhone_price);
        addColumnHeaderCell(columnHeader, PNLColumns::Closed, "Closed", true, proto::DataType::rhone_price);
        addColumnHeaderCell(columnHeader, PNLColumns::Total, "Total", true, proto::DataType::rhone_price);
        addColumnHeaderCell(columnHeader, PNLColumns::Drawdown, "Drawdown", true, proto::DataType::rhone_price);
        addColumnHeaderCell(columnHeader, PNLColumns::PnlLimit, "PnlLimit", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(columnHeader, PNLColumns::DrawdownLimit, "DrawdownLimit", false, proto::DataType::unsigned_type);

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
            setCell(cells, i, PNLColumns::PnlLimit, std::to_string(pnlLimit));
            setCell(cells, i, PNLColumns::DrawdownLimit, std::to_string(drawdownLimit));
        }

        auto buttons = definition->mutable_buttons();
        addButton(buttons, toUInt(0), "Submit", proto::ButtonFrontBehavior_CommitValues, proto::ButtonBackBehavior_Send);
    }

    void populateStackerParameters(proto::GridDefinition* definition) {
        definition->set_gridid(toUInt(WindowIds::StackerParameters));
        definition->set_name("Stacker Parameters");
        definition->set_type(proto::GridType::Table);

        auto columnHeader = definition->mutable_columnheader();

        addColumnHeaderCell(columnHeader, StackerParametersColumns::QuoteSize, "QuoteSize", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(columnHeader, StackerParametersColumns::Enabled, "Enabled", false, proto::DataType::bool_type);
        addColumnHeaderCell(columnHeader, StackerParametersColumns::StackLevels, "StackLevels", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(columnHeader, StackerParametersColumns::StackSlack, "StackSlack", false, proto::DataType::unsigned_type);

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
            setCell(cells, internalId, StackerParametersColumns::StackSlack, std::to_string(config._stackSlack));
        }

        auto buttons = definition->mutable_buttons();
        addButton(buttons, toUInt(0), "Submit", proto::ButtonFrontBehavior_CommitValues, proto::ButtonBackBehavior_Send);
    }

    void populateMarketMakerParameters(proto::GridDefinition* definition) {
        definition->set_gridid(toUInt(WindowIds::MarketMakerParameters));
        definition->set_name("Market Maker Parameters");
        definition->set_type(proto::GridType::Table);

        auto columnHeader = definition->mutable_columnheader();

        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::QuoteExternal, "QuoteExternal", false, proto::DataType::bool_type);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::QuoteSize, "QuoteSize", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::Width, "WidthPercentage", false, proto::DataType::double_type);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::MinInternalPriceChange, "MinInternalPriceChange", false, proto::DataType::double_type);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::EstablishedLevel, "EstablishedLevel", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::JoinSize, "JoinSize", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::LeaveSize, "LeaveSize", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::PayUpQty, "PayUpQty", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::PayUpOrders, "PayUpOrders", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::PayUpMultiple, "PayUpMultiple", false, proto::DataType::double_type);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::MaxPosInterrnal, "MaxPosInternal", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::MaxPosExternal, "MaxPosExternal", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::Commission, "Commission", false, proto::DataType::double_type);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::Enabled, "Enabled", false, proto::DataType::bool_type);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::InternalPosition, "InternalPosition", true, proto::DataType::signed_type);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::ExternalPosition, "ExternalPosition", true, proto::DataType::signed_type);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::BidInternalPrice, "BidIntPrice", true, proto::DataType::rhone_price);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::AskInternalPrice, "AskIntPrice", true, proto::DataType::rhone_price);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::BidInternalQty, "BidIntQty", true, proto::DataType::unsigned_type);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::AskInternalQty, "AskIntQty", true, proto::DataType::unsigned_type);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::BidExternalPrice, "BidExtPrice", true, proto::DataType::rhone_price);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::AskExternalPrice, "AskExtPrice", true, proto::DataType::rhone_price);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::BidExternalQty, "BidExtQty", true, proto::DataType::unsigned_type);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::AskExternalQty, "AskExtQty", true, proto::DataType::unsigned_type);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::MidPoint, "MidPoint", true, proto::DataType::rhone_price);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::BidHedgeInternalPrice, "BidHedgeIntPrice", true, proto::DataType::rhone_price);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::BidHedgeExternalPrice, "BidHedgeExtPrice", true, proto::DataType::rhone_price);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::BidHedgeSize, "BidHedgeSize", true, proto::DataType::unsigned_type);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::AskHedgeInternalPrice, "AskHedgeIntPrice", true, proto::DataType::rhone_price);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::AskHedgeExternalPrice, "AskHedgeExtPrice", true, proto::DataType::rhone_price);
        addColumnHeaderCell(columnHeader, MarketMakerParametersColumns::AskHedgeSize, "AskHedgeSize", true, proto::DataType::unsigned_type);

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
            setCell(cells, internalId, MarketMakerParametersColumns::Width, std::to_string(config._widthPercentage));
            setCell(cells, internalId, MarketMakerParametersColumns::MinInternalPriceChange, std::to_string(config._minInternalPriceChange));
            setCell(cells, internalId, MarketMakerParametersColumns::EstablishedLevel, std::to_string(config._establishedLevel));
            setCell(cells, internalId, MarketMakerParametersColumns::JoinSize, std::to_string(config._joinSize));
            setCell(cells, internalId, MarketMakerParametersColumns::LeaveSize, std::to_string(config._leaveSize));
            setCell(cells, internalId, MarketMakerParametersColumns::PayUpQty, std::to_string(config._payUpQty));
            setCell(cells, internalId, MarketMakerParametersColumns::PayUpOrders, std::to_string(config._payUpOrders));
            setCell(cells, internalId, MarketMakerParametersColumns::PayUpMultiple, std::to_string(config._payUpMultiple));
            setCell(cells, internalId, MarketMakerParametersColumns::MaxPosInterrnal, std::to_string(config._maxPosInternal));
            setCell(cells, internalId, MarketMakerParametersColumns::MaxPosExternal, std::to_string(config._maxPosExternal));
            setCell(cells, internalId, MarketMakerParametersColumns::Commission, std::to_string(config._commission));
            setCell(cells, internalId, MarketMakerParametersColumns::InternalPosition, "0");
            setCell(cells, internalId, MarketMakerParametersColumns::ExternalPosition, "0");
            setCell(cells, internalId, MarketMakerParametersColumns::BidInternalPrice, "0");
            setCell(cells, internalId, MarketMakerParametersColumns::AskInternalPrice, "0");
            setCell(cells, internalId, MarketMakerParametersColumns::BidInternalQty, "0");
            setCell(cells, internalId, MarketMakerParametersColumns::AskInternalQty, "0");
            setCell(cells, internalId, MarketMakerParametersColumns::BidExternalPrice, "0");
            setCell(cells, internalId, MarketMakerParametersColumns::AskExternalPrice, "0");
            setCell(cells, internalId, MarketMakerParametersColumns::BidExternalQty, "0");
            setCell(cells, internalId, MarketMakerParametersColumns::AskExternalQty, "0");
            setCell(cells, internalId, MarketMakerParametersColumns::MidPoint, "0");
            setCell(cells, internalId, MarketMakerParametersColumns::BidHedgeExternalPrice, "0");
            setCell(cells, internalId, MarketMakerParametersColumns::BidHedgeInternalPrice, "0");
            setCell(cells, internalId, MarketMakerParametersColumns::BidHedgeSize, "0");
            setCell(cells, internalId, MarketMakerParametersColumns::AskHedgeExternalPrice, "0");
            setCell(cells, internalId, MarketMakerParametersColumns::AskHedgeInternalPrice, "0");
            setCell(cells, internalId, MarketMakerParametersColumns::AskHedgeSize, "0");
        }

        auto buttons = definition->mutable_buttons();
        addButton(buttons, toUInt(0), "Submit", proto::ButtonFrontBehavior_CommitValues, proto::ButtonBackBehavior_Send);
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

        addColumnHeaderCell(columnHeader, FillsColumns::Number, "", true, proto::DataType::string_type);
        addColumnHeaderCell(columnHeader, FillsColumns::Time, "Time", true, proto::DataType::string_type);
        addColumnHeaderCell(columnHeader, FillsColumns::Contract, "Contract", true, proto::DataType::enum_type, "contracts");
        addColumnHeaderCell(columnHeader, FillsColumns::Side, "Side", true, proto::DataType::enum_type, "fillSides");
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
        addColumnHeaderCell(columnHeader, ManualFillsEntryColumns::Type, "Type", false, proto::DataType::enum_type, "manual_fill_types");

        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t index = 0; index < 5; ++index) {
            std::string name;
            name.append(std::to_string(index)).append(". ");
            addRowHeaderCell(rowHeader, index, name);
        }

        auto cells = definition->mutable_cells();

        for (uint32_t index = 0; index < 5; ++index) {
            setCell(cells, index, ManualFillsEntryColumns::Contract, NO_VALUE_ID);
            setCell(cells, index, ManualFillsEntryColumns::Size, "0");
            setCell(cells, index, ManualFillsEntryColumns::Type, NO_VALUE_ID);
        }

        auto buttons = definition->mutable_buttons();
        addButton(buttons, toUInt(0), "Submit", proto::ButtonFrontBehavior_Clear, proto::ButtonBackBehavior_Send);
    }

    void populateCommissions(proto::GridDefinition* definition, SimpleMatrix<hf::price_t>& commissions) {
        definition->set_gridid(toUInt(WindowIds::Commissions));
        definition->set_name("Commissions");
        definition->set_type(proto::GridType::Matrix);

        auto columnHeader = definition->mutable_columnheader();
        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t index = _config->_indexOfFirstTradedContract; index < _config->_allContracts.size(); ++index) {
            ConfigContract& contract = _config->_allContracts[index];
            addColumnHeaderCell(columnHeader, index, contract._name, true, proto::DataType::double_type);
        }

        for (uint32_t index = _config->_indexOfFirstTradedContract; index < _config->_allContracts.size(); ++index) {
            ConfigContract& contract = _config->_allContracts[index];
            addRowHeaderCell(rowHeader, index, contract._name);
        }

        auto cells = definition->mutable_cells();

        for (uint32_t row = _config->_indexOfFirstTradedContract; row < _config->_allContracts.size(); ++row) {
            for (uint32_t col = _config->_indexOfFirstTradedContract; col < _config->_allContracts.size(); ++col) {
                setCell(cells, row, col, std::to_string(ToDollars(commissions.getValue(row, col))));
            }
        }
    }

    void populateQuoteSizes(proto::GridDefinition* definition, Matrix& quoteSizes) {
        definition->set_gridid(toUInt(WindowIds::QuoteSizes));
        definition->set_name("Quote sizes");
        definition->set_type(proto::GridType::Matrix);

        auto columnHeader = definition->mutable_columnheader();
        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t index = _config->_indexOfFirstTradedContract; index < _config->_allContracts.size(); ++index) {
            ConfigContract& contract = _config->_allContracts[index];
            addColumnHeaderCell(columnHeader, index, contract._name, true, proto::DataType::double_type);
        }

        for (uint32_t index = _config->_indexOfFirstTradedContract; index < _config->_allContracts.size(); ++index) {
            ConfigContract& contract = _config->_allContracts[index];
            addRowHeaderCell(rowHeader, index, contract._name);
        }

        auto cells = definition->mutable_cells();

        for (uint32_t row = _config->_indexOfFirstTradedContract; row < _config->_allContracts.size(); ++row) {
            for (uint32_t col = _config->_indexOfFirstTradedContract; col < _config->_allContracts.size(); ++col) {
                setCell(cells, row, col, std::to_string(quoteSizes.getValue(row, col)));
            }
        }
    }

    void populateHedgeRatios(proto::GridDefinition* definition, SimpleMatrix<double>& hedgeRatios) {
        definition->set_gridid(toUInt(WindowIds::HedgeRatios));
        definition->set_name("Hedge ratios");
        definition->set_type(proto::GridType::Matrix);

        auto columnHeader = definition->mutable_columnheader();
        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t index = _config->_indexOfFirstTradedContract; index < _config->_allContracts.size(); ++index) {
            ConfigContract& contract = _config->_allContracts[index];
            addColumnHeaderCell(columnHeader, index, contract._name, true, proto::DataType::double_type);
        }

        for (uint32_t index = _config->_indexOfFirstTradedContract; index < _config->_allContracts.size(); ++index) {
            ConfigContract& contract = _config->_allContracts[index];
            addRowHeaderCell(rowHeader, index, contract._name);
        }

        auto cells = definition->mutable_cells();

        for (uint32_t row = _config->_indexOfFirstTradedContract; row < _config->_allContracts.size(); ++row) {
            for (uint32_t col = _config->_indexOfFirstTradedContract; col < _config->_allContracts.size(); ++col) {
                setCell(cells, row, col, std::to_string(hedgeRatios.getValue(row, col)));
            }
        }
    }

    void populateRiskMatrix(proto::GridDefinition* definition, Matrix& riskMatrix) {
        definition->set_gridid(toUInt(WindowIds::RiskMatrix));
        definition->set_name("Risk Matrix");
        definition->set_type(proto::GridType::Matrix);

        auto columnHeader = definition->mutable_columnheader();
        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t index = 0; index < _config->_pricingOutrights.size(); ++index) {
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

        for (uint32_t index = 0; index < _config->_pricingOutrights.size(); ++index) {
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
        addColumnHeaderCell(columnHeader, PricingConfigsColumns::PricingMultiplier, "PricingMultiplier", true, proto::DataType::double_type);
        addColumnHeaderCell(columnHeader, PricingConfigsColumns::QuantityMultiplier, "QuantityMultiplier", true, proto::DataType::double_type);

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
            setCell(cells, contractIndex, PricingConfigsColumns::PricingMultiplier, std::to_string(config._pricingMultiplier));
            setCell(cells, contractIndex, PricingConfigsColumns::QuantityMultiplier, std::to_string(config._quantityMultiplier));
        }

        auto buttons = definition->mutable_buttons();
        addButton(buttons, toUInt(0), "Submit", proto::ButtonFrontBehavior_CommitValues, proto::ButtonBackBehavior_Send);
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

        auto buttons = definition->mutable_buttons();
        addButton(buttons, toUInt(0), "Submit", proto::ButtonFrontBehavior_CommitValues, proto::ButtonBackBehavior_Send);
    }

    void populateContractParameterRVColumns(proto::GridDefinition* definition) {
        definition->set_gridid(toUInt(WindowIds::ContractParametersRv));
        definition->set_name("Relative Value Parameters");
        definition->set_type(proto::GridType::Table);

        auto columnHeader = definition->mutable_columnheader();

        addColumnHeaderCell(columnHeader, ContractParameterRVColumns::Position, "Position", true, proto::DataType::signed_type);
        addColumnHeaderCell(columnHeader, ContractParameterRVColumns::BidLean, "BidLean", true, proto::DataType::enum_type, "contracts");
        addColumnHeaderCell(columnHeader, ContractParameterRVColumns::AskLean, "AskLean", true, proto::DataType::enum_type, "contracts");
        addColumnHeaderCell(columnHeader, ContractParameterRVColumns::BidLeanType, "BidLeanType", true, proto::DataType::enum_type, "LeanType");
        addColumnHeaderCell(columnHeader, ContractParameterRVColumns::AskLeanType, "AskLeanType", true, proto::DataType::enum_type, "LeanType");
        addColumnHeaderCell(columnHeader, ContractParameterRVColumns::BidInternalPrice, "BidIntPrice", true, proto::DataType::rhone_price);
        addColumnHeaderCell(columnHeader, ContractParameterRVColumns::AskInternalPrice, "AskIntPrice", true, proto::DataType::rhone_price);
        addColumnHeaderCell(columnHeader, ContractParameterRVColumns::BidExternalPrice, "BidExtPrice", true, proto::DataType::rhone_price);
        addColumnHeaderCell(columnHeader, ContractParameterRVColumns::AskExternalPrice, "AskExtPrice", true, proto::DataType::rhone_price);
        addColumnHeaderCell(columnHeader, ContractParameterRVColumns::BidCross, "BidCross", true, proto::DataType::enum_type, "YesNo");
        addColumnHeaderCell(columnHeader, ContractParameterRVColumns::AskCross, "AskCross", true, proto::DataType::enum_type, "YesNo");
        addColumnHeaderCell(columnHeader, ContractParameterRVColumns::BidInternalQty, "BidIntQty", true, proto::DataType::unsigned_type);
        addColumnHeaderCell(columnHeader, ContractParameterRVColumns::AskInternalQty, "AskIntQty", true, proto::DataType::unsigned_type);
        addColumnHeaderCell(columnHeader, ContractParameterRVColumns::BidExternalQty, "BidExtQty", true, proto::DataType::unsigned_type);
        addColumnHeaderCell(columnHeader, ContractParameterRVColumns::AskExternalQty, "AskExtQty", true, proto::DataType::unsigned_type);
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
            setCell(cells, internalId, ContractParameterRVColumns::BidLeanType, std::to_string(toUInt(LeanType::None)));
            setCell(cells, internalId, ContractParameterRVColumns::AskLeanType, std::to_string(toUInt(LeanType::None)));
            setCell(cells, internalId, ContractParameterRVColumns::BidInternalPrice, "0");
            setCell(cells, internalId, ContractParameterRVColumns::BidExternalPrice, "0");
            setCell(cells, internalId, ContractParameterRVColumns::AskInternalPrice, "0");
            setCell(cells, internalId, ContractParameterRVColumns::AskExternalPrice, "0");
            setCell(cells, internalId, ContractParameterRVColumns::BidInternalQty, "0");
            setCell(cells, internalId, ContractParameterRVColumns::BidExternalQty, "0");
            setCell(cells, internalId, ContractParameterRVColumns::AskInternalQty, "0");
            setCell(cells, internalId, ContractParameterRVColumns::AskExternalQty, "0");
            setCell(cells, internalId, ContractParameterRVColumns::STF, "0");
            setCell(cells, internalId, ContractParameterRVColumns::Mkt, "0");
            setCell(cells, internalId, ContractParameterRVColumns::QuoteExternal, std::to_string(config._quoteExternal));
            setCell(cells, internalId, ContractParameterRVColumns::HedgeTolerance, std::to_string(config._hedgeTolerance));
            setCell(cells, internalId, ContractParameterRVColumns::Width, std::to_string(config._width));
            setCell(cells, internalId, ContractParameterRVColumns::Commission, std::to_string(config._commission));
            setCell(cells, internalId, ContractParameterRVColumns::BidCross, std::to_string(toUInt(YesNoType::Unknown)));
            setCell(cells, internalId, ContractParameterRVColumns::AskCross, std::to_string(toUInt(YesNoType::Unknown)));
        }

        auto buttons = definition->mutable_buttons();
        addButton(buttons, toUInt(0), "Submit", proto::ButtonFrontBehavior_CommitValues, proto::ButtonBackBehavior_Send);
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
        definition->set_name("Factor Parameters");
        definition->set_type(proto::GridType::Table);

        auto columnHeader = definition->mutable_columnheader();

        addColumnHeaderCell(columnHeader, VectorParametersColumns::Position, "Position", true, proto::DataType::double_type);
        addColumnHeaderCell(columnHeader, VectorParametersColumns::Mkt, "Mkt", true, proto::DataType::double_type);
        addColumnHeaderCell(columnHeader, VectorParametersColumns::STF, "STF", true, proto::DataType::double_type);
        addColumnHeaderCell(columnHeader, VectorParametersColumns::LTF, "LTF", false, proto::DataType::double_type);
        addColumnHeaderCell(columnHeader, VectorParametersColumns::MaxPos, "MaxPos", false, proto::DataType::signed_type);
        addColumnHeaderCell(columnHeader, VectorParametersColumns::Range, "Range", false, proto::DataType::double_type);
        addColumnHeaderCell(columnHeader, VectorParametersColumns::EMA_ON, "EmaOn", false, proto::DataType::bool_type);
        addColumnHeaderCell(columnHeader, VectorParametersColumns::EMA_Time, "EmaSeconds", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(columnHeader, VectorParametersColumns::SetToMarket, "SetToMarket", false, proto::DataType::bool_type);
        addColumnHeaderCell(columnHeader, VectorParametersColumns::Hedged, "Hedged", false, proto::DataType::bool_type);

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
            setCell(cells, vectorIndex, VectorParametersColumns::EMA_ON, std::to_string(config._useEMA));
            setCell(cells, vectorIndex, VectorParametersColumns::EMA_Time, std::to_string(config._EMALength));
            setCell(cells, vectorIndex, VectorParametersColumns::SetToMarket, std::to_string(config._setToMarket));
            setCell(cells, vectorIndex, VectorParametersColumns::Hedged, std::to_string(config._hedged));
        }

        auto buttons = definition->mutable_buttons();
        addButton(buttons, toUInt(0), "Submit", proto::ButtonFrontBehavior_CommitValues, proto::ButtonBackBehavior_Send);
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
    uint32_t _fillNumber = 1;
    uint32_t _errorMessageNumber = 0;
};

};

#endif //VIPER_VIPER_CONFIG_H
