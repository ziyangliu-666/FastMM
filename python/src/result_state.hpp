#pragma once
// BacktestResult <-> bytes, for pickle: a sweep of a Python strategy runs its points in worker
// processes and sends each result back to the parent this way. The layout is private to one build
// of the module (no compatibility across versions): a magic and version word, then every field in
// declaration order, trivially copyable structs as their bytes.
#include "fastmm/backtest/result.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace fastmm::py_bind::result_state {

inline constexpr std::uint64_t kMagic = 0x31'54'4c'53'45'52'4d'46ULL;  // "FMRESLT1"

class Writer {
 public:
  template <class T>
  void pod(const T& v) {
    static_assert(std::is_trivially_copyable_v<T>);
    const auto* p = reinterpret_cast<const char*>(&v);
    out_.append(p, sizeof(T));
  }
  template <class T>
  void vec(const std::vector<T>& v) {
    static_assert(std::is_trivially_copyable_v<T>);
    count(v.size());
    if (!v.empty()) out_.append(reinterpret_cast<const char*>(v.data()), v.size() * sizeof(T));
  }
  void str(const std::string& s) {
    count(s.size());
    out_.append(s);
  }
  void count(std::size_t n) {
    const std::uint64_t v = n;
    pod(v);
  }
  std::string take() { return std::move(out_); }

 private:
  std::string out_;
};

class Reader {
 public:
  explicit Reader(std::string_view in) : in_(in) {}
  template <class T>
  void pod(T& v) {
    static_assert(std::is_trivially_copyable_v<T>);
    need(sizeof(T));
    std::memcpy(&v, in_.data() + at_, sizeof(T));
    at_ += sizeof(T);
  }
  template <class T>
  void vec(std::vector<T>& v) {
    static_assert(std::is_trivially_copyable_v<T>);
    const std::size_t n = count(sizeof(T));
    v.resize(n);
    if (n != 0) std::memcpy(v.data(), in_.data() + at_, n * sizeof(T));
    at_ += n * sizeof(T);
  }
  void str(std::string& s) {
    const std::size_t n = count(1);
    s.assign(in_.data() + at_, n);
    at_ += n;
  }
  std::size_t count(std::size_t elem) {
    std::uint64_t n = 0;
    pod(n);
    if (elem != 0 && n > (in_.size() - at_) / elem) fail();
    return static_cast<std::size_t>(n);
  }
  [[nodiscard]] bool done() const noexcept { return at_ == in_.size(); }

 private:
  void need(std::size_t n) const {
    if (in_.size() - at_ < n) fail();
  }
  [[noreturn]] static void fail() {
    throw std::invalid_argument("BacktestResult: truncated or corrupt pickle state");
  }
  std::string_view in_;
  std::size_t at_ = 0;
};

// One function per struct, for both directions: `a` is a Writer (const fields) or a Reader.
template <class A, class FillRowsT>
void fills(A& a, FillRowsT& f) {
  a.vec(f.ts);
  a.vec(f.instrument);
  a.vec(f.side);
  a.vec(f.price);
  a.vec(f.qty);
  a.vec(f.fee);
  a.vec(f.cl_ord_id);
  a.vec(f.liquidity);
  a.vec(f.mid);
  a.vec(f.best_bid);
  a.vec(f.best_ask);
  a.vec(f.queue_ahead);
  a.vec(f.markout_horizon_ns);
}

template <class A, class EquityRowsT>
void equity_columns(A& a, EquityRowsT& e) {
  a.vec(e.ts);
  a.vec(e.realized);
  a.vec(e.unrealized);
  a.vec(e.fees);
  a.vec(e.position);
  a.vec(e.mid);
  a.vec(e.quoted);
}

template <class A, class R>
void instrument_equity(A& a, R& r) {
  a.vec(r.pnl);
  a.vec(r.position);
  a.vec(r.mid);
  a.vec(r.quoted);
}

template <class A, class OrderRowsT>
void orders(A& a, OrderRowsT& o) {
  a.vec(o.ts);
  a.vec(o.venue_ts);
  a.vec(o.trigger_ts);
  a.vec(o.cl_ord_id);
  a.vec(o.instrument);
  a.vec(o.side);
  a.vec(o.price);
  a.vec(o.qty);
  a.vec(o.kind);
  a.vec(o.type);
}

template <class A, class M>
void metric_scalars(A& a, M& m) {
  a.pod(m.net_pnl);
  a.pod(m.realized_pnl);
  a.pod(m.unrealized_pnl);
  a.pod(m.fees);
  a.pod(m.final_position);
  a.pod(m.sharpe_bar);
  a.pod(m.sharpe_annualized);
  a.pod(m.max_drawdown);
  a.pod(m.max_drawdown_pct);
  a.pod(m.fills);
  a.pod(m.maker_fills);
  a.pod(m.taker_fills);
  a.pod(m.orders);
  a.pod(m.cancels);
  a.pod(m.replaces);
  a.pod(m.rejects);
  a.pod(m.fill_ratio);
  a.pod(m.spread_captured_bps);
  a.pod(m.volume_base);
  a.pod(m.volume_quote);
  a.pod(m.inventory_mean);
  a.pod(m.inventory_abs_mean);
  a.pod(m.inventory_max);
  a.pod(m.quote_uptime);
  a.pod(m.bars);
  a.pod(m.duration_s);
  a.pod(m.virtual_tick_to_order_p50_ns);
  a.pod(m.virtual_tick_to_order_p99_ns);
  a.pod(m.wall_tick_to_order_p50_ns);
  a.pod(m.wall_tick_to_order_p99_ns);
  a.pod(m.decomposition);
  a.pod(m.fill_quality);
}

