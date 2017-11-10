//
// Created by pkusch on 1/18/17.
//

#ifndef RHONEOMSTEST_SPREADERFIXTURE_H
#define RHONEOMSTEST_SPREADERFIXTURE_H

#include "CommonHelper.h"
#include "TestSpreaderClientsAcceptor.h"
#include "TestSymbolsResource.h"

namespace oms { namespace spreader {

typedef SyntheticSpreadStrategy<TestStrategy, 10, TestSpreaderClientsAcceptor, TestSymbolsResource> TestSpreader;
typedef SyntheticSpreadStrategy<TestStrategy, 2, TestSpreaderClientsAcceptor, TestSymbolsResource> TestSpreader2;
typedef AlgoManager<TestStrategy::Context> TestAlgoManager;
typedef SpreaderFrontSide<TestStrategy::Context, TestSpreaderClientsAcceptor, TestSymbolsResource> TestSpreaderFrontSide;

struct spread_leg_data_t {
    double div;
    double fac;
    hf::qty_t iq;
    hf::qty_t tq;
    bool quote;
    hf::qty_t qf;
    hf::symbol_gid_t gid;
    hf::qty_t payup;
    hf::qty_t hammer;
    LeanPricingType lpt;
    InitiationQuantityType iqt;
    double inMult;
    hf::qty_t _syntheticAdjustment = 0;
    hf::qty_t _syntheticMinSize = 0;
};

class SpreaderFixture : public ::testing::Test {
protected:
    SpreaderFixture() : _venue(hf::VENUE::CME), _spreaderType(SpreaderType::Standalone) {}
    SpreaderFixture(hf::VENUE venue, SpreaderType spreaderType) : _venue(venue), _spreaderType(spreaderType) {}

    void SetUp() override {
        _testSpreader = new TestSpreader();

        _testSpreader->_type = _spreaderType;
        _testSpreader->_venue = _venue;

        _testAlgoManager = new TestAlgoManager();
        _testAlgoManager->setInQueue(_testSpreader->_inQueue);
        _testAlgoManager->setStrategyContext(&_testSpreader->context());
        _testSpreaderFrontSide = new TestSpreaderFrontSide();
        _testSpreaderFrontSide->_algoManager = _testAlgoManager;

        std::string netIf = "";
        std::string host = "";
        _testSpreaderFrontSide->init(_testSpreader->_inQueue, _testSpreader->_outQueue, &_testSpreader->context(), _spreaderType, netIf, host, 0, _venue);
        _testSpreaderFrontSide->_clientAcceptor = new TestSpreaderClientsAcceptor("", 0);
    }

    void TearDown() override{
    }

    struct level_data_t {
        double pr;
        hf::qty_t q;
        md::order_count_t o;
    };

    proto::NewSpreadOrder createSpreadOrder(hf::SIDE side, hf::price_t adjustor, hf::qty_t clips, hf::qty_t lotSize) {
        proto::NewSpreadOrder message;
        message.set_side(toOms(side));
        message.set_adjustor(adjustor);
        message.set_numberofclips(clips);
        message.set_clipsize(lotSize);
        message.set_quoteoffset(1000);
        return message;
    }

    void addLeg(proto::NewSpreadOrder& message, double div, double fac, hf::qty_t iq, hf::qty_t tq, bool quote, hf::qty_t qf, hf::symbol_gid_t gid, hf::qty_t payup, hf::qty_t hammer,
                proto::LeanPricingType lpt, proto::InitiationQuantityType iqt, double inMult) {
        proto::SpreadLegDetails* leg = message.add_legs();
        leg->set_pricedivisor(div);
        leg->set_pricefactor(fac);
        leg->set_initialqty(iq);
        leg->set_triggerqty(tq);
        leg->set_activequoting(quote);
        leg->set_quantityfactor(qf);
        leg->set_contractid(gid);
        leg->set_payupticks(payup);
        leg->set_thorhammersize(hammer);
        leg->set_leanpricingtype(lpt);
        leg->set_initiationquantitytype(iqt);
        leg->set_initiationmultiplier(inMult);
    }

