#include "fastmm/backtest/own_orders.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <limits>
#include <unordered_set>

namespace fastmm::bt {

std::string_view to_string(OrderEnd e) noexcept {
  switch (e) {
    case OrderEnd::Open:
      return "open";
    case OrderEnd::Canceled:
      return "canceled";
    case OrderEnd::Filled:
      return "filled";
    case OrderEnd::Expired:
      return "expired";
    case OrderEnd::Replaced:
      return "replaced";
    case OrderEnd::Reconciled:
      return "reconciled";
  }
  return "?";
}

const OwnOrder* OwnOrderLog::find(ClientOrderId id) const noexcept {
  const auto it = index.find(id.value);
  return it == index.end() ? nullptr : &orders[it->second];
}

Timestamp OwnOrderLog::gone(const OwnOrder& o) const noexcept {
  if (!o.ended) return Timestamp{};
  Timestamp t = o.end.ts;
  // A replaced order leaves when its successor is acknowledged (which may keep the queue
  // position), not at its own cancel ack.
  if (const OwnOrder* next = o.replaced_by.valid() ? find(o.replaced_by) : nullptr;
      next != nullptr && next->acked && next->ack.ts > t)
    t = next->ack.ts;
  if (ms_order_times && !o.end.from_recv) t = Timestamp{t.ns + 1'000'000 - 1};
  return t;
}

namespace {

constexpr std::int64_t kMs = 1'000'000;

class Collector {
 public:
  explicit Collector(bool live) { log_.live = live; }

  void on_event(const EventHeader& h) {
    if (h.type == EventType::EngineTime || h.type == EventType::Padding ||
        h.type == EventType::LatencySample)
      return;
    const Timestamp vts = venue_ts(h);
    if (vts.valid() && vts > log_.last_ts) log_.last_ts = vts;
    if ((h.flags & EventHeader::kOutbound) != 0) {
      if ((h.flags & EventHeader::kDropped) == 0) on_outbound(h);
      return;
    }
    const auto order_time = [&] {
      if (h.exch_ts.valid()) {
        ++log_.order_venue_times;
        if (h.exch_ts.ns % kMs != 0) ms_ = false;
      }
    };
    switch (h.type) {
      case EventType::OrderAck:
        order_time();
        on_ack(h, msg_cast<OrderAckMsg>(&h).cl_ord_id);
        break;
      case EventType::OrderFill:
        order_time();
        on_fill(h, msg_cast<OrderFillMsg>(&h));
        break;
      case EventType::OrderCancelAck:
        order_time();
        on_cancel_ack(h, msg_cast<OrderCancelAckMsg>(&h).cl_ord_id);
        break;
      case EventType::OrderExpired:
        order_time();
        if (OwnOrder* s = find(msg_cast<OrderExpiredMsg>(&h).cl_ord_id))
          end(*s, h, OrderEnd::Expired);
        break;
      case EventType::OrderReject:
        on_reject(msg_cast<OrderRejectMsg>(&h).cl_ord_id);
        break;
      case EventType::Reconcile:
        on_reconcile(h, msg_cast<ReconcileMsg>(&h));
        break;
      default:
        break;
    }
  }

  OwnOrderLog finish() {
    log_.ms_order_times = ms_ && log_.order_venue_times > 0;
    return std::move(log_);
  }

 private:
  OwnOrder* find(ClientOrderId id) {
    const auto it = log_.index.find(id.value);
    return it == log_.index.end() ? nullptr : &log_.orders[it->second];
  }
  void add(const OwnOrder& s) {
    if (const auto it = log_.index.find(s.id.value); it != log_.index.end()) {
      log_.orders[it->second] = s;
      return;
    }
    log_.index.emplace(s.id.value, log_.orders.size());
    log_.orders.push_back(s);
  }

