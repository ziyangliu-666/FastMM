// FIX simulation: an initiator FixSession (FixEncoder + FixDecoder) talks through an in-memory byte
// pipe to an acceptor FixSession bridged to the sim MatchingEngine, which answers with
// ExecutionReports / OrderCancelRejects and publishes MarketDataIncrementalRefresh. A seeded
// random order flow (new / cancel / replace, plus counter-party liquidity and lost messages that
// force resends) must leave the client with exactly the fills, open orders and book the matching
// engine reports.
#include "fix_test_util.hpp"

#include "fastmm/sim/matching_engine.hpp"

#include <map>
#include <memory>
#include <string>

using namespace fastmm;
using namespace fastmm::codecs;
using namespace fastmm::codecs::fix;
using namespace fastmm::codecs::fix::test;
using sim::CancelReason;
using sim::MatchingEngine;
using sim::NewOrder;
using sim::SimOrder;
using venues::OrderCommand;
using venues::ParseStatus;

namespace {

const Price kTick = Price::from_decimal("0.01").value();
const Qty kLot = Qty::from_decimal("0.001").value();
const Price kMid = Price::from_int(100);

// Acceptor side: FIX order entry -> MatchingEngine -> FIX execution reports and market data.
class SimVenue final : public sim::MatchingSink {
 public:
  SimVenue(Endpoint& ep, const FixSymbolTable& symbols)
      : ep_(ep), symbols_(symbols), engine_(std::make_unique<MatchingEngine>(2, this)) {}

  MatchingEngine& engine() { return *engine_; }

  void on_app(const FixView& v) {
    now_ = Timestamp{ep_.session.now_ns()};
    const std::string_view t = v.msg_type();
    if (t == msg::kNewOrderSingle) {
      new_order(v);
    } else if (t == msg::kOrderCancelRequest) {
      cancel_req_id_ = std::string(v.get(tag::kClOrdID));
      engine_->cancel(
          sim::kStrategyAccount, decode_cl_ord_id(v.get(tag::kOrigClOrdID)).value(), now_);
      cancel_req_id_.clear();
    } else if (t == msg::kOrderCancelReplaceRequest) {
      rep_ = Replacing{};
      rep_.active = true;
      rep_.orig = decode_cl_ord_id(v.get(tag::kOrigClOrdID)).value();
      rep_.neu = decode_cl_ord_id(v.get(tag::kClOrdID)).value();
      engine_->replace(sim::kStrategyAccount,
                       rep_.orig,
                       rep_.neu,
                       v.get_price(tag::kPrice).value(),
                       v.get_qty(tag::kOrderQty).value(),
                       now_);
      rep_.active = false;
    } else {
      FAIL("unexpected application message " << t);
    }
    flush_market_data();
  }

  void counterparty_order(Xoshiro256ss& rng) {
    NewOrder o;
    o.account = sim::kGeneratorAccount;
    o.cl_ord_id = make_cl_ord_id(7, ++generator_seq_);
    o.instrument = InstrumentId{0};
    o.side = rng.uniform(2) == 0 ? Side::Buy : Side::Sell;
    const std::int64_t ticks = o.side == Side::Buy ? rng.between(-6, 2) : rng.between(-2, 6);
    o.price = kMid + kTick * ticks;
    o.qty = kLot * rng.between(1, 6);
    engine_->submit(o, now_);
    flush_market_data();
  }

  std::map<std::uint64_t, std::int64_t> fills;  // cl_ord_id -> executed raw qty (engine truth)
  std::uint64_t fill_events = 0;

 private:
  struct Replacing {
    bool active = false;
    bool old_canceled = false;
    bool cancel_rejected = false;
    ClientOrderId orig{};
    ClientOrderId neu{};
    SimOrder old{};
  };
  struct MdChange {
    InstrumentId id;
    Side side;
    Price px;
    Qty qty;
  };

