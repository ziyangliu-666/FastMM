#pragma once
// StrategyHarness<S>: test a strategy's hooks against the real engine (ADR-0012, section 8).
//
// The harness owns an Engine<S, SimClock, SimTransport, InlineFeed>, a simulated venue (the
// matching engine, fixed order and ack latency) and a virtual clock. Market data goes straight to
// the engine; the venue only sees orders, so our orders fill only through fill(). Every call runs
// the engine until it is idle, so the strategy's reaction is visible when the call returns:
//
//   fastmm::sim::StrategyHarness<MyMM> h({{"half_spread_bps", "10"}});
//   h.book("100.00", "100.02");       // snapshot: on_book runs, quotes go out
//   h.advance(milliseconds(1));       // the orders reach the venue and their acks come back
//   REQUIRE(h.working_orders().size() == 2);
//   h.fill(Side::Buy);                // a taker at the venue fills our best bid; on_fill runs
//   CHECK(h.engine().position(h.instrument()).qty.is_positive());
//   h.pull_quotes();                  // on_quoting(false); resume_quotes() gives on_quoting(true)
//   h.publish({{"half_spread_bps", "7.5"}});   // a ParamUpdate: new values, then on_params
//
// Not for the hot path: the helpers allocate, and the harness is single-threaded.
#include "fastmm/core/engine.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/sim/matching_engine.hpp"
#include "fastmm/sim/sim_transport.hpp"
#include "fastmm/strategies/param_publisher.hpp"
#include "fastmm/strategies/strategy.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm::sim {

// BTCUSDT on venue 0: tick 0.01, lot 0.001.
[[nodiscard]] inline InstrumentTable harness_instruments() {
  InstrumentTable t;
  Instrument i{};
  i.symbol = "BTCUSDT";
  i.venue = VenueId{0};
  i.flags = Instrument::kEnabled;
  i.tick = *Price::from_decimal("0.01");
  i.lot = *Qty::from_decimal("0.001");
  i.min_qty = i.lot;
  static_cast<void>(t.add(i));
  return t;
}

