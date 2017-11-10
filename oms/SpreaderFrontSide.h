//
// Created by pkusch on 12/26/16.
//

#ifndef RHONEOMS_SPREADERFRONTSIDE_H
#define RHONEOMS_SPREADERFRONTSIDE_H

#include <hf/Service.h>
#include <hf/hf.h>
#include "SpreaderClientAcceptor.h"
#include <md/sym/SymbolsResource.h>
#include <md/queue/MDQueue.h>
#include <hf/log.h>
#include <strat/Context.hpp>
#include "AlgoManager.h"
#include <functional>

namespace oms { namespace spreader  {

template <typename ContextT, typename ClientAcceptorT, typename SymbolsResourceT>
class SpreaderFrontSide : public virtual hf::Service {
public:
    typedef SpreaderFrontSide<ContextT, ClientAcceptorT, SymbolsResourceT> MeT;

    SpreaderFrontSide() {
    }

    void onCreate(const libconfig::Setting& config);

    void log(const std::string& s);

    void onPostCreate(const libconfig::Setting& config);

    void init(SpreaderQueue* inQueue, SpreaderQueue* outQueue, ContextT* context, SpreaderType type, std::string& netIf, std::string& host, int port, hf::VENUE venue) {
        _outQueue = outQueue;
        _clientAcceptor->initialize(inQueue, type, netIf, host, port);
        _algoManager->setInQueue(inQueue);
        _algoManager->setStrategyContext(context);
        _algoManager->setFrontCallback(std::bind(&MeT::handleAlgoMessage, this, std::placeholders::_1));
        _algoManager->setVenue(venue);
        _algoManager->setType(type);
        _spreaderType = type;
        _venue = venue;
    }

    void setVenue(hf::VENUE venue) {
        _venue = venue;
    }

    hf::VENUE getVenue() {
        return _venue;
    }

    void handleAlgoMessage(AlgoMessage* message)  {
        switch (message->_type) {
            case AlgoMessageType::CoverOrder: {
                SpreadCoverIds & info = reinterpret_cast<SpreadCoverIds &> (message->_data[0]);
                log(boost::str(boost::format("Send Cover Order OrigId=%1% CoverId=%2%") % info._original % info._cover));
                _clientAcceptor->sendCoverOrder(info._original, info._cover, info._price);
                break;
            }
            case AlgoMessageType::Theo: {
                IntUpdate& info = reinterpret_cast<IntUpdate &> (message->_data[0]);
                _clientAcceptor->sendSMATheo(info._id, info._update);
                break;
            }
            case AlgoMessageType::SMACancelled: {
                IntUpdate& info = reinterpret_cast<IntUpdate &> (message->_data[0]);
                _clientAcceptor->sendSpreadStatus(0, info._id, SpreadMessageType::CancelSpreadOrderReply, ErrorCode::Success , "");
                break;
            }
            case AlgoMessageType::SMAResumed: {
                IntUpdate& info = reinterpret_cast<IntUpdate &> (message->_data[0]);
                _clientAcceptor->sendSpreadStatus(0, info._id, SpreadMessageType::ResumeSpreadOrderReply, ErrorCode::Success , "");
                break;
            }
            case AlgoMessageType::SMAPaused: {
                IntUpdate& info = reinterpret_cast<IntUpdate &> (message->_data[0]);
                _clientAcceptor->sendSpreadStatus(0, info._id, SpreadMessageType::PauseSpreadOrderReply, ErrorCode::Success , "");
                break;
            }
            case AlgoMessageType::OrdePrice: {
                IntUpdate& info = reinterpret_cast<IntUpdate &> (message->_data[0]);
                _clientAcceptor->sendSpreadOrderPrice(info._id, info._update);
                break;
            }
        }
    }

    void onRun() override;

