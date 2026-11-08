#include "liquidity_ledger.hpp"

#include <algorithm>
#include <limits>

using namespace tachyon;
using namespace tachyon::trade;

namespace tachyon::trade {

qty_t liquidity_ledger::available(market_key market,
                                  qty_t display_size,
                                  reservation_filter filter) const noexcept {
    if (display_size == 0) {
        return 0;
    }

    qty_t reserved_quantity = 0;
    for (const auto& reservation : reservations_) {
        if (!counts_toward_availability(reservation, market, filter)) {
            continue;
        }

        // Saturate at zero instead of allowing unsigned arithmetic to wrap.
        const qty_t remaining = display_size - reserved_quantity;
        if (reservation.quantity >= remaining) {
            return 0;
        }
        reserved_quantity += reservation.quantity;
    }

    return display_size - reserved_quantity;
}

qty_t liquidity_ledger::reserved(market_key market,
                                 reservation_filter filter) const noexcept {
    qty_t reserved_quantity = 0;
    for (const auto& reservation : reservations_) {
        if (!counts_toward_availability(reservation, market, filter)) {
            continue;
        }

        if (reservation.quantity >
            std::numeric_limits<qty_t>::max() - reserved_quantity) {
            return std::numeric_limits<qty_t>::max();
        }
        reserved_quantity += reservation.quantity;
    }
    return reserved_quantity;
}

reservation_id liquidity_ledger::reserve_hedge(market_key market,
                                               liquidity_source source,
                                               uint32_t hedge_id,
                                               qty_t quantity) {
    return reserve(reservation_type::HEDGE, market, source, quantity, hedge_id);
}

reservation_id liquidity_ledger::reserve(reservation_type type,
                                         market_key market,
                                         liquidity_source source,
                                         qty_t quantity,
                                         uint32_t hedge_id,
                                         uint32_t group_id) {
    if (quantity == 0) {
        return {};
    }
    if (group_id != 0 && find_group(group_id) == nullptr) {
        return {};
    }

    reservation reservation{};
    reservation.id = {next_reservation_id()};
    reservation.target = market;
    reservation.source = source;
    reservation.hedge_id = hedge_id;
    reservation.group_id = group_id;
    reservation.quantity = quantity;
    reservation.type = type;
    reservation.state = reservation_state::RESERVED;

    reservations_.push_back(reservation);
    return reservation.id;
}

bool liquidity_ledger::counts_toward_availability(
    const reservation& reservation,
    market_key market,
    const reservation_filter& filter) const noexcept {
    if (reservation.target != market ||
        reservation.state == reservation_state::COMPLETED) {
        return false;
    }
    if ((filter.exclude_type & to_mask(reservation.type)) != 0) {
        return false;
    }
    if (filter.ignore_group_id != 0 &&
        reservation.group_id == filter.ignore_group_id) {
        return false;
    }
    if (filter.ignore_reservation.value != reservation_id::INVALID &&
        filter.ignore_reservation == reservation.id) {
        return false;
    }

    // Hedge reservations are associated with the market source that supplied
    // the displayed size. Quote-group reservations are market-wide and are not
    // source-filtered.
    if (reservation.type == reservation_type::HEDGE &&
        filter.source != liquidity_source::__LIQUIDITY_SOURCE_END__ &&
        reservation.source != filter.source) {
        return false;
    }

    return true;
}

reservation* liquidity_ledger::find_reservation(reservation_id id) noexcept {
    if (id.value == reservation_id::INVALID) {
        return nullptr;
    }
    auto it = std::find_if(
        reservations_.begin(), reservations_.end(),
        [id](const reservation& reservation) { return reservation.id == id; });
    return it == reservations_.end() ? nullptr : &*it;
}

reservation_group* liquidity_ledger::find_group(uint32_t group_id) noexcept {
    if (group_id == 0) {
        return nullptr;
    }
    auto it = std::find_if(
        groups_.begin(), groups_.end(),
        [group_id](const reservation_group& group) {
            return group.active && group.id == group_id;
        });
    return it == groups_.end() ? nullptr : &*it;
}

uint32_t liquidity_ledger::next_reservation_id() noexcept {
    if (next_reservation_id_ == reservation_id::INVALID) {
        next_reservation_id_ = reservation_id::INVALID + 1;
    }
    return next_reservation_id_++;
}

uint32_t liquidity_ledger::next_group_id() noexcept {
    if (next_group_id_ == 0) {
        next_group_id_ = 1;
    }
    return next_group_id_++;
}

bool liquidity_ledger::acknowledge_external(
    reservation_id id,
    ord_id_t ord_id,
    qty_t actual_external_quantity) {
    reservation* reservation = find_reservation(id);
    if (reservation == nullptr || reservation->type != reservation_type::HEDGE) {
        return false;
    }

    if (actual_external_quantity == 0) {
        return complete(id);
    }

    reservation->ord_id = ord_id;
    reservation->quantity = actual_external_quantity;
    reservation->state = reservation_state::EXTERNAL_WORKING;
    return true;
}

bool liquidity_ledger::acknowledge_external(
    uint32_t hedge_id,
    market_key target,
    ord_id_t ord_id,
    qty_t actual_external_quantity) {
    auto it = std::find_if(
        reservations_.begin(), reservations_.end(),
        [hedge_id, target](const reservation& reservation) {
            return reservation.type == reservation_type::HEDGE &&
                   reservation.hedge_id == hedge_id &&
                   reservation.target == target;
        });
    if (it == reservations_.end()) {
        return false;
    }
    return acknowledge_external(it->id, ord_id, actual_external_quantity);
}

bool liquidity_ledger::complete(reservation_id id) {
    auto it = std::find_if(
        reservations_.begin(), reservations_.end(),
        [id](const reservation& reservation) { return reservation.id == id; });
    if (it == reservations_.end()) {
        return false;
    }

    // Group reservations have a shared lifecycle. Completing one member must
    // not leave stale anticipated reservations on the other group markets.
    if (it->group_id != 0) {
        return release_group(it->group_id);
    }

    reservations_.erase(it);
    return true;
}

bool liquidity_ledger::complete_by_hedge_id(uint32_t hedge_id,
                                            market_key target) {
    auto it = std::find_if(
        reservations_.begin(), reservations_.end(),
        [hedge_id, target](const reservation& reservation) {
            return reservation.type == reservation_type::HEDGE &&
                   reservation.hedge_id == hedge_id &&
                   reservation.target == target;
        });
    if (it == reservations_.end()) {
        return false;
    }
    return complete(it->id);
}

bool liquidity_ledger::complete_by_order_id(ord_id_t ord_id) {
    if (ord_id == INVALID_ORDER_ID) {
        return false;
    }

    auto it = std::find_if(
        reservations_.begin(), reservations_.end(),
        [ord_id](const reservation& reservation) {
            return reservation.ord_id == ord_id &&
                   reservation.state == reservation_state::EXTERNAL_WORKING;
        });
    if (it == reservations_.end()) {
        return false;
    }
    return complete(it->id);
}


uint32_t liquidity_ledger::create_quote_group(
    market_key opposite_market,
    liquidity_source source,
    qty_t quantity,
    std::span<const market_key> hedge_markets) {
    if (quantity == 0 ||
        hedge_markets.size() > reservation_group::MAX_HEDGE_COUNT) {
        return 0;
    }

    reservation_group group{};
    group.id = next_group_id();
    group.quantity = quantity;
    group.source = source;
    group.opposite_market = opposite_market;
    group.hedge_market_count =
        static_cast<uint8_t>(hedge_markets.size());
    group.active = true;
    std::copy(hedge_markets.begin(), hedge_markets.end(),
              group.hedge_markets.begin());

    const uint32_t group_id = group.id;
    groups_.push_back(group);

    reserve(reservation_type::AGGRESSIVE_QUOTE, opposite_market, source,
            quantity, 0, group_id);

    for (const market_key hedge_market : hedge_markets) {
        reserve(reservation_type::ANTICIPATED_HEDGE, hedge_market, source,
                quantity, 0, group_id);
    }

    return group_id;
}

bool liquidity_ledger::resize_quote_group(uint32_t group_id,
                                          qty_t quantity) {
    reservation_group* group = find_group(group_id);
    if (group == nullptr) {
        return false;
    }
    if (quantity == 0) {
        return release_group(group_id);
    }

    group->quantity = quantity;
    for (auto& reservation : reservations_) {
        if (reservation.group_id == group_id) {
            reservation.quantity = quantity;
        }
    }
    return true;
}

bool liquidity_ledger::release_group(
    uint32_t group_id,
    std::vector<market_key>* affected_markets) {
    reservation_group* group = find_group(group_id);
    if (group == nullptr) {
        return false;
    }

    add_affected_market(affected_markets, group->opposite_market);
    for (uint8_t i = 0; i < group->hedge_market_count; ++i) {
        add_affected_market(affected_markets, group->hedge_markets[i]);
    }

    std::erase_if(reservations_,
                  [group_id](const reservation& reservation) {
                      return reservation.group_id == group_id;
                  });

    group->active = false;
    group->quantity = 0;
    group->hedge_market_count = 0;
    return true;
}

bool liquidity_ledger::invalidate_market(
    market_key market,
    std::vector<market_key>* affected_markets) {
    bool changed = false;
    std::vector<uint32_t> groups_to_release;

    for (const auto& reservation : reservations_) {
        if (reservation.target != market) {
            continue;
        }

        changed = true;
        if (reservation.group_id == 0) {
            continue;
        }

        const uint32_t group_id = reservation.group_id;
        if (std::find(groups_to_release.begin(), groups_to_release.end(),
                      group_id) == groups_to_release.end()) {
            groups_to_release.push_back(group_id);
        }
    }

    for (const uint32_t group_id : groups_to_release) {
        release_group(group_id, affected_markets);
    }

    std::erase_if(reservations_,
                  [market](const reservation& reservation) {
                      return reservation.target == market &&
                             reservation.group_id == 0;
                  });

    return changed;
}

const reservation_group* liquidity_ledger::group(
    uint32_t group_id) const noexcept {
    if (group_id == 0) {
        return nullptr;
    }
    auto it = std::find_if(
        groups_.begin(), groups_.end(),
        [group_id](const reservation_group& group) {
            return group.active && group.id == group_id;
        });
    return it == groups_.end() ? nullptr : &*it;
}

std::size_t liquidity_ledger::active_reservation_count() const noexcept {
    return static_cast<std::size_t>(std::count_if(
        reservations_.begin(), reservations_.end(),
        [](const reservation& reservation) {
            return reservation.state != reservation_state::COMPLETED;
        }));
}

void liquidity_ledger::add_affected_market(
    std::vector<market_key>* affected_markets,
    market_key market) {
    if (affected_markets == nullptr) {
        return;
    }
    if (std::find(affected_markets->begin(), affected_markets->end(), market) ==
        affected_markets->end()) {
        affected_markets->push_back(market);
    }
}

}   // end of namespace tachyon::trade
