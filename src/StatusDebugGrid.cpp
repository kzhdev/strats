//
// Created by cmilutinovic on 4/5/20.
//

#include "GUIThings.h"
#include "FrontSide.h"

namespace Python {

    COLD
    void GUIThings::tryPopulateStatusDebugGridColumns(Python::proto::GUIInitialization& message)
    {
        if(!allowStatusGrid()) {
            return;
        }

        auto* definition = message.mutable_definitions()->Add();

        definition->set_gridid(toUInt(WindowIds::StatusDebug));
        definition->set_name("Status Debug");
        definition->set_type(proto::GridType::EntryTable);

        auto* columnHeader = definition->mutable_columnheader();

        addColumnHeaderCell(WindowIds::StatusDebug, columnHeader, StatusDebugColumns::Contract, "Contract", false, proto::DataType::enum_type, "contracts");
        addColumnHeaderCell(WindowIds::StatusDebug, columnHeader, StatusDebugColumns::StatusEvent, "Status Event", false, proto::DataType::enum_type, "status_debug_types");

        auto buttons = definition->mutable_buttons();
        addButton(buttons, toUInt(0), "Submit", proto::ButtonFrontBehavior_Clear, proto::ButtonBackBehavior_Send, RequiresConfirmation::NO);
        addButton(buttons, toUInt(1), "Clear", proto::ButtonFrontBehavior_Clear, proto::ButtonBackBehavior_Nothing, RequiresConfirmation::NO);
    }

    COLD
    void GUIThings::addStatusDebugEnum(proto::EnumDefinition* definition) {
        definition->set_name("status_debug_types");
        auto members = definition->mutable_members();
        addEnumMember(members, "PreOpen", toUInt(StatusDebugType::PreOpen));
        addEnumMember(members, "Open", toUInt(StatusDebugType::Open));
        addEnumMember(members, "Close", toUInt(StatusDebugType::Close));
        addEnumMember(members, "CME_NoCancel", toUInt(StatusDebugType::CME_NoCancel));
        addEnumMember(members, "CME_VelocityHalt", toUInt(StatusDebugType::CME_VelocityHalt));
        addEnumMember(members, "CME_VelocityHaltEND", toUInt(StatusDebugType::CME_VelocityHaltEnd));
    }