    void addManualEntryAlgo(proto::NewSpreadOrder& message, hf::price_t price) {
        auto entryAlgo = message.mutable_entryalgo();
        entryAlgo->set_type(oms::proto::EntryAlgoType::Manual);
        oms::proto::ManualEntryAlgo* manual = new oms::proto::ManualEntryAlgo();
        entryAlgo->set_allocated_manual(manual);
        manual->set_price(price);
    }

    void addSMAEntryAlgo(proto::NewSpreadOrder& message, double offset, uint32_t numberOfPeriods, uint32_t periodSize) {
        auto entryAlgo = message.mutable_entryalgo();
        entryAlgo->set_type(oms::proto::EntryAlgoType::SMA);
        oms::proto::SMAEntryAlgo* sma = new oms::proto::SMAEntryAlgo();
        entryAlgo->set_allocated_sma(sma);
        sma->set_offset(toRhonePrice(offset));
        sma->set_periodsize(periodSize);
        sma->set_numberofperiods(numberOfPeriods);
    }

    void addNoExitAlgo(proto::NewSpreadOrder& message) {
        auto exitAlgo = message.mutable_exitalgo();
        exitAlgo->set_type(oms::proto::ExitAlgoType::None);
    }

    void addCoverExitAlgo(proto::NewSpreadOrder& message, bool coverClips, bool workBothSides, hf::price_t edge, hf::price_t decayEdge, uint32_t decayPeriod) {
        auto exitAlgo = message.mutable_exitalgo();
        exitAlgo->set_type(oms::proto::ExitAlgoType::Autocover);
        oms::proto::AutocoverExitAlgo* cover = new oms::proto::AutocoverExitAlgo();
        exitAlgo->set_allocated_autocover(cover);
        cover->set_coverclips(coverClips);
        cover->set_workbothsides(workBothSides);
        cover->set_edge(edge);
        cover->set_decayedge(decayEdge);
        cover->set_decayperiod(decayPeriod);
    }

    void createNewSpreadMessageTest(char *orderPtr, hf::price_t price, hf::SIDE side, hf::price_t adjustor, hf::qty_t clips, hf::qty_t lotSize, hf::qty_t maxTicks, bool skipCrossTest,
                                spread_leg_data_t *legsData, uint nLegs, hf::VENUE venue = hf::VENUE::UNKNOWN) {

        Spread &order = reinterpret_cast<Spread&>(*orderPtr);

        order._order._clientOrderId = ++_orderId;
        order._order._price = toRhonePrice(price);
        order._order._side = side;
        order._messageId = 0;

        order._definition._adjustor = toRhonePrice(adjustor);
        order._order._numberOfClips = clips;
        order._order._allowedNumberOfClips = clips;
        order._order._lotSize = lotSize;
        order._definition._nLegs = nLegs;
        order._definition._venue = venue;

        spread_leg_data_t *legsDataCurrent = legsData;
        for(uint i(0); i < nLegs; ++i) {
            LegDefinition& leg = reinterpret_cast<LegDefinition &>(order._definition._legs[sizeof(LegDefinition) * i]);
            leg._divisor = legsDataCurrent->div;
            leg._factor = legsDataCurrent->fac;
            leg._initiationQuantity = legsDataCurrent->iq;
            leg._triggerQuantity = legsDataCurrent->tq;
            leg._toQuote = legsDataCurrent->quote;
            leg._quantityFactor = legsDataCurrent->qf;
            leg._gid = legsDataCurrent->gid;
            leg._nPayUpTicks = legsDataCurrent->payup;
            leg._nTicksToDoLastCross = legsDataCurrent->hammer;
            leg._leanPricingType = legsDataCurrent->lpt;
            leg._initiationQuantityType = legsDataCurrent->iqt;
            leg._initiationMultiplier = legsDataCurrent->inMult;

            ++legsDataCurrent;
        }

        order._order._numberOfTicksFromTopToQuote = maxTicks;

        order._order._skipCrossTest = skipCrossTest;
    }

