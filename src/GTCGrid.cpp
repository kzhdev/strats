//
// Created by cmilutinovic on 1/22/20.
//

#include "GUIThings.h"

#define VIPER_LOG_IOP_UPDATES
#ifdef VIPER_LOG_IOP_UPDATES
#define LOG_IOP LOG
#else
#define LOG_IOP(...) do {} while(0)
#endif

namespace Python {

    COLD
    void GUIThings::tryPopulateGTCGridColumns(proto::GUIInitialization& message) {
        if(!allowGTCs()) {
            return;
        }

        auto *definition = message.mutable_definitions()->Add();

        definition->set_gridid(toUInt(WindowIds::GTCManagement));
        definition->set_name("GTC Management");
        definition->set_type(proto::GridType::Table);

        auto *columnHeader = definition->mutable_columnheader();

        addColumnHeaderCell(WindowIds::GTCManagement, columnHeader, GTCManagementColumns::PreOpenBid, "PreOpen Bid", true, proto::DataType::rhone_price);
        addColumnHeaderCell(WindowIds::GTCManagement, columnHeader, GTCManagementColumns::PreOpenAsk, "PreOpen Ask", true, proto::DataType::rhone_price);
        addColumnHeaderCell(WindowIds::GTCManagement, columnHeader, GTCManagementColumns::IOP, "IOP", true, proto::DataType::rhone_price);
        addColumnHeaderCell(WindowIds::GTCManagement, columnHeader, GTCManagementColumns::NumBid, "#BidOrders", true, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::GTCManagement, columnHeader, GTCManagementColumns::NumOffer, "#AskOrders", true, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::GTCManagement, columnHeader, GTCManagementColumns::NumTotal, "#Orders", true, proto::DataType::unsigned_type);
        addColumnHeaderCell(WindowIds::GTCManagement, columnHeader, GTCManagementColumns::CancelAllBid, "Cancel ALL Bids", false, proto::DataType::button);
        addColumnHeaderCell(WindowIds::GTCManagement, columnHeader, GTCManagementColumns::Cancel_2_Bid, "Cancel 2 Bids", false, proto::DataType::button);
        addColumnHeaderCell(WindowIds::GTCManagement, columnHeader, GTCManagementColumns::Cancel_1_Bid, "Cancel 1 Bid", false, proto::DataType::button);
        addColumnHeaderCell(WindowIds::GTCManagement, columnHeader, GTCManagementColumns::TicksThroughAsk, "TicksThroughAsk", true, proto::DataType::signed_type);
        addColumnHeaderCell(WindowIds::GTCManagement, columnHeader, GTCManagementColumns::isCrossedBid, "BidCrossed", true, proto::DataType::enum_type, "YesNo");
        addColumnHeaderCell(WindowIds::GTCManagement, columnHeader, GTCManagementColumns::isCrossedAsk, "AskCrossed", true, proto::DataType::enum_type, "YesNo");
        addColumnHeaderCell(WindowIds::GTCManagement, columnHeader, GTCManagementColumns::TicksThroughBid, "TickThroughBid", true, proto::DataType::signed_type);
        addColumnHeaderCell(WindowIds::GTCManagement, columnHeader, GTCManagementColumns::Cancel_1_Ask, "Cancel 1 Ask", false, proto::DataType::button);
        addColumnHeaderCell(WindowIds::GTCManagement, columnHeader, GTCManagementColumns::Cancel_2_Ask, "Cancel 2 Ask", false, proto::DataType::button);
        addColumnHeaderCell(WindowIds::GTCManagement, columnHeader, GTCManagementColumns::CancelAllAsk, "Cancel ALL Asks", false, proto::DataType::button);
        addColumnHeaderCell(WindowIds::GTCManagement, columnHeader, GTCManagementColumns::CancelAll, "Cancel ALL", false, proto::DataType::button);

        //Same rows as MM
        auto *rowHeader = definition->mutable_rowheader();
        for (uint32_t contractIndex = 0; contractIndex < _config->_marketMakerConfigs.size(); ++contractIndex) {
            MarketMakerConfig &config = _config->_marketMakerConfigs.at(contractIndex);
            uint32_t internalId = _config->getInternalId(config._contract);
            addRowHeaderCell(WindowIds::GTCManagement, rowHeader, internalId, config._contract);
        }

        if(!tryPopulateResetGridCells(*definition->mutable_cells())) {
            BOOST_ASSERT_MSG(false, "Shouldn't be possible for this to return false because allowGTCs is only check which we've already done");
            throw std::runtime_error("tryPopulateResetGridCells on GUI init returned false.  Should not be possible.");
        }
    }