struct HarnessOptions {
  InstrumentTable instruments = harness_instruments();
  EngineConfig engine = [] {
    EngineConfig c;
    c.quotes.min_requote_interval = Duration{};  // requote on every change
    return c;
  }();
  Duration latency = microseconds(100);  // order out and ack back, each way
  Timestamp start{seconds(1'700'000'000).ns};
};

template <class S>
class StrategyHarness {
 public:
  using EngineType = Engine<S, SimClock, SimTransport, InlineFeed>;

  // Configures the strategy with `params` (std::invalid_argument on a bad value), then warms up
  // and starts the engine (on_start runs here).
  explicit StrategyHarness(const ParamMap& params = {}, HarnessOptions options = {})
      : options_(std::move(options)),
        clock_(options_.start),
        transport_(
            std::make_unique<SimTransport>(clock_, options_.instruments, transport_config())),
        feed_(std::make_unique<InlineFeed>(1U << 20)),
        strategy_(std::make_unique<S>()) {
    if constexpr (requires { strategy_->configure(params); }) {
      if (auto err = strategy_->configure(params)) throw std::invalid_argument(*err);
    } else {
      if (!params.empty()) throw std::invalid_argument("strategy takes no parameters");
    }
    engine_ = std::make_unique<EngineType>(
        options_.engine, options_.instruments, clock_, *transport_, *feed_, *strategy_);
    engine_->warm_up();
    engine_->start();
  }

  // ---- market data (delivered to the engine at the current time) ------------------------------

  // A one-level snapshot each side.
  void book(Price bid, Price ask, Qty size = Qty::from_int(1), InstrumentId id = InstrumentId{0}) {
    std::byte* p = feed_->reserve(BookDeltaMsg::size_for(1, 1));
    if (p == nullptr) throw std::length_error("StrategyHarness: feed full");
    auto* d = reinterpret_cast<BookDeltaMsg*>(p);
    std::memset(p, 0, BookDeltaMsg::size_for(1, 1));
    init_header(*d, EventType::BookSnapshot, id, venue(id), BookDeltaMsg::size_for(1, 1));
    d->hdr.flags |= EventHeader::kSnapshot;
    stamp(d->hdr);
    d->bid_count = d->ask_count = 1;
    d->last_update_id = ++seq_;
    d->levels()[0] = Level{bid, size};
    d->levels()[1] = Level{ask, size};
    feed_->commit();
    drain();
  }
  void book(std::string_view bid, std::string_view ask, InstrumentId id = InstrumentId{0}) {
    book(price(bid), price(ask), Qty::from_int(1), id);
  }
  void trade(Price px, Qty qty, Side aggressor, InstrumentId id = InstrumentId{0}) {
    TradeMsg t{};
    init_header(t, EventType::Trade, id, venue(id));
    t.price = px;
    t.qty = qty;
    t.aggressor = aggressor;
    t.trade_id = ++seq_;
    push(t.hdr);
  }

  // ---- orders ---------------------------------------------------------------------------------

  // Fills our best working order on `side` (quantity 0: its whole remainder) with a taker order
  // at the venue, then advances by the ack latency so the fill reaches the strategy. False when
  // there is no working order on that side.
  bool fill(Side side, Qty qty = Qty{}, InstrumentId id = InstrumentId{0}) {
    const std::vector<Order> orders = working_orders(id);
    const auto it = std::find_if(
        orders.begin(), orders.end(), [side](const Order& o) { return o.side == side; });
    if (it == orders.end()) return false;
    NewOrder taker;
    taker.account = kTakerAccount;
    taker.cl_ord_id = ClientOrderId{++taker_seq_};
    taker.instrument = id;
    taker.side = opposite(side);
    taker.type = OrderType::Limit;
    taker.tif = TimeInForce::Ioc;
    taker.price = it->price;
    taker.qty = qty.is_positive() ? qty : it->leaves_qty();
    static_cast<void>(transport_->matching_engine().submit(taker, clock_.now()));
    advance(options_.latency);
    return true;
  }
  // Our working orders (Live or PartiallyFilled): bids best first, then asks best first.
  [[nodiscard]] std::vector<Order> working_orders(InstrumentId id = InstrumentId{0}) const {
    std::vector<Order> v;
    engine_->oms().for_each_open_order(id, [&](Handle<Order>, const Order& o) {
      if (o.is_working()) v.push_back(o);
    });
    std::sort(v.begin(), v.end(), [](const Order& a, const Order& b) {
      if (a.side != b.side) return a.side == Side::Buy;
      return a.side == Side::Buy ? a.price > b.price : a.price < b.price;
    });
    return v;
  }

  // ---- venue and control events ---------------------------------------------------------------

  // Channel 0 is market data (the engine clears the venue's books), 1 is order entry.
  void disconnect(std::uint8_t channel = 0, VenueId v = VenueId{0}) {
    connection(ConnState::Disconnected, channel, v);
  }
  void reconnect(std::uint8_t channel = 0, VenueId v = VenueId{0}) {
    connection(ConnState::Live, channel, v);
  }
  void pull_quotes() { control(ControlCommand::PullQuotes); }
  void resume_quotes() { control(ControlCommand::ResumeQuotes); }

  // A ParamUpdate of `values` (names and values as in [strategy.params]) for all instruments, at
  // the current time: the strategy's parameters change and on_params runs. Validated against the
  // strategy's current parameters; std::invalid_argument when a publisher would reject it.
  void publish(const std::vector<ParamPublisher::ParamValue>& values) {
    ParamUpdateMsg m{};
    if constexpr (requires { strategy_->params(); }) {
      const ParamPublisher pub(ParamSink{}, strategy_->params());
      if (auto err = pub.build(values, ParamPublisher::kAllInstruments, m))
        throw std::invalid_argument(*err);
    } else if (!values.empty()) {
      throw std::invalid_argument("strategy takes no parameters");
    } else {
      init_header(m, EventType::ParamUpdate);
    }
    push(m.hdr);
  }

  // Any other message, delivered to the engine at the current time.
  void push(EventHeader& h) {
    stamp(h);
    if (!feed_->push(h)) throw std::length_error("StrategyHarness: feed full");
    drain();
  }

  // ---- time -------------------------------------------------------------------------------------

  // Moves virtual time forward by `d`, delivering orders to the venue, acks and fills to the engine
  // and firing timers in time order.
  void advance(Duration d) {
    const Timestamp until = clock_.now() + d;
    for (;;) {
      const Timestamp t_ord = transport_->next_order_arrival();
      const Timestamp t_in = transport_->next_inbound_ts();
      const Timestamp t_timer = engine_->timers().next_expiry();
      Timestamp t = t_ord;
      if (t_in < t) t = t_in;
      if (t_timer < t) t = t_timer;
      if (t > until) break;
      if (t > clock_.now()) clock_.set(t);
      if (t == t_ord) {
        transport_->process_order_arrival();
      } else if (t == t_in) {
        static_cast<void>(transport_->deliver_next_inbound(*feed_));
        drain();
      } else if (engine_->step() == 0 && engine_->timers().next_expiry() <= clock_.now()) {
        clock_.set(clock_.now() + Duration{1});  // the wheel fires on its next tick
      }
    }
    clock_.set(until);
    drain();
  }
  [[nodiscard]] Timestamp now() const noexcept { return clock_.now(); }

  // ---- access -----------------------------------------------------------------------------------

  [[nodiscard]] EngineType& engine() noexcept { return *engine_; }
  [[nodiscard]] S& strategy() noexcept { return *strategy_; }
  [[nodiscard]] SimTransport& venue_transport() noexcept { return *transport_; }
  [[nodiscard]] static InstrumentId instrument() noexcept { return InstrumentId{0}; }

  [[nodiscard]] static Price price(std::string_view s) {
    auto r = Price::from_decimal(s);
    if (!r) throw std::invalid_argument("StrategyHarness: bad price " + std::string(s));
    return *r;
  }
  [[nodiscard]] static Qty quantity(std::string_view s) {
    auto r = Qty::from_decimal(s);
    if (!r) throw std::invalid_argument("StrategyHarness: bad quantity " + std::string(s));
    return *r;
  }

 private:
  static constexpr AccountId kTakerAccount = 7;

  [[nodiscard]] SimTransportConfig transport_config() const {
    SimTransportConfig c;
    c.order_out = LatencyParams{options_.latency, Duration{}};
    c.ack_in = LatencyParams{options_.latency, Duration{}};
    c.md_in = LatencyParams{};
    return c;
  }
  [[nodiscard]] VenueId venue(InstrumentId id) const {
    return options_.instruments.contains(id) ? options_.instruments.get(id).venue : VenueId{0};
  }
  void stamp(EventHeader& h) const noexcept {
    h.recv_ts = clock_.now();
    h.exch_ts = clock_.now();
    h.t0_cycles = clock_.cycles();
  }
  void connection(ConnState state, std::uint8_t channel, VenueId v) {
    ConnectionStateMsg m{};
    init_header(m, EventType::ConnectionState, InstrumentId{}, v);
    m.state = state;
    m.channel = channel;
    push(m.hdr);
  }
  void control(ControlCommand command) {
    ControlMsg c{};
    init_header(c, EventType::Control);
    c.command = command;
    push(c.hdr);
  }
  void drain() {
    while (engine_->step() > 0) {
    }
  }

  HarnessOptions options_;
  SimClock clock_;
  std::unique_ptr<SimTransport> transport_;
  std::unique_ptr<InlineFeed> feed_;
  std::unique_ptr<S> strategy_;
  std::unique_ptr<EngineType> engine_;
  std::uint64_t seq_ = 0;
  std::uint64_t taker_seq_ = 0;
};

}  // namespace fastmm::sim
