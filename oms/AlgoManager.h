//
// Created by pkusch on 3/2/17.
//

#ifndef RHONEOMS_ENTRYALGOMANAGER_H
#define RHONEOMS_ENTRYALGOMANAGER_H

#include <md/queue/MDQueue.h>
#include "messages/omsproto.pb.h"
#include "SpreaderQueues.h"
#include "CommonHelper.h"
#include "SpreadCommon.h"
#include "EnLegSpread.h"
#include "ClockUtil.h"

namespace oms { namespace spreader {

static InitiationQuantityType GetInitiationQuantityType(proto::InitiationQuantityType type) {
    switch (type) {
        case proto::InitiationQuantityType::Absolute:
            return InitiationQuantityType::Absolute;
        case proto::InitiationQuantityType::Relative:
            return InitiationQuantityType::Relative;
        default:
            throw std::runtime_error("unrecognized initiation quantity type");
    }
}

static LeanPricingType GetLeanPricingType(proto::LeanPricingType type) {
    switch (type) {
        case proto::LeanPricingType::FullLevel:
            return LeanPricingType::FullLevel;
        case proto::LeanPricingType::TopLevel:
            return LeanPricingType::TopLevel;
        case proto::LeanPricingType::TopOrSecond:
            return LeanPricingType::TopOrSecond;
        case proto::LeanPricingType::WeightedAvg:
            return LeanPricingType::WeightedAvg;
        case proto::LeanPricingType::SyntheticTop:
            return LeanPricingType::SyntheticTop;
        default:
            throw std::runtime_error("unrecognized lean pricing type");
    }
}

static void PopulateLegDefinition(const proto::SpreadLegDetails& leg, LegDefinition& definition) {
    definition._divisor = leg.pricedivisor();
    definition._factor = leg.pricefactor();
    definition._triggerQuantity = leg.triggerqty();
    definition._toQuote = leg.activequoting();
    definition._quantityFactor = leg.quantityfactor();
    definition._gid = leg.contractid();
    definition._nPayUpTicks = leg.payupticks();
    definition._nTicksToDoLastCross = leg.thorhammersize();
    definition._leanPricingType = GetLeanPricingType(leg.leanpricingtype());
    definition._initiationQuantityType = GetInitiationQuantityType(leg.initiationquantitytype());

    if (definition._initiationQuantityType == InitiationQuantityType::Absolute)
        definition._initiationQuantity = leg.initialqty();

    if (definition._initiationQuantityType == InitiationQuantityType::Relative)
        definition._initiationMultiplier = leg.initiationmultiplier();

    if (definition._leanPricingType == LeanPricingType::SyntheticTop) {
        definition._syntheticAdjustment = leg.synthetictopadjustment();
        definition._syntheticMinSize = leg.synthetictopminsize();
    }
}

static void PopulateNewSpread(Spread &order, proto::NewSpreadOrder& message, int id, message_id_t messageId, hf::price_t price, hf::SIDE side, int allowedNumberOfClips, bool skipCrossTest) {
    order._messageId = messageId;
    order._order._clientOrderId = id;

    order._order._price = price;
    order._order._numberOfTicksFromTopToQuote = message.quoteoffset();
    order._order._side = side;
    order._order._lotSize = message.clipsize();
    order._order._numberOfClips = message.numberofclips();
    order._order._allowedNumberOfClips = allowedNumberOfClips;
    order._order._test = message.testorder();
    order._order._skipCrossTest = skipCrossTest;

    order._definition._adjustor = message.adjustor();
    order._definition._nLegs = message.legs().size();
    int legOffset = 0;
    for (const proto::SpreadLegDetails &leg : message.legs()) {
        LegDefinition &legDefinition = reinterpret_cast<LegDefinition &>(order._definition._legs[legOffset]);
        PopulateLegDefinition(leg, legDefinition);
        legOffset += sizeof(LegDefinition);
    }
}

static void PopulateModifyLegDefinition(const proto::ModifySpreadLeg& leg, LegDefinition& definition) {
    definition._initiationQuantity = leg.has_initialqty() ? leg.initialqty() : NULL_QTY;
    definition._triggerQuantity = leg.has_triggerqty() ? leg.triggerqty() : NULL_QTY;
    definition._toQuote = leg.has_activequoting() ? (leg.activequoting() ? 1 : 0) : -1;
    definition._gid = leg.contractid();
    definition._nPayUpTicks = leg.has_payupticks() ? leg.payupticks() : hf::NULL_PRICE;
    definition._nTicksToDoLastCross = leg.has_thorhammersize() ? leg.thorhammersize() : hf::NULL_PRICE;
}

static int PopulateModifySpread(Spread &order, proto::ModifySpreadOrder& message, int id, hf::price_t price, hf::SIDE side, bool setNumberOfClips) {
    order._order._clientOrderId = id;
    order._order._price = price;
    order._order._numberOfTicksFromTopToQuote = message.has_maxticks() ? message.maxticks() : hf::NULL_PRICE;
    order._order._side = side;
    order._order._lotSize = message.has_clipsize() ? message.clipsize() : NULL_QTY;
    order._order._numberOfClips = message.has_numberofclips() ? message.numberofclips() : NULL_QTY;
    order._order._allowedNumberOfClips = setNumberOfClips ? message.numberofclips() : NULL_QTY;
    order._order._test = false;
    order._order._skipCrossTest = true;

    order._definition._nLegs = message.modifiedlegs().size();

    if (message.modifiedlegs().size() > 0) {
        int legOffset = 0;
        for (const proto::ModifySpreadLeg& leg : message.modifiedlegs()) {
            LegDefinition& legDefinition = reinterpret_cast<LegDefinition&>(order._definition._legs[legOffset]);
            PopulateModifyLegDefinition(leg, legDefinition);
            legOffset += sizeof(LegDefinition);
        }
    }
    return message.modifiedlegs().size();
}

static const uint64_t ONE_SECOND = 1000000000ll;

class FakeExecutor {
public:
    void createMarketData(hf::symbol_id_t sid, Leg& leg) {
        Books* books = new Books();

        auto& bidBook = books->_bidBook;
        bidBook.l[0].pr = hf::NULL_PRICE;
        bidBook.l[1].pr = hf::NULL_PRICE;
        bidBook.l[2].pr = hf::NULL_PRICE;
        bidBook.l[3].pr = hf::NULL_PRICE;
        bidBook.l[4].pr = hf::NULL_PRICE;

        auto& askBook = books->_askBook;
        askBook.l[0].pr = hf::NULL_PRICE;
        askBook.l[1].pr = hf::NULL_PRICE;
        askBook.l[2].pr = hf::NULL_PRICE;
        askBook.l[3].pr = hf::NULL_PRICE;
        askBook.l[4].pr = hf::NULL_PRICE;

        leg.setBooks(books);
    }
};

typedef EnLegSpread<FakeExecutor> SpreadT;

template <typename ManagerT>
class CoverExitAlgo {
public:
    CoverExitAlgo(ManagerT& manager, int id, int origId, proto::NewSpreadOrder& message, double frequency)
    :
        _manager(manager),
        _id(id),
        _origId(origId),
        _message(message)
    {
        _period = calculateDecayPeriod(message.exitalgo().autocover().decayperiod(), frequency);
    }