    void createNewSpreadMessage(hf::price_t price, hf::SIDE side, hf::price_t adjustor, hf::qty_t clips, hf::qty_t lotSize, hf::qty_t maxTicks, bool skipCrossTest,
                                spread_leg_data_t *legsData, uint nLegs, hf::VENUE venue = hf::VENUE::UNKNOWN) {

        auto &inMessage = _testSpreader->_inQueue->pool()->getNextMessage();
        inMessage._type = SpreadMessageType::NewSpreadOrder;
        Spread &order = reinterpret_cast<Spread &> (inMessage._data[0]);

        order._order._clientOrderId = ++_orderId;
        order._order._price = toRhonePrice(price);
        order._order._side = side;
        order._messageId = 0;

        order._definition._adjustor = toRhonePrice(adjustor);
        order._order._numberOfClips = clips;
        order._order._allowedNumberOfClips = clips;
        order._order._lotSize = lotSize;
        order._definition._nLegs = nLegs;
        order._definition._venue = venue;

        spread_leg_data_t *legsDataCurrent = legsData;
        for(uint i(0); i < nLegs; ++i) {
            LegDefinition& leg = reinterpret_cast<LegDefinition &>(order._definition._legs[sizeof(LegDefinition) * i]);
            leg._divisor = legsDataCurrent->div;
            leg._factor = legsDataCurrent->fac;
            leg._initiationQuantity = legsDataCurrent->iq;
            leg._triggerQuantity = legsDataCurrent->tq;
            leg._toQuote = legsDataCurrent->quote;
            leg._quantityFactor = legsDataCurrent->qf;
            leg._gid = legsDataCurrent->gid;
            leg._nPayUpTicks = legsDataCurrent->payup;
            leg._nTicksToDoLastCross = legsDataCurrent->hammer;
            leg._leanPricingType = legsDataCurrent->lpt;
            leg._initiationQuantityType = legsDataCurrent->iqt;
            leg._initiationMultiplier = legsDataCurrent->inMult;
            leg._syntheticAdjustment = legsDataCurrent->_syntheticAdjustment;
            leg._syntheticMinSize = legsDataCurrent->_syntheticMinSize;

            ++legsDataCurrent;
        }

        order._order._numberOfTicksFromTopToQuote = maxTicks;

        order._order._skipCrossTest = skipCrossTest;

        const auto outIdx = _testSpreader->_inQueue->pool()->append(sizeof(Spread) + sizeof(SpreadMessageType));

        mb();

        _testSpreader->_inQueue->appendNew(outIdx);
    }

