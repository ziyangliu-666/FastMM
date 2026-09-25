#include "fastmm/backtest/fill_check.hpp"

#include "fastmm/core/book/l2_book.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/sim/queue_model.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace fastmm::bt {

std::string_view to_string(FillCheckEnd e) noexcept {
  switch (e) {
    case FillCheckEnd::Open:
      return "open";
    case FillCheckEnd::Canceled:
      return "canceled";
    case FillCheckEnd::Filled:
      return "filled";
    case FillCheckEnd::Expired:
      return "expired";
    case FillCheckEnd::Replaced:
      return "replaced";
    case FillCheckEnd::Reconciled:
      return "reconciled";
  }
  return "?";
}

namespace {

using sim::QueuePositionModel;
using Book = L2Book<256>;

Timestamp event_ts(const EventHeader& h) noexcept {
  return h.recv_ts.valid() ? h.recv_ts : h.exch_ts;
}

// An order the session sent, from its outbound copy.
struct Sent {
  ClientOrderId id;
  InstrumentId instrument;
  Side side = Side::Buy;
  OrderType type = OrderType::Limit;
  TimeInForce tif = TimeInForce::Gtc;
  Price price;
  Qty qty;
  ClientOrderId replaces;     // OutReplace: the order it replaces
  ClientOrderId replaced_by;  // the OutReplace that replaces it
  bool acked = false;
  bool rejected = false;
  bool ended = false;     // live order gone (before or after the ack)
  std::int64_t row = -1;  // index in FillCheckResult::orders, -1 when not in the models
};

class Walker {
 public:
  Walker(std::span<const double> conservatism, std::size_t instruments) : books_(instruments) {
    for (const double c : conservatism) {
      const auto bps = static_cast<std::int64_t>(std::llround(std::clamp(c, 0.0, 1.0) * 10'000));
      models_.push_back(std::make_unique<QueuePositionModel>(bps));
      model_ptrs_.push_back(models_.back().get());
      res_.conservatism.push_back(static_cast<double>(bps) / 10'000);
    }
  }

  void on_event(const EventHeader& h) {
    if (h.type == EventType::EngineTime || h.type == EventType::Padding ||
        h.type == EventType::LatencySample)
      return;
    const Timestamp ts = event_ts(h);
    if (ts.valid()) last_ts_ = ts;
    if ((h.flags & EventHeader::kOutbound) != 0) {
      if ((h.flags & EventHeader::kDropped) == 0) on_outbound(h);
      return;
    }
    switch (h.type) {
      case EventType::BookDelta:
      case EventType::BookSnapshot:
        ++res_.md_events;
        sim::queue_apply_book(book(h.instrument), msg_cast<BookDeltaMsg>(&h), ts, model_ptrs_);
        break;
      case EventType::Trade:
        ++res_.md_events;
        on_trade(msg_cast<TradeMsg>(&h), ts);
        break;
      case EventType::OrderAck:
        on_ack(msg_cast<OrderAckMsg>(&h).cl_ord_id, ts);
        break;
      case EventType::OrderFill:
        on_fill(msg_cast<OrderFillMsg>(&h), ts);
        break;
      case EventType::OrderCancelAck:
        on_cancel_ack(msg_cast<OrderCancelAckMsg>(&h).cl_ord_id, ts);
        break;
      case EventType::OrderExpired:
        if (Sent* s = find(msg_cast<OrderExpiredMsg>(&h).cl_ord_id))
          end(*s, ts, FillCheckEnd::Expired);
        break;
      case EventType::OrderReject:
        on_reject(msg_cast<OrderRejectMsg>(&h).cl_ord_id);
        break;
      case EventType::Reconcile:
        on_reconcile(msg_cast<ReconcileMsg>(&h), ts);
        break;
      default:
        break;
    }
  }

  FillCheckResult finish() {
    for (auto& [id, s] : sent_) {
      if (!s.acked && !s.rejected) ++res_.not_acked;
    }
    for (FillCheckOrder& o : res_.orders) {
      if (o.end == FillCheckEnd::Open) o.end_ts = last_ts_;
    }
    return std::move(res_);
  }

 private:
  Book& book(InstrumentId id) {
    if (id.value >= books_.size()) books_.resize(id.value + 1);
    if (!books_[id.value]) books_[id.value] = std::make_unique<Book>();
    return *books_[id.value];
  }
  Sent* find(ClientOrderId id) {
    const auto it = sent_.find(id.value);
    return it == sent_.end() ? nullptr : &it->second;
  }
  FillCheckOrder* row(const Sent& s) {
    return s.row < 0 ? nullptr : &res_.orders[static_cast<std::size_t>(s.row)];
  }

  void on_outbound(const EventHeader& h) {
    if (h.type == EventType::OutNewOrder) {
      const auto& m = msg_cast<OutNewOrderMsg>(&h);
      Sent s;
      s.id = m.cl_ord_id;
      s.instrument = h.instrument;
      s.side = m.side;
      s.type = m.type;
      s.tif = m.tif;
      s.price = m.price;
      s.qty = m.qty;
      sent_.insert_or_assign(m.cl_ord_id.value, s);
      ++res_.orders_sent;
    } else if (h.type == EventType::OutReplace) {
      const auto& m = msg_cast<OutReplaceMsg>(&h);
      Sent s;
      s.id = m.cl_ord_id;
      s.instrument = h.instrument;
      s.price = m.price;
      s.qty = m.qty;
      s.replaces = m.orig_cl_ord_id;
      if (Sent* orig = find(m.orig_cl_ord_id)) {
        s.side = orig->side;
        s.type = orig->type == OrderType::PostOnly ? OrderType::PostOnly : OrderType::Limit;
        orig->replaced_by = m.cl_ord_id;
      }
      sent_.insert_or_assign(m.cl_ord_id.value, s);
      ++res_.orders_sent;
    }
  }

  void on_ack(ClientOrderId id, Timestamp ts) {
    Sent* s = find(id);
    if (s == nullptr) {
      ++res_.unknown_acks;
      return;
    }
    if (s->acked) return;  // a venue may acknowledge twice (API response and user stream)
    s->acked = true;
    Sent* orig = s->replaces.valid() ? find(s->replaces) : nullptr;
    if (orig != nullptr) end(*orig, ts, FillCheckEnd::Replaced, false);
    if (s->ended) {
      ++res_.ended_before_ack;
      drop(s->replaces);
      return;
    }
    if (s->type == OrderType::Market || s->tif == TimeInForce::Ioc || s->tif == TimeInForce::Fok) {
      ++res_.not_resting;
      drop(s->replaces);
      return;
    }
    Book& b = book(s->instrument);
    const Level best = s->side == Side::Buy ? b.best_ask() : b.best_bid();
    if (best.qty.is_positive() &&
        (s->side == Side::Buy ? s->price >= best.price : s->price <= best.price)) {
      ++res_.crossed_at_ack;
      drop(s->replaces);
      return;
    }
    FillCheckOrder o;
    o.cl_ord_id = id;
    o.instrument = s->instrument;
    o.side = s->side;
    o.price = s->price;
    o.qty = s->qty;
    o.queue_ahead = sim::level_qty(b, s->side, s->price);
    o.ack_ts = ts;
    o.model_filled.assign(models_.size(), Qty{});
    o.model_first_fill_ts.assign(models_.size(), Timestamp{});
    for (QueuePositionModel* q : model_ptrs_) {
      // SimTransport::queue_replace: the same price at no more than the leaves keeps the place.
      if (s->replaces.valid()) {
        const auto h = q->find(s->replaces);
        if (h.valid()) {
          const sim::QueuedOrder& old = q->get(h);
          if (old.price == s->price && s->qty <= old.leaves() &&
              q->amend_keep_priority(h, id, 0, s->qty))
            continue;
          q->remove(h);
        }
      }
      if (!q->place(id, 0, s->instrument, s->side, s->price, s->qty, o.queue_ahead).valid())
        ++res_.model_full;
    }
    s->row = static_cast<std::int64_t>(res_.orders.size());
    res_.orders.push_back(std::move(o));
  }

  void on_fill(const OrderFillMsg& m, Timestamp ts) {
    Sent* s = find(m.cl_ord_id);
    if (s == nullptr) return;
    if (FillCheckOrder* o = row(*s); o != nullptr && o->end == FillCheckEnd::Open) {
      o->live_filled += m.qty;
      if (!o->live_first_fill_ts.valid()) o->live_first_fill_ts = ts;
    }
    const bool last =
        (m.flags & OrderFillMsg::kReplayed) == 0 ? m.leaves_qty.is_zero() : m.cum_qty >= s->qty;
    if (last || (row(*s) != nullptr && row(*s)->live_filled >= s->qty))
      end(*s, ts, FillCheckEnd::Filled);
  }

  void on_cancel_ack(ClientOrderId id, Timestamp ts) {
    Sent* s = find(id);
    if (s == nullptr) return;
    // A replace whose new leg is not acknowledged yet: the model order waits for it, so that an
    // unchanged price keeps its queue position.
    const Sent* next = s->replaced_by.valid() ? find(s->replaced_by) : nullptr;
    const bool pending = next != nullptr && !next->acked && !next->rejected;
    end(*s, ts, next != nullptr ? FillCheckEnd::Replaced : FillCheckEnd::Canceled, !pending);
  }

  void on_reject(ClientOrderId id) {
    Sent* s = find(id);
    if (s == nullptr || s->acked || s->rejected) return;
    s->rejected = true;
    ++res_.rejected;
    s->ended = true;
    if (s->replaces.valid()) {
      // A failed replace: the original keeps working unless the venue already cancelled it.
      const Sent* orig = find(s->replaces);
      if (orig != nullptr && orig->ended) drop(s->replaces);
    }
  }

  // Begin, OpenOrder..., End: an acked order the snapshot does not list is gone. With a sent
  // watermark, orders above it were not yet sent when the snapshot was taken.
  void on_reconcile(const ReconcileMsg& m, Timestamp ts) {
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
        for (auto& [id, s] : sent_) {
          if (!s.acked || s.ended || listed_.contains(id)) continue;
          if (watermark_.valid() && id > watermark_.value) continue;
          end(s, ts, FillCheckEnd::Reconciled);
        }
        break;
      default:
        break;
    }
  }

  // The live order ended; `remove` takes it out of the models too.
  void end(Sent& s, Timestamp ts, FillCheckEnd why, bool remove = true) {
    if (!s.ended) {
      s.ended = true;
      if (FillCheckOrder* o = row(s); o != nullptr && o->end == FillCheckEnd::Open) {
        o->end = why;
        o->end_ts = ts;
      }
    }
    if (remove) drop(s.id);
  }
  void drop(ClientOrderId id) {
    if (!id.valid()) return;
    for (QueuePositionModel* q : model_ptrs_) {
      if (const auto h = q->find(id); h.valid()) q->remove(h);
    }
  }

  void on_trade(const TradeMsg& t, Timestamp ts) {
    for (std::size_t k = 0; k < models_.size(); ++k) {
      QueuePositionModel& q = *models_[k];
      q.on_trade(t.hdr.instrument,
                 t.price,
                 t.qty,
                 t.aggressor,
                 [&](QueuePositionModel::Handle32 h, sim::QueuedOrder& o, Qty fill, Qty) {
                   if (Sent* s = find(o.cl_ord_id)) {
                     if (FillCheckOrder* r = row(*s);
                         r != nullptr && r->end == FillCheckEnd::Open) {
                       r->model_filled[k] += fill;
                       if (!r->model_first_fill_ts[k].valid()) r->model_first_fill_ts[k] = ts;
                     }
                   }
                   if (o.leaves().is_zero()) q.remove(h);
                 });
    }
  }

  std::vector<std::unique_ptr<Book>> books_;
  std::vector<std::unique_ptr<QueuePositionModel>> models_;
  std::vector<QueuePositionModel*> model_ptrs_;
  std::unordered_map<std::uint64_t, Sent> sent_;
  std::unordered_set<std::uint64_t> listed_;
  ClientOrderId watermark_;
  Timestamp last_ts_;
  FillCheckResult res_;
};

std::string qty_text(Qty q) {
  char buf[kMaxDecimalChars];
  return {buf, q.to_decimal(buf)};
}
std::string price_text(Price p) {
  char buf[kMaxDecimalChars];
  return {buf, p.to_decimal(buf)};
}

}  // namespace

FillCheckSummary FillCheckResult::summary(std::size_t k) const {
  FillCheckSummary s;
  s.conservatism = conservatism.at(k);
  std::vector<std::int64_t> dt;
  for (const FillCheckOrder& o : orders) {
    const bool live = o.live_filled.is_positive();
    const bool model = o.model_filled[k].is_positive();
    s.live_qty += o.live_filled;
    s.model_qty += o.model_filled[k];
    if (live && model) {
      ++s.both;
      if (o.live_first_fill_ts.valid() && o.model_first_fill_ts[k].valid())
        dt.push_back(std::abs((o.model_first_fill_ts[k] - o.live_first_fill_ts).ns));
    } else if (live) {
      ++s.live_only;
    } else if (model) {
      ++s.model_only;
    } else {
      ++s.neither;
    }
  }
  if (!dt.empty()) {
    const auto mid = dt.begin() + static_cast<std::ptrdiff_t>(dt.size() / 2);
    std::nth_element(dt.begin(), mid, dt.end());
    if (dt.size() % 2 == 1) {
      s.median_abs_dt_ns = *mid;
    } else {
      const std::int64_t hi = *mid;
      const std::int64_t lo = *std::max_element(dt.begin(), mid);
      s.median_abs_dt_ns = lo + (hi - lo) / 2;
    }
  }
  return s;
}

FillCheckResult fill_check(JournalReader& reader, std::span<const double> conservatism) {
  Walker w(conservatism, reader.instruments().size());
  reader.for_each([&](const EventHeader* h) { w.on_event(*h); });
  reader.reset();
  return w.finish();
}

FillCheckResult fill_check(const std::string& path, std::span<const double> conservatism) {
  JournalReader reader;
  if (auto r = reader.open(path); !r) {
    throw std::runtime_error("cannot open " + path + ": " + std::string(to_string(r.error())));
  }
  return fill_check(reader, conservatism);
}

std::string format_fill_check(const FillCheckResult& r) {
  std::string out;
  auto it = std::back_inserter(out);
  fmt::format_to(it,
                 "orders   {} sent, {} resting after the ack; left out: {} rejected, {} not acked, "
                 "{} ended before the ack, {} market/IOC/FOK, {} crossing the book at the ack\n",
                 r.orders_sent,
                 r.orders.size(),
                 r.rejected,
                 r.not_acked,
                 r.ended_before_ack,
                 r.not_resting,
                 r.crossed_at_ack);
  if (r.unknown_acks > 0 || r.model_full > 0) {
    fmt::format_to(it,
                   "warning  {} acks without an outbound copy, {} orders the queue model had no "
                   "room for\n",
                   r.unknown_acks,
                   r.model_full);
  }
  fmt::format_to(it, "market   {} book and trade messages\n", r.md_events);
  if (r.conservatism.empty()) return out;
  const FillCheckSummary live = r.summary(0);
  fmt::format_to(
      it, "live     {} filled, qty {}\n", live.both + live.live_only, qty_text(live.live_qty));
  fmt::format_to(it,
                 "\n{:<12} {:>7} {:>6} {:>9} {:>10} {:>7} {:>10} {:>12} {:>9}\n",
                 "conservatism",
                 "filled",
                 "both",
                 "live only",
                 "model only",
                 "neither",
                 "model/live",
                 "model qty",
                 "|dt| p50");
  for (std::size_t k = 0; k < r.conservatism.size(); ++k) {
    const FillCheckSummary s = r.summary(k);
    const std::string ratio = s.live_qty.is_zero() ? "-" : fmt::format("{:.3f}", s.qty_ratio());
    const std::string dt =
        s.median_abs_dt_ns < 0
            ? "-"
            : fmt::format("{:.1f}ms", static_cast<double>(s.median_abs_dt_ns) / 1e6);
    fmt::format_to(it,
                   "{:<12.2f} {:>7} {:>6} {:>9} {:>10} {:>7} {:>10} {:>12} {:>9}\n",
                   s.conservatism,
                   s.both + s.model_only,
                   s.both,
                   s.live_only,
                   s.model_only,
                   s.neither,
                   ratio,
                   qty_text(s.model_qty),
                   dt);
  }
  return out;
}

std::string fill_check_csv(const FillCheckResult& r) {
  std::string out;
  auto it = std::back_inserter(out);
  out +=
      "cl_ord_id,instrument,side,price,qty,queue_ahead,ack_ts_ns,end_ts_ns,resting_ns,end,"
      "live_filled,live_first_fill_ts_ns";
  for (const double c : r.conservatism)
    fmt::format_to(it, ",model_filled_c{0:.2f},model_first_fill_ts_ns_c{0:.2f}", c);
  out += '\n';
  for (const FillCheckOrder& o : r.orders) {
    fmt::format_to(it,
                   "{},{},{},{},{},{},{},{},{},{},{},{}",
                   o.cl_ord_id.value,
                   o.instrument.value,
                   o.side == Side::Buy ? "buy" : "sell",
                   price_text(o.price),
                   qty_text(o.qty),
                   qty_text(o.queue_ahead),
                   o.ack_ts.ns,
                   o.end_ts.ns,
                   o.resting().ns,
                   to_string(o.end),
                   qty_text(o.live_filled),
                   o.live_first_fill_ts.ns);
    for (std::size_t k = 0; k < o.model_filled.size(); ++k)
      fmt::format_to(it, ",{},{}", qty_text(o.model_filled[k]), o.model_first_fill_ts[k].ns);
    out += '\n';
  }
  return out;
}

}  // namespace fastmm::bt