    uint64_t calculateDecayPeriod(uint64_t periodInSecons, double frequency) {
        return static_cast<double>(periodInSecons * ONE_SECOND) * frequency;
    }

    bool handleOtherCancelled() {
        if (!_startedCovering) {
            return true;
        } else {
            if (!_pauseInfo._isPaused) {
                _manager.setNumberOfClipsToAllowedClips(_id);
            } else {
                _pauseInfo._wasOtherCancelled = true;
            }
            return false;
        }
    }

    void handleCancelled() {
        if (_startedCovering) {
            _manager.setNumberOfClipsToAllowedClips(_origId);
        }
    }

    void poll(uint64_t now) {
        if (!_pauseInfo._isPaused) {
            if (_canDecay) {
                if (now >= _nextDecayTime) {
                    _manager.changeSpreadPriceBy(_id, getDecayAmount());
                    setNextDecayTime(now);
                }
            }
        }
    }

    void handleClipDone(bool spreadDone) {
        if (shouldCoverClips()) {
            _canDecay = false;
        } else {
            if (!spreadDone) {
                _manager.setSpreadPrice(_id, getCoverPrice());
            }
        }
    }

    void handleOtherSpreadClipDone(bool spreadDone, hf::price_t spreadPrice) {
        _manager.log(boost::str(boost::format("Handling clip done in Autocover")));
        _basePrice = spreadPrice;
        if (_pauseInfo._isPaused) {
            _pauseInfo._receivedNewClip = true;
            _pauseInfo._spreadDone = spreadDone;
        } else {
            executeClip(spreadDone);
        }
        _originalSpreadDone = spreadDone;
    }

    void pause() {
        _pauseInfo._isPaused = true;
    }

    bool isOriginalSpreadDone() { return _originalSpreadDone; }

    void resume() {
        if (_pauseInfo._receivedNewClip) {
            executeClip(_pauseInfo._spreadDone);
        } else if (_canDecay) {
            resumeDecaying();
        }

        if (_pauseInfo._wasOtherCancelled) {
            _manager.setNumberOfClipsToAllowedClips(_id);
        }

        _pauseInfo.reset();
    }

    int getId() { return _id; }
    int getOrigId() { return _origId; }

    void update(const proto::AutocoverExitAlgo& update, double frequency) {
        _message.mutable_exitalgo()->mutable_autocover()->set_decayedge(update.decayedge());
        _period = calculateDecayPeriod(update.decayperiod(), frequency);
        _message.mutable_exitalgo()->mutable_autocover()->set_edge(update.edge());
    }

private:
    void executeClip(bool spreadDone) {
        _manager.log(boost::str(boost::format("Executing clip.")));
        startDecay();
        if (shouldCoverClips()) {
            if (!_startedCovering) {
                _manager.log(boost::str(boost::format("Sending cover order ids")));
                _manager.sendCoverSpreadOrder(_message, _id, _origId, _messageId, getCoverPrice(), getCoverSide(), 1, true);
                _startedCovering = true;
            } else {
                _manager.setSpreadPrice(_id, getCoverPrice());
                _manager.incrementAllowedNumberOfClips(_id);
            }
        } else {
            if (spreadDone) {
                _manager.log(boost::str(boost::format("Sending cover order ids")));
                _manager.sendCoverSpreadOrder(_message, _id, _origId, _messageId, getCoverPrice(), getCoverSide(), _message.numberofclips(), true);
            }
        }
    }

    void resumeDecaying() {
        setNextDecayTime(clockUtil::GetCycles());
    }

    struct PauseInfo {
        bool _isPaused = false;
        bool _receivedNewClip = false;
        bool _spreadDone = false;
        bool _wasOtherCancelled = false;

        void reset() {
            _isPaused = false;
            _receivedNewClip = false;
            _spreadDone = false;
            _wasOtherCancelled = false;
        }
    };

    hf::price_t getDecayAmount() {
        hf::price_t decayAmount = _message.exitalgo().autocover().decayedge();
        return isCoverBuy() ? decayAmount : -decayAmount;
    }

    void startDecay() {
        _canDecay = true;
        setNextDecayTime(clockUtil::GetCycles());
    }

    void setNextDecayTime(uint64_t now) {
        _nextDecayTime = now + _period;
    }

    bool shouldCoverClips() {
        return _message.exitalgo().autocover().coverclips();
    }

    bool isCoverBuy() {
        return getCoverSide() == hf::SIDE::BUY;
    }

    hf::price_t getCoverPrice() {
        hf::price_t edge = _message.exitalgo().autocover().edge();
        return isCoverBuy() ? _basePrice - edge : _basePrice + edge;
    }

