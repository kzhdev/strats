#ifndef VIPER_GUI_THINGS_H
#define VIPER_GUI_THINGS_H

#include <hf/util/TimeUtils.h>
#include <ven/VenueUtils.h>
#include "messages/python.pb.h"
#include "ViperConfig.h"
#include "RelativeValue.h"
#include "TimeUtils.h"
#include "NegativeScalpChecker.h"
#include "Common.h"
#include "MarketMaker.h"
#include "SimpleMarketMaker.h"

namespace Python {

class GUIThings {
    enum class RequiresConfirmation : bool { NO = false, YES = true };

public:
    static GUIThings& get() {
        static GUIThings instance;
        return instance;
    }

    static constexpr const char* NO_VALUE_ID = "-1";

    void init(ViperConfig* config) {
        _config = config;
    }

    void populateQuoteSizes(ViperConfig& config, proto::GridUpdate& message, Matrix& quoteSizes) {
        message.set_gridid(toUInt(WindowIds::QuoteSizes));
        message.set_updatetype(proto::GridUpdateType::CellsType);
        auto cells = message.mutable_cells();
        for (uint32_t row = config._indexOfFirstTradedContract; row < config._allContracts.size(); ++row) {
            for (uint32_t col = config._indexOfFirstTradedContract; col < config._allContracts.size(); ++col) {
                setCell(cells, row, col, std::to_string(quoteSizes.getValue(row, col)));
            }
        }
    }

    void updateHedgeRatios(proto::GridUpdate& message, SimpleMatrix<double>& hedgeRatios) {
        message.set_gridid(toUInt(WindowIds::HedgeRatios));
        message.set_updatetype(proto::GridUpdateType::CellsType);
        auto cells = message.mutable_cells();
        for (uint32_t row = _config->_indexOfFirstTradedContract; row < _config->_allContracts.size(); ++row) {
            for (uint32_t col = _config->_indexOfFirstTradedContract; col < _config->_allContracts.size(); ++col) {
                setCell(cells, row, col, std::to_string(hedgeRatios.getValue(row, col)));
            }
        }
    }

    void updateFactorHedged(proto::GridUpdate& message, SimpleMatrix<int>& factorHedged) {
        message.set_gridid(toUInt(WindowIds::FactorHedged));
        message.set_updatetype(proto::GridUpdateType::CellsType);
        auto cells = message.mutable_cells();
        for (uint32_t row = _config->_indexOfFirstTradedContract; row < _config->_allContracts.size(); ++row) {
            for (uint32_t col = _config->_indexOfFirstTradedContract; col < _config->_allContracts.size(); ++col) {
                setCell(cells, row, col, std::to_string(factorHedged.getValue(row, col)));
            }
        }
    }

    void updateExtraContractWidth(proto::GridUpdate& message, RelativeValue* relativeValue) {
        message.set_gridid(toUInt(WindowIds::ContractParametersRv));
        message.set_updatetype(proto::GridUpdateType::CellsType);
        auto cells = message.mutable_cells();
        for (uint32_t contractIndex = 0; contractIndex < relativeValue->_numberOfAllContracts; ++contractIndex) {
            RvContract& contract = relativeValue->_contracts[contractIndex];
            setCell(cells, contract._internalId, ContractParameterRVColumns::ExtraWidth, std::to_string(ToDollars(relativeValue->_extraContractWidths.getValue(contract._internalId))));
        }
    }

    void updatePctStfMove(proto::GridUpdate& message, RelativeValue* relativeValue) {
        message.set_gridid(toUInt(WindowIds::ContractParametersRv));
        message.set_updatetype(proto::GridUpdateType::CellsType);
        auto cells = message.mutable_cells();
        for (uint32_t contractIndex = 0; contractIndex < relativeValue->_numberOfAllContracts; ++contractIndex) {
            RvContract& contract = relativeValue->_contracts[contractIndex];
            if(contract._traded) {
                setCell(cells, contract._internalId, ContractParameterRVColumns::PCTStfMove, std::to_string(relativeValue->_pctStfMoves.getValue(contractIndex)));
            }
        }
    }

    void updateMaxTicksThroughMarket(proto::GridUpdate& message, RelativeValue* relativeValue) {
        message.set_gridid(toUInt(WindowIds::ContractParametersRv));
        message.set_updatetype(proto::GridUpdateType::CellsType);
        auto cells = message.mutable_cells();
        for (uint32_t contractIndex = 0; contractIndex < relativeValue->_numberOfAllContracts; ++contractIndex) {
            RvContract& contract = relativeValue->_contracts[contractIndex];
                setCell(cells, contract._internalId, ContractParameterRVColumns::MaxTicksThroughMarket, std::to_string(relativeValue->_maxTicksThroughMarket.getValue(contractIndex)));
        }
    }

    struct Position {
        hf::qty_t _rv = 0;
        hf::qty_t _mmInternal = 0;
        hf::qty_t _mmExternal = 0;
        hf::qty_t _total = 0;
    };

    Position& getPosition(uint32_t index, std::map<uint32_t, Position>& positions) {
        auto it = positions.find(index);
        if (it == positions.end()) {
            return positions.insert({index, Position()}).first->second;
        }
        return it->second;
    }

