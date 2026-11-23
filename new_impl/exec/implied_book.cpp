#include "implied_book.hpp"

// The book's hot path is inline in the header (see the note there). This translation unit keeps the
// invariant gate: the sizes the design relies on, checked at compile time in every configuration.
namespace tachyon::trade::exec {
static_assert(sizeof(implied_book::order) <= 64, "an internal order must fit in one cache line");
static_assert(SLOTS_PER_CONTRACT == 12, "three strategies x two kinds x two sides");
static_assert(MAX_INTERNAL_ORDERS == 6, "one resting order per stream per side");

// Force the out-of-line symbols the design documents, so a missing definition is a link error
// here rather than in a consumer.
void implied_book_link_anchor(implied_book& book, const order_request& request) noexcept {
    book.apply(request);
}
}  // namespace tachyon::trade::exec