    hf::SIDE getOriginalSide() {
        return toHf(_message.side());
    }

    hf::SIDE getCoverSide() {
        return getOriginalSide() == hf::SIDE::BUY ? hf::SIDE::SELL : hf::SIDE::BUY;
    }

    hf::price_t _basePrice;
    bool _startedCovering = false;
    bool _canDecay = false;
    uint64_t _nextDecayTime = 0;
    uint64_t _period = 0;
    ManagerT& _manager;
    int _id;
    int _origId;
    proto::NewSpreadOrder _message;
    message_id_t _messageId;
    PauseInfo _pauseInfo;
    bool _originalSpreadDone = false;
};

template <typename ManagerT>
class ManualEntryAlgo {
public:
    ManualEntryAlgo(ManagerT& manager, bool sendClipByClip, int id)
    :
        _manager(manager),
        _sendClipByClip(sendClipByClip),
        _id(id)
    {}

    void handleOtherSpreadClipDone(bool spreadDone) {
        if (_sendClipByClip && !spreadDone) {
            _manager.incrementAllowedNumberOfClips(_id);
        }
    }

    ManagerT& _manager;
    const bool _sendClipByClip;
    const int _id;
};

template <typename ManagerT>
class SMAEntryAlgo {
public:
    SMAEntryAlgo(ManagerT& manager, proto::NewSpreadOrder& message, int id, message_id_t messageId, bool sendClipByClip, double frequency)
    :
        _manager(manager),
        _message(message),
        _id(id),
        _messageId(messageId),
        _sendClipByClip(sendClipByClip)
    {
        _spread = createSpread(message);
        _prices.reserve(message.entryalgo().sma().numberofperiods());
        _period = calculatePeriod(frequency, message.entryalgo().sma().periodsize());
        subscribeMarketData();
        _lastPrice = _spread->getSpreadPriceUsingMidPrice();
        doSMA();
    }

    void handleOtherSpreadClipDone(bool spreadDone) {
        if (_sendClipByClip && !spreadDone) {
            _manager.incrementAllowedNumberOfClips(_id);
        }
    }

    SpreadT* createSpread(proto::NewSpreadOrder& message) {
        std::vector<char> buffer;
        size_t bufferSize = sizeof(Spread) + sizeof(LegDefinition) * message.legs_size();
        buffer.resize(bufferSize);
        Spread& newSpread = reinterpret_cast<Spread &>(buffer[0]);
        PopulateNewSpread(newSpread, message, 0, 0, 0, getSide(), 1, false);
        SpreadT* spread = new SpreadT();
        std::string errorMessage;
        PopulateSpread(newSpread, *_manager.getStrategyContext(), false, spread, _manager._venue, _manager._type, errorMessage);
        return spread;
    }

    void subscribeMarketData() {
        for (uint32_t i = 0; i < _spread->_nLegs; ++i) {
            Leg &leg = _spread->_legs[i];
            _manager.subscribeMarketData(leg, this);
        }
    }

    void unsubscribeMarketData() {
        for (uint32_t i = 0; i < _spread->_nLegs; ++i) {
            Leg &leg = _spread->_legs[i];
            _manager.unsubscribeMarketData(leg, this);
        }
    }

    void onMDUpdate(hf::VENUE venue, const md::MDUpdateEvent &event) {
        Leg* legPtr = _spread->getLeg(event.sid);

        if (legPtr == nullptr)
            return;

        Leg& leg = *legPtr;

        const bool askChanged = event.sell.updatedQtyNum == 1;

        if (askChanged) {
            md::top_t::level_t& top = leg._books->_askBook.l[0];
            top.pr = event.sell.top.l[0].pr;
            top.q = event.sell.top.l[0].q;
        }

        const bool bidChanged = event.buy.updatedQtyNum == 1;

        if (bidChanged) {
            md::top_t::level_t& top = leg._books->_bidBook.l[0];
            top.pr = event.buy.top.l[0].pr;
            top.q = event.buy.top.l[0].q;
        }

        const bool shouldCalculate = askChanged || bidChanged;

        if (shouldCalculate) {
            const hf::price_t price = _spread->getSpreadPriceUsingMidPrice();
            if (price != hf::NULL_PRICE) {
                _lastPrice = price;
                if (!_triggered) {
                    tryTrigger();
                }
            } else {
                flushSMA();
            }
        }
    }

    void poll(uint64_t currentTime) {
        if (currentTime > _nextSampleTime) {
            setNextSampleTime(currentTime);
            doSMA();
        }
    }

    void flushSMA() {
        _sma = hf::NULL_PRICE;
        _prices.clear();
        _pricesIndex = -1;
        _lastPrice = hf::NULL_PRICE;
    }

    void doSMA() {
        updateSMA();
        if (_lastPrice != hf::NULL_PRICE && _sma != hf::NULL_PRICE) {
            if (!_triggered) {
                tryTrigger();
            } else {
                hf::price_t price = getWantedPrice();
                _manager.setSpreadPrice(_id, price);
                _manager.sendSpreadOrderPrice(_id, price);
            }
            _manager.sendSMATheo(_id, _sma);
        }
    }

    void updateSMA() {
        if (_lastPrice != hf::NULL_PRICE) {
            addPrice(_lastPrice);
            calculateSMA();
        }
    }

    void calculateSMA() {
        double total = 0;
        for (auto price : _prices) {
            total += price;
        }
        _sma = total / _prices.size();
    }

    void addPrice(hf::price_t price) {
        _pricesIndex = getNextPricesIndex();
        if (_prices.size() < _message.entryalgo().sma().numberofperiods()) {
            _prices.push_back(price);
        } else {
            _prices[_pricesIndex] = price;
        }
    }