  void new_order(const FixView& v) {
    NewOrder o;
    o.account = sim::kStrategyAccount;
    o.cl_ord_id = decode_cl_ord_id(v.get(tag::kClOrdID)).value();
    o.instrument = symbols_.find(v.get(tag::kSymbol));
    o.side = v.get_char(tag::kSide) == kSideSell ? Side::Sell : Side::Buy;
    const bool post_only =
        v.get(tag::kExecInst).find(kExecInstParticipateDontInitiate) != std::string_view::npos;
    o.type = v.get_char(tag::kOrdType) == kOrdTypeMarket
                 ? OrderType::Market
                 : (post_only ? OrderType::PostOnly : OrderType::Limit);
    switch (v.get_char(tag::kTimeInForce).value_or(kTifDay)) {
      case kTifIoc:
        o.tif = TimeInForce::Ioc;
        break;
      case kTifFok:
        o.tif = TimeInForce::Fok;
        break;
      case kTifGtc:
        o.tif = TimeInForce::Gtc;
        break;
      default:
        o.tif = TimeInForce::Day;
        break;
    }
    o.price = v.get_price(tag::kPrice).value_or(Price{});
    o.qty = v.get_qty(tag::kOrderQty).value();
    engine_->submit(o, now_);
  }

  FixBuilder begin(std::string_view type) {
    return ep_.session.begin_app(std::span<char>(buf_.data(), buf_.size()), type);
  }
  void send(FixBuilder& b) {
    const std::size_t n = b.finish();
    REQUIRE(n > 0);
    REQUIRE(ep_.session.send_app(std::span<const char>(buf_.data(), n)));
  }
  std::string next_exec_id() { return "E" + std::to_string(++exec_seq_); }

  void exec_report(const SimOrder& o,
                   char exec_type,
                   char status,
                   Qty leaves,
                   std::string_view cl_ord_id,
                   const ClientOrderId* orig) {
    FixBuilder b = begin(msg::kExecutionReport);
    b.field_uint(tag::kOrderID, o.order_id).field(tag::kClOrdID, cl_ord_id);
    if (orig != nullptr) b.field(tag::kOrigClOrdID, encode_cl_ord_id(*orig).view());
    const std::string exec_id = next_exec_id();
    b.field(tag::kExecID, exec_id)
        .field_char(tag::kExecType, exec_type)
        .field_char(tag::kOrdStatus, status)
        .field(tag::kSymbol, symbols_.symbol(o.instrument))
        .field_char(tag::kSide, o.side == Side::Sell ? kSideSell : kSideBuy)
        .field_decimal(tag::kOrderQty, o.qty)
        .field_char(tag::kOrdType, o.type == OrderType::Market ? kOrdTypeMarket : kOrdTypeLimit);
    if (o.type != OrderType::Market) b.field_decimal(tag::kPrice, o.price);
    b.field_decimal(tag::kLeavesQty, leaves)
        .field_decimal(tag::kCumQty, o.cum_qty)
        .field_decimal(tag::kAvgPx, Price{})
        .field_timestamp(tag::kTransactTime, now_.ns);
    send(b);
  }
  void cancel_reject(std::string_view cl_ord_id, ClientOrderId orig, char response_to) {
    FixBuilder b = begin(msg::kOrderCancelReject);
    b.field(tag::kOrderID, "NONE")
        .field(tag::kClOrdID, cl_ord_id)
        .field(tag::kOrigClOrdID, encode_cl_ord_id(orig).view())
        .field_char(tag::kOrdStatus, ord_status::kRejected)
        .field_char(tag::kCxlRejResponseTo, response_to)
        .field_int(tag::kCxlRejReason, kCxlRejUnknownOrder)
        .field(tag::kText, "Unknown order");
    send(b);
  }