    bool GUIThings::tryPopulateIOPUpdate(const IOPUpdate& iopUpdate, proto::GridUpdate& gridUpdate) {
        if(!allowGTCs()) {
            return false;
        }

        bool throughBid = iopUpdate.forceCrossed | (iopUpdate.ticksThroughBid == INT32_MIN ? false : iopUpdate.ticksThroughBid >= 0);
        bool throughAsk = iopUpdate.forceCrossed | (iopUpdate.ticksThroughAsk == INT32_MIN ? false : iopUpdate.ticksThroughAsk >= 0);

        const auto color = [](bool requiresHighlighting) { return requiresHighlighting ? toUInt(Color::TRANSPARENT_RED) : toUInt(Color::TRANSPARENT_GREEN); };
        const auto ttString = [](int ticksThrough) { return ticksThrough == INT32_MIN ? std::string{} : std::to_string(ticksThrough); };

        gridUpdate.set_gridid(toUInt(WindowIds::GTCManagement));
        gridUpdate.set_updatetype(proto::GridUpdateType::CellsType);
        auto* cells = gridUpdate.mutable_cells();
        setCell(cells, iopUpdate._internalId, GTCManagementColumns::IOP, std::to_string(iopUpdate.price));
        setCell(cells, iopUpdate._internalId, GTCManagementColumns::NumBid, std::to_string(iopUpdate.numBidGTCs));
        setCell(cells, iopUpdate._internalId, GTCManagementColumns::NumOffer, std::to_string(iopUpdate.numOfferGTCs));
        setCell(cells, iopUpdate._internalId, GTCManagementColumns::NumTotal, std::to_string(iopUpdate.numBidGTCs + iopUpdate.numOfferGTCs));
        setCell(cells, iopUpdate._internalId, GTCManagementColumns::TicksThroughBid, ttString(iopUpdate.ticksThroughBid), color(throughBid));
        setCell(cells, iopUpdate._internalId, GTCManagementColumns::isCrossedBid, std::to_string(toUInt(throughBid ? YesNoType::Yes : YesNoType::No)), color(throughBid));
        setCell(cells, iopUpdate._internalId, GTCManagementColumns::isCrossedAsk, std::to_string(toUInt(throughAsk ? YesNoType::Yes : YesNoType::No)), color(throughAsk));
        setCell(cells, iopUpdate._internalId, GTCManagementColumns::TicksThroughAsk, ttString(iopUpdate.ticksThroughAsk), color(throughAsk));

        LOG_IOP(0, "GTCGrid tryPopulateIOPUpdate (1/2) Contract={C[i1]} price={i2} numBidGTCs={i3} numOfferGTCs={i4} ticksThroughBid={i5} ticksThroughAsk={i6} forceCrossed={i7}",
            iopUpdate._internalId, iopUpdate.price, iopUpdate.numBidGTCs, iopUpdate.numOfferGTCs, iopUpdate.ticksThroughBid, iopUpdate.ticksThroughAsk, iopUpdate.forceCrossed);
        LOG_IOP(0, "GTCGrid tryPopulateIOPUpdate (2/2) Contract={C[i1]} throughBid={i2} throughAsk={i3} colorThroughBid={i4} colorThroughAsk={i5}",
                iopUpdate._internalId, throughBid, throughAsk, color(throughBid), color(throughAsk));

        return true;
    }

    bool GUIThings::tryPopulateResetGTCGrid(proto::GridUpdate& gridUpdate) {
        if(!allowGTCs()) {
            return false;
        }

        gridUpdate.set_gridid(toUInt(WindowIds::GTCManagement));
        gridUpdate.set_updatetype(proto::GridUpdateType::CellsType);

        LOG_IOP(0, "GTCGrid tryPopulateResetGTCGrid {i1}", 0);

        return tryPopulateResetGridCells(*gridUpdate.mutable_cells());
    }