    void update(const proto::SMAEntryAlgo& algo, double frequency) {
        _period = calculatePeriod(frequency, algo.periodsize());

        bool numberOfPeriodsChanged = algo.numberofperiods() != _prices.size();
        if (numberOfPeriodsChanged) {
            adjustPrices(algo.numberofperiods());
            calculateSMA();
            _manager.sendSMATheo(_id, _sma);
        }

        bool offsetChanged = algo.offset() != _message.entryalgo().sma().offset();
        if (offsetChanged) {
            _message.mutable_entryalgo()->mutable_sma()->set_offset(algo.offset());
        }

        if (offsetChanged || numberOfPeriodsChanged) {
            hf::price_t price = getWantedPrice();
            _manager.setSpreadPrice(_id, price);
            _manager.sendSpreadOrderPrice(_id, price);
        }
    }

    void pause() {
        _isPaused = true;
    }

    void resume() {
        _isPaused = false;
        if (!_triggered) {
            tryTrigger();
        }
    }

    bool hasTriggered() { return _triggered; }

    hf::price_t getLastPrice() { return _lastPrice; }
    void setLastPrice(hf::price_t price) { _lastPrice = price; }
    hf::price_t getSMA() { return _sma; }

private:
    hf::SIDE getSide() {
        return toHf(_message.side());
    }

    bool isBuy() {
        return getSide() == hf::SIDE::BUY;
    }

    void tryTrigger() {
        if (!_isPaused) {
            hf::price_t wantedPrice = getWantedPrice();
            if (wantedPrice != hf::NULL_PRICE) {
                _triggered = shouldTrigger(wantedPrice);
                if (_triggered) {
                    int nClips = _sendClipByClip ? 1 : _message.numberofclips();
                    _manager.sendSpreadOrder(_message, _id, _messageId, wantedPrice, getSide(), nClips, true);
                    _manager.sendSpreadOrderPrice(_id, wantedPrice);
                }
            }
        }
    }

    bool shouldTrigger(hf::price_t price) {
        return isBuy() ? _lastPrice <= price : _lastPrice >= price;
    }

    hf::price_t getWantedPrice() {
        if (_sma != hf::NULL_PRICE) {
            const hf::price_t distance = _message.entryalgo().sma().offset();
            return isBuy() ? _sma - distance : _sma + distance;
        }
        return hf::NULL_PRICE;
    }

    void setNextSampleTime(uint64_t currentTime) {
        _nextSampleTime = currentTime + _period;
    }

    int getNextPricesIndex() {
        return _pricesIndex == (int64_t) (_message.entryalgo().sma().numberofperiods() - 1) ? 0 : _pricesIndex + 1;
    }

    void adjustPrices(uint64_t numberOfPeriods) {
        std::vector<hf::price_t> pricesCopy = _prices;
        int nextIndex = _pricesIndex;
        bool pricesFull = pricesCopy.size() == _message.entryalgo().sma().numberofperiods();
        uint64_t oldNumberOfPeriods = _message.entryalgo().sma().numberofperiods();

        _prices.reserve(numberOfPeriods);
        _prices.clear();
        _pricesIndex = -1;
        _message.mutable_entryalgo()->mutable_sma()->set_numberofperiods(numberOfPeriods);

        if (pricesFull) {
            int index = nextIndex;
            do {
                addPrice(pricesCopy[index]);
                index = index == (int64_t) oldNumberOfPeriods - 1 ? 0 : nextIndex + 1;
            } while (_prices.size() < numberOfPeriods && index != nextIndex);
        } else {
            for (int i = 0; i < nextIndex; ++i) {
                addPrice(pricesCopy[i]);
            }
        }
    }

    uint64_t calculatePeriod(double frequency, uint64_t periodSize) {
        return static_cast<double>(periodSize * ONE_SECOND) * frequency;
    }

    ManagerT& _manager;
    SpreadT* _spread = nullptr;
    hf::price_t _sma = hf::NULL_PRICE;
    hf::price_t _lastPrice = hf::NULL_PRICE;
    std::vector<hf::price_t> _prices;
    int _pricesIndex = -1;
    proto::NewSpreadOrder _message;
    int _id;
    message_id_t _messageId;
    uint64_t _nextSampleTime = 0;
    uint64_t _period = 0;
    bool _triggered = false;
    bool _sendClipByClip;
    bool _isPaused = false;
};

template <typename ContextT>
class AlgoManager {

public:
    AlgoManager() {
        _cpuFrequency = clockUtil::GetCpuFrequency();
    }

    void log(const std::string& s);

    typedef CoverExitAlgo<AlgoManager> CoverExitAlgoT;
    typedef SMAEntryAlgo<AlgoManager> SMAEntryAlgoT;
    typedef ManualEntryAlgo<AlgoManager> ManualEntryAlgoT;

    void setInQueue(SpreaderQueue* queue) { _inQueue = queue; }

    void setStrategyContext(ContextT* context) {
        _context = context;
    }

    ContextT* getStrategyContext() {
        return _context;
    }

    void setFrontCallback(std::function<void(AlgoMessage*)> callback) {
        _algoMessages = callback;
    }

    void sendCoverSpreadOrder(proto::NewSpreadOrder& message, int id, int origId, message_id_t messageId, hf::price_t price, hf::SIDE side, int allowedNumberOfClips, bool skipCrossTest) {
        sendSpreadOrder(message, id, messageId, price, side, allowedNumberOfClips, skipCrossTest);
        AlgoMessage* am = new AlgoMessage();
        am->_type = AlgoMessageType::CoverOrder;
        SpreadCoverIds& ids = reinterpret_cast<SpreadCoverIds &>(am->_data[0]);
        ids._cover = id;
        ids._original = origId;
        ids._price = price;
        _algoMessages(am);
        delete am;
    }

    void sendSMATheo(int id, hf::price_t theo) {
        AlgoMessage* am = new AlgoMessage();
        am->_type = AlgoMessageType::Theo;
        IntUpdate& theoPart = reinterpret_cast<IntUpdate &>(am->_data[0]);
        theoPart._id = id;
        theoPart._update = theo;
        _algoMessages(am);
        delete am;
    }

