#include "fastmm/live/paper_venue.hpp"

#include "fastmm/core/messages.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/venues/order_commands.hpp"
#include "fastmm/venues/wire_latency.hpp"

#include <algorithm>
#include <string>

namespace fastmm::live {

namespace {

VenueOrderId paper_id(std::uint64_t n) {
  return VenueOrderId("dry-" + std::to_string(n));
}

template <class M>
M make(EventType type, InstrumentId inst, VenueId venue) {
  M m{};
  init_header(m, type, inst, venue);
  const Timestamp now = wall_now();
  m.hdr.recv_ts = now;
  m.hdr.exch_ts = now;
  m.hdr.flags = EventHeader::kSynthetic;
  return m;
}

}  // namespace

PaperVenue::PaperVenue(std::unique_ptr<venues::Venue> inner, bool replace)
    : inner_(std::move(inner)), replace_(replace) {}

venues::VenueCaps PaperVenue::caps() const noexcept {
  venues::VenueCaps c = inner_->caps();
  c.supports_replace = replace_;
  return c;
}

void PaperVenue::attach(const venues::SymbolTable& symbols,
                        const InstrumentTable& instruments,
                        venues::EventSink& md_sink,
                        venues::EventSink& order_sink,
                        MsgRing* outbound) {
  instruments_ = &instruments;
  order_sink_ = &order_sink;
  outbound_ = outbound;
  // The connector gets a ring of its own that nothing writes: the engine's orders stop here.
  inner_->attach(symbols, instruments, md_sink, order_sink, &inner_outbound_);
}

void PaperVenue::on_wake() {
  if (outbound_ == nullptr) return;
  const std::lock_guard lock(mu_);
  while (const std::byte* p = outbound_->try_peek()) {
    handle(*reinterpret_cast<const EventHeader*>(p));
    outbound_->release();
  }
}

void PaperVenue::send_now(std::span<const EventHeader* const> batch) {
  const std::lock_guard lock(mu_);
  for (const EventHeader* h : batch) handle(*h);
}

void PaperVenue::request_open_orders() {
  const std::lock_guard lock(mu_);
  reconcile_snapshot();
}

bool PaperVenue::cancel_all() {
  return true;  // nothing rests anywhere
}

void PaperVenue::ack(InstrumentId inst, ClientOrderId id, std::uint64_t venue_id) {
  auto m = make<OrderAckMsg>(EventType::OrderAck, inst, inner_->id());
  m.cl_ord_id = id;
  m.venue_order_id = paper_id(venue_id);
  static_cast<void>(order_sink_->push(m.hdr));
}

void PaperVenue::expire(InstrumentId inst, ClientOrderId id, std::uint64_t venue_id) {
  auto m = make<OrderExpiredMsg>(EventType::OrderExpired, inst, inner_->id());
  m.cl_ord_id = id;
  m.venue_order_id = paper_id(venue_id);
  static_cast<void>(order_sink_->push(m.hdr));
}

void PaperVenue::handle(const EventHeader& h) {
  if (venues::is_reconcile_request(h)) {
    reconcile_snapshot();
    return;
  }
  const auto cmd = venues::OrderCommand::from(h);
  if (!cmd) return;
  Counts& n = counts_[cmd->instrument.value];
  switch (cmd->kind) {
    case venues::OrderCommandKind::New: {
      ++n.news;
      const std::uint64_t vid = ++next_venue_id_;
      ack(cmd->instrument, cmd->cl_ord_id, vid);
      if (cmd->type == OrderType::Market || cmd->tif == TimeInForce::Ioc ||
          cmd->tif == TimeInForce::Fok) {
        expire(cmd->instrument, cmd->cl_ord_id, vid);  // nothing to trade against
        return;
      }
      orders_[cmd->cl_ord_id.value] = Order{cmd->instrument, cmd->side, cmd->price, cmd->qty};
      return;
    }
    case venues::OrderCommandKind::Cancel: {
      ++n.cancels;
      const auto it = orders_.find(cmd->cl_ord_id.value);
      if (it == orders_.end()) {
        auto m = make<OrderCancelRejectMsg>(EventType::OrderCancelReject, h.instrument, h.venue);
        m.cl_ord_id = cmd->cl_ord_id;
        m.reason = RejectReason::VenueUnknownOrder;
        static_cast<void>(order_sink_->push(m.hdr));
        return;
      }
      orders_.erase(it);
      auto m = make<OrderCancelAckMsg>(EventType::OrderCancelAck, h.instrument, h.venue);
      m.cl_ord_id = cmd->cl_ord_id;
      static_cast<void>(order_sink_->push(m.hdr));
      return;
    }
    case venues::OrderCommandKind::Replace: {
      ++n.replaces;
      const auto it = orders_.find(cmd->orig_cl_ord_id.value);
      if (it == orders_.end()) {
        auto m = make<OrderRejectMsg>(EventType::OrderReject, h.instrument, h.venue);
        m.cl_ord_id = cmd->cl_ord_id;
        m.reason = RejectReason::VenueUnknownOrder;
        static_cast<void>(order_sink_->push(m.hdr));
        return;
      }
      Order o = it->second;
      orders_.erase(it);
      o.price = cmd->price;
      o.qty = cmd->qty;
      orders_[cmd->cl_ord_id.value] = o;
      ack(cmd->instrument, cmd->cl_ord_id, ++next_venue_id_);
      return;
    }
  }
}

void PaperVenue::reconcile_snapshot() {
  const VenueId venue = inner_->id();
  auto begin = make<ReconcileMsg>(EventType::Reconcile, InstrumentId::invalid(), venue);
  begin.kind = ReconcileMsg::Kind::Begin;
  begin.flags = ReconcileMsg::kExecutionsExact;  // nothing ever fills
  static_cast<void>(order_sink_->push(begin.hdr));
  for (const auto& [id, o] : orders_) {
    auto m = make<ReconcileMsg>(EventType::Reconcile, o.instrument, venue);
    m.kind = ReconcileMsg::Kind::OpenOrder;
    m.side = o.side;
    m.state = OrderState::Live;
    m.cl_ord_id = ClientOrderId{id};
    m.price = o.price;
    m.orig_qty = o.qty;
    static_cast<void>(order_sink_->push(m.hdr));
  }
  auto end = make<ReconcileMsg>(EventType::Reconcile, InstrumentId::invalid(), venue);
  end.kind = ReconcileMsg::Kind::End;
  static_cast<void>(order_sink_->push(end.hdr));
}

std::vector<PaperQuotes> PaperVenue::quotes() const {
  const std::lock_guard lock(mu_);
  std::vector<PaperQuotes> out;
  const auto row = [&](InstrumentId inst) -> PaperQuotes& {
    for (PaperQuotes& q : out) {
      if (q.instrument == inst) return q;
    }
    PaperQuotes& q = out.emplace_back();
    q.instrument = inst;
    return q;
  };
  for (const auto& [inst, n] : counts_) {
    PaperQuotes& q = row(InstrumentId{inst});
    q.news = n.news;
    q.cancels = n.cancels;
    q.replaces = n.replaces;
  }
  for (const auto& [id, o] : orders_) {
    PaperQuotes& q = row(o.instrument);
    Level& best = o.side == Side::Buy ? q.bid : q.ask;
    ++(o.side == Side::Buy ? q.bid_orders : q.ask_orders);
    const bool better =
        best.qty.is_zero() || (o.side == Side::Buy ? o.price > best.price : o.price < best.price);
    if (better) {
      best = Level{o.price, o.qty};
    } else if (o.price == best.price) {
      best.qty = best.qty + o.qty;
    }
  }
  std::sort(out.begin(), out.end(), [](const PaperQuotes& a, const PaperQuotes& b) {
    return a.instrument.value < b.instrument.value;
  });
  return out;
}

PaperTotals PaperVenue::totals() const {
  const std::lock_guard lock(mu_);
  PaperTotals t;
  for (const auto& [inst, n] : counts_) {
    t.news += n.news;
    t.cancels += n.cancels;
    t.replaces += n.replaces;
  }
  return t;
}

}  // namespace fastmm::live