    void createNewSpreadMessage(hf::price_t price, hf::SIDE side, hf::price_t adjustor, hf::qty_t clips, hf::qty_t lotSize,
                                double div1, double fac1, hf::qty_t iq1, hf::qty_t tq1, bool quote1, hf::qty_t qf1, hf::symbol_gid_t gid1, hf::qty_t payup1, hf::qty_t hammer1,
                                LeanPricingType lpt1, InitiationQuantityType iqt1, double inMult1,
                                double div2, double fac2, hf::qty_t iq2, hf::qty_t tq2, bool quote2, hf::qty_t qf2, hf::symbol_gid_t gid2, hf::qty_t payup2, hf::qty_t hammer2,
                                LeanPricingType lpt2, InitiationQuantityType iqt2, double inMult2, hf::VENUE venue = hf::VENUE::UNKNOWN) {
        auto &inMessage = _testSpreader->_inQueue->pool()->getNextMessage();
        inMessage._type = SpreadMessageType::NewSpreadOrder;
        Spread &order = reinterpret_cast<Spread &> (inMessage._data[0]);

        order._order._clientOrderId = ++_orderId;
        order._order._price = toRhonePrice(price);
        order._order._side = side;
        order._messageId = 0;

        order._definition._adjustor = toRhonePrice(adjustor);
        order._order._numberOfClips = clips;
        order._order._allowedNumberOfClips = clips;
        order._order._lotSize = lotSize;
        order._definition._nLegs = 2;
        order._definition._venue = venue;

        LegDefinition& legOne = reinterpret_cast<LegDefinition &>(order._definition._legs[0]);
        legOne._divisor = div1;
        legOne._factor = fac1;
        legOne._initiationQuantity = iq1;
        legOne._triggerQuantity = tq1;
        legOne._toQuote = quote1;
        legOne._quantityFactor = qf1;
        legOne._gid = gid1;
        legOne._nPayUpTicks = payup1;
        legOne._nTicksToDoLastCross = hammer1;
        legOne._leanPricingType = lpt1;
        legOne._initiationQuantityType = iqt1;
        legOne._initiationMultiplier = inMult1;

        LegDefinition& legTwo = reinterpret_cast<LegDefinition &>(order._definition._legs[sizeof(LegDefinition)]);
        legTwo._divisor = div2;
        legTwo._factor = fac2;
        legTwo._initiationQuantity = iq2;
        legTwo._triggerQuantity = tq2;
        legTwo._toQuote = quote2;
        legTwo._quantityFactor = qf2;
        legTwo._gid = gid2;
        legTwo._nPayUpTicks = payup2;
        legTwo._nTicksToDoLastCross = hammer2;
        legTwo._leanPricingType = lpt2;
        legTwo._initiationQuantityType = iqt2;
        legTwo._initiationMultiplier = inMult2;

        order._order._numberOfTicksFromTopToQuote = 1000;

        order._order._skipCrossTest = true;

        const auto outIdx = _testSpreader->_inQueue->pool()->append(sizeof(Spread) + sizeof(SpreadMessageType));

        mb();

        _testSpreader->_inQueue->appendNew(outIdx);
    }

    void createNewSpreadMessage(hf::price_t price, hf::SIDE side, hf::price_t adjustor, hf::qty_t clips, hf::qty_t lotSize, hf::qty_t maxTicks,
                                double div1, double fac1, hf::qty_t iq1, hf::qty_t tq1, bool quote1, hf::qty_t qf1, hf::symbol_gid_t gid1, hf::qty_t payup1, hf::qty_t hammer1, LeanPricingType lpt1, InitiationQuantityType iqt1, double inMult1,
                                double div2, double fac2, hf::qty_t iq2, hf::qty_t tq2, bool quote2, hf::qty_t qf2, hf::symbol_gid_t gid2, hf::qty_t payup2, hf::qty_t hammer2, LeanPricingType lpt2, InitiationQuantityType iqt2, double inMult2,
                                double div3, double fac3, hf::qty_t iq3, hf::qty_t tq3, bool quote3, hf::qty_t qf3, hf::symbol_gid_t gid3, hf::qty_t payup3, hf::qty_t hammer3, LeanPricingType lpt3, InitiationQuantityType iqt3, double inMult3,
                                hf::VENUE venue = hf::VENUE::UNKNOWN
    ) {
        auto &inMessage = _testSpreader->_inQueue->pool()->getNextMessage();
        inMessage._type = SpreadMessageType::NewSpreadOrder;
        Spread &order = reinterpret_cast<Spread &> (inMessage._data[0]);

        order._order._clientOrderId = ++_orderId;
        order._order._price = toRhonePrice(price);
        order._order._side = side;
        order._messageId = 0;

        order._definition._adjustor = toRhonePrice(adjustor);
        order._order._numberOfClips = clips;
        order._order._allowedNumberOfClips = clips;
        order._order._lotSize = lotSize;
        order._definition._nLegs = 3;
        order._definition._venue = venue;

        LegDefinition& legOne = reinterpret_cast<LegDefinition &>(order._definition._legs[0]);
        legOne._divisor = div1;
        legOne._factor = fac1;
        legOne._initiationQuantity = iq1;
        legOne._triggerQuantity = tq1;
        legOne._toQuote = quote1;
        legOne._quantityFactor = qf1;
        legOne._gid = gid1;
        legOne._nPayUpTicks = payup1;
        legOne._nTicksToDoLastCross = hammer1;
        legOne._leanPricingType = lpt1;
        legOne._initiationQuantityType = iqt1;
        legOne._initiationMultiplier = inMult1;

        LegDefinition& legTwo = reinterpret_cast<LegDefinition &>(order._definition._legs[sizeof(LegDefinition)]);
        legTwo._divisor = div2;
        legTwo._factor = fac2;
        legTwo._initiationQuantity = iq2;
        legTwo._triggerQuantity = tq2;
        legTwo._toQuote = quote2;
        legTwo._quantityFactor = qf2;
        legTwo._gid = gid2;
        legTwo._nPayUpTicks = payup2;
        legTwo._nTicksToDoLastCross = hammer2;
        legTwo._leanPricingType = lpt2;
        legTwo._initiationQuantityType = iqt2;
        legTwo._initiationMultiplier = inMult2;

        LegDefinition& legThree = reinterpret_cast<LegDefinition &>(order._definition._legs[sizeof(LegDefinition) * 2]);
        legThree._divisor = div3;
        legThree._factor = fac3;
        legThree._initiationQuantity = iq3;
        legThree._triggerQuantity = tq3;
        legThree._toQuote = quote3;
        legThree._quantityFactor = qf3;
        legThree._gid = gid3;
        legThree._nPayUpTicks = payup3;
        legThree._nTicksToDoLastCross = hammer3;
        legThree._leanPricingType = lpt3;
        legThree._initiationQuantityType = iqt3;
        legThree._initiationMultiplier = inMult3;

        order._order._numberOfTicksFromTopToQuote = maxTicks;

        const auto outIdx = _testSpreader->_inQueue->pool()->append(sizeof(Spread) + sizeof(SpreadMessageType));

        mb();

        _testSpreader->_inQueue->appendNew(outIdx);
    }


