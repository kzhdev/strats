#include "positions.hpp"

namespace tachyon::trade::mm {

qty_t position_book::try_offset(qty_t& position, qty_t remaining, qty_t& allocated,
                                side sd) noexcept {
    // Legacy :28-48: a bid fill can only offset a short position and vice versa.
    if (sd == side::bid) {
        if (position < 0) {
            const qty_t abs_position = -position;
            const qty_t taken = std::min(abs_position, remaining);
            allocated += taken;
            position += taken;
            return remaining - taken;
        }
        return remaining;
    }
    if (position > 0) {
        const qty_t taken = std::min(position, remaining);
        allocated += taken;
        position -= taken;
        return remaining - taken;
    }
    return remaining;
}

qty_t position_book::offset_position(contract_state& s, side sd, qty_t remaining,
                                     qty_t& alloc_internal, qty_t& alloc_external) noexcept {
    remaining = try_offset(s.internal_pos, remaining, alloc_internal, sd);
    remaining = try_offset(s.external_pos, remaining, alloc_external, sd);
    return remaining;
}

void position_book::allocate_external_quote_fill(contract_state& s, quote_state& quote, qty_t size,
                                                 side sd) noexcept {
    // Legacy :1058-1072: never let the external size go negative, even if the exchange fills more
    // than we thought we were showing.
    quote.external_size = size <= quote.external_size ? quote.external_size - size : 0;
    s.external_pos += (sd == side::bid ? size : -size);
}

qty_t position_book::allocate_hedging_fill(contract_state& s, side sd, qty_t size,
                                           qty_t& alloc_internal, qty_t& alloc_external) noexcept {
    // Legacy :957-990: offset the hedge fill against the existing positions, and report the
    // allocation with the sign the fill moves them.
    qty_t remaining = try_offset(s.internal_pos, size, alloc_internal, sd);
    if (remaining > 0) {
        remaining = try_offset(s.external_pos, remaining, alloc_external, sd);
    }
    if (sd == side::ask) {
        alloc_internal = -alloc_internal;
        alloc_external = -alloc_external;
    }
    return remaining;
}

void position_book::allocate_hedging_fill_remainder(contract_state& s, side sd, bool internal,
                                                    qty_t remaining, qty_t& alloc_internal,
                                                    qty_t& alloc_external) noexcept {
    // Legacy :944-955: whatever could not be offset opens a new position.
    const qty_t signed_remaining = sd == side::bid ? remaining : -remaining;
    if (internal) {
        s.internal_pos += signed_remaining;
        alloc_internal += signed_remaining;
    } else {
        s.external_pos += signed_remaining;
        alloc_external += signed_remaining;
    }
}

fill_outcome position_book::on_fill(contract_state& s, uint32_t c, const fill& f) const noexcept {
    fill_outcome out;
    out.reported_size = f.qty;

    // Legacy :987-1006: anything that is not a quote or hedge fill is a takeover. A fill handed
    // over by another strategy also ends the cycle inline (:1000).
    if (f.source != fill_source::quote && f.source != fill_source::hedge) {
        if (f.from_other_strategy || f.force_over_fill) {
            force_take_over_fill(s, c, f, out);
            out.ends_cycle_inline = f.from_other_strategy;
            return out;
        }
        return take_over_fill(s, c, f);
    }

    const bool is_hedging_fill = f.source == fill_source::hedge;
    const bool internal = f.type == fill_type::internal;

    if (!is_hedging_fill) {
        // Legacy :1019-1046: a quote fill is offset first, then applied to the quote's own size.
        quote_state& quote = s.quote_of(f.sd);
        qty_t remaining = f.qty;
        if (internal) {
            remaining = offset_position(s, f.sd, remaining, out.allocated_internal,
                                        out.allocated_external);
            if (remaining > 0) {
                // More than we were showing internally: the excess is booked external (:1023-1030).
                const qty_t overfill = std::max(remaining - quote.internal_size, qty_t{0});
                if (overfill > 0) {
                    allocate_external_quote_fill(s, quote, overfill, f.sd);
                    out.allocated_external += overfill;
                    remaining = quote.internal_size;
                }
                quote.internal_size -= remaining;
                s.internal_pos += (f.sd == side::bid ? remaining : -remaining);
                out.allocated_internal += remaining;
            }
        } else {
            remaining = offset_position(s, f.sd, remaining, out.allocated_internal,
                                        out.allocated_external);
            if (remaining > 0) {
                allocate_external_quote_fill(s, quote, remaining, f.sd);
                out.allocated_external += remaining;
            }
        }
        return out;
    }

    // Legacy :1047-1055: a hedge fill offsets the positions first, and whatever is left opens the
    // position the fill came from.
    const qty_t remaining =
        allocate_hedging_fill(s, f.sd, f.qty, out.allocated_internal, out.allocated_external);
    if (remaining > 0) {
        allocate_hedging_fill_remainder(s, f.sd, internal, remaining, out.allocated_internal,
                                        out.allocated_external);
    }
    return out;
}

fill_outcome position_book::take_over_fill(contract_state& s, uint32_t c,
                                           const fill& f) const noexcept {
    fill_outcome out;
    // Legacy :238-317: offset what we can, then take the rest as external exposure up to the
    // external position limit.
    qty_t remaining =
        offset_position(s, f.sd, f.qty, out.allocated_internal, out.allocated_external);
    qty_t size_taken = out.allocated_internal + out.allocated_external;

    if (remaining > 0) {
        const qty_t multiplier = f.sd == side::bid ? 1 : -1;
        const qty_t abs_external = s.external_pos < 0 ? -s.external_pos : s.external_pos;
        if (m_.max_pos_external[c] > abs_external) {
            const qty_t new_position = remaining * multiplier + s.external_pos;
            const qty_t abs_new = new_position < 0 ? -new_position : new_position;
            if (abs_new > m_.max_pos_external[c]) {
                const qty_t taken = m_.max_pos_external[c] - abs_external;
                s.external_pos += taken * multiplier;
                out.allocated_external += taken;
                size_taken += taken;
                remaining -= taken;
            } else {
                s.external_pos = new_position;
                out.allocated_external += remaining;
                size_taken += remaining;
                remaining = 0;
            }
        }
    }

    out.reported_size = size_taken;
    // Legacy :288-309: whatever is left cannot be taken by the MM. It hands the remainder to
    // another strategy, or takes it anyway and stops quoting externally.
    out.untaken_remaining = remaining;
    return out;
}

void position_book::force_take_over_fill(contract_state& s, uint32_t c, const fill& f,
                                         fill_outcome& out) const noexcept {
    // Legacy :318-340: no limit is honoured here; the position is simply taken.
    qty_t remaining =
        offset_position(s, f.sd, f.qty, out.allocated_internal, out.allocated_external);
    if (remaining > 0) {
        const qty_t multiplier = f.sd == side::bid ? 1 : -1;
        s.external_pos += remaining * multiplier;
        out.allocated_external += remaining;
    }
    out.reported_size = f.qty;
    if (f.check_limit && !within_limits(s, c)) {
        out.disable_external_quoting = true;
    }
}

bool position_book::within_limits(const contract_state& s, uint32_t c) const noexcept {
    // Legacy :1211-1225. The comparison is strictly greater-than, so sitting exactly on the limit
    // is still within it.
    const qty_t abs_internal = s.internal_pos < 0 ? -s.internal_pos : s.internal_pos;
    const qty_t abs_external = s.external_pos < 0 ? -s.external_pos : s.external_pos;
    if (abs_internal > m_.max_pos_internal[c]) return false;
    if (abs_external > m_.max_pos_external[c]) return false;
    return true;
}

}  // namespace tachyon::trade::mm