    bool GUIThings::tryPopulateAuctionMDUpdate(const AuctionMDUpdate& update, proto::GridUpdate& gridUpdate) {
        if(!allowGTCs()) {
            return false;
        }

        gridUpdate.set_gridid(toUInt(WindowIds::GTCManagement));
        gridUpdate.set_updatetype(proto::GridUpdateType::CellsType);
        auto *cells = gridUpdate.mutable_cells();
        setCell(cells, update._internalId, GTCManagementColumns::PreOpenBid, std::to_string(update._bid));
        setCell(cells, update._internalId, GTCManagementColumns::PreOpenAsk, std::to_string(update._offer));

        LOG_IOP(0, "GTCGrid tryPopulateAuctionMDUpdate contract={C[i1]} bid={i2} ask={i3}", update._internalId, update._bid, update._offer);
        return true;
    }

    bool GUIThings::tryPopulateNumGTCUpdate(const GenericMessage& update, proto::GridUpdate& gridUpdate) {
        if(!allowGTCs()) {
            return false;
        }

        const auto& internalId = update._number_1;
        const auto& numBid = update._number_2;
        const auto& numAsk = update._number_3;

        gridUpdate.set_gridid(toUInt(WindowIds::GTCManagement));
        gridUpdate.set_updatetype(proto::GridUpdateType::CellsType);
        auto *cells = gridUpdate.mutable_cells();
        setCell(cells, internalId, GTCManagementColumns::NumBid, std::to_string(numBid));
        setCell(cells, internalId, GTCManagementColumns::NumOffer, std::to_string(numAsk));
        setCell(cells, internalId, GTCManagementColumns::NumTotal, std::to_string(numBid + numAsk));

        LOG_IOP(0, "GTCGrid tryPopulateNuMGTCUpdate contract={C[i1]} numBid={i2} numAsk={i3}", internalId, numBid, numAsk);
        return true;
    }

    template<typename GridCellContainerType>
    bool GUIThings::tryPopulateResetGridCells(GridCellContainerType& cells) {
        if(!allowGTCs()) {
            return false;
        }

        auto setTransparentCell = [this, &cells](uint32_t internalId, auto columnType, const std::string& value) {
            setCell(&cells, internalId, columnType, value, toUInt(Color::TRANSPARENT));
        };

        for (uint32_t contractIndex = 0; contractIndex < _config->_marketMakerConfigs.size(); ++contractIndex) {
            MarketMakerConfig& config = _config->_marketMakerConfigs.at(contractIndex);
            uint32_t internalId = _config->getInternalId(config._contract);
            setTransparentCell(internalId, GTCManagementColumns::PreOpenBid, std::to_string(hf::NULL_PRICE));
            setTransparentCell(internalId, GTCManagementColumns::PreOpenAsk, std::to_string(hf::NULL_PRICE));
            setTransparentCell(internalId, GTCManagementColumns::IOP, std::to_string(hf::NULL_PRICE));
            setTransparentCell(internalId, GTCManagementColumns::NumBid, NoValue<proto::DataType::unsigned_type>());
            setTransparentCell(internalId, GTCManagementColumns::NumOffer, NoValue<proto::DataType::unsigned_type>());
            setTransparentCell(internalId, GTCManagementColumns::NumTotal, NoValue<proto::DataType::unsigned_type>());
            setTransparentCell(internalId, GTCManagementColumns::CancelAllBid, config._contract);
            setTransparentCell(internalId, GTCManagementColumns::Cancel_2_Bid, config._contract);
            setTransparentCell(internalId, GTCManagementColumns::Cancel_1_Bid, config._contract);
            setTransparentCell(internalId, GTCManagementColumns::TicksThroughBid, NoValue<proto::DataType::signed_type>());
            setTransparentCell(internalId, GTCManagementColumns::isCrossedBid, std::to_string(toUInt(YesNoType::Unknown)));
            setTransparentCell(internalId, GTCManagementColumns::isCrossedAsk, std::to_string(toUInt(YesNoType::Unknown)));
            setTransparentCell(internalId, GTCManagementColumns::TicksThroughAsk, NoValue<proto::DataType::signed_type>());
            setTransparentCell(internalId, GTCManagementColumns::Cancel_1_Ask, config._contract);
            setTransparentCell(internalId, GTCManagementColumns::Cancel_2_Ask, config._contract);
            setTransparentCell(internalId, GTCManagementColumns::CancelAllAsk, config._contract);
            setTransparentCell(internalId, GTCManagementColumns::CancelAll, config._contract);
        }
        return true;
    }
}
