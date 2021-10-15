#ifdef VIPER_BACKTEST

#include "Backtester.h"
#include "src/FrontSide.h"

namespace Python  {

static rhone::util::ResourceRegistrator<BackTester> s_registrator("BackTester");
static BackTester* s_inst = nullptr;

}

using namespace Python;

BackTester* BackTester::get() noexcept {
    return s_inst;
}

void BackTester::onStratBegin() {
    s_inst = this;
    _begin = clockUtil::GetCycles();
    for (auto& contract : _viperConfig._allContracts) {
        _limitBooks[contract._internalId].init(contract._name, contract._internalId, contract._sid);
    }
    Executor::onStratBegin();
    auto _frontSide = reinterpret_cast<FrontSide*>(getResource(ResourceId::FrontSide));
    _frontSide->onOtherRequest(ViperMessageType::Login);
}

void BackTester::onStratIdle() {
    if (_event_queue.empty()) {
        return;
    }

    auto event = _event_queue.top();
    _event_queue.pop();

    switch (event->_type) {
        case ViperMessageType::MD_Add_Order: {
            auto& e = reinterpret_cast<md::MDAddOrderEvent&>(event->_data[0]);
            handleMDAddOrder(e, event->_venue);
            break;
        }
        case ViperMessageType::MD_Modify_Order: {
            auto& e = reinterpret_cast<md::MDModifyOrderEvent&>(event->_data[0]);
            handleMDModifyOrder(e, event->_venue);
            break;
        }
        case ViperMessageType::MD_Delete_Order: {
            auto& e = reinterpret_cast<md::MDDeleteOrderEvent&>(event->_data[0]);
            handleMDDeleteOrder(e, event->_venue);
            break;
        }
        case ViperMessageType::MD_Status: {
            md::MDBatch batch;
            auto& e = reinterpret_cast<const md::MDStatusEvent&>(event->_data[0]);
            batch.venue = event->_venue;
            Executor::onMDStatus(e, batch);
            break;
        }
        case ViperMessageType::MD_TradeDetails: {
            auto& e = reinterpret_cast<md::MDTradeDetailsEvent&>(event->_data[0]);
            handleMDTradeDetails(e, event->_venue);
            break;
        }
        case ViperMessageType::MD_UpdateToLimitBook: {
            auto& e = reinterpret_cast<md::MDUpdateEvent&>(event->_data[0]);
            handleMDUpdate(e, event->_venue);
            break;
        }
        case ViperMessageType::MD_UpdateToStrategy: {
            auto& e = reinterpret_cast<md::MDUpdateEvent&>(event->_data[0]);
            sendMDUpdateToStrategy(e, event->_venue);
            break;
        }
        case ViperMessageType::OM_SubmitOrder: {
            auto& e = reinterpret_cast<const om::pr::SendOmsOrderNewEvent&>(event->_data[0]);
            submitOrder(e);
            break;
        }
        case ViperMessageType::OM_ModifyOrder: {
            break;
        }
        case ViperMessageType::OM_CancelOrder: {
            break;
        }
        case ViperMessageType::OM_OrderAccepted: {
            break;
        }
        case ViperMessageType::OM_OrderCanceled: {
            break;
        }
        case ViperMessageType::OM_OrderExecution: {
            break;
        }
        default:
            break;
    }
}

void BackTester::sendMDUpdateToStrategy(const md::MDUpdateEvent& event, hf::VENUE venue) {

}


#endif