    COLD
    void FrontSide::sendStatusDebugUpdatesToMM(const GridUpdate& update) {
        LOG_DECLARE();

        auto& outMessage = *static_cast<ViperMessage *>(_outToMM->getPoolP()->getNextP());
        outMessage._type = ViperMessageType::DebugStatuses;
        DebugStatusEvents& events = reinterpret_cast<DebugStatusEvents &>(outMessage._data[0]);
        events._numDebugStatusEvents = 0;


        //We're going to have to sort these events by when it would have happened, and need a way to interleave correctly
        //since a command can generate multiple events
        struct StatusUpdate {
            int rowNum;
            uint64_t timeOffset;
            DebugStatusEvent debugStatusEvent;
        };
        std::vector<StatusUpdate> updates;

        int numRows = update._update.cells_size() / 2;
        for(int i = 0; i < numRows; ++i) {
            ConfigContract& contract = _config._allContracts[std::atoi(update._update.cells(i*2).value().c_str())];
            StatusDebugType sdType = static_cast<StatusDebugType>(std::atoi(update._update.cells((i*2)+1).value().c_str()));
            LOG_INFO("Received StatusDebug request. Contract=" + contract._name + " StatusDebugType=" + StatusDebugTypeToString(sdType));

            BOOST_ASSERT(contract._venue == hf::VENUE::CME || contract._venue == hf::VENUE::ICE);
            auto cmeds = [&contract](uint8_t extStatus, uint8_t cmeSecurityTradingEvent) {
                return DebugStatusEvent{ ._sid = contract._sid, ._venue = contract._venue, ._extStatus = extStatus, ._cmeSecurityTradingEvent = cmeSecurityTradingEvent };
            };
            auto iceds = [&contract](char extStatus) {
                return DebugStatusEvent{._sid = contract._sid, ._venue=contract._venue, ._extStatus = static_cast<uint8_t>(extStatus), ._cmeSecurityTradingEvent = std::numeric_limits<decltype(DebugStatusEvent::_cmeSecurityTradingEvent)>::max() };
            };
            const auto ONE_MINUTE = Python::TimeUtils::ONE_MINUTE;
            const auto ONE_SECOND = Python::TimeUtils::ONE_SECOND;
            const auto ONE_NS = Python::TimeUtils::ONE_NS;

            if(contract._venue == hf::VENUE::CME) {
                switch(sdType) {
                    case StatusDebugType ::PreOpen: {
                        updates.emplace_back(StatusUpdate{i, (ONE_MINUTE * 45) + (ONE_NS * 104199), cmeds(21, 4)});
                    }
                    break;
                    case StatusDebugType::Open: {
                        const auto ot = (ONE_MINUTE * 60);
                        updates.emplace_back(StatusUpdate{i, ot + ONE_NS*210334, cmeds(15, 0)}); // New Price Indication
                        updates.emplace_back(StatusUpdate{i, ot + ONE_NS*213000, cmeds(17, 0)}); // Ready to trade
                        updates.emplace_back(StatusUpdate{i, ot + ONE_NS*222466, cmeds(103, 5)}); // Implieds On
                        updates.emplace_back(StatusUpdate{i, ot + ONE_NS*222730, cmeds(103, 5)}); // Implieds On
                        updates.emplace_back(StatusUpdate{i, ot + ONE_NS*222931, cmeds(103, 5)}); // Implieds On
                    }
                    break;
                    case StatusDebugType::Close: {
                        updates.emplace_back(StatusUpdate{i, ONE_NS * 067134, cmeds(4, 0)}); //Close
                        updates.emplace_back(StatusUpdate{i, ONE_NS*78516, cmeds(103, 6)}); //Implieds off
                        updates.emplace_back(StatusUpdate{i, ONE_NS*78780, cmeds(103, 6)}); //Implieds off
                        updates.emplace_back(StatusUpdate{i, ONE_NS*78975, cmeds(103, 6)}); //Implieds off
                    }
                    break;
                    case StatusDebugType::CME_NoCancel: {
                        updates.emplace_back(StatusUpdate{i, (ONE_MINUTE * 59) + (ONE_SECOND * 30) + (ONE_NS*85870), cmeds(21, 1)}); //No Cancel
                    }
                    break;
                    //IMPORTANT NOTE: These will generate implieds on/off but need a good example
                    case StatusDebugType::CME_VelocityHalt: {
                        updates.emplace_back(StatusUpdate{i, 0, cmeds(21, 0)}); // Pre-open
                    }
                    case StatusDebugType::CME_VelocityHaltEnd: {
                        //I don't know why this generates a "PreOpen" event before the open event, but it does.
                        updates.emplace_back(StatusUpdate{i, 30 * ONE_SECOND, cmeds(21, 0)}); // Pre-open, sent again
                        updates.emplace_back(StatusUpdate{i, (30 * ONE_SECOND) + (ONE_NS * 6), cmeds(17, 0)}); // Ready to trade
                    }
                    break;
                }

            }
            else if(contract._venue == hf::VENUE::ICE) {
                switch(sdType) {
                    case StatusDebugType::PreOpen: {
                        //TODO; Best guess, needs to be confirmed
                        updates.emplace_back(StatusUpdate{i, 1, iceds('1')}); // Pre-open
                    }
                    break;
                    case StatusDebugType::Open: {
                        //TODO: Best guess, needs to be confirmed
                        updates.emplace_back(StatusUpdate{i, 2, iceds('O')}); // Open
                    }
                    break;
                    case StatusDebugType::Close: {
                        updates.emplace_back(StatusUpdate{i, 0, iceds('C')}); // Close
                    }
                    break;
                    case StatusDebugType::CME_VelocityHalt: FALLTHROUGH
                    case StatusDebugType::CME_VelocityHaltEnd: FALLTHROUGH
                    case StatusDebugType::CME_NoCancel:
                        LOG_ERROR("Cannot handle cme-specific event type for ICE product");
                        break;
                }
            }
        }

        //Sort by time, then by row.
        std::sort(std::begin(updates), std::end(updates), [](const auto& lhs, const auto& rhs) {
           if(lhs.timeOffset < rhs.timeOffset) { return true; }
           if(lhs.rowNum < rhs.rowNum) { return true; }
           return false;
        });

        events._numDebugStatusEvents = updates.size();
        DebugStatusEvent* event = events._statusEvents;
        for(const auto& update : updates) {
            *event = update.debugStatusEvent;
            ++event;
        }

        const auto outIdx = _outToMM->getPoolP()->append(sizeof(ViperMessageType) + sizeof(DebugStatusEvents) + (events._numDebugStatusEvents * sizeof(DebugStatusEvent)));
        mb();
        _outToMM->appendNew(outIdx);
    }
}