  // ---- MatchingSink ------------------------------------------------------------------------
  void on_ack(const SimOrder& o, Timestamp) override {
    if (o.account != sim::kStrategyAccount) return;
    const auto cl = encode_cl_ord_id(o.cl_ord_id);
    if (rep_.active && o.cl_ord_id == rep_.neu) {
      exec_report(o, exec_type::kReplaced, ord_status::kNew, o.leaves(), cl.view(), &rep_.orig);
      return;
    }
    exec_report(o, exec_type::kNew, ord_status::kNew, o.leaves(), cl.view(), nullptr);
  }
  void on_reject(const NewOrder& n, RejectReason why, Timestamp) override {
    if (n.account != sim::kStrategyAccount) return;
    if (rep_.active && n.cl_ord_id == rep_.neu) {
      if (rep_.cancel_rejected) return;  // reported by the OrderCancelReject
      if (rep_.old_canceled) {
        const auto cl = encode_cl_ord_id(rep_.old.cl_ord_id);
        exec_report(
            rep_.old, exec_type::kCanceled, ord_status::kCanceled, Qty{}, cl.view(), nullptr);
      }
    }
    FixBuilder b = begin(msg::kExecutionReport);
    const std::string exec_id = next_exec_id();
    b.field(tag::kOrderID, "NONE")
        .field(tag::kClOrdID, encode_cl_ord_id(n.cl_ord_id).view())
        .field(tag::kExecID, exec_id)
        .field_char(tag::kExecType, exec_type::kRejected)
        .field_char(tag::kOrdStatus, ord_status::kRejected)
        .field_int(tag::kOrdRejReason,
                   why == RejectReason::DuplicateId ? ord_rej::kDuplicateOrder : ord_rej::kOther)
        .field(tag::kText, to_string(why));
    if (n.instrument.valid()) b.field(tag::kSymbol, symbols_.symbol(n.instrument));
    b.field_char(tag::kSide, n.side == Side::Sell ? kSideSell : kSideBuy)
        .field_decimal(tag::kOrderQty, n.qty)
        .field_decimal(tag::kLeavesQty, Qty{})
        .field_decimal(tag::kCumQty, Qty{})
        .field_decimal(tag::kAvgPx, Price{});
    send(b);
  }
  void on_cancel(const SimOrder& o, CancelReason why, Timestamp) override {
    if (o.account != sim::kStrategyAccount) return;
    const auto cl = encode_cl_ord_id(o.cl_ord_id);
    if (why == CancelReason::Replaced) {
      if (rep_.active && o.cl_ord_id == rep_.orig) {
        rep_.old_canceled = true;
        rep_.old = o;
      }
      return;
    }
    if (sim::is_expiry(why)) {
      exec_report(o, exec_type::kExpired, ord_status::kExpired, Qty{}, cl.view(), nullptr);
      return;
    }
    if (why == CancelReason::Requested && !cancel_req_id_.empty()) {
      exec_report(
          o, exec_type::kCanceled, ord_status::kCanceled, Qty{}, cancel_req_id_, &o.cl_ord_id);
      return;
    }
    exec_report(o, exec_type::kCanceled, ord_status::kCanceled, Qty{}, cl.view(), nullptr);
  }
  void on_cancel_reject(sim::AccountId a, ClientOrderId id, InstrumentId, Timestamp) override {
    if (a != sim::kStrategyAccount) return;
    if (rep_.active) {
      rep_.cancel_rejected = true;
      cancel_reject(encode_cl_ord_id(rep_.neu).view(), id, kCxlRejToReplace);
      return;
    }
    cancel_reject(cancel_req_id_, id, kCxlRejToCancel);
  }
  void on_fill(const SimOrder& maker, const SimOrder& taker, Price px, Qty q, Timestamp) override {
    for (const SimOrder* o : {&maker, &taker}) {
      if (o->account != sim::kStrategyAccount) continue;
      fills[o->cl_ord_id.value] += q.raw;
      ++fill_events;
      FixBuilder b = begin(msg::kExecutionReport);
      const std::string exec_id = next_exec_id();
      b.field_uint(tag::kOrderID, o->order_id)
          .field(tag::kClOrdID, encode_cl_ord_id(o->cl_ord_id).view())
          .field(tag::kExecID, exec_id)
          .field_char(tag::kExecType, exec_type::kTrade)
          .field_char(tag::kOrdStatus,
                      o->leaves().is_zero() ? ord_status::kFilled : ord_status::kPartiallyFilled)
          .field(tag::kSymbol, symbols_.symbol(o->instrument))
          .field_char(tag::kSide, o->side == Side::Sell ? kSideSell : kSideBuy)
          .field_decimal(tag::kOrderQty, o->qty)
          .field_decimal(tag::kLastQty, q)
          .field_decimal(tag::kLastPx, px)
          .field_decimal(tag::kLeavesQty, o->leaves())
          .field_decimal(tag::kCumQty, o->cum_qty)
          .field_decimal(tag::kAvgPx, px)
          .field_int(tag::kLastLiquidityInd, o == &maker ? kLiquidityAdded : kLiquidityRemoved);
      send(b);
    }
  }
  void on_book_change(InstrumentId id, Side side, Price px, Qty qty, std::uint64_t) override {
    md_.push_back({id, side, px, qty});
  }