    void sendSpreadReply(int id, AlgoMessageType type) {
        AlgoMessage* am = new AlgoMessage();
        am->_type = type;
        IntUpdate& theoPart = reinterpret_cast<IntUpdate &>(am->_data[0]);
        theoPart._id = id;
        _algoMessages(am);
        delete am;
    }

    void sendSpreadOrder(proto::NewSpreadOrder& message, int id, message_id_t messageId, hf::price_t price, hf::SIDE side, int allowedNumberOfClips, bool skipCrossTest) {
        auto &inMessage = _inQueue->pool()->getNextMessage();
        inMessage._type = SpreadMessageType::NewSpreadOrder;
        Spread &order = reinterpret_cast<Spread &> (inMessage._data[0]);
        PopulateNewSpread(order, message, id, messageId, price, side, allowedNumberOfClips, skipCrossTest);
        const auto outIdx = _inQueue->pool()->append(sizeof(Spread) + sizeof(SpreadMessageType) + (message.legs().size() * sizeof(LegDefinition)));
        mb();
        _inQueue->appendNew(outIdx);
    }

    void sendSpreadOrderPrice(int id, hf::price_t price) {
        AlgoMessage* am = new AlgoMessage();
        am->_type = AlgoMessageType::OrdePrice;
        IntUpdate& theoPart = reinterpret_cast<IntUpdate &>(am->_data[0]);
        theoPart._id = id;
        theoPart._update = price;
        _algoMessages(am);
        delete am;
    }

    ErrorCode processSpreadOrder(proto::NewSpreadOrder& message, int spreadId, message_id_t messageId, int coverSpreadId) {
        if (message.exitalgo().type() == proto::ExitAlgoType::Autocover && message.exitalgo().autocover().workbothsides()) {
            return ErrorCode::WorkBothSidesNotSupported;
        }

        switch (message.entryalgo().type()) {
            case proto::EntryAlgoType::Manual: {
                const bool sendClipByClip = message.exitalgo().type() == proto::ExitAlgoType::Autocover && message.exitalgo().autocover().coverclips();
                int numberOfClips = sendClipByClip ? 1 : message.numberofclips();
                sendSpreadOrder(message, spreadId, messageId, message.entryalgo().manual().price(), toHf(message.side()), numberOfClips, false);
                SpreadInfo info = {message, spreadId, coverSpreadId, messageId};
                _spreadInfos.insert(std::pair<int, SpreadInfo>(spreadId, info));
                break;
            }
            case proto::EntryAlgoType::SMA: {
                createAlgos(message, spreadId, messageId, coverSpreadId);
                break;
            }
        }

        return ErrorCode::Success;
    }

    void processOtherRequest(SpreadMessageType type, int id, message_id_t messageId) {
        auto it = _spreadAlgos.find(id);
        if (it != _spreadAlgos.end()) {
            SpreadAlgos& algos = it->second;

            if (algos._isEntry && algos._entry && !algos._entry->hasTriggered()) {
                switch (type) {
                    case SpreadMessageType::CancelSpreadOrder: {
                        cancelNotTriggeredSMAEntryAlgo(id);
                        break;
                    }
                    case SpreadMessageType::UnsolicitedSpreadPause:
                    case SpreadMessageType::PauseSpreadOrder: {
                        pauseNotTriggeredSMAEntryAlgo(id);
                        break;
                    }
                    case SpreadMessageType::ResumeSpreadOrder: {
                        resumeNotTriggeredSMAEntryAlgo(id);
                        break;
                    }
                    default:
                        break;
                }
            } else {
                sendOtherRequest(type, id, messageId);
            }
        } else {
            // TODO - remove this after fixing unit tests
            sendOtherRequest(type, id, messageId);
        }
    }

    ErrorCode modifySpreadOrder(proto::ModifySpreadOrder& message, int id) {
        bool setNumberOfClips = true;
        auto algoIt = _spreadAlgos.find(id);
        bool hasAlgos = algoIt !=_spreadAlgos.end();
        if (hasAlgos) {
            setNumberOfClips = algoIt->second._exit == nullptr;
        }

        auto &inMessage = _inQueue->pool()->getNextMessage();
        inMessage._type = SpreadMessageType::ModifySpreadOrder;
        Spread &order = reinterpret_cast<Spread &>(inMessage._data[0]);

        hf::price_t price = hf::NULL_PRICE;
        if (message.has_entryalgo() && message.entryalgo().type() == proto::EntryAlgoType::Manual && message.entryalgo().manual().has_price()) {
            price = message.entryalgo().manual().price();
        }

        int nLegs = PopulateModifySpread(order, message, id, price, hf::SIDE::UNKNOWN, setNumberOfClips);

        const auto outIdx = _inQueue->pool()->append(sizeof(Spread) + sizeof(SpreadMessageType) + (nLegs * sizeof(LegDefinition)));
        mb();
        _inQueue->appendNew(outIdx);

        if (hasAlgos) {
            if (message.has_entryalgo()) {
                algoIt->second.updateEntryAlgo(message.entryalgo(), _cpuFrequency);
            }
            if (message.has_exitalgo()) {
                algoIt->second.updateExitAlgo(message.exitalgo(), _cpuFrequency);
            }
        }

        return ErrorCode::Success;
    }

    void cancelNotTriggeredSMAEntryAlgo(int id) {
        auto it = _spreadAlgos.find(id);
        SpreadAlgos& algos = it->second;
        removeEntryAlgo(algos._entry);
        if (algos._exit) {
            removeExitAlgo(algos._exit);
            auto eit = _spreadAlgos.find(algos._exit->getId());
            _spreadAlgos.erase(eit);
        }
        _spreadAlgos.erase(it);
        sendSpreadReply(id, AlgoMessageType::SMACancelled);
    }

    void pauseNotTriggeredSMAEntryAlgo(int id) {
        auto it = _spreadAlgos.find(id);
        SpreadAlgos& algos = it->second;
        algos._entry->pause();
        sendSpreadReply(id, AlgoMessageType::SMAPaused);
    }

