#include "fastmm/backtest/recorded_latency.hpp"

#include "fastmm/core/messages.hpp"
#include "fastmm/core/own_quantity.hpp"

namespace fastmm::bt {

namespace {

constexpr std::int64_t kMs = 1'000'000;

std::uint64_t trade_key(std::uint32_t instrument, std::uint64_t trade) {
  return (static_cast<std::uint64_t>(instrument) << 44U) ^ trade;
}

}  // namespace

void RecordedLatencyCollector::on_event(const EventHeader& h) {
  using Path = sim::RecordedLatency::Path;
  if ((h.flags & EventHeader::kOutbound) != 0) {
    if ((h.flags & EventHeader::kDropped) != 0) return;
    if (h.type == EventType::OutNewOrder || h.type == EventType::OutReplace) {
      const std::uint64_t id = h.type == EventType::OutNewOrder
                                   ? msg_cast<OutNewOrderMsg>(&h).cl_ord_id.value
                                   : msg_cast<OutReplaceMsg>(&h).cl_ord_id.value;
      Sent& s = sent_[id];
      s.sent = h.recv_ts.ns;
      s.instrument = h.instrument.value;
      s.venue = h.venue;
    } else if (h.type == EventType::OutCancel) {
      const auto it = sent_.find(msg_cast<OutCancelMsg>(&h).cl_ord_id.value);
      if (it != sent_.end() && it->second.cancel == 0) it->second.cancel = h.recv_ts.ns;
    }
    return;
  }
  if (h.type == EventType::Trade) {
    if (h.exch_ts.valid())
      trades_.emplace(trade_key(h.instrument.value, msg_cast<TradeMsg>(&h).trade_id), h.exch_ts.ns);
    return;
  }
  const bool order_event = h.type == EventType::OrderAck || h.type == EventType::OrderCancelAck ||
                           h.type == EventType::OrderFill;
  if (!order_event || !h.exch_ts.valid()) return;
  ++venue_times_;
  if (h.exch_ts.ns % kMs != 0) ms_ = false;
  Reply r;
  r.venue = h.venue;
  r.instrument = h.instrument.value;
  r.venue_ns = h.exch_ts.ns;
  r.recv_ns = h.recv_ts.ns;
  if (h.type == EventType::OrderFill) {
    const auto& m = msg_cast<OrderFillMsg>(&h);
    if ((m.flags & OrderFillMsg::kReplayed) != 0 || (h.flags & EventHeader::kSynthetic) != 0)
      return;
    r.path = Path::Fill;
    r.trade = numeric_exec_id(m.exec_id);
    replies_.push_back(r);
    return;
  }
  const std::uint64_t id = h.type == EventType::OrderAck
                               ? msg_cast<OrderAckMsg>(&h).cl_ord_id.value
                               : msg_cast<OrderCancelAckMsg>(&h).cl_ord_id.value;
  const auto it = sent_.find(id);
  if (it == sent_.end()) return;
  Sent& s = it->second;
  if (h.type == EventType::OrderAck) {
    if (s.acked) return;  // the first of its copies (an API's reply and its private stream's)
    s.acked = true;
    r.path = Path::New;
    r.sent_ns = s.sent;
  } else {
    if (s.cancel_acked || s.cancel == 0) return;
    s.cancel_acked = true;
    r.path = Path::Cancel;
    r.sent_ns = s.cancel;
  }
  r.instrument = s.instrument;
  replies_.push_back(r);
}

sim::RecordedLatency RecordedLatencyCollector::finish() const {
  using Path = sim::RecordedLatency::Path;
  // A venue time in whole milliseconds is somewhere in its millisecond: take the middle.
  const std::int64_t half = ms_ && venue_times_ > 0 ? kMs / 2 : 0;
  std::vector<sim::RecordedLatency::Sample> out;
  out.reserve(replies_.size() * 2);
  for (const Reply& r : replies_) {
    std::int64_t venue_ns = r.venue_ns + half;
    if (r.path == Path::Fill && r.trade != 0) {
      if (const auto t = trades_.find(trade_key(r.instrument, r.trade)); t != trades_.end())
        venue_ns = t->second;  // the public trade's time is exact
    }
    if (r.sent_ns != 0) {  // the way out: send to the venue's time
      sim::RecordedLatency::Sample s;
      s.t_ns = r.sent_ns;
      s.took_ns = venue_ns - r.sent_ns;
      s.instrument = r.instrument;
      s.venue = r.venue;
      s.path = r.path;
      if (s.took_ns > 0) out.push_back(s);
    }
    sim::RecordedLatency::Sample back;  // the way back: the venue's time to the receipt
    back.t_ns = venue_ns;
    back.took_ns = r.recv_ns - venue_ns;
    back.instrument = r.instrument;
    back.venue = r.venue;
    back.path = r.path == Path::Fill ? Path::Fill : Path::Ack;
    if (back.took_ns > 0) out.push_back(back);
  }
  return sim::RecordedLatency(out);
}

sim::RecordedLatency recorded_latency(std::span<JournalReader> parts) {
  RecordedLatencyCollector c;
  for (JournalReader& r : parts) {
    r.for_each([&](const EventHeader* h) { c.on_event(*h); });
    r.reset();
  }
  return c.finish();
}

}  // namespace fastmm::bt