  void flush_market_data() {
    if (md_.empty()) return;
    FixBuilder b = begin(msg::kMarketDataIncrementalRefresh);
    b.field_uint(tag::kNoMDEntries, md_.size());
    InstrumentId last{};
    for (const MdChange& c : md_) {
      b.field_char(tag::kMDUpdateAction, c.qty.is_zero() ? kMdDelete : kMdChange)
          .field_char(tag::kMDEntryType, c.side == Side::Buy ? kMdBid : kMdOffer);
      if (c.id != last) b.field(tag::kSymbol, symbols_.symbol(c.id));  // carried forward
      last = c.id;
      b.field_decimal(tag::kMDEntryPx, c.px);
      if (!c.qty.is_zero()) b.field_decimal(tag::kMDEntrySize, c.qty);
    }
    send(b);
    md_.clear();
  }

  Endpoint& ep_;
  const FixSymbolTable& symbols_;
  std::unique_ptr<MatchingEngine> engine_;
  Timestamp now_{};
  Replacing rep_{};
  std::string cancel_req_id_;
  std::vector<MdChange> md_;
  std::vector<char> buf_ = std::vector<char>(1U << 16);
  std::uint64_t exec_seq_ = 0;
  std::uint32_t generator_seq_ = 0;
};

// Initiator side: the engine-facing view built only from decoded events.
struct Client {
  struct Open {
    std::int64_t qty;
    std::int64_t cum;
  };
  struct PendingReplace {
    std::uint64_t orig;
    std::int64_t qty;
  };

  Client(Endpoint& ep, const FixSymbolTable& symbols)
      : ep_(ep),
        decoder(std::make_unique<FixDecoder>(symbols, VenueId{5})),
        encoder(std::make_unique<FixEncoder>(ep.session, symbols)) {}

  void on_app(const FixView& v) {
    REQUIRE(decoder->decode_view(v, ep_.session.now_ns(), rec.sink) == ParseStatus::Ok);
    for (const auto& raw : rec.drain()) apply(raw);
  }

  void apply(const std::vector<std::byte>& raw) {
    switch (RecordingSink::type_of(raw)) {
      case EventType::OrderAck: {
        const auto& m = RecordingSink::as<OrderAckMsg>(raw);
        ++acks;
        if (const auto it = pending.find(m.cl_ord_id.value); it != pending.end()) {
          open.erase(it->second.orig);
          open[m.cl_ord_id.value] = Open{it->second.qty, 0};
          pending.erase(it);
          ++replaced;
        } else {
          CHECK(open.count(m.cl_ord_id.value) == 1);
        }
        break;
      }
      case EventType::OrderReject: {
        const auto& m = RecordingSink::as<OrderRejectMsg>(raw);
        ++rejects;
        if (pending.erase(m.cl_ord_id.value) == 0) open.erase(m.cl_ord_id.value);
        break;
      }
      case EventType::OrderCancelAck:
        ++cancel_acks;
        open.erase(RecordingSink::as<OrderCancelAckMsg>(raw).cl_ord_id.value);
        break;
      case EventType::OrderCancelReject:
        ++cancel_rejects;
        break;
      case EventType::OrderExpired:
        ++expired;
        open.erase(RecordingSink::as<OrderExpiredMsg>(raw).cl_ord_id.value);
        break;
      case EventType::OrderFill: {
        const auto& m = RecordingSink::as<OrderFillMsg>(raw);
        fills[m.cl_ord_id.value] += m.qty.raw;
        ++fill_events;
        net += m.side == Side::Buy ? m.qty.raw : -m.qty.raw;
        const auto it = open.find(m.cl_ord_id.value);
        REQUIRE(it != open.end());
        it->second.cum = m.cum_qty.raw;
        CHECK(m.leaves_qty.raw == it->second.qty - it->second.cum);
        if (m.leaves_qty.is_zero()) open.erase(it);
        break;
      }
      case EventType::BookDelta: {
        const auto& m = RecordingSink::as<BookDeltaMsg>(raw);
        REQUIRE(m.hdr.instrument == InstrumentId{0});
        for (const Level& l : m.bids()) set_level(bids, l);
        for (const Level& l : m.asks()) set_level(asks, l);
        ++book_deltas;
        break;
      }
      default:
        FAIL("unexpected event " << to_string(RecordingSink::type_of(raw)));
    }
  }
  static void set_level(std::map<std::int64_t, std::int64_t>& side, const Level& l) {
    if (l.qty.is_zero()) {
      side.erase(l.price.raw);
    } else {
      side[l.price.raw] = l.qty.raw;
    }
  }