    void resumeNotTriggeredSMAEntryAlgo(int id) {
        auto it = _spreadAlgos.find(id);
        SpreadAlgos& algos = it->second;
        algos._entry->resume();
        sendSpreadReply(id, AlgoMessageType::SMAResumed);
    }

    void sendOtherRequest(SpreadMessageType type, int id, message_id_t messageId) {
        auto &inMessage = _inQueue->pool()->getNextMessage();
        inMessage._type = type;
        ClientOrderId &orderId = reinterpret_cast<ClientOrderId &> (inMessage._data[0]);
        orderId._id = id;
        orderId._messageId = messageId;
        const auto outIdx = _inQueue->pool()->append(sizeof(ClientOrderId) + sizeof(SpreadMessageType));
        mb();
        _inQueue->appendNew(outIdx);
    }

    void handleSpreadOrderReply(SpreadMessageType type, SpreadOrderReply& reply) {
        if (reply._errorCode == ErrorCode::Success) {
            switch (type) {
                case SpreadMessageType::AcceptCrossedOrderReply:
                case SpreadMessageType::NewSpreadOrderReply:
                    handleSpreadCreated(reply._id);
                    break;
                case SpreadMessageType::PauseSpreadOrderReply:
                    handleSpreadPaused(reply._id);
                    break;
                case SpreadMessageType::CancelSpreadOrderReply:
                case SpreadMessageType::UnsolicitedCancel:
                    handleSpreadCancelled(reply._id);
                    break;
                case SpreadMessageType::ResumeSpreadOrderReply:
                    handleSpreadResumed(reply._id);
                    break;
                case SpreadMessageType::SpreadFinished:
                    handleSpreadFinished(reply._id);
                    break;
                default :
                    break;
            }
        }

        if (type == SpreadMessageType::UnsolicitedSpreadPause && reply._errorCode == ErrorCode::Unsolicited) {
            handleSpreadPaused(reply._id);
        }
    }

    void handleSpreadCreated(int id) {
        auto it = _spreadInfos.find(id);
        if (it != _spreadInfos.end()) {
            SpreadInfo& info = it->second;
            createAlgos(info._message, info._spreadId, info._messageId, info._coverSpreadId);
            _spreadInfos.erase(it);
        }
    }

    void handleSpreadPaused(int id) {
        auto it = _spreadAlgos.find(id);
        if (it != _spreadAlgos.end()) {
            SpreadAlgos& algos = it->second;
            algos.pause();
        }
    }

    void handleSpreadResumed(int id) {
        auto it = _spreadAlgos.find(id);
        if (it != _spreadAlgos.end()) {
            SpreadAlgos& algos = it->second;
            algos.resume();
        }
    }

    void handleSpreadCancelled(int id) {
        auto it = _spreadAlgos.find(id);
        if (it != _spreadAlgos.end()) {
            SpreadAlgos& algos = it->second;
            if (algos._isEntry) {
                if (algos._exit) {
                    bool done = algos._exit->handleOtherCancelled();
                    if (done) {
                        auto eit = _spreadAlgos.find(algos._exit->getId());
                        if (eit != _spreadAlgos.end()) {
                            _spreadAlgos.erase(eit);
                        }
                        removeExitAlgo(algos._exit);
                    }
                }
                removeEntryAlgo(algos._entry);
            } else {
                if (algos._exit) {
                    algos._exit->handleCancelled();
                }
                removeExitAlgo(algos._exit);

                auto eit = _spreadAlgos.find(algos._exit->getOrigId());
                if (eit != _spreadAlgos.end()) {
                    eit->second._exit = nullptr;
                }
            }
            _spreadAlgos.erase(it);
        }
    }

    void removeExitAlgo(CoverExitAlgoT* algo) {
        if (algo) {
            for (auto it = _exitAlgos.begin(); it != _exitAlgos.end(); ++it) {
                if (*it == algo) {
                    _exitAlgos.erase(it);
                    break;
                }
            }
            delete algo;
        }
    }

    void removeEntryAlgo(SMAEntryAlgoT* algo) {
        if (algo) {
            for (auto it = _entryAlgos.begin(); it != _entryAlgos.end(); ++it) {
                if (*it == algo) {
                    _entryAlgos.erase(it);
                    break;
                }
            }
            algo->unsubscribeMarketData();
            delete algo;
        }
    }

    void removeManualEntryAlgo(ManualEntryAlgoT* algo) {
        if (algo) {
            delete algo;
        }
    }

    void handleSpreadFinished(int id) {
        auto it = _spreadAlgos.find(id);
        if (it != _spreadAlgos.end()) {
            SpreadAlgos &algos = it->second;
            if (algos._isEntry) {
                removeEntryAlgo(algos._entry);
                removeManualEntryAlgo(algos._manualEntry);
            } else {
                removeExitAlgo(algos._exit);
            }
            _spreadAlgos.erase(it);
        }
    }

    void createAlgos(proto::NewSpreadOrder& message, int spreadId, message_id_t messageId, int coverSpreadId) {
        const bool sendClipByClip = message.exitalgo().type() == proto::ExitAlgoType::Autocover && message.exitalgo().autocover().coverclips();

        SpreadAlgos algos;

        switch (message.entryalgo().type()) {
            case proto::EntryAlgoType::Manual: {
                algos._manualEntry = new ManualEntryAlgoT(*this, sendClipByClip, spreadId);
                break;
            }
            case proto::EntryAlgoType::SMA: {
                algos._entry = createSMAEntryAlgo(message, spreadId, messageId, sendClipByClip);
                break;
            }
        }

        switch (message.exitalgo().type()) {
            case proto::ExitAlgoType::None:
                break;
            case proto::ExitAlgoType::Autocover: {
                algos._exit = createAutocoverAlgo(message, coverSpreadId, spreadId);
            }
        }

        algos._isEntry = true;
        _spreadAlgos.insert(std::pair<int, SpreadAlgos>(spreadId, algos));

        if (coverSpreadId != -1) {
            algos._isEntry = false;
            _spreadAlgos.insert(std::pair<int, SpreadAlgos>(coverSpreadId, algos));
        }
    }