  void on_outbound(const EventHeader& h) {
    if (h.type == EventType::OutNewOrder) {
      const auto& m = msg_cast<OutNewOrderMsg>(&h);
      OwnOrder s;
      s.id = m.cl_ord_id;
      s.instrument = h.instrument;
      s.side = m.side;
      s.type = m.type;
      s.tif = m.tif;
      s.price = m.price;
      s.qty = m.qty;
      add(s);
      ++log_.orders_sent;
    } else if (h.type == EventType::OutReplace) {
      const auto& m = msg_cast<OutReplaceMsg>(&h);
      OwnOrder s;
      s.id = m.cl_ord_id;
      s.instrument = h.instrument;
      s.price = m.price;
      s.qty = m.qty;
      s.replaces = m.orig_cl_ord_id;
      if (OwnOrder* orig = find(m.orig_cl_ord_id)) {
        s.side = orig->side;
        s.type = orig->type == OrderType::PostOnly ? OrderType::PostOnly : OrderType::Limit;
        orig->replaced_by = m.cl_ord_id;
      }
      add(s);
      ++log_.orders_sent;
    }
  }

  void on_ack(const EventHeader& h, ClientOrderId id) {
    OwnOrder* s = find(id);
    if (s == nullptr) {
      ++log_.unknown_acks;
      return;
    }
    s->ack.offer(h);
    if (s->acked) return;
    s->acked = true;
    if (OwnOrder* orig = s->replaces.valid() ? find(s->replaces) : nullptr)
      end(*orig, h, OrderEnd::Replaced);
  }

  void on_fill(const EventHeader& h, const OrderFillMsg& m) {
    OwnOrder* s = find(m.cl_ord_id);
    if (s == nullptr) return;
    Qty filled;
    for (const OwnFill& f : s->fills) filled += f.qty;
    const std::uint64_t exec = numeric_exec_id(m.exec_id);
    const bool seen = exec != 0 && std::any_of(s->fills.begin(),
                                               s->fills.end(),
                                               [&](const OwnFill& f) { return f.exec == exec; });
    if (!s->ended && !seen) {
      OwnFill f;
      f.exec = exec;
      f.at.offer(h);
      f.qty = m.qty;
      s->fills.push_back(f);
      filled += m.qty;
      if (exec != 0) s->last_exec = exec;
    }
    const bool last =
        (m.flags & OrderFillMsg::kReplayed) == 0 ? m.leaves_qty.is_zero() : m.cum_qty >= s->qty;
    if (last || filled >= s->qty) end(*s, h, OrderEnd::Filled);
  }

  void on_cancel_ack(const EventHeader& h, ClientOrderId id) {
    OwnOrder* s = find(id);
    if (s == nullptr) return;
    // A second cancel ack may carry the venue time the first did not.
    if (s->ended && (s->why == OrderEnd::Canceled || s->why == OrderEnd::Replaced)) {
      s->end.offer(h);
      return;
    }
    end(*s, h, s->replaced_by.valid() ? OrderEnd::Replaced : OrderEnd::Canceled);
  }

  void on_reject(ClientOrderId id) {
    OwnOrder* s = find(id);
    if (s == nullptr || s->acked || s->rejected) return;
    s->rejected = true;
    ++log_.rejected;
    s->ended = true;
  }

  // Begin, OpenOrder..., End: an acked order the snapshot does not list is gone. With a sent
  // watermark, orders above it were not yet sent when the snapshot was taken.
  void on_reconcile(const EventHeader& h, const ReconcileMsg& m) {
    switch (m.kind) {
      case ReconcileMsg::Kind::Begin:
        listed_.clear();
        watermark_ =
            (m.flags & ReconcileMsg::kSentWatermark) != 0 ? m.sent_watermark : ClientOrderId{};
        break;
      case ReconcileMsg::Kind::OpenOrder:
        listed_.insert(m.cl_ord_id.value);
        break;
      case ReconcileMsg::Kind::End:
        for (OwnOrder& s : log_.orders) {
          if (!s.acked || s.ended || listed_.contains(s.id.value)) continue;
          if (watermark_.valid() && s.id.value > watermark_.value) continue;
          end(s, h, OrderEnd::Reconciled);
        }
        break;
      default:
        break;
    }
  }