    void createOtherRequest(SpreadMessageType mt, int id) {
        _testAlgoManager->processOtherRequest(mt, id, 0);
    }

    hf::price_t toRhonePrice(double price) {
        return price * 10000000;
    }

    double toDollars(hf::price_t price) {
        return static_cast<double>(price) / 10000000.0;
    }

    hf::symbol_id_t getSid(TestSpreader::SyntheticSpreadT* spread, uint legIndex) {
        return spread->getLegByIndex(legIndex)._sid;
    }

    bool areEqual(const level_data_t &bookUserData, const md::top_t::level_t &bookLevelData) {
        return toRhonePrice(bookUserData.pr) == bookLevelData.pr && bookUserData.q == bookLevelData.q && bookUserData.o == bookLevelData.o;
    }

    uint getBookChangeLevel(TestSpreader::SyntheticSpreadT* spread, uint legIndex, hf::SIDE side, const level_data_t *bookData, uint levels = 1) {
        auto *books = spread->getLegByIndex(legIndex)._books;
        auto &book = side == hf::SIDE::BUY ? books->_bidBook : books->_askBook;
        uint i(0);
        for(; i < levels; ++i) {
            if( !areEqual(*bookData, book.l[i])) return i + 1;
        }
        ++bookData;
        return 0;
    }

    void sendAskBidMD(hf::symbol_id_t sid, int changeSell, int changeBuy, level_data_t *bookDataSell, level_data_t *bookDataBuy, uint levelsSell = 1, uint levelsBuy = 1) {
        md::MDUpdateEvent event = createAskMDEvent(sid, changeSell, bookDataSell, levelsSell);
        populateMDEvent<hf::SIDE::BUY>(event, sid, changeBuy, bookDataBuy, levelsBuy);
        _testSpreader->onMDUpdate(hf::VENUE::CME, event);
    }

    void sendAskMD(hf::symbol_id_t sid, int change, level_data_t *bookData, uint levels = 1) {
        md::MDUpdateEvent event = createAskMDEvent(sid, change, bookData, levels);
        _testSpreader->onMDUpdate(hf::VENUE::CME, event);
    }

