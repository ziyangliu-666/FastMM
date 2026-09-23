#include "echo/echo_venue.hpp"

#include <fastmm/core/messages.hpp>
#include <fastmm/venues/order_events.hpp>

namespace echo {

using fastmm::BookTickerMsg;
using fastmm::EventHeader;
using fastmm::EventType;
using fastmm::OutCancelMsg;
using fastmm::OutNewOrderMsg;
using fastmm::OutReplaceMsg;
using fastmm::ReconcileMsg;
using fastmm::RejectReason;

VenueCaps EchoVenue::caps() const noexcept {
  VenueCaps c;
  c.supports_replace = false;
  c.supports_post_only = true;
  c.ws_order_entry = false;
  c.user_stream = false;  // this venue has no private stream at all
  return c;
}

void EchoVenue::attach(const SymbolTable&,
                       const InstrumentTable&,
                       EventSink& md_sink,
                       EventSink& order_sink,
                       MsgRing* outbound) {
  md_sink_ = &md_sink;
  order_sink_ = &order_sink;
  outbound_ = outbound;
}

void EchoVenue::subscribe(std::span<const InstrumentId> instruments) {
  subscribed_.assign(instruments.begin(), instruments.end());
  status_.books_total = static_cast<std::uint32_t>(subscribed_.size());
  if (connected_) publish();
}

void EchoVenue::connect(fastmm::net::Reactor&) {
  connected_ = true;
  status_.md = fastmm::venues::ChannelState::Live;
  publish();
}

void EchoVenue::publish() {
  if (md_sink_ == nullptr) return;
  for (InstrumentId inst : subscribed_) {
    BookTickerMsg* m = md_sink_->reserve<BookTickerMsg>();
    if (m == nullptr) return;  // lossy sink: the engine resyncs
    fastmm::init_header(*m, EventType::BookTicker, inst, id_);
    m->bid_px = cfg_.bid;
    m->bid_qty = cfg_.qty;
    m->ask_px = cfg_.ask;
    m->ask_qty = cfg_.qty;
    md_sink_->commit();
    ++status_.md_messages;
  }
  status_.books_synced = static_cast<std::uint32_t>(subscribed_.size());
}

// No order entry: answer every request so the OMS never waits for an ack that will not come.
void EchoVenue::refuse(const EventHeader& h) {
  if (order_sink_ == nullptr) return;
  constexpr std::string_view why = "echo: this venue has no order entry";
  switch (h.type) {
    case EventType::OutNewOrder: {
      const auto& m = reinterpret_cast<const OutNewOrderMsg&>(h);
      fastmm::venues::emit_order_reject(
          *order_sink_, id_, h.instrument, m.cl_ord_id, RejectReason::VenueReject, 0, why);
      break;
    }
    case EventType::OutCancel: {
      const auto& m = reinterpret_cast<const OutCancelMsg&>(h);
      fastmm::venues::emit_cancel_reject(
          *order_sink_, id_, h.instrument, m.cl_ord_id, RejectReason::VenueUnknownOrder, 0, why);
      break;
    }
    case EventType::OutReplace: {
      const auto& m = reinterpret_cast<const OutReplaceMsg&>(h);
      fastmm::venues::emit_order_reject(
          *order_sink_, id_, h.instrument, m.cl_ord_id, RejectReason::VenueReject, 0, why);
      break;
    }
    default:
      break;
  }
  ++status_.order_send_failures;
}

void EchoVenue::on_wake() {
  if (outbound_ == nullptr) return;
  while (const std::byte* p = outbound_->try_peek()) {
    refuse(*reinterpret_cast<const EventHeader*>(p));
    outbound_->release();
  }
}

void EchoVenue::send_now(std::span<const EventHeader* const> batch) {
  for (const EventHeader* h : batch) refuse(*h);
}

// Nothing rests on this venue: an empty snapshot still has to bracket itself, or the engine keeps
// quoting paused.
void EchoVenue::request_open_orders() {
  if (order_sink_ == nullptr) return;
  for (ReconcileMsg::Kind kind : {ReconcileMsg::Kind::Begin, ReconcileMsg::Kind::End}) {
    ReconcileMsg* m = order_sink_->reserve<ReconcileMsg>();
    if (m == nullptr) return;
    fastmm::init_header(*m, EventType::Reconcile, fastmm::InstrumentId{}, id_);
    m->kind = kind;
    order_sink_->commit();
  }
}

}  // namespace echo