  void send(const EventHeader& h) { REQUIRE(encoder->send(ep_.session, command_of(h))); }

  void new_order(Xoshiro256ss& rng) {
    OutNewOrderMsg n{};
    init_header(n, EventType::OutNewOrder, InstrumentId{0}, VenueId{5});
    n.cl_ord_id = make_cl_ord_id(1, ++seq);
    n.side = rng.uniform(2) == 0 ? Side::Buy : Side::Sell;
    const std::uint64_t kind = rng.uniform(10);
    n.type = kind < 7 ? OrderType::Limit : (kind < 9 ? OrderType::PostOnly : OrderType::Limit);
    n.tif = kind == 9 ? TimeInForce::Ioc : TimeInForce::Gtc;
    const std::int64_t ticks = n.side == Side::Buy ? rng.between(-5, 2) : rng.between(-2, 5);
    n.price = kMid + kTick * ticks;
    n.qty = kLot * rng.between(1, 5);
    open[n.cl_ord_id.value] = Open{n.qty.raw, 0};
    prices[n.cl_ord_id.value] = n.price.raw;
    send(n.hdr);
  }
  void cancel(Xoshiro256ss& rng) {
    OutCancelMsg c{};
    init_header(c, EventType::OutCancel, InstrumentId{0}, VenueId{5});
    if (open.empty()) return;
    auto it = open.begin();
    std::advance(it, static_cast<std::ptrdiff_t>(rng.uniform(open.size())));
    c.cl_ord_id = ClientOrderId{it->first};
    send(c.hdr);
  }
  void replace(Xoshiro256ss& rng) {
    if (open.empty()) return;
    auto it = open.begin();
    std::advance(it, static_cast<std::ptrdiff_t>(rng.uniform(open.size())));
    OutReplaceMsg r{};
    init_header(r, EventType::OutReplace, InstrumentId{0}, VenueId{5});
    r.orig_cl_ord_id = ClientOrderId{it->first};
    r.cl_ord_id = make_cl_ord_id(1, ++seq);
    const std::int64_t leaves = it->second.qty - it->second.cum;
    if (rng.uniform(2) == 0 && leaves > kLot.raw) {
      // Same price, smaller quantity: the matching engine amends in place.
      const OrderInfoLookup info = lookup_price(r.orig_cl_ord_id);
      r.price = info.price;
      r.qty = Qty::from_raw(leaves - kLot.raw);
    } else {
      r.price = kMid + kTick * rng.between(-5, 5);
      r.qty = kLot * rng.between(1, 5);
    }
    pending[r.cl_ord_id.value] = PendingReplace{r.orig_cl_ord_id.value, r.qty.raw};
    prices[r.cl_ord_id.value] = r.price.raw;
    send(r.hdr);
  }
  void cancel_unknown() {
    OutCancelMsg c{};
    init_header(c, EventType::OutCancel, InstrumentId{0}, VenueId{5});
    c.cl_ord_id = make_cl_ord_id(9, ++unknown_seq);
    encoder->remember(
        c.cl_ord_id, InstrumentId{0}, Side::Buy, OrderType::Limit, TimeInForce::Gtc, kMid, kLot);
    send(c.hdr);
  }

  struct OrderInfoLookup {
    Price price;
  };
  OrderInfoLookup lookup_price(ClientOrderId id) const {
    const auto it = prices.find(id.value);
    return {it == prices.end() ? kMid : Price::from_raw(it->second)};
  }

  Endpoint& ep_;
  RecordingSink rec{1U << 22};
  std::unique_ptr<FixDecoder> decoder;
  std::unique_ptr<FixEncoder> encoder;
  std::map<std::uint64_t, Open> open;
  std::map<std::uint64_t, PendingReplace> pending;
  std::map<std::uint64_t, std::int64_t> prices;
  std::map<std::uint64_t, std::int64_t> fills;
  std::map<std::int64_t, std::int64_t> bids;
  std::map<std::int64_t, std::int64_t> asks;
  std::uint64_t fill_events = 0;
  std::int64_t net = 0;
  std::uint32_t seq = 0;
  std::uint32_t unknown_seq = 0;
  std::uint64_t acks = 0, rejects = 0, cancel_acks = 0, cancel_rejects = 0, expired = 0,
                replaced = 0, book_deltas = 0;
};

std::map<std::int64_t, std::int64_t> engine_side(const MatchingEngine& e, Side side) {
  std::vector<Level> levels(4096);
  const std::size_t n = e.l2_snapshot(InstrumentId{0}, side, levels.data(), levels.size());
  std::map<std::int64_t, std::int64_t> out;
  for (std::size_t i = 0; i < n; ++i) out[levels[i].price.raw] = levels[i].qty.raw;
  return out;
}

}  // namespace

