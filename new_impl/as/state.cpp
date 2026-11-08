#include "state.hpp"

#include <algorithm>

namespace tachyon::trade::as {

void cycle_state::init(const model& m) noexcept {
    books_.fill(market_state{});
    best_external_.fill(0);  // internal book is best until a refresh says otherwise
    paths_.fill(path_state{});
    order_.fill(0);
    quotes_.fill(quote{});
    last_sent_.fill(quote{});
    force_.fill(0);
    stf_.fill(NULL_PRICE);           // legacy ShortTermFair::_fair starts NULL
    last_path_price_.fill(NULL_PRICE);  // legacy leaves this uninitialised (see D-AS6)
    rv_quote_.fill(0);
    offset_value_.fill(NULL_PRICE);  // legacy _stfOffset starts NULL
    can_trade_.fill(1);           // legacy Contract::_canTrade starts true
    dirty_.fill(0);
    dirty_count_ = 0;

    // Per-(contract, side) path order starts as the config order (legacy `_paths[Side]` is
    // appended in definition order, ArbieSpreader.cpp:158-166).
    for (uint32_t c = 0; c < m.n_all; ++c) {
        reset_order(m, c, side::bid);
        reset_order(m, c, side::ask);
    }
}

bool cycle_state::update_external(const quote_policy& policy, uint32_t contract, side sd,
                                  price_t price, qty_t size) noexcept {
    market_state& b = book(contract, sd, true);
    const bool changed = (b.price != price) || (b.size != size);
    if (b.price != price) {
        // Common.h:668-676: a replaced level releases everything AS held on this book. The host
        // performs the release (the ledger is not ours) and the new level lands afterwards.
        policy.on_price_changed(contract, sd, true, size);
    }
    b.price = price;
    b.size = size;
    return changed;
}

bool cycle_state::refresh_best(const model& m, const quote_policy& policy, uint32_t contract,
                               side sd, bool updated_external) noexcept {
    const uint32_t k = key(contract, sd);
    const bool was_external = best_external_[k] != 0;

    const market_state& internal = book(contract, sd, false);
    const market_state& external = book(contract, sd, true);
    const qty_t internal_size = adjusted_size(policy, contract, sd, false);

    bool now_external;
    if (internal_size == 0) {
        now_external = true;  // :318-319 -- note this wins even when both books are empty
    } else {
        const qty_t external_size = adjusted_size(policy, contract, sd, true);
        if (external_size == 0) {
            now_external = false;  // :320-321
        } else if (is_better_price(sd, internal.price, external.price)) {
            now_external = false;  // :322-323
        } else if (internal.price == external.price) {
            now_external = !(internal_size <= external_size);  // :324-329, tie goes to internal
        } else {
            now_external = true;  // :330-332
        }
    }
    best_external_[k] = now_external ? 1 : 0;

    // :335 -- "the updated book is the new best, or it was the old best". Deliberately not
    // "the best changed": when the best moves from the updated book to the other one, legacy
    // does not propagate here (the other book's own update will).
    return (updated_external == now_external) || (updated_external == was_external);
}

}  // namespace tachyon::trade::as
