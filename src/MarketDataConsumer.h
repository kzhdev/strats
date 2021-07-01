#pragma once

#include <md/md_event.h>
#include <md/queue/MDQueue.h>
#include <util/log.h>
#include <atomic>
#include <unordered_set>

namespace Python {

LOCAL_EXPLICIT_LOG_DECLARE("MarketDataConsumer.h", MDConsumerSpace)

struct MarketDataConsumerBase {
    static std::vector<md::queue::MDQueue*>& mdQueue() {
        static std::vector<md::queue::MDQueue*> s_mdQueue;
        return s_mdQueue;
    }
};

template<typename T>
class MarketDataConsumer : MarketDataConsumerBase {

    struct QueueState {
        uint_fast64_t _peekIndex = 0;
    };

    std::vector<QueueState> _queueState;

    T& underlying() { return static_cast<T&>(*this); }

protected:
    std::vector<md::queue::MDQueue*>& _marketDataQueues = mdQueue();
    std::unordered_set<uint32_t> _mdUpdated;

public:
    void initMarkDataQueueState(size_t numContracts) {
        _queueState.resize(_marketDataQueues.size());
        size_t i = 0;
        for (auto q : _marketDataQueues) {
            _queueState[i++]._peekIndex = q->peekW();
        }
        _mdUpdated.reserve(numContracts);
    }

    void drainAllMarketData() {
        for (uint32_t i = 0; i < _marketDataQueues.size(); ++i) {
            auto queue = _marketDataQueues[i];
            auto& state = _queueState[i];
            uint_fast64_t index = queue->peekW();

            uint32_t drainSize = 0;
            for (uint_fast64_t j = state._peekIndex; j != index; queue->decrement(j)) {
                auto idx = queue->peek(j);
                if (BOOST_LIKELY(!idx)) continue;

                auto& batch = queue->getPoolP()->getBatch(idx);
                onMDBatch(batch);

                ++drainSize;
            }

            if (BOOST_UNLIKELY((drainSize > 0) && (logging::loggingLevel() > logging::LOG_LEVEL::INFO))) {
                LOG(0, "{i1} MD Drain {i2} batch from mdQueue {i3}", resourceName(underlying().id()), drainSize, i)
            }
            state._peekIndex = index;
        }
    }