  static void end(OwnOrder& s, const EventHeader& h, OrderEnd why) {
    if (s.ended) return;
    s.ended = true;
    s.why = why;
    s.end.offer(h);
  }

  OwnOrderLog log_;
  bool ms_ = true;
  std::unordered_set<std::uint64_t> listed_;
  ClientOrderId watermark_;
};

Qty less(Qty a, Qty b) noexcept {
  return b >= a ? Qty{} : a - b;
}

}  // namespace

OwnOrderLog collect_own_orders(JournalReader& reader) {
  Collector c(reader.header().tsc0 != 0);
  reader.for_each([&](const EventHeader* h) { c.on_event(*h); });
  reader.reset();
  return c.finish();
}

OwnOrderStripper::OwnOrderStripper(JournalReader& reader) {
  reader.for_each([&](const EventHeader* h) {
    if ((h->flags & EventHeader::kOutbound) != 0) {
      if ((h->flags & EventHeader::kDropped) == 0) own_.on_outbound(*h);
    } else {
      own_.on_inbound(*h);
    }
  });
  reader.reset();
  own_.index();
}

const EventHeader* OwnOrderStripper::strip(const EventHeader& h, sim::EventBuf& buf) noexcept {
  if (own_.empty()) return &h;
  const Timestamp t = venue_ts(h);
  switch (h.type) {
    case EventType::BookDelta:
    case EventType::BookSnapshot: {
      const auto& d = msg_cast<BookDeltaMsg>(&h);
      bool touched = false;
      for (Side s : {Side::Buy, Side::Sell}) {
        for (const Level& l : (s == Side::Buy ? d.bids() : d.asks()))
          touched =
              touched || (l.qty.is_positive() && own_at(h.instrument, s, l.price, t).is_positive());
      }
      if (!touched || h.len > sizeof(buf.bytes)) return &h;
      std::memcpy(buf.bytes, &h, h.len);
      auto& out = buf.as<BookDeltaMsg>();
      // A delta keeps its levels (quantity 0 deletes one); a snapshot drops the emptied ones.
      std::uint32_t nb = 0;
      std::uint32_t na = 0;
      Level* lv = out.levels();
      const Level* in = d.levels();
      for (std::uint32_t k = 0; k < d.bid_count + d.ask_count; ++k) {
        const Side s = k < d.bid_count ? Side::Buy : Side::Sell;
        Level l = in[k];
        if (l.qty.is_positive()) {
          const Qty own = own_at(h.instrument, s, l.price, t);
          if (own.is_positive()) {
            l.qty = less(l.qty, own);
            ++(l.qty.is_zero() ? stats_.levels_removed : stats_.levels_adjusted);
          }
        }
        if (d.is_snapshot() && l.qty.is_zero()) continue;
        lv[nb + na] = l;
        ++(s == Side::Buy ? nb : na);
      }
      out.bid_count = nb;
      out.ask_count = na;
      out.hdr.len = BookDeltaMsg::size_for(nb, na);
      return &out.hdr;
    }
    case EventType::BookTicker: {
      const auto& m = msg_cast<BookTickerMsg>(&h);
      const Qty own_bid = own_at(h.instrument, Side::Buy, m.bid_px, t);
      const Qty own_ask = own_at(h.instrument, Side::Sell, m.ask_px, t);
      if (!own_bid.is_positive() && !own_ask.is_positive()) return &h;
      if ((own_bid.is_positive() && own_bid >= m.bid_qty) ||
          (own_ask.is_positive() && own_ask >= m.ask_qty)) {
        ++stats_.tickers_dropped;
        return nullptr;
      }
      std::memcpy(buf.bytes, &h, sizeof(BookTickerMsg));
      auto& out = buf.as<BookTickerMsg>();
      out.bid_qty = less(m.bid_qty, own_bid);
      out.ask_qty = less(m.ask_qty, own_ask);
      ++stats_.tickers_adjusted;
      return &out.hdr;
    }
    default:
      return &h;
  }
}

}  // namespace fastmm::bt