template <class A, class H>
void markout_scalars(A& a, H& h) {
  a.pod(h.horizon_ns);
  a.pod(h.total);
  a.pod(h.buy);
  a.pod(h.sell);
  a.pod(h.maker);
  a.pod(h.taker);
  a.vec(h.instrument);
  a.pod(h.excluded_fills);
  a.pod(h.excluded_past_end);
  a.pod(h.excluded_no_mid);
}

template <class A, class T>
void transfer(A& a, T& t) {
  a.pod(t.ts);
  a.pod(t.from);
  a.pod(t.to);
  a.str(t.asset);
  a.pod(t.amount);
  a.pod(t.state);
  a.str(t.client_id);
}

template <class A, class B>
void treasury_scalars(A& a, B& t) {
  a.pod(t.pools);
  a.pod(t.sent);
  a.pod(t.done);
  a.pod(t.failed);
  a.pod(t.timed_out);
  a.pod(t.limited);
  a.pod(t.dry_run_plans);
}

template <class A, class S>
void slow_method(A& a, S& s) {
  a.str(s.name);
  a.pod(s.calls);
  a.pod(s.wall_p50_ns);
  a.pod(s.wall_p99_ns);
  a.pod(s.wall_max_ns);
}

template <class A, class R>
void result_scalars(A& a, R& r) {
  a.str(r.strategy);
  a.pod(r.seed);
  a.pod(r.engine);
  a.pod(r.transport);
  a.str(r.outbound_sha256);
  a.pod(r.outbound_messages);
  a.pod(r.md_events);
  a.pod(r.md_reordered);
  a.pod(r.md_late);
  a.pod(r.engine_steps);
  a.pod(r.start_ts);
  a.pod(r.end_ts);
  a.pod(r.wall_seconds);
}

inline std::string to_bytes(const bt::BacktestResult& r) {
  Writer w;
  w.pod(kMagic);
  result_scalars(w, r);
  w.count(r.params.size());
  for (const auto& [k, v] : r.params) {
    w.str(k);
    w.str(v);
  }
  fills(w, r.fills);
  w.count(r.fills.markout_mid.size());
  for (const auto& col : r.fills.markout_mid) w.vec(col);
  equity_columns(w, r.equity);
  w.count(r.equity.by_instrument.size());
  for (const auto& e : r.equity.by_instrument) instrument_equity(w, e);
  orders(w, r.orders);
  metric_scalars(w, r.metrics);
  w.count(r.metrics.markouts.size());
  for (const auto& h : r.metrics.markouts) markout_scalars(w, h);
  w.count(r.slow_methods.size());
  for (const auto& s : r.slow_methods) slow_method(w, s);
  treasury_scalars(w, r.treasury);
  w.count(r.treasury.transfers.size());
  for (const auto& t : r.treasury.transfers) transfer(w, t);
  return w.take();
}

inline bt::BacktestResult from_bytes(std::string_view in) {
  Reader rd(in);
  std::uint64_t magic = 0;
  rd.pod(magic);
  if (magic != kMagic) throw std::invalid_argument("BacktestResult: not a pickle state");
  bt::BacktestResult r;
  result_scalars(rd, r);
  for (std::size_t n = rd.count(16); n > 0; --n) {
    std::string k;
    std::string v;
    rd.str(k);
    rd.str(v);
    r.params.emplace(std::move(k), std::move(v));
  }
  fills(rd, r.fills);
  r.fills.markout_mid.resize(rd.count(8));
  for (auto& col : r.fills.markout_mid) rd.vec(col);
  equity_columns(rd, r.equity);
  r.equity.by_instrument.resize(rd.count(32));
  for (auto& e : r.equity.by_instrument) instrument_equity(rd, e);
  orders(rd, r.orders);
  metric_scalars(rd, r.metrics);
  r.metrics.markouts.resize(rd.count(8));
  for (auto& h : r.metrics.markouts) markout_scalars(rd, h);
  r.slow_methods.resize(rd.count(8));
  for (auto& s : r.slow_methods) slow_method(rd, s);
  treasury_scalars(rd, r.treasury);
  r.treasury.transfers.resize(rd.count(16));
  for (auto& t : r.treasury.transfers) transfer(rd, t);
  if (!rd.done()) throw std::invalid_argument("BacktestResult: trailing bytes in pickle state");
  return r;
}

}  // namespace fastmm::py_bind::result_state