TEST_CASE("codecs.fix.sim: random order flow matches the matching engine") {
  for (const std::uint64_t seed : {1ULL, 7ULL, 2026ULL}) {
    CAPTURE(seed);
    Link link;
    link.rng.reseed(seed);
    const auto symbols = std::make_unique<FixSymbolTable>(test_symbols());
    auto venue = std::make_unique<SimVenue>(*link.acceptor, *symbols);
    auto client = std::make_unique<Client>(*link.initiator, *symbols);
    link.acceptor->on_app = [&](const FixView& v) { venue->on_app(v); };
    link.initiator->on_app = [&](const FixView& v) { client->on_app(v); };
    link.logon();

    // Lose ~2% of the venue's first transmissions: the session must recover them by resend.
    Xoshiro256ss loss(seed * 31 + 1);
    link.to_initiator.drop = [&](const std::string& m) {
      return field_of(m, tag::kPossDupFlag).empty() && loss.uniform(100) < 2;
    };

    Xoshiro256ss rng(seed);
    for (int step = 0; step < 3000; ++step) {
      const std::uint64_t r = rng.uniform(100);
      if (r < 30) {
        venue->counterparty_order(rng);
      } else if (r < 62) {
        client->new_order(rng);
      } else if (r < 78) {
        client->cancel(rng);
      } else if (r < 97) {
        client->replace(rng);
      } else {
        client->cancel_unknown();
      }
      link.pump();
    }
    // A final heartbeat exposes a trailing lost message.
    link.to_initiator.drop = nullptr;
    link.clock.ns += seconds(30).ns;
    link.acceptor->session.on_timer(link.clock.ns);
    link.pump();

    const FixSession& ini = link.initiator->session;
    const FixSession& acc = link.acceptor->session;
    CHECK(ini.state() == SessionState::Up);
    CHECK(acc.state() == SessionState::Up);
    CHECK(ini.next_target_seq() == acc.next_sender_seq());
    CHECK(acc.next_target_seq() == ini.next_sender_seq());
    CHECK(link.to_initiator.dropped > 0);
    CHECK(ini.stats().resend_requests_sent > 0);
    CHECK(acc.stats().messages_resent > 0);
    CHECK(client->decoder->stats().malformed == 0);
    CHECK(client->decoder->stats().duplicate_fills == 0);
    CHECK(client->encoder->stats().failures == 0);

    // Fills: identical per order, and the same net position as the engine's ledger.
    CHECK(client->fill_events == venue->fill_events);
    CHECK(client->fills == venue->fills);
    CHECK(client->net == venue->engine().ledger(sim::kStrategyAccount).net().raw);
    CHECK(client->fill_events > 100);

    // Open orders and their leaves.
    std::map<std::uint64_t, std::int64_t> engine_open;
    venue->engine().for_each_open_order([&](const SimOrder& o) {
      if (o.account == sim::kStrategyAccount) engine_open[o.cl_ord_id.value] = o.leaves().raw;
    });
    std::map<std::uint64_t, std::int64_t> client_open;
    for (const auto& [id, o] : client->open) client_open[id] = o.qty - o.cum;
    CHECK(client->pending.empty());
    CHECK(client_open == engine_open);

    // The book rebuilt from MarketDataIncrementalRefresh equals the engine's.
    CHECK(client->bids == engine_side(venue->engine(), Side::Buy));
    CHECK(client->asks == engine_side(venue->engine(), Side::Sell));

    // Every path was exercised.
    CHECK(client->acks > 0);
    CHECK(client->replaced > 0);
    CHECK(client->cancel_acks > 0);
    CHECK(client->cancel_rejects > 0);
    CHECK(client->rejects > 0);
    CHECK(client->expired > 0);
    CHECK(client->book_deltas > 0);
  }
}