    void sendBidMD(hf::symbol_id_t sid, int change, level_data_t *bookData, uint levels = 1) {
        md::MDUpdateEvent event = createBidMDEvent(sid, change, bookData, levels);
        _testSpreader->onMDUpdate(hf::VENUE::CME, event);
    }

    void sendBidMD(hf::symbol_id_t sid, double price, int qty) {
        md::MDUpdateEvent event = createBidMDEvent(sid, 1, price, qty, hf::NULL_PRICE, 0, hf::NULL_PRICE, 0);
        _testSpreader->onMDUpdate(hf::VENUE::CME, event);
    }

    void sendBidMD(hf::symbol_id_t sid, int change, double price1, int qty1, double price2, int qty2, double price3, int qty3) {
        md::MDUpdateEvent event = createBidMDEvent(sid, change, price1, qty1, price2, qty2, price3, qty3);
        _testSpreader->onMDUpdate(hf::VENUE::CME, event);
    }

    void sendBidMDToAlgo(hf::symbol_id_t sid, double price, int qty) {
        md::MDUpdateEvent event = createBidMDEvent(sid, 1, price, qty, hf::NULL_PRICE, 0, hf::NULL_PRICE, 0);
        _testAlgoManager->onMDUpdate(hf::VENUE::CME, event);
    }

    template<hf::SIDE side>
    md::MDUpdateEvent::side_t &getSide(md::MDUpdateEvent &event) {
        return side == hf::SIDE::BUY ? event.buy : event.sell;
    }

    template<hf::SIDE side>
    md::MDUpdateEvent::side_t &getSideOpposite(md::MDUpdateEvent &event) {
        return side == hf::SIDE::BUY ? event.sell : event.buy;
    }

    template<hf::SIDE side>
    md::MDUpdateEvent populateMDEvent(md::MDUpdateEvent &event, hf::symbol_id_t sid, int change, level_data_t *bookData, uint levels = 1) {
        auto &eventSide = getSide<side>(event);

        event.type = md::MDUpdateEvent::TYPE::UPDATE;
        event.sid = sid;
        eventSide.updatedQtyNum = change;
        uint i(0);
        for(; i < levels; ++i) {
            eventSide.top.l[i].pr = toRhonePrice(bookData->pr);
            eventSide.top.l[i].q = bookData->q;
            eventSide.top.l[i].o = bookData->o;
            ++bookData;
        }

        for(; i < 10; ++i) {
            eventSide.top.l[i].pr = hf::NULL_PRICE;
            eventSide.top.l[i].q = 0;
            eventSide.top.l[i].o = 0;
        }

        event.implied = false;
        return event;
    }

    template<hf::SIDE side>
    md::MDUpdateEvent createMDEvent(hf::symbol_id_t sid, int change, level_data_t *bookData, uint levels = 1) {
        md::MDUpdateEvent event;
        populateMDEvent<side>(event, sid, change, bookData, levels);
        getSideOpposite<side>(event).updatedQtyNum = 0;
        return event;
    }

    md::MDUpdateEvent createAskMDEvent(hf::symbol_id_t sid, int change, level_data_t *bookData, uint levels = 1) {
        return createMDEvent<hf::SIDE::SELL>(sid, change, bookData, levels);
    }

    md::MDUpdateEvent createBidMDEvent(hf::symbol_id_t sid, int change, level_data_t *bookData, uint levels = 1) {
        return createMDEvent<hf::SIDE::BUY>(sid, change, bookData, levels);
    }