    CoverExitAlgoT* createAutocoverAlgo(proto::NewSpreadOrder& message, int id, int origId) {
        CoverExitAlgoT* algo = new CoverExitAlgoT(*this, id, origId, message, _cpuFrequency);
        _exitAlgos.push_back(algo);
        return algo;
    }

    SMAEntryAlgoT* createSMAEntryAlgo(proto::NewSpreadOrder& message, int id, message_id_t messageId, bool sendClipByClip) {
        SMAEntryAlgoT* algo = new SMAEntryAlgoT(*this, message, id, messageId, sendClipByClip, _cpuFrequency);
        _entryAlgos.push_back(algo);
        return algo;
    }

    void onMDUpdate(hf::VENUE venue, const md::MDUpdateEvent &event) {
        auto subcribedSpreads = _marketDataSubscriptions.find(event.sid);
        if (subcribedSpreads != _marketDataSubscriptions.end()) {
            for (auto entryAlgo : subcribedSpreads->second) {
                entryAlgo->onMDUpdate(venue, event);
            }
        }
    }

    uint64_t getTime() {
        return clockUtil::GetCycles();
    }

    void pollEntryAlgos(uint64_t now ) {
        for (auto it = _entryAlgos.begin(); it != _entryAlgos.end(); ++it) {
            auto algo = *it;
            algo->poll(now);
        }
    }

    void pollExitAlgos(uint64_t now ) {
        for (auto algo : _exitAlgos) {
            algo->poll(now);
        }
    }

    void poll() {
        const uint64_t now = getTime();
        pollEntryAlgos(now);
        pollExitAlgos(now);
        processMarketData();
    }

    void changeSpreadPriceBy(int id , hf::price_t priceDelta) {
        auto &inMessage = _inQueue->pool()->getNextMessage();
        inMessage._type = SpreadMessageType::ChangeSpreadPriceBy;
        IntUpdate &priceChange = reinterpret_cast<IntUpdate &> (inMessage._data[0]);
        priceChange._id = id;
        priceChange._update = priceDelta;
        const auto outIdx = _inQueue->pool()->append(sizeof(IntUpdate) + sizeof(SpreadMessageType));
        mb();
        _inQueue->appendNew(outIdx);
    }

    void incrementAllowedNumberOfClips(int id) {
        auto &inMessage = _inQueue->pool()->getNextMessage();
        inMessage._type = SpreadMessageType::IncrementAllowedNumberOfClips;
        ClientOrderId &orderId = reinterpret_cast<ClientOrderId &> (inMessage._data[0]);
        orderId._id = id;
        const auto outIdx = _inQueue->pool()->append(sizeof(ClientOrderId) + sizeof(SpreadMessageType));
        mb();
        _inQueue->appendNew(outIdx);
    }

    void setSpreadPrice(int id, hf::price_t price) {
        auto &inMessage = _inQueue->pool()->getNextMessage();
        inMessage._type = SpreadMessageType::SetSpreadPrice;
        IntUpdate& message = reinterpret_cast<IntUpdate &> (inMessage._data[0]);
        message._id = id;
        message._update = price;
        const auto outIdx = _inQueue->pool()->append(sizeof(IntUpdate) + sizeof(SpreadMessageType));
        mb();
        _inQueue->appendNew(outIdx);
    }

    void setNumberOfClipsToAllowedClips(int id) {
        auto &inMessage = _inQueue->pool()->getNextMessage();
        inMessage._type = SpreadMessageType::NumberOfClipsToAllowedClips;
        IntUpdate& message = reinterpret_cast<IntUpdate &> (inMessage._data[0]);
        message._id = id;
        const auto outIdx = _inQueue->pool()->append(sizeof(IntUpdate) + sizeof(SpreadMessageType));
        mb();
        _inQueue->appendNew(outIdx);
    }

    void handleClipDone(int spreadId, bool spreadDone, hf::price_t price) {
        log(boost::str(boost::format("Handling clip done. Id=%1% Done=%2% Price=%3%") % spreadId % spreadDone % price));
        auto it = _spreadAlgos.find(spreadId);
        if (it != _spreadAlgos.end()) {
            SpreadAlgos& algos = it->second;
            if (algos._isEntry) {
                if (algos._exit) {
                    algos._exit->handleOtherSpreadClipDone(spreadDone, price);
                }
            } else {
                if (algos._exit) {
                    algos._exit->handleClipDone(spreadDone);
                }
                if (!algos._exit->isOriginalSpreadDone()) {
                    if (algos._entry) {
                        algos._entry->handleOtherSpreadClipDone(spreadDone);
                    } else if (algos._manualEntry) {
                        algos._manualEntry->handleOtherSpreadClipDone(spreadDone);
                    }
                }
            }
        } else {
            log(boost::str(boost::format("Could not handle clip done. No algo.")));
        }
    }

    void subscribeMarketData(Leg& leg, SMAEntryAlgoT* algo) {
        const auto& pair = _marketDataSubscriptions.insert(std::pair<hf::symbol_id_t, std::vector<SMAEntryAlgoT*>>(leg._sid, std::vector<SMAEntryAlgoT*>()));
        if (!pair.second) {
            subscribeMarketData(leg);
        }
        pair.first->second.push_back(algo);
    }

    void subscribeMarketData(Leg& leg) {
        for (auto *const queuePtr : _marketDataQueues) {
            auto &si = queuePtr->getSI(leg._sid);
            auto &vi = si.venues[unsigned(leg._venue)];
            vi.level_num = 1;
            vi.level_qty = true;
            vi.trades = false;
        }
    }