    void getPositions(std::map<uint32_t, Position>& positions) {
        uint32_t adj = _config->_indexOfFirstTradedContract;

        for(uint32_t row = 0; row < _config->_rvPosition.size(); ++row) {
            hf::qty_t rv = _config->_rvPosition[row];
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

        for (uint32_t row = _config->_indexOfFirstTradedContract; row < _config->_allContracts.size(); ++row) {
            hf::qty_t mmInt = _config->_marketMakerConfigs[row - adj]._internalPosition;
            hf::qty_t mmExt = _config->_marketMakerConfigs[row - adj]._externalPosition;
            ConfigContract& contract = _config->_allContracts[row];
            if (contract._legs.size() > 1) {
                for (uint32_t i = 0; i < contract._legs.size(); ++i) {
                    auto& leg = contract._legs[i];
                    Position& position = getPosition(leg._index, positions);
                    position._mmInternal += mmInt * leg._ratio;
                    position._mmExternal += mmExt * leg._ratio;
                    position._total = position._rv + position._mmInternal + position._mmExternal;
                }
            } else {
                Position& position = getPosition(row, positions);
                position._mmInternal += mmInt;
                position._mmExternal += mmExt;
                position._total = position._rv + position._mmInternal + position._mmExternal;
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
            setCell(cells, pair.first, PositionsColumns ::RV, std::to_string(position._rv));
            setCell(cells, pair.first, PositionsColumns ::MMInternal, std::to_string(position._mmInternal));
            setCell(cells, pair.first, PositionsColumns ::MMExternal, std::to_string(position._mmExternal));
            setCell(cells, pair.first, PositionsColumns ::Total, std::to_string(position._total));
        }
    }

    template <typename T>
    void populateMessageCountsUpdate(proto::GridUpdate& message, T* executor) {
        message.set_gridid(toUInt(WindowIds::MessagesCounts));
        message.set_updatetype(proto::GridUpdateType::CellsType);
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
                            FALLTHROUGH
                        case hf::VENUE::ICE: {
                            fillsRatio = static_cast<double>(state._ime._messages._inserts + state._ime._messages._modifies) / state._ime._messages._fills;
                            break;
                        }
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
        setCell(cells, 1000, MessageCountsColumns::Inserts, std::to_string(inserts));
        setCell(cells, 1000, MessageCountsColumns::Modifies, std::to_string(modifies));
        setCell(cells, 1000, MessageCountsColumns::Cancels, std::to_string(cancels));
        setCell(cells, 1000, MessageCountsColumns::CancelRejects, std::to_string(cancelRejects));
        setCell(cells, 1000, MessageCountsColumns::ModifyRejects, std::to_string(modifyRejects));
        setCell(cells, 1000, MessageCountsColumns::Fills, std::to_string(fills));
        double fillsRatio = 0;
        if (fills != 0) {
            switch (venue) {
                //todo_eurex
                case hf::VENUE::EUREX: FALLTHROUGH;
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

    void populateMarketMakerState(proto::GridUpdate& message, const MarketMaker* marketMaker) {
        message.set_gridid(toUInt(WindowIds::MarketMakerParameters));
        message.set_updatetype(proto::GridUpdateType::CellsType);
        auto cells = message.mutable_cells();
        for (uint32_t contractIndex = 0; contractIndex < _config->_allContracts.size(); ++contractIndex) {
            const auto& mm = marketMaker->_marketMakers[contractIndex];
            if (mm._contract._toMarketMake) {
                setCell(cells, contractIndex, MarketMakerParametersColumns::MidPoint, std::to_string(mm._midPoint));
                setCell(cells, contractIndex, MarketMakerParametersColumns::InternalPosition, std::to_string(mm._internallyInitiatedPosition));
                setCell(cells, contractIndex, MarketMakerParametersColumns::ExternalPosition, std::to_string(mm._externallyInitiatedPosition));
                auto& bidQuote = mm._quotes[BID];
                setCell(cells, contractIndex, MarketMakerParametersColumns::BidInternalPrice, std::to_string(bidQuote._internalPrice));
                setCell(cells, contractIndex, MarketMakerParametersColumns::BidInternalQty, std::to_string(bidQuote._internalSize + bidQuote._externalSize));
                setCell(cells, contractIndex, MarketMakerParametersColumns::BidExternalPrice, std::to_string(bidQuote._externalPrice));
                setCell(cells, contractIndex, MarketMakerParametersColumns::BidExternalQty, std::to_string(bidQuote._externalSize));
                auto& askQuote = mm._quotes[ASK];
                setCell(cells, contractIndex, MarketMakerParametersColumns::AskInternalPrice, std::to_string(askQuote._internalPrice));
                setCell(cells, contractIndex, MarketMakerParametersColumns::AskInternalQty, std::to_string(askQuote._internalSize + askQuote._externalSize));
                setCell(cells, contractIndex, MarketMakerParametersColumns::AskExternalPrice, std::to_string(askQuote._externalPrice));
                setCell(cells, contractIndex, MarketMakerParametersColumns::AskExternalQty, std::to_string(askQuote._externalSize));
                auto& bidHedge = mm._hedges[BID];
                setCell(cells, contractIndex, MarketMakerParametersColumns::BidHedgeInternalPrice, std::to_string(bidHedge._internalPrice));
                setCell(cells, contractIndex, MarketMakerParametersColumns::BidHedgeExternalPrice, std::to_string(bidHedge._externalPrice));
                setCell(cells, contractIndex, MarketMakerParametersColumns::BidHedgeSize, std::to_string(bidHedge._sizeToQuote));
                auto& askHedge = mm._hedges[ASK];
                setCell(cells, contractIndex, MarketMakerParametersColumns::AskHedgeInternalPrice, std::to_string(askHedge._internalPrice));
                setCell(cells, contractIndex, MarketMakerParametersColumns::AskHedgeExternalPrice, std::to_string(askHedge._externalPrice));
                setCell(cells, contractIndex, MarketMakerParametersColumns::AskHedgeSize, std::to_string(askHedge._sizeToQuote));
                _config->_marketMakerConfigs[mm._contract._marketMakeIndex]._establishedLevel = mm._emaEstLevel;
                setCell(cells, contractIndex, MarketMakerParametersColumns::EstablishedLevel, std::to_string(_config->_marketMakerConfigs[mm._contract._marketMakeIndex]._establishedLevel));
            }
        }
    }

    PNLRows getPNLRow(StrategyId strategy) {
        switch(strategy) {
            case StrategyId::MarketMaker:
                return PNLRows::MarketMakers;
            case StrategyId::RelativeValue:
                return PNLRows::RelativeValue;
            default:
                return PNLRows::__end_marker__;
        }
    }

    void populatePnlUpdate(proto::GridUpdate& message, StrategyId strategy, const Pnl& pnl, const Pnl& totalPnl) {
        message.set_gridid(toUInt(WindowIds::PNL));
        message.set_updatetype(proto::GridUpdateType::CellsType);
        auto cells = message.mutable_cells();
        auto row = getPNLRow(strategy);
        if (row != PNLRows::__end_marker__) {
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

    void populatePnlDetailUpdate(proto::GridUpdate& message, Pnl& rvPnl, Pnl& mmPnl, StrategyId id, uint32_t contractIndex) {
        message.set_gridid(toUInt(WindowIds::PnlDetail));
        message.set_updatetype(proto::GridUpdateType::CellsType);
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
            case StrategyId::Execution:
            case StrategyId::MAX:
                break;
        }
        setCell(cells, contractIndex, PnlDetailColumns::Total, std::to_string(mmPnl.getTotal() + rvPnl.getTotal()));
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

    void populateAuditMessage(uint32_t gridId, proto::GridUpdate& message, GridUpdate& update, Traders trader) {
        message.set_gridid(toUInt(WindowIds::Audit));
        message.set_updatetype(proto::GridUpdateType::CellsType);
        auto cells = message.mutable_cells();
        for (int i = 0; i < update._update.cells_size(); ++i) {
            proto::GridCell& gridCell = *update._update.mutable_cells(i);
            setCell(cells, _auditMessageNumber, AuditColumns::Time, std::to_string(TimeUtils::clock_gettime()));
            setCell(cells, _auditMessageNumber, AuditColumns::Trader, std::to_string(toUInt(trader)));
            setCell(cells, _auditMessageNumber, AuditColumns::Grid, std::to_string(toUInt(gridId)));
            setCell(cells, _auditMessageNumber, AuditColumns::Row, getRowLabel(gridId, gridCell.row()));
            setCell(cells, _auditMessageNumber, AuditColumns::Column, getColumnLabel(gridId, gridCell.col()));
            setCell(cells, _auditMessageNumber, AuditColumns::OldValue, getOldValue(update, gridCell.row(), gridCell.col()));
            setCell(cells, _auditMessageNumber, AuditColumns::NewValue, getNewValue(gridId, gridCell.col(), gridCell.value()));
            ++_auditMessageNumber;
        }
    }

    void populateManualFilAuditMessage(proto::GridUpdate& message, uint32_t contractId, hf::qty_t size, Traders trader, StrategyId strategy) {
        message.set_gridid(toUInt(WindowIds::Audit));
        message.set_updatetype(proto::GridUpdateType::CellsType);
        auto cells = message.mutable_cells();
        setCell(cells, _auditMessageNumber, AuditColumns::Time, std::to_string(TimeUtils::clock_gettime()));
        setCell(cells, _auditMessageNumber, AuditColumns::Trader, std::to_string(toUInt(trader)));
        setCell(cells, _auditMessageNumber, AuditColumns::Grid, std::to_string(toUInt(WindowIds::ManualFillsEntry)));
        std::string value = createManualFillString(contractId, size, strategy);
        setCell(cells, _auditMessageNumber, AuditColumns::NewValue, value);
        ++_auditMessageNumber;
    }
    
    std::string createManualFillString(uint32_t contractId, hf::qty_t size, StrategyId strategy) {
        std::string fillType = strategy == StrategyId::MarketMaker ? "MM" : "RV";
        auto& contract = _config->_allContracts[contractId];
        std::string value;
        value.append(contract._name).append(": ").append(std::to_string(size)).append(": ").append(fillType);
        return value;
    }

    void populateFillMessage(FillMessage& fill, proto::GridUpdate& message, RelativeValue* relativeValue, MarketMaker* marketMaker) {
        message.set_gridid(toUInt(WindowIds::FillsDisplay));
        message.set_updatetype(proto::GridUpdateType::CellsType);
        auto cells = message.mutable_cells();
        auto& contract = _config->_allContracts[fill._contractId];
        auto color = static_cast<uint32_t>(fill._side == BID ? Color::TRANSPARENT_BID_BLUE : Color::TRANSPARENT_OFFER_RED);
        setCell(cells, _fillNumber, FillsColumns::Number, std::to_string(_fillNumber), color);
        setCell(cells, _fillNumber, FillsColumns::Time, std::to_string(TimeUtils::clock_gettime()), color);
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
            width = ToDollars(relativeValue->_contractWidths.getValue(fill._contractId));
            if (width != 0) {
                edgeStf = ToDollars(fill._side == BID ? fill._rvStf - fill._price : fill._price - fill._rvStf) / width;
            }
        } else {
            auto& mm = marketMaker->_marketMakers[fill._contractId];
            if (mm._contract._toMarketMake) {
                const hf::price_t marketSpread = mm._books[ASK]._price - mm._books[BID]._price;
                width = ToDollars(marketSpread / 2.0 * mm._widthPercentage);
            }
        }
        RvContract& rvContract = relativeValue->_contracts[fill._contractId];
        const bool useWightedMid = relativeValue->_useWeightedMid.getValue(fill._contractId);
        double mkt = -1;
        if ((rvContract._externalMarket[BID].getPrice() != hf::NULL_PRICE) & (rvContract._externalMarket[ASK].getPrice() != hf::NULL_PRICE)) {
            if (useWightedMid) {
                mkt = relativeValue->calculateWeightedMidpoint(rvContract._externalMarket[BID], rvContract._externalMarket[ASK], rvContract._establishedLevel);
            } else {
                mkt = relativeValue->calculateSimpleAverage(rvContract._externalMarket[BID], rvContract._externalMarket[ASK]);
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
    }

    void populateVectorGridUpdate(proto::GridUpdate& message, const RelativeValue* relativeValue) const {
        message.set_gridid(toUInt(WindowIds::VectorParameters));
        message.set_updatetype(proto::GridUpdateType::CellsType);
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

    void populateRelativeValueGridUpdate(proto::GridUpdate& message, const RelativeValue* relativeValue, bool checkForCross) const {
        auto crossedColor = [](YesNoType ynt) { return ynt == YesNoType::Yes ? toUInt(Color::TRANSPARENT_RED) : ynt == YesNoType::No ? toUInt(Color::TRANSPARENT_GREEN) : toUInt(Color::TRANSPARENT); };
        message.set_gridid(toUInt(WindowIds::ContractParametersRv));
        message.set_updatetype(proto::GridUpdateType::CellsType);
        auto cells = message.mutable_cells();
        for (uint32_t contractIndex = 0; contractIndex < relativeValue->_numberOfAllContracts; ++contractIndex) {
            const RvContract& contract = relativeValue->_contracts[contractIndex];
            if (contract._traded) {
                const Quote& bid = contract._quotes[BID];
                if (bid._bestPrice != hf::NULL_PRICE) {
                    setCell(cells, contract._internalId, ContractParameterRVColumns::BidInternalPrice, std::to_string(bid._bestPrice));
                    setCell(cells, contract._internalId, ContractParameterRVColumns::BidInternalQty, std::to_string(bid._bestSize));
                    setCell(cells, contract._internalId, ContractParameterRVColumns::BidExternalPrice, std::to_string(bid._externalPrice));
                    setCell(cells, contract._internalId, ContractParameterRVColumns::BidExternalQty, std::to_string(bid._externalSize));
                    const RvContract& bidBestLean = relativeValue->_contracts[bid._bestLeanIndex];
                    setCell(cells, contract._internalId, ContractParameterRVColumns::BidLean, std::to_string(bidBestLean._definition._internalId));
                    LeanType leanType = LeanType::None;
                    if (bid._leans.getNumberOfLeans() > 0) {
                        const Lean& bidLean = bid._leans._leans[0];
                        if (!bidLean._isExternal) {
                            leanType = bidLean._marketIndex == orderId(StrategyId::MarketMaker, Order::Type::Hedging) ? LeanType::MM_Hedge : LeanType::MM_Quote;
                        } else {
                            leanType = LeanType::External;
                        }
                    }
                    setCell(cells, contract._internalId, ContractParameterRVColumns::BidLeanType, std::to_string(toUInt(leanType)));
                    YesNoType bidCrossed = YesNoType::Unknown;
                    if (checkForCross && bid._externalPrice != hf::NULL_PRICE && contract._externalMarket[ASK].getPrice() != hf::NULL_PRICE) {
                        bidCrossed = bid._externalPrice >= contract._externalMarket[ASK].getPrice() ? YesNoType::Yes : YesNoType::No;
                    }
                    setCell(cells, contract._internalId, ContractParameterRVColumns::BidCross, std::to_string(toUInt(bidCrossed)), crossedColor(bidCrossed));
                }
                else {
                    setCell(cells, contract._internalId, ContractParameterRVColumns::BidLean, NO_VALUE_ID);
                    setCell(cells, contract._internalId, ContractParameterRVColumns::BidLeanType, std::to_string(toUInt(LeanType::None)));
                    setCell(cells, contract._internalId, ContractParameterRVColumns::BidCross, std::to_string(toUInt(YesNoType::Unknown)));
                }
                const Quote& ask = contract._quotes[ASK];
                if (ask._bestPrice != hf::NULL_PRICE) {
                    setCell(cells, contract._internalId, ContractParameterRVColumns::AskInternalPrice, std::to_string(ask._bestPrice));
                    setCell(cells, contract._internalId, ContractParameterRVColumns::AskInternalQty, std::to_string(ask._bestSize));
                    setCell(cells, contract._internalId, ContractParameterRVColumns::AskExternalPrice, std::to_string(ask._externalPrice));
                    setCell(cells, contract._internalId, ContractParameterRVColumns::AskExternalQty, std::to_string(ask._externalSize));
                    const RvContract& askBestLean = relativeValue->_contracts[ask._bestLeanIndex];
                    setCell(cells, contract._internalId, ContractParameterRVColumns::AskLean, std::to_string(askBestLean._definition._internalId));
                    LeanType leanType = LeanType::None;
                    if (ask._leans.getNumberOfLeans() > 0) {
                        const Lean& askLean = ask._leans._leans[0];
                        if (!askLean._isExternal) {
                            leanType = askLean._marketIndex == orderId(StrategyId::MarketMaker, Order::Type::Hedging) ? LeanType::MM_Hedge : LeanType::MM_Quote;
                        } else {
                            leanType = LeanType::External;
                        }
                    }
                    setCell(cells, contract._internalId, ContractParameterRVColumns::AskLeanType, std::to_string(toUInt(leanType)));
                    YesNoType askCrossed = YesNoType::Unknown;
                    if (checkForCross && ask._externalPrice != hf::NULL_PRICE && contract._externalMarket[BID].getPrice() != hf::NULL_PRICE) {
                        askCrossed = ask._externalPrice <= contract._externalMarket[BID].getPrice() ? YesNoType::Yes : YesNoType::No;
                    }
                    setCell(cells, contract._internalId, ContractParameterRVColumns::AskCross, std::to_string(toUInt(askCrossed)), crossedColor(askCrossed));
                }
                else {
                    setCell(cells, contract._internalId, ContractParameterRVColumns::AskLean, NO_VALUE_ID);
                    setCell(cells, contract._internalId, ContractParameterRVColumns::AskLeanType, std::to_string(toUInt(LeanType::None)));
                    setCell(cells, contract._internalId, ContractParameterRVColumns::AskCross, std::to_string(toUInt(YesNoType::Unknown)));
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
            }
        }
    }

    void populateMarketMakerEnabled(proto::GridUpdate& message, uint32_t id, bool enabled) {
        message.set_gridid(toUInt(WindowIds::MarketMakerParameters));
        message.set_updatetype(proto::GridUpdateType::CellsType);
        auto cells = message.mutable_cells();
        setCell(cells, id, MarketMakerParametersColumns::Enabled, std::to_string(enabled));
    }

    template<typename ExecutorT>
    void populateGUIInitialization(proto::GUIInitialization& message, std::vector<ViperStrategyBase*>& strategies, ExecutorT* executor) {
        auto definitions = message.mutable_definitions();
        populateMainControl(definitions->Add(), executor);
        populateErrors(definitions->Add());
        populateTraderSwitch(definitions->Add());
        for (auto strategy : strategies) {
            strategy->populateGUIInitialization(definitions);
        }
        populateLeanMatrix(definitions->Add());
        populatePricingConfig(definitions->Add());
        populateVectorParametersDefinition(definitions->Add());
        populateManualFillsEntry(definitions->Add());
        populateFills(definitions->Add());
        populateGatewayStatuses(definitions->Add());
        populateStackerParameters(definitions->Add());
        populatePNL(definitions->Add());
        populatePnlDetail(definitions->Add());
        populateGlobalConfigs(definitions->Add());
        populateMessageCounts(definitions->Add());
        populatePositionInOutrights(definitions->Add());
        populateAudit(definitions->Add());
        tryPopulateGTCGridColumns(message);
        tryPopulateStatusDebugGridColumns(message);

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

        addChartDefinitions(message);

        addColorDefinitions(message);
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
        addEnumMember(members, "Steven", toUInt(Traders::Steven));
        addEnumMember(members, "Mark", toUInt(Traders::Mark));
        addEnumMember(members, "Taj", toUInt(Traders::Taj));
        addEnumMember(members, "Zhi", toUInt(Traders::Zhi));
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

    void populatePositionInOutrights(proto::GridDefinition* definition) {
        definition->set_gridid(toUInt(WindowIds::PositionInOutrights));
        definition->set_name("Positions");
        definition->set_type(proto::GridType::Table);

        auto columnHeader = definition->mutable_columnheader();

        addColumnHeaderCell(WindowIds::PositionInOutrights, columnHeader, PositionsColumns::RV, "RV", false, proto::DataType::signed_type);
        addColumnHeaderCell(WindowIds::PositionInOutrights, columnHeader, PositionsColumns::MMInternal, "MMInternal", false, proto::DataType::signed_type);
        addColumnHeaderCell(WindowIds::PositionInOutrights, columnHeader, PositionsColumns::MMExternal, "MMExternal", false, proto::DataType::signed_type);
        addColumnHeaderCell(WindowIds::PositionInOutrights, columnHeader, PositionsColumns::Total, "Total", false, proto::DataType::signed_type);

        std::map<uint32_t, Position> positions;
        getPositions(positions);

        auto rowHeader = definition->mutable_rowheader();
        for (auto& pair : positions) {
            ConfigContract& contract = _config->_allContracts[pair.first];
            addRowHeaderCell(WindowIds::PositionInOutrights, rowHeader, pair.first, contract._name);
        }

        auto cells = definition->mutable_cells();
        for (auto& pair : positions) {
            Position& position = pair.second;
            setCell(cells, pair.first, PositionsColumns::RV, std::to_string(position._rv));
            setCell(cells, pair.first, PositionsColumns::MMInternal, std::to_string(position._mmInternal));
            setCell(cells, pair.first, PositionsColumns::MMExternal, std::to_string(position._mmExternal));
            setCell(cells, pair.first, PositionsColumns::Total, std::to_string(position._total));
        }
    }

    void populateAudit(proto::GridDefinition* definition) {
        definition->set_gridid(toUInt(WindowIds::Audit));
        definition->set_name("Audit Trail");
        definition->set_type(proto::GridType::List);
        auto columnHeader = definition->mutable_columnheader();
        addColumnHeaderCell(WindowIds::Audit, columnHeader, AuditColumns::Time, "Time", true, proto::DataType::rhone_time);
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
        setCell(cells, 0, TraderSwitchColumns::Trader, std::to_string(toUInt(Traders::Steven)));
        setCell(cells, 0, TraderSwitchColumns::Shift, std::to_string(toUInt(Shifts::US)));

        auto buttons = definition->mutable_buttons();
        addButton(buttons, toUInt(0), "Submit", proto::ButtonFrontBehavior_CommitValues, proto::ButtonBackBehavior_Send, RequiresConfirmation::NO);
    }

    void populateMessageCounts(proto::GridDefinition* definition) {
        definition->set_gridid(toUInt(WindowIds::MessagesCounts));
        definition->set_name("Message Counts");
        definition->set_type(proto::GridType::Table);

        auto columnHeader = definition->mutable_columnheader();

        addColumnHeaderCell(WindowIds::MessagesCounts, columnHeader, MessageCountsColumns::Inserts, "Insert", true, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::MessagesCounts, columnHeader, MessageCountsColumns::Modifies, "Modify", true, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::MessagesCounts, columnHeader, MessageCountsColumns::Cancels, "Cancel", true, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::MessagesCounts, columnHeader, MessageCountsColumns::CancelRejects, "CancelRejects", true, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::MessagesCounts, columnHeader, MessageCountsColumns::ModifyRejects, "ModifyRejects", true, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::MessagesCounts, columnHeader, MessageCountsColumns::Fills, "Fills", true, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::MessagesCounts, columnHeader, MessageCountsColumns::FillsRatio, "FillsRatio", true, proto::DataType::double_type);

        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t contractIndex = 0; contractIndex < _config->_marketMakerConfigs.size(); ++contractIndex) {
            MarketMakerConfig& config = _config->_marketMakerConfigs.at(contractIndex);
            uint32_t internalId = _config->getInternalId(config._contract);
            addRowHeaderCell(WindowIds::MessagesCounts, rowHeader,internalId, config._contract);
        }
        addRowHeaderCell(WindowIds::MessagesCounts, rowHeader,1000, "Total");

        auto cells = definition->mutable_cells();

        for (uint32_t contractIndex = 0; contractIndex < _config->_marketMakerConfigs.size(); ++contractIndex) {
            MarketMakerConfig& config = _config->_marketMakerConfigs.at(contractIndex);
            uint32_t internalId = _config->getInternalId(config._contract);
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

        addColumnHeaderCell(WindowIds::PnlDetail, columnHeader, PnlDetailColumns::MMOpen, "MM Open", true, proto::DataType::rhone_price);
        addColumnHeaderCell(WindowIds::PnlDetail, columnHeader, PnlDetailColumns::MMClosed, "MM Closed", true, proto::DataType::rhone_price);
        addColumnHeaderCell(WindowIds::PnlDetail, columnHeader, PnlDetailColumns::MMTotal, "MM Total", true, proto::DataType::rhone_price);
        addColumnHeaderCell(WindowIds::PnlDetail, columnHeader, PnlDetailColumns::RVOpen, "RV Open", true, proto::DataType::rhone_price);
        addColumnHeaderCell(WindowIds::PnlDetail, columnHeader, PnlDetailColumns::RVClosed, "RV Closed", true, proto::DataType::rhone_price);
        addColumnHeaderCell(WindowIds::PnlDetail, columnHeader, PnlDetailColumns::RVTotal, "RV Total", true, proto::DataType::rhone_price);
        addColumnHeaderCell(WindowIds::PnlDetail, columnHeader, PnlDetailColumns::Total, "Total", true, proto::DataType::rhone_price);

        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t contractIndex = 0; contractIndex < _config->_marketMakerConfigs.size(); ++contractIndex) {
            MarketMakerConfig& config = _config->_marketMakerConfigs.at(contractIndex);
            uint32_t internalId = _config->getInternalId(config._contract);
            addRowHeaderCell(WindowIds::PnlDetail, rowHeader,internalId, config._contract);
        }

        auto cells = definition->mutable_cells();

        for (uint32_t contractIndex = 0; contractIndex < _config->_marketMakerConfigs.size(); ++contractIndex) {
            MarketMakerConfig& config = _config->_marketMakerConfigs.at(contractIndex);
            uint32_t internalId = _config->getInternalId(config._contract);
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

    void populatePNL(proto::GridDefinition* definition) {
        definition->set_gridid(toUInt(WindowIds::PNL));
        definition->set_name("PNL");
        definition->set_type(proto::GridType::Table);

        auto columnHeader = definition->mutable_columnheader();

        addColumnHeaderCell(WindowIds::PNL, columnHeader, PNLColumns::Open, "Open", true, proto::DataType::rhone_price);
        addColumnHeaderCell(WindowIds::PNL, columnHeader, PNLColumns::Closed, "Closed", true, proto::DataType::rhone_price);
        addColumnHeaderCell(WindowIds::PNL, columnHeader, PNLColumns::Total, "Total", true, proto::DataType::rhone_price);
        addColumnHeaderCell(WindowIds::PNL, columnHeader, PNLColumns::Drawdown, "Drawdown", true, proto::DataType::rhone_price);
        addColumnHeaderCell(WindowIds::PNL, columnHeader, PNLColumns::PnlLimit, "PnlLimit", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::PNL, columnHeader, PNLColumns::DrawdownLimit, "DrawdownLimit", false, proto::DataType::unsigned_type);

        auto rowHeader = definition->mutable_rowheader();

        addRowHeaderCell(WindowIds::PNL, rowHeader, PNLRows::RelativeValue, "Relative Value");
        addRowHeaderCell(WindowIds::PNL, rowHeader, PNLRows::MarketMakers, "Market Maker");
        addRowHeaderCell(WindowIds::PNL, rowHeader, PNLRows::Execution, "Execution");
        addRowHeaderCell(WindowIds::PNL, rowHeader, PNLRows::Total, "Total");

        auto cells = definition->mutable_cells();

        for (uint32_t i = toUInt(PNLRows::__start_marker__) + 1; i < toUInt(PNLRows::__end_marker__); ++i) {
            setCell(cells, i, PNLColumns::Open, "0");
            setCell(cells, i, PNLColumns::Closed, "0");
            setCell(cells, i, PNLColumns::Total, "0");
            setCell(cells, i, PNLColumns::Drawdown, "0");
            uint32_t pnlLimit = 0;
            uint32_t drawdownLimit = 0;
            if (i == toUInt(PNLRows::MarketMakers)) {
                const PNLConfig* config = _config->getPnlConfig("MM");
                pnlLimit = config->_limit;
                drawdownLimit = config->_drawdown;
            } else if (i == toUInt(PNLRows::RelativeValue)) {
                const PNLConfig* config = _config->getPnlConfig("RV");
                pnlLimit = config->_limit;
                drawdownLimit = config->_drawdown;
            } else if (i == toUInt(PNLRows::Total)) {
                const PNLConfig* config = _config->getPnlConfig("Total");
                pnlLimit = config->_limit;
                drawdownLimit = config->_drawdown;
            }
            setCell(cells, i, PNLColumns::PnlLimit, std::to_string(pnlLimit));
            setCell(cells, i, PNLColumns::DrawdownLimit, std::to_string(drawdownLimit));
        }

        auto buttons = definition->mutable_buttons();
        addButton(buttons, toUInt(0), "Submit", proto::ButtonFrontBehavior_CommitValues, proto::ButtonBackBehavior_Send, RequiresConfirmation::NO);
    }

    void populateStackerParameters(proto::GridDefinition* definition) {
        definition->set_gridid(toUInt(WindowIds::StackerParameters));
        definition->set_name("Stacker Parameters");
        definition->set_type(proto::GridType::Table);

        auto columnHeader = definition->mutable_columnheader();

        addColumnHeaderCell(WindowIds::StackerParameters, columnHeader, StackerParametersColumns::QuoteSize, "QuoteSize", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::StackerParameters, columnHeader, StackerParametersColumns::QuoteGap, "QuoteGap", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::StackerParameters, columnHeader, StackerParametersColumns::MaxQuote, "MaxQuote", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::StackerParameters, columnHeader, StackerParametersColumns::QuotePctTob, "Quote%TopBook", false, proto::DataType::double_type);
        addColumnHeaderCell(WindowIds::StackerParameters, columnHeader, StackerParametersColumns::Enabled, "Enabled", false, proto::DataType::bool_type);
        addColumnHeaderCell(WindowIds::StackerParameters, columnHeader, StackerParametersColumns::StackLevels, "StackLevels", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::StackerParameters, columnHeader, StackerParametersColumns::StackSlack, "StackSlack", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::StackerParameters, columnHeader, StackerParametersColumns::MaxTicksOffTopLevel, "MaxTicksOffTopLevel", false, proto::DataType::signed_type);
        addColumnHeaderCell(WindowIds::StackerParameters, columnHeader, StackerParametersColumns::UseGTC, "UseGTC", !allowGTCs(), proto::DataType::bool_type);
        addColumnHeaderCell(WindowIds::StackerParameters, columnHeader, StackerParametersColumns::AllowMultipleNewHedge, "AllowMultipleNewHedge", false, proto::DataType::bool_type);
        addColumnHeaderCell(WindowIds::StackerParameters, columnHeader, StackerParametersColumns::EnableQuoteSprayer, "EnableQuoteSprayer", false, proto::DataType::bool_type);

        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t contractIndex = 0; contractIndex < _config->_stackerConfigs.size(); ++contractIndex) {
            StackerConfig& config = _config->_stackerConfigs.at(contractIndex);
            uint32_t internalId = _config->getInternalId(config._contract);
            addRowHeaderCell(WindowIds::StackerParameters, rowHeader,internalId, config._contract);
        }

        auto cells = definition->mutable_cells();

        for (uint32_t contractIndex = 0; contractIndex < _config->_stackerConfigs.size(); ++contractIndex) {
            StackerConfig& config = _config->_stackerConfigs.at(contractIndex);
            uint32_t internalId = _config->getInternalId(config._contract);
            setCell(cells, internalId, StackerParametersColumns::Enabled, std::to_string(config._enabled));
            setCell(cells, internalId, StackerParametersColumns::QuoteSize, std::to_string(config._quoteSize));
            setCell(cells, internalId, StackerParametersColumns::QuoteGap, std::to_string(config._quoteGap));
            setCell(cells, internalId, StackerParametersColumns::MaxQuote, std::to_string(config._maxQuote));
            setCell(cells, internalId, StackerParametersColumns::QuotePctTob, std::to_string(config._quotePctTob));
            setCell(cells, internalId, StackerParametersColumns::StackLevels, std::to_string(config._numberOfLevels));
            setCell(cells, internalId, StackerParametersColumns::StackSlack, std::to_string(config._stackSlack));
            setCell(cells, internalId, StackerParametersColumns::MaxTicksOffTopLevel, std::to_string(config._maxTicksOffTopLevel));
            setCell(cells, internalId, StackerParametersColumns::UseGTC, std::to_string(config._useGtc));
            setCell(cells, internalId, StackerParametersColumns::AllowMultipleNewHedge, std::to_string(config._allowMultipleNewHedge));
            setCell(cells, internalId, StackerParametersColumns::EnableQuoteSprayer, std::to_string(config._enableQuoteSprayer));
        }

        auto buttons = definition->mutable_buttons();
        addButton(buttons, toUInt(0), "Submit", proto::ButtonFrontBehavior_CommitValues, proto::ButtonBackBehavior_Send, RequiresConfirmation::NO);
    }

    void populateMarketMakerParameters(proto::GridDefinition* definition) {
        definition->set_gridid(toUInt(WindowIds::MarketMakerParameters));
        definition->set_name("Market Maker Parameters");
        definition->set_type(proto::GridType::Table);

        auto columnHeader = definition->mutable_columnheader();

        addColumnHeaderCell(WindowIds::MarketMakerParameters, columnHeader, MarketMakerParametersColumns::QuoteExternal, "QuoteExternal", false, proto::DataType::bool_type);
        addColumnHeaderCell(WindowIds::MarketMakerParameters, columnHeader, MarketMakerParametersColumns::QuoteSize, "QuoteSize", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::MarketMakerParameters, columnHeader, MarketMakerParametersColumns::QuotePctTob, "Quote%TOB", false, proto::DataType::double_type);
        addColumnHeaderCell(WindowIds::MarketMakerParameters, columnHeader, MarketMakerParametersColumns::MaxTicksWide, "MaxTicksWide", false, proto::DataType::signed_type);
        addColumnHeaderCell(WindowIds::MarketMakerParameters, columnHeader, MarketMakerParametersColumns::Width, "WidthPercentage", true, proto::DataType::double_type);
        addColumnHeaderCell(WindowIds::MarketMakerParameters, columnHeader, MarketMakerParametersColumns::AsiaWidth, "AsiaWidthPercentage", false, proto::DataType::double_type);
        addColumnHeaderCell(WindowIds::MarketMakerParameters, columnHeader, MarketMakerParametersColumns::EuroWidth, "EuroWidthPercentage", false, proto::DataType::double_type);
        addColumnHeaderCell(WindowIds::MarketMakerParameters, columnHeader, MarketMakerParametersColumns::USWidth, "USWidthPercentage", false, proto::DataType::double_type);
        addColumnHeaderCell(WindowIds::MarketMakerParameters, columnHeader, MarketMakerParametersColumns::SafeModeWidth, "SafeModeWidthPercentage", false, proto::DataType::double_type);
        addColumnHeaderCell(WindowIds::MarketMakerParameters, columnHeader, MarketMakerParametersColumns::MinInternalPriceChange, "MinInternalPriceChange", false, proto::DataType::double_type);
        addColumnHeaderCell(WindowIds::MarketMakerParameters, columnHeader, MarketMakerParametersColumns::EstablishedLevel, "EstLevelEMA", true, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::MarketMakerParameters, columnHeader, MarketMakerParametersColumns::EstLevelEMASeconds, "EstLevelEMASeconds", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::MarketMakerParameters, columnHeader, MarketMakerParametersColumns::EstLevelPctEMA, "EstLevel%EMA", false, proto::DataType::double_type);
        addColumnHeaderCell(WindowIds::MarketMakerParameters, columnHeader, MarketMakerParametersColumns::HedgeOffset, "HedgeOffset", false, proto::DataType::double_type);
        addColumnHeaderCell(WindowIds::MarketMakerParameters, columnHeader, MarketMakerParametersColumns::JoinSize, "JoinSize", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::MarketMakerParameters, columnHeader, MarketMakerParametersColumns::LeaveSize, "LeaveSize", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::MarketMakerParameters, columnHeader, MarketMakerParametersColumns::PayUpQty, "PayUpQty", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::MarketMakerParameters, columnHeader, MarketMakerParametersColumns::PayUpOrders, "PayUpOrders", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::MarketMakerParameters, columnHeader, MarketMakerParametersColumns::PayUpMultiple, "PayUpMultiple", false, proto::DataType::double_type);
        addColumnHeaderCell(WindowIds::MarketMakerParameters, columnHeader, MarketMakerParametersColumns::MaxPosInterrnal, "MaxPosInternal", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::MarketMakerParameters, columnHeader, MarketMakerParametersColumns::MaxPosExternal, "MaxPosExternal", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::MarketMakerParameters, columnHeader, MarketMakerParametersColumns::Commission, "Commission", false, proto::DataType::double_type);
        addColumnHeaderCell(WindowIds::MarketMakerParameters, columnHeader, MarketMakerParametersColumns::Enabled, "Enabled", false, proto::DataType::bool_type);
        addColumnHeaderCell(WindowIds::MarketMakerParameters, columnHeader, MarketMakerParametersColumns::InternalPosition, "InternalPosition", true, proto::DataType::signed_type);
        addColumnHeaderCell(WindowIds::MarketMakerParameters, columnHeader, MarketMakerParametersColumns::ExternalPosition, "ExternalPosition", true, proto::DataType::signed_type);
        addColumnHeaderCell(WindowIds::MarketMakerParameters, columnHeader, MarketMakerParametersColumns::BidInternalPrice, "BidIntPrice", true, proto::DataType::rhone_price);
        addColumnHeaderCell(WindowIds::MarketMakerParameters, columnHeader, MarketMakerParametersColumns::AskInternalPrice, "AskIntPrice", true, proto::DataType::rhone_price);
        addColumnHeaderCell(WindowIds::MarketMakerParameters, columnHeader, MarketMakerParametersColumns::BidInternalQty, "BidIntQty", true, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::MarketMakerParameters, columnHeader, MarketMakerParametersColumns::AskInternalQty, "AskIntQty", true, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::MarketMakerParameters, columnHeader, MarketMakerParametersColumns::BidExternalPrice, "BidExtPrice", true, proto::DataType::rhone_price);
        addColumnHeaderCell(WindowIds::MarketMakerParameters, columnHeader, MarketMakerParametersColumns::AskExternalPrice, "AskExtPrice", true, proto::DataType::rhone_price);
        addColumnHeaderCell(WindowIds::MarketMakerParameters, columnHeader, MarketMakerParametersColumns::BidExternalQty, "BidExtQty", true, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::MarketMakerParameters, columnHeader, MarketMakerParametersColumns::AskExternalQty, "AskExtQty", true, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::MarketMakerParameters, columnHeader, MarketMakerParametersColumns::MidPoint, "MidPoint", true, proto::DataType::rhone_price);
        addColumnHeaderCell(WindowIds::MarketMakerParameters, columnHeader, MarketMakerParametersColumns::BidHedgeInternalPrice, "BidHedgeIntPrice", true, proto::DataType::rhone_price);
        addColumnHeaderCell(WindowIds::MarketMakerParameters, columnHeader, MarketMakerParametersColumns::BidHedgeExternalPrice, "BidHedgeExtPrice", true, proto::DataType::rhone_price);
        addColumnHeaderCell(WindowIds::MarketMakerParameters, columnHeader, MarketMakerParametersColumns::BidHedgeSize, "BidHedgeSize", true, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::MarketMakerParameters, columnHeader, MarketMakerParametersColumns::AskHedgeInternalPrice, "AskHedgeIntPrice", true, proto::DataType::rhone_price);
        addColumnHeaderCell(WindowIds::MarketMakerParameters, columnHeader, MarketMakerParametersColumns::AskHedgeExternalPrice, "AskHedgeExtPrice", true, proto::DataType::rhone_price);
        addColumnHeaderCell(WindowIds::MarketMakerParameters, columnHeader, MarketMakerParametersColumns::AskHedgeSize, "AskHedgeSize", true, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::MarketMakerParameters, columnHeader, MarketMakerParametersColumns::WorsenPayupCondPercent, "WorsenPayupCond%", false, proto::DataType::double_type);
        addColumnHeaderCell(WindowIds::MarketMakerParameters, columnHeader, MarketMakerParametersColumns::QueueImportance, "QueueImportance", false, proto::DataType::enum_type, "queue_importance");

        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t contractIndex = 0; contractIndex < _config->_marketMakerConfigs.size(); ++contractIndex) {
            MarketMakerConfig& config = _config->_marketMakerConfigs.at(contractIndex);
            uint32_t internalId = _config->getInternalId(config._contract);
            addRowHeaderCell(WindowIds::MarketMakerParameters, rowHeader,internalId, config._contract);
        }

        auto cells = definition->mutable_cells();

        for (uint32_t contractIndex = 0; contractIndex < _config->_marketMakerConfigs.size(); ++contractIndex) {
            MarketMakerConfig& config = _config->_marketMakerConfigs.at(contractIndex);
            uint32_t internalId = _config->getInternalId(config._contract);
            setCell(cells, internalId, MarketMakerParametersColumns::Enabled, std::to_string(config._enabled));
            setCell(cells, internalId, MarketMakerParametersColumns::QuoteExternal, std::to_string(config._quoteExternal));
            setCell(cells, internalId, MarketMakerParametersColumns::QuoteSize, std::to_string(config._quoteSize));
            setCell(cells, internalId, MarketMakerParametersColumns::QuotePctTob, std::to_string(config._quotePctTob));
            setCell(cells, internalId, MarketMakerParametersColumns::MaxTicksWide, std::to_string(config._maxTicksWide));
            setCell(cells, internalId, MarketMakerParametersColumns::Width, std::to_string(config._widthPercentage[toUInt(Shifts::__current_marker__)]));
            for (uint32_t i = 0; i < toUInt(Shifts::__current_marker__); ++i) {
                setCell(cells, internalId, (MarketMakerParametersColumns)(toUInt(MarketMakerParametersColumns::Width) + i + 1), std::to_string(config._widthPercentage[i]));
            }
            setCell(cells, internalId, MarketMakerParametersColumns::MinInternalPriceChange, std::to_string(config._minInternalPriceChange));
            setCell(cells, internalId, MarketMakerParametersColumns::EstablishedLevel, std::to_string(config._establishedLevel));
            setCell(cells, internalId, MarketMakerParametersColumns::EstLevelEMASeconds, std::to_string(config._estLevelEMASeconds));
            setCell(cells, internalId, MarketMakerParametersColumns::EstLevelPctEMA, std::to_string(config._estLevelPctEMA));
            setCell(cells, internalId, MarketMakerParametersColumns::HedgeOffset, std::to_string(config._hedgeOffset));
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
            setCell(cells, internalId, MarketMakerParametersColumns::WorsenPayupCondPercent, std::to_string(config._worsenPayupCondPercent));
            setCell(cells, internalId, MarketMakerParametersColumns::QueueImportance, std::to_string(config._queueImportance));
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
        for (; headerOffset < toUInt(StrategyId::Execution); ++headerOffset) {
            addRowHeaderCell(WindowIds::MainControl, rowHeader, headerOffset, strategyFullName((StrategyId)headerOffset));
            setCell(cells, headerOffset, MainControlAction::Enabled, std::to_string(proto::SystemState::SystemStateDisabled), toUInt(Color::TRANSPARENT_SWITCH_DISABLED));
        }

        // Strategy Quote External
        for (; headerOffset < toUInt(StrategyId::Execution) * 2; ++headerOffset) {
            addRowHeaderCell(WindowIds::MainControl, rowHeader, headerOffset,
                             boost::str(boost::format("%1% QuoteExternal") % toString((StrategyId)(headerOffset - toUInt(StrategyId::Execution)))));
            setCell(cells, headerOffset, MainControlAction::Enabled, std::to_string(proto::SystemState::SystemStateDisabled), toUInt(Color::TRANSPARENT_SWITCH_DISABLED));
        }

        // Exchange ExtQuote Control
        auto extQuote = executor->s_systemState._exchQuoteExternally.load(std::memory_order_relaxed);
        for (auto iter = executor->_usedVenues.begin(); iter != executor->_usedVenues.end(); ++iter) {
            addRowHeaderCell(WindowIds::MainControl, rowHeader, headerOffset, boost::str(boost::format("%1% QuoteExternal") % ven::VenueUtils::getName(*iter)));
            setCell(cells, headerOffset, MainControlAction::Enabled, std::to_string(proto::SystemState::SystemStateDisabled), toUInt(Color::TRANSPARENT_SWITCH_DISABLED));
            _mapRowIndexVenue.emplace(headerOffset, static_cast<uint8_t>(*iter));
            _mapVenueRowIndex.emplace(static_cast<uint8_t>(*iter), headerOffset++);
            extQuote.reset(static_cast<uint8_t>(*iter));
        }
        executor->s_systemState._exchQuoteExternally.store(extQuote, std::memory_order_release);

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

    template <typename HeaderT, typename EnumValue>
    void addColumnHeaderCell(WindowIds gridId, HeaderT* header, EnumValue id, const std::string& label, bool readyOnly, proto::DataType type, const std::string& e = "") {
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
    }

    std::string getRowLabel(uint32_t gridId, uint32_t rowId) {
        return _rows[gridId][rowId];
    }

    std::string getColumnLabel(uint32_t gridId, uint32_t id) {
        return _columns[gridId][id];
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
                Traders trader = static_cast<Traders>(atoi(value.c_str()));
                return GetTraderName(trader);
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

    void populateCommissions(proto::GridDefinition* definition, const SimpleMatrix<hf::price_t>& commissions) {
        definition->set_gridid(toUInt(WindowIds::Commissions));
        definition->set_name("Commissions");
        definition->set_type(proto::GridType::Matrix);

        auto columnHeader = definition->mutable_columnheader();
        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t index = _config->_indexOfFirstTradedContract; index < _config->_allContracts.size(); ++index) {
            ConfigContract& contract = _config->_allContracts[index];
            addColumnHeaderCell(WindowIds::Commissions, columnHeader, index, contract._name, true, proto::DataType::double_type);
        }

        for (uint32_t index = _config->_indexOfFirstTradedContract; index < _config->_allContracts.size(); ++index) {
            ConfigContract& contract = _config->_allContracts[index];
            addRowHeaderCell(WindowIds::Commissions, rowHeader, index, contract._name);
        }

        auto cells = definition->mutable_cells();

        for (uint32_t row = _config->_indexOfFirstTradedContract; row < _config->_allContracts.size(); ++row) {
            for (uint32_t col = _config->_indexOfFirstTradedContract; col < _config->_allContracts.size(); ++col) {
                setCell(cells, row, col, std::to_string(ToDollars(commissions.getValue(row, col))));
            }
        }
    }

    void populateQuoteSizes(proto::GridDefinition* definition, const Matrix& quoteSizes) {
        definition->set_gridid(toUInt(WindowIds::QuoteSizes));
        definition->set_name("Quote sizes");
        definition->set_type(proto::GridType::Matrix);

        auto columnHeader = definition->mutable_columnheader();
        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t index = _config->_indexOfFirstTradedContract; index < _config->_allContracts.size(); ++index) {
            ConfigContract& contract = _config->_allContracts[index];
            addColumnHeaderCell(WindowIds::QuoteSizes, columnHeader, index, contract._name, true, proto::DataType::double_type);
        }

        for (uint32_t index = _config->_indexOfFirstTradedContract; index < _config->_allContracts.size(); ++index) {
            ConfigContract& contract = _config->_allContracts[index];
            addRowHeaderCell(WindowIds::QuoteSizes, rowHeader, index, contract._name);
        }

        auto cells = definition->mutable_cells();

        for (uint32_t row = _config->_indexOfFirstTradedContract; row < _config->_allContracts.size(); ++row) {
            for (uint32_t col = _config->_indexOfFirstTradedContract; col < _config->_allContracts.size(); ++col) {
                setCell(cells, row, col, std::to_string(quoteSizes.getValue(row, col)));
            }
        }
    }

    void populateFactorHedged(proto::GridDefinition* definition, const SimpleMatrix<int>& factorHedged) {
        definition->set_gridid(toUInt(WindowIds::FactorHedged));
        definition->set_name("Factor hedged");
        definition->set_type(proto::GridType::Matrix);

        auto columnHeader = definition->mutable_columnheader();
        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t index = _config->_indexOfFirstTradedContract; index < _config->_allContracts.size(); ++index) {
            ConfigContract& contract = _config->_allContracts[index];
            addColumnHeaderCell(WindowIds::FactorHedged, columnHeader, index, contract._name, true, proto::DataType::enum_type, "factors");
        }

        for (uint32_t index = _config->_indexOfFirstTradedContract; index < _config->_allContracts.size(); ++index) {
            ConfigContract& contract = _config->_allContracts[index];
            addRowHeaderCell(WindowIds::FactorHedged, rowHeader, index, contract._name);
        }

        auto cells = definition->mutable_cells();

        for (uint32_t row = _config->_indexOfFirstTradedContract; row < _config->_allContracts.size(); ++row) {
            for (uint32_t col = _config->_indexOfFirstTradedContract; col < _config->_allContracts.size(); ++col) {
                setCell(cells, row, col, std::to_string(factorHedged.getValue(row, col)));
            }
        }
    }

    void populateHedgeRatios(proto::GridDefinition* definition, const SimpleMatrix<double>& hedgeRatios) {
        definition->set_gridid(toUInt(WindowIds::HedgeRatios));
        definition->set_name("Hedge Ratios");
        definition->set_type(proto::GridType::Matrix);

        auto columnHeader = definition->mutable_columnheader();
        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t index = _config->_indexOfFirstTradedContract; index < _config->_allContracts.size(); ++index) {
            ConfigContract& contract = _config->_allContracts[index];
            addColumnHeaderCell(WindowIds::HedgeRatios, columnHeader, index, contract._name, true, proto::DataType::double_type);
        }

        for (uint32_t index = _config->_indexOfFirstTradedContract; index < _config->_allContracts.size(); ++index) {
            ConfigContract& contract = _config->_allContracts[index];
            addRowHeaderCell(WindowIds::HedgeRatios, rowHeader, index, contract._name);
        }

        auto cells = definition->mutable_cells();

        for (uint32_t row = _config->_indexOfFirstTradedContract; row < _config->_allContracts.size(); ++row) {
            for (uint32_t col = _config->_indexOfFirstTradedContract; col < _config->_allContracts.size(); ++col) {
                setCell(cells, row, col, std::to_string(hedgeRatios.getValue(row, col)));
            }
        }
    }

    void populateRiskMatrix(proto::GridDefinition* definition, const Matrix& riskMatrix) {
        definition->set_gridid(toUInt(WindowIds::RiskMatrix));
        definition->set_name("Risk Matrix");
        definition->set_type(proto::GridType::Matrix);

        auto columnHeader = definition->mutable_columnheader();
        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t index = 0; index < _config->_pricingOutrights.size(); ++index) {
            ConfigContract& contract = _config->_allContracts[index];
            addColumnHeaderCell(WindowIds::RiskMatrix, columnHeader, index, contract._name, true, proto::DataType::double_type);
        }

        for (uint32_t index = 0; index < _config->_vectorConfigs.size(); ++index) {
            VectorConfig& config = _config->_vectorConfigs[index];
            addRowHeaderCell(WindowIds::RiskMatrix, rowHeader, index, config._vectorName);
        }

        auto cells = definition->mutable_cells();

        for (uint32_t row = 0; row < riskMatrix._numberOfRows; ++row) {
            for (uint32_t col = 0; col < riskMatrix._numberOfColumns; ++col) {
                setCell(cells, row, col, std::to_string(riskMatrix.getValue(row, col)));
            }
        }
    }

    void populatePricingMatrix(proto::GridDefinition* definition, const Matrix& pricingMatrix) {
        definition->set_gridid(toUInt(WindowIds::PricingMatrix));
        definition->set_name("Pricing Matrix");
        definition->set_type(proto::GridType::Matrix);

        auto columnHeader = definition->mutable_columnheader();
        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t index = 0; index < _config->_pricingOutrights.size(); ++index) {
            ConfigContract& contract = _config->_allContracts[index];
            addColumnHeaderCell(WindowIds::PricingMatrix, columnHeader, index, contract._name, true, proto::DataType::double_type);
        }

        for (uint32_t index = 0; index < _config->_vectorConfigs.size(); ++index) {
            VectorConfig& config = _config->_vectorConfigs[index];
            addRowHeaderCell(WindowIds::PricingMatrix, rowHeader, index, config._vectorName);
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

        addColumnHeaderCell(WindowIds::PricingConfig, columnHeader, PricingConfigsColumns::Weight, "Weight", false, proto::DataType::double_type);
        addColumnHeaderCell(WindowIds::PricingConfig, columnHeader, PricingConfigsColumns::UseWeighted, "UseWeighted", false, proto::DataType::bool_type);
        addColumnHeaderCell(WindowIds::PricingConfig, columnHeader, PricingConfigsColumns::EstablishedLevel, "EstablishedLevel", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::PricingConfig, columnHeader, PricingConfigsColumns::PricingMultiplier, "PricingMultiplier", true, proto::DataType::double_type);
        addColumnHeaderCell(WindowIds::PricingConfig, columnHeader, PricingConfigsColumns::QuantityMultiplier, "QuantityMultiplier", true, proto::DataType::double_type);
        addColumnHeaderCell(WindowIds::PricingConfig, columnHeader, PricingConfigsColumns::SweepTicks, "SweepTicks", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::PricingConfig, columnHeader, PricingConfigsColumns::SweepQty, "SweepQty", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::PricingConfig, columnHeader, PricingConfigsColumns::MaxTicksWide, "MaxTicksWide", false, proto::DataType::signed_type);

        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t contractIndex = 0; contractIndex < _config->_pricingConfigs.size(); ++contractIndex) {
            PricingConfig& config = _config->_pricingConfigs.at(contractIndex);
            uint32_t internalId = _config->getInternalId(config._contract);
            addRowHeaderCell(WindowIds::PricingConfig, rowHeader,internalId, config._contract);
        }

        auto cells = definition->mutable_cells();

        for (uint32_t contractIndex = 0; contractIndex < _config->_pricingConfigs.size(); ++contractIndex) {
            PricingConfig& config = _config->_pricingConfigs.at(contractIndex);
            setCell(cells, contractIndex, PricingConfigsColumns::Weight, std::to_string(config._weight));
            setCell(cells, contractIndex, PricingConfigsColumns::UseWeighted, std::to_string(config._useWeightedMid));
            setCell(cells, contractIndex, PricingConfigsColumns::EstablishedLevel, std::to_string(config._establishedLevel));
            setCell(cells, contractIndex, PricingConfigsColumns::PricingMultiplier, std::to_string(config._pricingMultiplier));
            setCell(cells, contractIndex, PricingConfigsColumns::QuantityMultiplier, std::to_string(config._quantityMultiplier));
            setCell(cells, contractIndex, PricingConfigsColumns::SweepTicks, std::to_string(config._sweepTicks));
            setCell(cells, contractIndex, PricingConfigsColumns::SweepQty, std::to_string(config._sweepQuantity));
            setCell(cells, contractIndex, PricingConfigsColumns::MaxTicksWide, std::to_string(config._maxTicksWide));
        }

        auto buttons = definition->mutable_buttons();
        addButton(buttons, toUInt(0), "Submit", proto::ButtonFrontBehavior_CommitValues, proto::ButtonBackBehavior_Send, RequiresConfirmation::NO);
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

        auto buttons = definition->mutable_buttons();
        addButton(buttons, toUInt(0), "Submit", proto::ButtonFrontBehavior_CommitValues, proto::ButtonBackBehavior_Send, RequiresConfirmation::NO);
    }

    void populateContractParameterRVColumns(proto::GridDefinition* definition, const Vector<hf::price_t>& extraWidth) {
        definition->set_gridid(toUInt(WindowIds::ContractParametersRv));
        definition->set_name("Relative Value Parameters");
        definition->set_type(proto::GridType::Table);

        auto columnHeader = definition->mutable_columnheader();

        addColumnHeaderCell(WindowIds::ContractParametersRv, columnHeader, ContractParameterRVColumns::BidLean, "BidLean", true, proto::DataType::enum_type, "contracts");
        addColumnHeaderCell(WindowIds::ContractParametersRv, columnHeader, ContractParameterRVColumns::AskLean, "AskLean", true, proto::DataType::enum_type, "contracts");
        addColumnHeaderCell(WindowIds::ContractParametersRv, columnHeader, ContractParameterRVColumns::BidLeanType, "BidLeanType", true, proto::DataType::enum_type, "LeanType");
        addColumnHeaderCell(WindowIds::ContractParametersRv, columnHeader, ContractParameterRVColumns::AskLeanType, "AskLeanType", true, proto::DataType::enum_type, "LeanType");
        addColumnHeaderCell(WindowIds::ContractParametersRv, columnHeader, ContractParameterRVColumns::BidInternalPrice, "BidIntPrice", true, proto::DataType::rhone_price);
        addColumnHeaderCell(WindowIds::ContractParametersRv, columnHeader, ContractParameterRVColumns::AskInternalPrice, "AskIntPrice", true, proto::DataType::rhone_price);
        addColumnHeaderCell(WindowIds::ContractParametersRv, columnHeader, ContractParameterRVColumns::BidExternalPrice, "BidExtPrice", true, proto::DataType::rhone_price);
        addColumnHeaderCell(WindowIds::ContractParametersRv, columnHeader, ContractParameterRVColumns::AskExternalPrice, "AskExtPrice", true, proto::DataType::rhone_price);
        addColumnHeaderCell(WindowIds::ContractParametersRv, columnHeader, ContractParameterRVColumns::BidCross, "BidCross", true, proto::DataType::enum_type, "YesNo");
        addColumnHeaderCell(WindowIds::ContractParametersRv, columnHeader, ContractParameterRVColumns::AskCross, "AskCross", true, proto::DataType::enum_type, "YesNo");
        addColumnHeaderCell(WindowIds::ContractParametersRv, columnHeader, ContractParameterRVColumns::BidInternalQty, "BidIntQty", true, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::ContractParametersRv, columnHeader, ContractParameterRVColumns::AskInternalQty, "AskIntQty", true, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::ContractParametersRv, columnHeader, ContractParameterRVColumns::BidExternalQty, "BidExtQty", true, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::ContractParametersRv, columnHeader, ContractParameterRVColumns::AskExternalQty, "AskExtQty", true, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::ContractParametersRv, columnHeader, ContractParameterRVColumns::STF, "STF", true, proto::DataType::rhone_price);
        addColumnHeaderCell(WindowIds::ContractParametersRv, columnHeader, ContractParameterRVColumns::Mkt, "Mkt", true, proto::DataType::rhone_price);
        addColumnHeaderCell(WindowIds::ContractParametersRv, columnHeader, ContractParameterRVColumns::QuoteExternal, "QuoteExternal", false, proto::DataType::bool_type);
        addColumnHeaderCell(WindowIds::ContractParametersRv, columnHeader, ContractParameterRVColumns::HedgeTolerance, "HedgeTolerance", false, proto::DataType::double_type);
        addColumnHeaderCell(WindowIds::ContractParametersRv, columnHeader, ContractParameterRVColumns::Width, "Width", false, proto::DataType::double_type);
        addColumnHeaderCell(WindowIds::ContractParametersRv, columnHeader, ContractParameterRVColumns::PCTStfMove, "%stfMove", false, proto::DataType::double_type);
        addColumnHeaderCell(WindowIds::ContractParametersRv, columnHeader, ContractParameterRVColumns::MaxTicksThroughMarket, "MaxTicksThroughMarket", false, proto::DataType::double_type);
        addColumnHeaderCell(WindowIds::ContractParametersRv, columnHeader, ContractParameterRVColumns::Commission, "Commission", false, proto::DataType::double_type);
        addColumnHeaderCell(WindowIds::ContractParametersRv, columnHeader, ContractParameterRVColumns::ExtraWidth, "ExtraWidth", true, proto::DataType::double_type);
        addColumnHeaderCell(WindowIds::ContractParametersRv, columnHeader, ContractParameterRVColumns::StaticQuote, "StaticQuote", false, proto::DataType::bool_type);
        addColumnHeaderCell(WindowIds::ContractParametersRv, columnHeader, ContractParameterRVColumns::QuoteSize, "QuoteSize", false, proto::DataType::unsigned_type);

        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t index = 0; index < _config->_relativeValueConfigs.size(); ++index) {
            RelativeValueConfig& config = _config->_relativeValueConfigs[index];
            uint32_t internalId = _config->getInternalId(config._contract);
            addRowHeaderCell(WindowIds::ContractParametersRv, rowHeader, internalId, config._contract);
        }

        auto cells = definition->mutable_cells();

        for (uint32_t index = 0; index < _config->_relativeValueConfigs.size(); ++index) {
            RelativeValueConfig& config = _config->_relativeValueConfigs[index];
            uint32_t internalId = _config->getInternalId(config._contract);
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
            setCell(cells, internalId, ContractParameterRVColumns::PCTStfMove, std::to_string(config._pctStfMove));
            setCell(cells, internalId, ContractParameterRVColumns::MaxTicksThroughMarket, std::to_string(config._maxTicksThroughMarket));
            setCell(cells, internalId, ContractParameterRVColumns::Commission, std::to_string(config._commission));
            setCell(cells, internalId, ContractParameterRVColumns::BidCross, std::to_string(toUInt(YesNoType::Unknown)));
            setCell(cells, internalId, ContractParameterRVColumns::AskCross, std::to_string(toUInt(YesNoType::Unknown)));
            setCell(cells, internalId, ContractParameterRVColumns::ExtraWidth, std::to_string(ToDollars(extraWidth.getValue(internalId))));
            setCell(cells, internalId, ContractParameterRVColumns::StaticQuote, std::to_string(config._staticQuote));
            setCell(cells, internalId, ContractParameterRVColumns::QuoteSize, std::to_string(config._quoteSize));
        }

        auto buttons = definition->mutable_buttons();
        addButton(buttons, toUInt(0), "Submit", proto::ButtonFrontBehavior_CommitValues, proto::ButtonBackBehavior_Send, RequiresConfirmation::NO);
    }

    void populateVectorParametersDefinition(proto::GridDefinition* definition) {
        definition->set_gridid(toUInt(WindowIds::VectorParameters));
        definition->set_name("Factor Parameters");
        definition->set_type(proto::GridType::Table);

        auto columnHeader = definition->mutable_columnheader();

        addColumnHeaderCell(WindowIds::VectorParameters, columnHeader, VectorParametersColumns::Position, "Position", true, proto::DataType::double_type);
        addColumnHeaderCell(WindowIds::VectorParameters, columnHeader, VectorParametersColumns::Mkt, "Mkt", true, proto::DataType::double_type);
        addColumnHeaderCell(WindowIds::VectorParameters, columnHeader, VectorParametersColumns::STF, "STF", true, proto::DataType::double_type);
        addColumnHeaderCell(WindowIds::VectorParameters, columnHeader, VectorParametersColumns::LTF, "LTF", false, proto::DataType::double_type);
        addColumnHeaderCell(WindowIds::VectorParameters, columnHeader, VectorParametersColumns::MaxPos, "MaxPos", false, proto::DataType::signed_type);
        addColumnHeaderCell(WindowIds::VectorParameters, columnHeader, VectorParametersColumns::Range, "Range", false, proto::DataType::double_type);
        addColumnHeaderCell(WindowIds::VectorParameters, columnHeader, VectorParametersColumns::EMA_ON, "EmaOn", false, proto::DataType::bool_type);
        addColumnHeaderCell(WindowIds::VectorParameters, columnHeader, VectorParametersColumns::EMA_Time, "EmaSeconds", false, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::VectorParameters, columnHeader, VectorParametersColumns::EMA_On_Time, "EmaOnTime", false, proto::DataType::enum_type, "ema_on_time");
        addColumnHeaderCell(WindowIds::VectorParameters, columnHeader, VectorParametersColumns::EMA_Off_Time, "EmaOffTime", false, proto::DataType::enum_type, "ema_off_time");
        addColumnHeaderCell(WindowIds::VectorParameters, columnHeader, VectorParametersColumns::SetToMarket, "SetToMarket", true, proto::DataType::bool_type);
        addColumnHeaderCell(WindowIds::VectorParameters, columnHeader, VectorParametersColumns::Hedged, "Hedged", true, proto::DataType::bool_type);

        auto rowHeader = definition->mutable_rowheader();

        for (uint32_t vectorIndex = 0; vectorIndex < _config->_vectorConfigs.size(); ++vectorIndex) {
            VectorConfig& config = _config->_vectorConfigs[vectorIndex];
            addRowHeaderCell(WindowIds::VectorParameters, rowHeader, vectorIndex, config._vectorName);
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
            setCell(cells, vectorIndex, VectorParametersColumns::EMA_On_Time, std::to_string(config._EMAOn));
            setCell(cells, vectorIndex, VectorParametersColumns::EMA_Off_Time, std::to_string(config._EMAOff));
            setCell(cells, vectorIndex, VectorParametersColumns::SetToMarket, std::to_string(config._setToMarket));
            setCell(cells, vectorIndex, VectorParametersColumns::Hedged, std::to_string(config._hedged));
        }

        auto buttons = definition->mutable_buttons();
        addButton(buttons, toUInt(STF_TO_MARKET), "STF to Market", proto::ButtonFrontBehavior_Nothing, proto::ButtonBackBehavior_SubmitId, RequiresConfirmation::NO);
        addButton(buttons, toUInt(0), "Submit", proto::ButtonFrontBehavior_CommitValues, proto::ButtonBackBehavior_Send, RequiresConfirmation::NO);
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
};

};

#endif //VIPER_VIPER_CONFIG_H