    void processOutgoingMessages() {
        while(true) {
            uint64_t idx = _outQueue->getNext();

            if (BOOST_UNLIKELY(idx == 0))
                return;

            _outQueue->freeNext();

            auto &message = _outQueue->pool()->getMessage(idx);

            switch (message._type) {
                case SpreadMessageType::LegsCrossInfo: {
                    LegsCrossInfo & info = reinterpret_cast<LegsCrossInfo &> (message._data[0]);
                    _clientAcceptor->sendLegsCrossInfo(info);
                    break;
                }
                case SpreadMessageType::ModifySpreadOrderReply:
                case SpreadMessageType::UnsolicitedSpreadPause:
                case SpreadMessageType::DiscardCrossedOrderReply:
                case SpreadMessageType::AcceptCrossedOrderReply:
                case SpreadMessageType::CancelSpreadOrderReply:
                case SpreadMessageType::NewSpreadOrderReply:
                case SpreadMessageType::PauseSpreadOrderReply:
                case SpreadMessageType::SpreadFinished:
                case SpreadMessageType::ResumeSpreadOrderReply: {
                    SpreadOrderReply &reply = reinterpret_cast<SpreadOrderReply &> (message._data[0]);
                    std::string errorMessage = std::string(reply._errorMessage);
                    _clientAcceptor->sendSpreadStatus(reply._messageId, reply._id, message._type, reply._errorCode, errorMessage);
                    _algoManager->handleSpreadOrderReply(message._type, reply);
                    if (_spreaderType == SpreaderType::Master && message._type == SpreadMessageType::UnsolicitedSpreadPause) {
                        _clientAcceptor->forwardUnsolicitedPauseToSlave(reply._id);
                    }
                    break;
                }
                case SpreadMessageType::Price: {
                    SpreadPrice &price = reinterpret_cast<SpreadPrice &> (message._data[0]);
                    _clientAcceptor->sendSpreadPrice(price);
                    break;
                }
                case SpreadMessageType::LogoutReply:
                case SpreadMessageType::OnClientErrorReply: {
                    SpreadOrderReply &reply = reinterpret_cast<SpreadOrderReply &> (message._data[0]);
                    _clientAcceptor->handleEndOfTrading(reply, message._type);
                    break;
                }
                case SpreadMessageType::LegOrderStatus: {
                    IndividualLegOrderStatus &status = reinterpret_cast<IndividualLegOrderStatus &> (message._data[0]);
                    const hf::symbol_info_t &info = m_symbolsResourceP->getSymbol(status._sid);
                    log(boost::str(boost::format("Front Leg status. gid=%1% sid=%2% Price=%3% Qty=%4%") % info.gid % status._sid % status._price % status._qty));
                    _clientAcceptor->sendLegOrderStatus(status, info.gid);
                    if (_spreaderType == SpreaderType::Master) {
                        if (status._action == LegOrderAction::Fill) {
                            _clientAcceptor->sendFillToRemoteSpreader(status._spreadId, info.gid, status._qty);
                        }
                    }
                    break;
                }
                case SpreadMessageType::ForwardSpreadToSlave: {
                    SpreadOrderReply &reply = reinterpret_cast<SpreadOrderReply &> (message._data[0]);
                    _clientAcceptor->forwardSpreadToSlave(reply._id);
                    break;
                }
                case SpreadMessageType::LoginReply: {
                    log(boost::str(boost::format("Sent LoginReply")));
                    break;
                }
                case SpreadMessageType::GatewayStatus: {
                    OMStatus& status = reinterpret_cast<OMStatus&> (message._data[0]);
                    if (_spreaderType != SpreaderType::Master) {
                        _clientAcceptor->sendGatewayStatus(status._venue, status._id, status._active);
                    } else {
                        _algoManager->_connectedGateways.push_back({status._venue, status._id, status._active});
                    }
                    break;
                }
                case SpreadMessageType::InstrumentStatus: {
                    InstrumentMdStatus &status = reinterpret_cast<InstrumentMdStatus &> (message._data[0]);
                    const hf::symbol_info_t &info = m_symbolsResourceP->getSymbol(status._sid);
                    _clientAcceptor->sendInstrumentMdStatus(status._venue, info.gid, status._status);
                    break;
                }
                case SpreadMessageType::ClipDone: {
                    ClipDone& clipDone = reinterpret_cast<ClipDone &> (message._data[0]);
                    _algoManager->handleClipDone(clipDone._id, clipDone._spreadDone, clipDone._price);
                    break;
                }
                default:
                    BOOST_ASSERT(false);
            }
        }
    }

    AlgoManager<ContextT>* _algoManager = nullptr;
    ClientAcceptorT* _clientAcceptor = nullptr;
    SpreaderQueue* _outQueue = nullptr;
    SymbolsResourceT* m_symbolsResourceP = nullptr;
    hf::VENUE _venue = hf::VENUE::UNKNOWN;
    SpreaderType _spreaderType;
};

}}

#endif //RHONEOMS_SPREADERFRONTSIDE_H
