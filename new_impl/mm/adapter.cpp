#include "adapter.hpp"

#include <new>

namespace tachyon::trade::mm {

adapter::adapter(const mm_model_cfg& cfg, const secdb_table& secdb, double cycles_per_second,
                 uint32_t initial_shift, request_sink& sink)
    : sink_(&sink), current_shift_(initial_shift) {
    model_inputs inputs = build_model_inputs(cfg, secdb, cycles_per_second, initial_shift);
    // Allocated once at init: the cycle itself performs no heap traffic (PLAN.md §3).
    model_.reset(new model(model::build(inputs)));
    state_.reset(new cycle_state(*model_));

    host_ports ports;
    ports.send_quote = &adapter::trampoline_quote;
    ports.send_hedge = &adapter::trampoline_hedge;
    ports.notify = &adapter::trampoline_notify;
    ports.cancel_orders = &adapter::trampoline_cancel;
    ports.on_alert = &adapter::trampoline_alert;
    ports.on_fill_report = &adapter::trampoline_fill;
    ports.ctx = this;
    host_.reset(new host(*model_, *state_, ports));
}

void adapter::trampoline_quote(void* ctx, const order_intent& it) noexcept {
    static_cast<adapter*>(ctx)->sink_->send_quote(it);
}

void adapter::trampoline_hedge(void* ctx, const order_intent& it) noexcept {
    static_cast<adapter*>(ctx)->sink_->send_hedge(it);
}

void adapter::trampoline_notify(void* ctx, uint32_t contract) noexcept {
    static_cast<adapter*>(ctx)->sink_->notify(contract);
}

void adapter::trampoline_cancel(void* ctx, uint32_t contract, bool disable_gtc) noexcept {
    static_cast<adapter*>(ctx)->sink_->cancel_orders(contract, disable_gtc);
}

void adapter::trampoline_alert(void* ctx, uint32_t contract, const char* reason) noexcept {
    static_cast<adapter*>(ctx)->sink_->alert(contract, reason);
}

void adapter::trampoline_fill(void* ctx, uint32_t contract, const order_intent*,
                              const fill_outcome& outcome) noexcept {
    static_cast<adapter*>(ctx)->sink_->report_fill(contract, outcome);
}

}  // namespace tachyon::trade::mm