    md::MDUpdateEvent createBidMDEvent(hf::symbol_id_t sid, int change, double price1, int qty1, double price2, int qty2, double price3, int qty3) {
        md::MDUpdateEvent event;
        event.type = md::MDUpdateEvent::TYPE::UPDATE;
        event.sid = sid;
        event.buy.updatedQtyNum = change;
        event.buy.top.l[0].pr = toRhonePrice(price1);
        event.buy.top.l[0].q = qty1;
        event.buy.top.l[1].pr = toRhonePrice(price2);
        event.buy.top.l[1].q = qty2;
        event.buy.top.l[2].pr = toRhonePrice(price3);
        event.buy.top.l[2].q = qty3;
        event.buy.top.l[3].pr = hf::NULL_PRICE;
        event.buy.top.l[3].q = 0;
        event.sell.updatedQtyNum = 0;
        event.implied = false;
        return event;
    }

    void sendAskMD(hf::symbol_id_t sid, double price, int qty) {
        md::MDUpdateEvent event = createAskMDEvent(sid, price, qty);
        _testSpreader->onMDUpdate(hf::VENUE::CME, event);
    }

    void sendAskMDToAlgo(hf::symbol_id_t sid, double price, int qty) {
        md::MDUpdateEvent event = createAskMDEvent(sid, price, qty);
        _testAlgoManager->onMDUpdate(hf::VENUE::CME, event);
    }

    md::MDUpdateEvent createAskMDEvent(hf::symbol_id_t sid, double price, int qty) {
        md::MDUpdateEvent event;
        event.type = md::MDUpdateEvent::TYPE::UPDATE;
        event.sid = sid;
        event.sell.updatedQtyNum = 1;
        event.sell.top.l[0].pr = toRhonePrice(price);
        event.sell.top.l[0].q = qty;
        event.sell.top.l[1].pr = hf::NULL_PRICE;
        event.sell.top.l[1].q = 0;
        event.buy.updatedQtyNum = 0;
        event.implied = false;
        return event;
    }
    void sendOrderRejected(om::order_t& order, uint32_t reasonCode = 0) {
        om::OMOrderRejectedEvent event;
        order.is_active = false; //done in onOMBatch
        event.reasonCodeExt = reasonCode;
        _testSpreader->onOrderRejected(event, order);
    }

    void sendOrderAccepted(om::order_t& order) {
        om::OMOrderAcceptedEvent event;
        order.is_acked = true;
        _testSpreader->onOrderAccepted(event, order);
    }

    void sendOrderModified(om::order_t& order, double price, hf::qty_t qty) {
        om::OMOrderModifiedEvent event;
        event.acceptedPrice = toRhonePrice(price);
        event.acceptedQty = qty;

        order.is_modifying = false;

        _testSpreader->onOrderReplaced(event, order);
    }

    void sendOrderModifiedRejected(om::order_t& order, uint32_t reasonCodeExt = 0) {
        om::OMOrderModifyRejectedEvent event;
        event.reasonCodeExt = reasonCodeExt;

        order.is_modifying = false;

        _testSpreader->onOrderReplaceRejected(event, order);
    }

    void sendPartialFill(om::order_t& order, hf::qty_t qty, bool isActive = true) { //is active should be set by exchange
        om::OMExecutionEvent event;
        event.filledPrice = order.pr;
        event.filledQty = qty;
        order.executedQty += qty;

        if(!isActive ) order.is_active = false;
        else {
            if (order.is_modifying || order.is_cancelling)
                order.is_active = true;
            else
                order.is_active = order.executedQty < order.qty;
        }
        _testSpreader->onOrderExecution(event, order);
    }

    void sendFullFill(om::order_t& order, hf::qty_t qty) {
        om::OMExecutionEvent event;
        event.filledPrice = order.pr;
        event.filledQty = qty;
        order.is_active = false;
        _testSpreader->onOrderExecution(event, order);
    }

    void sendCancelled(om::order_t& order, uint32_t reasonCode = 0) {
        om::OMOrderCanceledEvent event;
        event.reasonCodeExt = reasonCode;
        order.is_active = false;
        _testSpreader->onOrderCanceled(event, order);
    }

    void sendCancelRejected(om::order_t& order, uint32_t reasonCode) {
        om::OMOrderCancelRejectedEvent event;
        event.reasonCodeExt = reasonCode;
        order.is_active = false;
        _testSpreader->onOrderCancelRejected(event, order);
    }