    void onMDBatch(const md::MDBatch &batch) {
        DLOG(0, "{i1} received MD batch. bid={i2} extSeqNum={i3} venue={i4}", resourceName(underlying().id()), batch.bid,
             batch.extSequenceNumber, toUInt(batch.venue))

        const auto *data = batch.data;
        for (auto i = batch.count; i; --i) {
            const auto &mde = reinterpret_cast<const md::MDEvent &> (*data);
            data += mde.size;
            auto internalId = underlying().sidToInternalId(mde.sid);
            if (internalId != INVALID_INTERNAL_ID) {
                switch (mde.type) {
                    case md::MDEvent::TYPE::UPDATE: {
                        const auto& event = static_cast<const md::MDUpdateEvent&> (mde);
                        underlying().onMDUpdate(event, internalId, batch.bid);
                        break;
                    }
                    case md::MDEvent::TYPE::TRADE: {
                        const auto& event = static_cast<const md::MDTradeEvent&> (mde);
                        underlying().onMDTrade(batch, event, internalId);
                        break;
                    }
                    case md::MDEvent::TYPE::STATUS: {
                        const auto& event = static_cast<const md::MDStatusEvent&> (mde);
                        underlying().onMDStatus(batch.venue, batch.extSequenceNumber, event, internalId);
                        break;
                    }
                    default:
                        underlying().handleUncommonMDEvent(mde, batch, internalId);
                        break;
                }
                _mdUpdated.emplace(internalId);
            }
#ifdef IS_DEV
            else {
                logging::GetLogger().logError(boost::str(boost::format("Received md update for unrecognized SID='%1%'") % mde.sid));
            }
#endif
        }
        underlying().onEndMDBatch(batch.bid);
        _mdUpdated.clear();
    }
};

template<typename BookOwnerT>
inline bool copyBooks(ViperResource& resource, BookOwnerT& owner, const md::MDUpdateEvent& event, std::unordered_set<uint32_t>& updatedContracts, bool notifyGUI = false) {
    bool success = true;
    auto checkAndSetChangeIndex = [&resource, &owner, &success, notifyGUI](int8_t& ourChangeIndex, md::level_index_t eventUpdatedNum, const char* side, const char* which) -> bool {
        int8_t changeIndex = -1;
        if (BOOST_UNLIKELY(eventUpdatedNum > md::top_t::LEVEL_NUM)) {
            success = false;
            if (notifyGUI) {
                std::string logMessage = boost::str(boost::format("onMdUpdate: Invalid index. %1%.updated%2%Num=%3%") % side % which % static_cast<int>(eventUpdatedNum));
                logging::GetLogger().logError(logMessage);
                resource.sendErrorMessage(logMessage, ErrorType::Critical);
            }
        } else {
            changeIndex = eventUpdatedNum - 1;
        }

        if (changeIndex != -1) {
            if (ourChangeIndex == -1 || changeIndex < ourChangeIndex) {
                ourChangeIndex = changeIndex;
            }
        }
        return changeIndex != -1;
    };

    checkAndSetChangeIndex(owner._mdPriceChangeIndex[BID], event.buy.updatedPriceNum, "buy", "Price");
    bool bidChanged = checkAndSetChangeIndex(owner._mdQtyChangeIndex[BID], event.buy.updatedQtyNum, "buy", "Qty");

    if (bidChanged) {
        memcpy(&owner._books[BID], &event.buy.top, sizeof(md::top_t));
    }

    checkAndSetChangeIndex(owner._mdPriceChangeIndex[ASK], event.sell.updatedPriceNum, "sell", "Price");;
    bool askChanged = checkAndSetChangeIndex(owner._mdQtyChangeIndex[ASK], event.sell.updatedQtyNum, "sell", "Qty");

    if (askChanged) {
        memcpy(&owner._books[ASK], &event.sell.top, sizeof(md::top_t));
    }

    if (bidChanged | askChanged) {
        updatedContracts.insert(owner._contract._internalId);
    }

    return success;
}

template<typename BookOwnerT, uint32_t Side>
void handleMDTrade(BookOwnerT& owner, const md::MDTradeEvent::trade_t& trade, uint32_t internalId, std::unordered_set<uint32_t>& updatedContracts) {
    auto& best = owner._books[Side].l[0];

    auto copyBook = [](md::top_t::level_t& book, hf::price_t price, hf::qty_t size, md::order_count_t orderCount) {
        book.pr = price;
        book.q = size;
        book.o = orderCount;
    };

    if (trade.pr == best.pr) {
        owner._mdQtyChangeIndex[Side] = 0;
        copyBook(best, trade.pr, trade.q, trade.ext2.cme_python_book_cleaning.orderCount);
    } else if (IsBetterPrice<Side>(trade.pr, best.pr)) {
        DLOG(0, "{C[i1]}. {i2} book adjustment due to trade. Trade: {i3}@{d1}. OurBest: {i4}@{d2}", owner.getContractName(), Side, trade.q, ToDollars(trade.pr), best.q, ToDollars(best.pr))
        owner._mdPriceChangeIndex[Side] = owner._mdQtyChangeIndex[Side] = 0;
        memmove(&owner._books[Side].l[1], &owner._books[Side].l[0], sizeof(md::top_t::level_t) * (md::top_t::LEVEL_NUM - 1));
        copyBook(best, trade.pr, trade.q, trade.ext2.cme_python_book_cleaning.orderCount);
    } else if (IsBetter(trade.pr, owner._books[Side].l[md::top_t::LEVEL_NUM - 1].pr, Side)) {
        DLOG(0, "{C[i1]}. {i2} book adjustment due to trade. Trade: {i3}@{d1}. OurBest: {i4}@{d2}", owner.getContractName(), Side, trade.q, ToDollars(trade.pr), best.q, ToDollars(best.pr))
        owner._mdPriceChangeIndex[Side] = owner._mdQtyChangeIndex[Side] = 0;
        int i = 1;
        for (; i < md::top_t::LEVEL_NUM; ++i) {
            if (trade.pr == owner._books[Side].l[i].pr) {
                break;
            }
        }

        if (BOOST_LIKELY(i < md::top_t::LEVEL_NUM - 1)) {
            memmove(&owner._books[Side].l[1], &owner._books[Side].l[i + 1],
                    sizeof(md::top_t::level_t) * (md::top_t::LEVEL_NUM - i - 1));
            owner._books[Side].l[md::top_t::LEVEL_NUM - i].pr = hf::NULL_PRICE;
            owner._books[Side].l[md::top_t::LEVEL_NUM - i].q = 0;
        } else {
            assert(false && "Should never come here");
            owner._books[Side].l[1].pr = hf::NULL_PRICE;
            owner._books[Side].l[1].q = 0;
        }
        copyBook(best, trade.pr, trade.q, trade.ext2.cme_python_book_cleaning.orderCount);
    } else {
        // traded through the book
        owner._mdPriceChangeIndex[Side] = owner._mdQtyChangeIndex[Side] = 0;
        copyBook(best, trade.pr, trade.q, trade.ext2.cme_python_book_cleaning.orderCount);
        owner._books[Side].l[1].pr = hf::NULL_PRICE;
        owner._books[Side].l[1].q = 0;
    }
    updatedContracts.insert(internalId);
}

}