    void unsubscribeMarketData(Leg& leg, SMAEntryAlgoT* algo) {
        auto it = _marketDataSubscriptions.find(leg._sid);
        if (it != _marketDataSubscriptions.end()) {
            auto& subs = it->second;
            for (auto mdIt = subs.begin(); mdIt != subs.end(); ++mdIt) {
                if (*mdIt == algo) {
                    subs.erase(mdIt);
                    break;
                }
            }
        }
        // still will be receiving data since it is expected that more spreads with same legs will be traded
    }

    void addMarketDataQueue(md::queue::MDQueue* q) {
        _marketDataQueues.emplace_back(q);
    }

    void processMarketData() {
        for (auto *const qp : _marketDataQueues) {
            const auto idx = qp->getNext();

            if (BOOST_UNLIKELY(!idx))
                continue;

            qp->freeNext();

            auto &batch = qp->getPoolP()->getBatch(idx);
            onMDBatch(batch);

            {
                qp->poolBytesRead(batch.size);
                uint64_t usage;
                if (BOOST_UNLIKELY(qp->poolThresholdExceeded(usage))) {
                    onPoolThresholdExceeded(qp->getQueueID(), usage);
                }
                if (BOOST_UNLIKELY(qp->queueThresholdExceeded(usage))) {
                    onQueueThresholdExceeded(qp->getQueueID(), usage);
                }
            }
        }
    }

    void onPoolThresholdExceeded(const std::string& queueName, uint64_t usage) {
    }

    void onQueueThresholdExceeded(const std::string& queueName, uint64_t usage) {
    }

    void onMDBatch(const md::MDBatch &batch)  {
        const auto *data = batch.data;
        for (auto i = batch.count; i; --i) {
            const auto &mde = reinterpret_cast<const md::MDEvent &> (*data);
            data += mde.size;
            switch (mde.type) {
                case md::MDEvent::TYPE::UPDATE: {
                    const auto &event = static_cast<const md::MDUpdateEvent &> (mde);
                    onMDUpdate(batch.venue, event);
                    break;
                }
                case md::MDEvent::TYPE::TRADE: {
                    const auto &event = static_cast<const md::MDTradeEvent &> (mde);
                    onMDTrade(batch.venue, event);
                    break;
                }
                case md::MDEvent::TYPE::GAP: {
                    const auto &event = static_cast<const md::MDGapEvent &> (mde);
                    onMDGap(batch.venue, event);
                    break;
                }
                case md::MDEvent::TYPE::STATUS: {
                    const auto &event = static_cast<const md::MDStatusEvent &> (mde);
                    onMDStatus(batch.venue, event);
                    break;
                }
                default:
                    break;
            }
        }
    }

    void onMDTrade(hf::VENUE venue, const md::MDTradeEvent &event) {
    }

    void onMDStatus(hf::VENUE venue, const md::MDStatusEvent &event) {
    }

    void onMDGap(hf::VENUE venue, const md::MDGapEvent &event) {
    }

    void processRemoteFill(int spreadId, hf::symbol_gid_t gid, hf::qty_t filledQty) {
        auto &inMessage = _inQueue->pool()->getNextMessage();
        inMessage._type = SpreadMessageType::RemoteFill;
        RemoteFill& remoteFill = reinterpret_cast<RemoteFill &> (inMessage._data[0]);
        remoteFill._spreadId = spreadId;
        remoteFill._gid = gid;
        remoteFill._filledQty = filledQty;
        const auto outIdx = _inQueue->pool()->append(sizeof(RemoteFill) + sizeof(SpreadMessageType));
        mb();
        _inQueue->appendNew(outIdx);
    }

    void setVenue(hf::VENUE venue) {
        _venue = venue;
    }

    void setType(SpreaderType type) {
        _type = type;
    }

    struct SpreadAlgos {
        SMAEntryAlgoT* _entry = nullptr;
        ManualEntryAlgoT* _manualEntry = nullptr;
        CoverExitAlgoT* _exit = nullptr;
        bool _isEntry;

        void pause() {
            if (_isEntry) {
                if (_entry) _entry->pause();
            } else {
                if (_exit) _exit->pause();
            }
        }

        void resume() {
            if (_isEntry) {
                if (_entry) _entry->resume();
            } else {
                if (_exit) _exit->resume();
            }
        }

        void updateEntryAlgo(const proto::EntryAlgo& entryAlgo, double frequency) {
            if (_entry && entryAlgo.type() == proto::EntryAlgoType::SMA) {
                _entry->update(entryAlgo.sma(), frequency);
            }
        }

        void updateExitAlgo(const proto::ExitAlgo& algo, double frequency) {
            if (_exit && algo.type() == proto::ExitAlgoType::Autocover) {
                _exit->update(algo.autocover(), frequency);
            }
        }
    };

    struct SpreadInfo {
        proto::NewSpreadOrder _message;
        int _spreadId;
        int _coverSpreadId;
        message_id_t _messageId;
    };

    std::vector<SMAEntryAlgoT*> _entryAlgos;
    std::vector<CoverExitAlgoT*> _exitAlgos;
    SpreaderQueue* _inQueue;
    std::unordered_map<hf::symbol_id_t, std::vector<SMAEntryAlgoT*>> _marketDataSubscriptions;
    std::unordered_map<int, SpreadAlgos> _spreadAlgos;
    std::unordered_map<int, SpreadInfo> _spreadInfos;
    ContextT* _context;
    std::vector<md::queue::MDQueue *> _marketDataQueues;
    double _cpuFrequency;
    std::function<void(AlgoMessage*)> _algoMessages;
    hf::VENUE _venue;
    SpreaderType _type;

    // distributed stuff

    struct GatewayInfo {
        hf::VENUE _venue;
        om::port_id_t _id;
        bool _active;
    };

    // LOGIN
    bool _masterLoggedIn = false;
    bool _slaveLoggedIn = false;
    std::vector<GatewayInfo> _connectedGateways;

};

}}

#endif //RHONEOMS_ENTRYALGOMANAGER_H