    void setEnabled(hf::VENUE venue, hf::symbol_info_t &symbol)
    {
        switch(venue) {
            case hf::VENUE::CME:
                symbol.cme.enabled = true;
                break;
            case hf::VENUE::ICE:
                symbol.ice.enabled = true;
                break;
            case hf::VENUE::EURONEXT:
                symbol.euronext.enabled = true;
                break;
            case hf::VENUE::EUREX:
                symbol.eurex.enabled = true;
                break;
            default:
                BOOST_ASSERT(false);
        }
    }

    uint getOrderIsNotInBookCode(hf::VENUE venue) const {
        switch(venue) {
            case hf::VENUE::CME:
                return 2045;
            case hf::VENUE::ICE:
                return 2045;
            case hf::VENUE::EUREX:
                return 2045;
            case hf::VENUE ::EURONEXT:
                return 2045;
            default:
                BOOST_ASSERT(false);
        }
    }

    /*
    template<bool waitNewAckExpected, bool waitModifyAckExpected>
    void MarketMove_NoAck_Test();

    template<bool waitNewAckExpected, bool waitModifyAckExpected>
    void MarketMove_NoAck_Ack_Test();
     */

    TestSpreader* _testSpreader;
    TestAlgoManager* _testAlgoManager;
    TestSpreaderFrontSide* _testSpreaderFrontSide;
    int _orderId = 0;

public:
    hf::VENUE _venue;
    SpreaderType _spreaderType;
};

        /*
template<hf::VENUE venue, SpreaderType spreaderType>
class SpreaderFixtureTyped : public SpreaderFixture {
public:
    SpreaderFixtureTyped() : SpreaderFixture(venue, spreaderType)  {}
};
        */

template<bool waitNewAckExpected, bool waitModifyAckExpected>
class SpreaderFixtureTypedLight : public SpreaderFixture {
public:
    SpreaderFixtureTypedLight(hf::VENUE venue, SpreaderType spreaderType) : SpreaderFixture(venue, spreaderType) {}

    void MarketMove_NoAck_Test();
    void MarketMove_NoAck_Ack_Test();
    void MarketMove_UserCancelled_Test();
    void MarketMove_UnsolicitedCancelled_Test();
    void CancelModifying_Test();
    void CancelImmediate_Test();
    void NO_Rejected_Test();
    void Modify_Rejected_Test();
    void Modify_RejectedFilled_Test();
    void CancelRejected_Test();
    void UnsolicitedCancelRejected_Test();
    void QuoteToHedge_Test();
    void HedgeModifying_1_Test();
    void HedgeModifying_2_Test();
    void HedgeModifying_3_Test();
    void HedgeModifying_11_Test();

    inline uint GetOrderIsNotInBookCode() const {
        switch(_venue) {
            case hf::VENUE::EUREX:
                return EUREX_ORDER_NOT_FOUND;
            case hf::VENUE ::EURONEXT:
                return EURONEXT_UNKNOWN_ORDER;
            case hf::VENUE ::CME:
                return CME_ORDER_IS_NOT_IN_BOOK;
            case hf::VENUE ::ICE:
                return ICE_TOO_LATE_TO_CANCEL;
            default:
                return 0;
        }
    }
};

template<hf::VENUE venue, SpreaderType spreaderType, bool waitNewAckExpected, bool waitModifyAckExpected>
class SpreaderFixtureTyped : public SpreaderFixtureTypedLight<waitNewAckExpected, waitModifyAckExpected> {
public:
    SpreaderFixtureTyped() : SpreaderFixtureTypedLight<waitNewAckExpected, waitModifyAckExpected>(venue, spreaderType) {
        BOOST_ASSERT(waitNewAckExpected == getWaitForNewOrderAck(venue));
        BOOST_ASSERT(waitModifyAckExpected == getWaitForModifyAck(venue));
    }
};


}}

#endif //RHONEOMSTEST_SPREADERFIXTURE_H
