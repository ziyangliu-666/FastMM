// OUCH simulation (plan 7): an OUCH client (OuchEncoder -> SoupBinTCP ClientSession -> byte
// pipe) talks to a small OUCH acceptor (SoupBinTCP ServerSession -> inbound OUCH -> simulator
// MatchingEngine -> outbound OUCH as Sequenced Data). A second account trades against the
// client's orders directly in the engine. The client decodes everything into engine events.
//
// Checked for OUCH 4.2 and OUCH 5.0 over many seeds: every fill the engine records for the
// client account arrives as an OrderFillMsg (count, bought / sold quantity and notional match
// the engine ledger), the client's open-order table equals the engine's open orders (ids and
// leaves), every accepted order was acked, cancels and replaces round-trip.
#include "nasdaq_test_util.hpp"

#include "fastmm/codecs/ouch/ouch42.hpp"
#include "fastmm/codecs/ouch/ouch50.hpp"
#include "fastmm/codecs/soupbin/soupbin_session.hpp"
#include "fastmm/sim/matching_engine.hpp"

#include <array>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <unordered_map>

using namespace fastmm;
using namespace fastmm::sim;
using namespace fastmm::codecs;
using fastmm::codecs::test::BytePipe;
using fastmm::codecs::test::Bytes;
using fastmm::codecs::test::RecordingSink;

namespace {

constexpr InstrumentId kInst{0};
constexpr AccountId kClient = 1;
constexpr AccountId kStreet = 2;
const Price kTick = Price::from_decimal("0.01").value();
Price ticks(std::int64_t t) {
  return Price::from_raw(t * kTick.raw);
}

// Protocol-neutral part of the acceptor: inbound requests -> MatchingEngine, engine effects for
// the client account -> outbound callbacks. Wire keys (4.2 token value, 5.0 UserRefNum) are the
// engine's ClientOrderIds.
class Bridge : public MatchingSink {
 public:
  explicit Bridge(MatchingEngine& eng) : eng_(eng) {}

  void enter(std::uint64_t key,
             Side side,
             Price price,
             Qty qty,
             TimeInForce tif,
             OrderType type,
             Timestamp now) {
    NewOrder o;
    o.account = kClient;
    o.cl_ord_id = ClientOrderId{key};
    o.instrument = kInst;
    o.side = side;
    o.type = type;
    o.tif = tif;
    o.price = price;
    o.qty = qty;
    eng_.submit(o, now);
  }
  // OUCH 4.2 2.2 / OUCH 5.0 2.2: a replace of an order that is no longer live is silently
  // ignored; a live order whose replacement is invalid (here: the engine rejects the new leg,
  // e.g. a post-only price that would cross) is taken out of the book with a Canceled message.
  void replace(std::uint64_t orig, std::uint64_t key, Price price, Qty qty, Timestamp now) {
    const SimOrder* old = eng_.find(kClient, ClientOrderId{orig});
    if (old == nullptr) return;
    const Qty old_leaves = old->leaves();
    replacing_ = true;
    replaced_ = false;
    replace_orig_ = orig;
    eng_.replace(kClient, ClientOrderId{orig}, ClientOrderId{key}, price, qty, now);
    replacing_ = false;
    if (!replaced_ && eng_.find(kClient, ClientOrderId{orig}) == nullptr)
      send_canceled(orig, old_leaves, 'U');
  }
  // "Superfluous Cancel Order Messages are silently ignored": the engine's cancel reject for an
  // unknown order is not forwarded (MatchingSink::on_cancel_reject is not overridden).
  void cancel(std::uint64_t key, Timestamp now) { eng_.cancel(kClient, ClientOrderId{key}, now); }

  void on_ack(const SimOrder& o, Timestamp) override {
    if (o.account != kClient) return;
    if (replacing_) {
      replaced_ = true;
      send_replaced(o.cl_ord_id.value, replace_orig_, o);
    } else {
      send_accepted(o.cl_ord_id.value, o);
    }
  }
  void on_reject(const NewOrder& n, RejectReason, Timestamp) override {
    if (n.account != kClient || replacing_) return;  // a replace of a dead order is ignored
    send_rejected(n.cl_ord_id.value);
  }
  void on_cancel(const SimOrder& o, CancelReason why, Timestamp) override {
    if (o.account != kClient || why == CancelReason::Replaced) return;
    char reason = 'U';
    if (is_expiry(why)) reason = 'I';
    if (why == CancelReason::Stp) reason = 'Q';
    send_canceled(o.cl_ord_id.value, o.leaves(), reason);
  }
  void on_fill(
      const SimOrder& maker, const SimOrder& taker, Price px, Qty qty, Timestamp) override {
    ++match_;
    if (maker.account == kClient) send_executed(maker.cl_ord_id.value, qty, px, 'A', match_);
    if (taker.account == kClient) send_executed(taker.cl_ord_id.value, qty, px, 'R', match_);
  }

  std::vector<Bytes> outbox;  // outbound OUCH messages waiting for the SoupBinTCP server
  std::uint64_t ts = 34'200'000'000'000ULL;

 protected:
  virtual void send_accepted(std::uint64_t key, const SimOrder& o) = 0;
  virtual void send_replaced(std::uint64_t key, std::uint64_t orig, const SimOrder& o) = 0;
  virtual void send_rejected(std::uint64_t key) = 0;
  virtual void send_canceled(std::uint64_t key, Qty decrement, char reason) = 0;
  virtual void send_executed(
      std::uint64_t key, Qty qty, Price px, char liquidity, std::uint64_t match) = 0;
  void push(std::span<std::byte> buf, std::size_t n) {
    REQUIRE(n != 0);
    outbox.emplace_back(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n));
  }
  static std::uint32_t shares(Qty q) {
    std::uint32_t n = 0;
    REQUIRE(nasdaq::qty_to_shares(q, n));
    return n;
  }

  MatchingEngine& eng_;
  bool replacing_ = false;
  bool replaced_ = false;
  std::uint64_t replace_orig_ = 0;
  std::uint64_t match_ = 0;
};

class Ouch42Acceptor final : public Bridge {
 public:
  using Bridge::Bridge;
  Timestamp now{};

  void on_unsequenced(std::span<const std::byte> msg) noexcept {
    REQUIRE_FALSE(msg.empty());
    const auto type = static_cast<char>(msg[0]);
    REQUIRE(msg.size() == ouch42::inbound_length(type));
    switch (type) {
      case 'O': {
        const auto& m = ouch::view_as<ouch42::EnterOrder>(msg.data());
        const auto id = ouch42::token_to_cl_ord_id(m.order_token);
        REQUIRE(id.has_value());
        enters_[id->value] = m;
        Side side = Side::Buy;
        REQUIRE(ouch::side_from_code(m.buy_sell_indicator, side));
        const std::uint32_t n = m.shares.get();
        TimeInForce tif = TimeInForce::Gtc;
        if (m.time_in_force.get() == ouch42::kTifImmediateOrCancel)
          tif = m.minimum_quantity.get() == n ? TimeInForce::Fok : TimeInForce::Ioc;
        enter(id->value,
              side,
              nasdaq::price4_to_price(m.price.get()),
              nasdaq::shares_to_qty(n),
              tif,
              m.display == 'P' ? OrderType::PostOnly : OrderType::Limit,
              now);
        break;
      }
      case 'U': {
        const auto& m = ouch::view_as<ouch42::ReplaceOrder>(msg.data());
        const auto orig = ouch42::token_to_cl_ord_id(m.existing_order_token);
        const auto id = ouch42::token_to_cl_ord_id(m.replacement_order_token);
        REQUIRE((orig.has_value() && id.has_value()));
        last_replace_ = m;
        replace(orig->value,
                id->value,
                nasdaq::price4_to_price(m.price.get()),
                nasdaq::shares_to_qty(m.shares.get()),
                now);
        break;
      }
      case 'X': {
        const auto& m = ouch::view_as<ouch42::CancelOrder>(msg.data());
        const auto id = ouch42::token_to_cl_ord_id(m.order_token);
        REQUIRE(id.has_value());
        REQUIRE(m.shares.get() == 0);
        cancel(id->value, now);
        break;
      }
      default:
        FAIL("unexpected inbound OUCH 4.2 message");
    }
  }

 private:
  void send_accepted(std::uint64_t key, const SimOrder& o) override {
    push(buf_, ouch42::host::accepted(buf_, ++ts, enters_.at(key), shares(o.qty), o.order_id, 'L'));
  }
  void send_replaced(std::uint64_t key, std::uint64_t orig, const SimOrder& o) override {
    const ouch42::EnterOrder original = enters_.at(orig);
    enters_[key] = original;
    push(buf_,
         ouch42::host::replaced(
             buf_, ++ts, last_replace_, original, shares(o.leaves()), o.order_id, 'L'));
  }
  void send_rejected(std::uint64_t key) override {
    char tok[14];
    ouch42::put_token(tok, ClientOrderId{key});
    push(buf_, ouch42::host::rejected(buf_, ++ts, tok, 'O'));
  }
  void send_canceled(std::uint64_t key, Qty decrement, char reason) override {
    char tok[14];
    ouch42::put_token(tok, ClientOrderId{key});
    push(buf_, ouch42::host::canceled(buf_, ++ts, tok, shares(decrement), reason));
  }
  void send_executed(
      std::uint64_t key, Qty qty, Price px, char liquidity, std::uint64_t match) override {
    char tok[14];
    ouch42::put_token(tok, ClientOrderId{key});
    std::uint32_t p4 = 0;
    REQUIRE(nasdaq::price_to_price4(px, p4));
    push(buf_, ouch42::host::executed(buf_, ++ts, tok, shares(qty), p4, liquidity, match));
  }

  std::array<std::byte, 128> buf_{};
  std::unordered_map<std::uint64_t, ouch42::EnterOrder> enters_;
  ouch42::ReplaceOrder last_replace_{};
};

class Ouch50Acceptor final : public Bridge {
 public:
  using Bridge::Bridge;
  Timestamp now{};

  void on_unsequenced(std::span<const std::byte> msg) noexcept {
    REQUIRE_FALSE(msg.empty());
    const auto type = static_cast<char>(msg[0]);
    std::span<const std::byte> app;
    REQUIRE(ouch50::split_message(msg, ouch50::inbound_base(type), false, app));
    switch (type) {
      case 'O': {
        const auto& m = ouch::view_as<ouch50::EnterOrder>(msg.data());
        const std::uint32_t ref_num = m.user_ref_num.get();
        REQUIRE(ref_num > last_urn_);  // strictly increasing per port
        last_urn_ = ref_num;
        enters_[ref_num] = m;
        Side side = Side::Buy;
        REQUIRE(ouch::side_from_code(m.side, side));
        Price price;
        REQUIRE(nasdaq::price4_to_price(m.price.get(), price));
        const std::uint32_t n = m.quantity.get();
        TimeInForce tif = TimeInForce::Gtc;
        std::span<const std::byte> v;
        if (m.time_in_force == ouch50::kTifIoc) {
          tif = TimeInForce::Ioc;
          if (ouch50::find_option(app, ouch50::OptionTag::MinQty, v)) {
            be32_t min{};
            std::memcpy(&min, v.data(), 4);
            if (min.get() == n) tif = TimeInForce::Fok;
          }
        }
        const bool post_only = ouch50::find_option(app, ouch50::OptionTag::PostOnly, v) &&
                               static_cast<char>(v[0]) == 'P';
        enter(ref_num,
              side,
              price,
              nasdaq::shares_to_qty(n),
              tif,
              post_only ? OrderType::PostOnly : OrderType::Limit,
              now);
        break;
      }
      case 'U': {
        const auto& m = ouch::view_as<ouch50::ReplaceOrder>(msg.data());
        REQUIRE(m.user_ref_num.get() > last_urn_);
        last_urn_ = m.user_ref_num.get();
        last_replace_ = m;
        Price price;
        REQUIRE(nasdaq::price4_to_price(m.price.get(), price));
        replace(m.orig_user_ref_num.get(),
                m.user_ref_num.get(),
                price,
                nasdaq::shares_to_qty(m.quantity.get()),
                now);
        break;
      }
      case 'X': {
        const auto& m = ouch::view_as<ouch50::CancelOrder>(msg.data());
        REQUIRE(m.quantity.get() == 0);
        cancel(m.user_ref_num.get(), now);
        break;
      }
      default:
        FAIL("unexpected inbound OUCH 5.0 message");
    }
  }

 private:
  static std::uint32_t urn(std::uint64_t key) { return static_cast<std::uint32_t>(key); }
  void send_accepted(std::uint64_t key, const SimOrder& o) override {
    push(buf_,
         ouch50::host::accepted(buf_, ++ts, enters_.at(urn(key)), shares(o.qty), o.order_id, 'L'));
  }
  void send_replaced(std::uint64_t key, std::uint64_t orig, const SimOrder& o) override {
    const ouch50::EnterOrder original = enters_.at(urn(orig));
    enters_[urn(key)] = original;
    push(buf_,
         ouch50::host::replaced(
             buf_, ++ts, last_replace_, original, shares(o.leaves()), o.order_id, 'L'));
  }
  void send_rejected(std::uint64_t key) override {
    const ouch50::EnterOrder& e = enters_.at(urn(key));
    push(buf_, ouch50::host::rejected(buf_, ++ts, urn(key), 0x000F, e.cl_ord_id));
  }
  void send_canceled(std::uint64_t key, Qty decrement, char reason) override {
    push(buf_, ouch50::host::canceled(buf_, ++ts, urn(key), shares(decrement), reason));
  }
  void send_executed(
      std::uint64_t key, Qty qty, Price px, char liquidity, std::uint64_t match) override {
    std::uint64_t p4 = 0;
    REQUIRE(nasdaq::price_to_price4(px, p4));
    push(buf_, ouch50::host::executed(buf_, ++ts, urn(key), shares(qty), p4, liquidity, match));
  }

  std::array<std::byte, 128> buf_{};
  std::unordered_map<std::uint32_t, ouch50::EnterOrder> enters_;
  ouch50::ReplaceOrder last_replace_{};
  std::uint32_t last_urn_ = 0;
};

// ---- per-version client state -------------------------------------------------------------

struct V42 {
  using Acceptor = Ouch42Acceptor;
  struct Client {
    ouch42::OuchEncoder enc;
    ouch42::OuchDecoder dec;
  };
  static std::unique_ptr<Client> make() {
    auto c = std::make_unique<Client>();
    REQUIRE(c->enc.add_symbol("FMSIM", kInst));
    REQUIRE(c->dec.add_symbol("FMSIM", kInst));
    return c;
  }
  static ClientOrderId client_id(const Client&, ClientOrderId engine_id) { return engine_id; }
  static const ouch::OrderEntry* open(const Client& c, ClientOrderId id) { return c.dec.order(id); }
};

struct V50 {
  using Acceptor = Ouch50Acceptor;
  struct Client {
    ouch50::UserRefMap ids;
    ouch50::OuchEncoder enc{ids};
    ouch50::OuchDecoder dec{&ids};
  };
  static std::unique_ptr<Client> make() {
    auto c = std::make_unique<Client>();
    REQUIRE(c->enc.add_symbol("FMSIM", kInst));
    REQUIRE(c->dec.add_symbol("FMSIM", kInst));
    return c;
  }
  static ClientOrderId client_id(const Client& c, ClientOrderId engine_id) {
    return c.ids.find(static_cast<std::uint32_t>(engine_id.value));
  }
  static const ouch::OrderEntry* open(const Client& c, ClientOrderId id) {
    const std::uint32_t urn = c.ids.find(id);
    return urn == 0 ? nullptr : c.dec.order(urn);
  }
};

struct Tally {
  std::uint64_t acks = 0;
  std::uint64_t fills = 0;
  std::uint64_t cancel_acks = 0;
  std::uint64_t expired = 0;
  std::uint64_t rejects = 0;
  Qty bought{};
  Qty sold{};
  Notional buy_notional{};
  Notional sell_notional{};
};

template <class V>
void run_session(std::uint64_t seed) {
  CAPTURE(seed);
  std::mt19937_64 rng(seed);
  auto uni = [&](std::int64_t lo, std::int64_t hi) {
    return std::uniform_int_distribution<std::int64_t>(lo, hi)(rng);
  };
  auto eng = std::make_unique<MatchingEngine>(1);
  typename V::Acceptor acceptor(*eng);
  eng->set_sink(&acceptor);

  BytePipe c2s;
  BytePipe s2c;
  soupbin::ServerConfig scfg;
  scfg.username = "FMUSER";
  scfg.password = "pw";
  scfg.session = "OUCHSIM";
  scfg.history_messages = 1U << 15;
  soupbin::ServerSession<BytePipe, typename V::Acceptor> server(s2c, acceptor, scfg);
  soupbin::ClientConfig ccfg;
  ccfg.username = "FMUSER";
  ccfg.password = "pw";
  soupbin::ClientSession<BytePipe> client(c2s, ccfg);
  auto cs = V::make();
  RecordingSink rec(1U << 22);
  Tally tally;
  std::set<std::uint64_t> sent_new;

  std::int64_t now_ns = 0;
  auto pump = [&] {
    soupbin::SoupBinFramer f;
    for (int rounds = 0; rounds < 8; ++rounds) {
      for (;;) {
        const FrameView v = f.next(c2s.pending());
        if (!v.complete()) break;
        REQUIRE(server.on_frame(v));
        c2s.consume(v.consumed);
      }
      for (const Bytes& m : acceptor.outbox)
        REQUIRE(server.send_sequenced(codecs::test::span_of(m)));
      acceptor.outbox.clear();
      for (;;) {
        const FrameView v = f.next(s2c.pending());
        if (!v.complete()) break;
        if (!client.on_frame(v)) {
          cs->dec.set_venue_seq(client.last_sequence());
          const venues::ParseStatus st =
              cs->dec.decode(FrameView{v.payload, v.payload.size(), 0}, now_ns, rec.sink);
          REQUIRE((st == venues::ParseStatus::Ok || st == venues::ParseStatus::Ignored));
        }
        s2c.consume(v.consumed);
      }
      if (c2s.pending().empty() && s2c.pending().empty() && acceptor.outbox.empty()) break;
    }
    for (const Bytes& raw : rec.drain()) {
      switch (RecordingSink::type_of(raw)) {
        case EventType::OrderAck:
          ++tally.acks;
          break;
        case EventType::OrderFill: {
          const auto m = RecordingSink::as<OrderFillMsg>(raw);
          ++tally.fills;
          const Notional n = mul(m.price, m.qty);
          if (m.side == Side::Buy) {
            tally.bought += m.qty;
            tally.buy_notional += n;
          } else {
            tally.sold += m.qty;
            tally.sell_notional += n;
          }
          break;
        }
        case EventType::OrderCancelAck:
          ++tally.cancel_acks;
          break;
        case EventType::OrderExpired:
          ++tally.expired;
          break;
        case EventType::OrderReject:
          ++tally.rejects;
          break;
        default:
          FAIL("unexpected event");
      }
    }
  };

  server.on_connect(now_ns);
  REQUIRE(client.login(now_ns));
  pump();
  REQUIRE(client.state() == SessionState::Up);

  std::array<std::byte, 128> buf{};
  std::vector<ClientOrderId> candidates;
  std::uint32_t next_seq = 1;
  std::uint32_t street_seq = 1;
  constexpr std::int64_t kMid = 10'000;
  std::uint64_t cancels_sent = 0;
  std::uint64_t replaces_sent = 0;

  for (int step = 0; step < 600; ++step) {
    CAPTURE(step);
    now_ns += 1'000'000;
    const Timestamp now{1'700'000'000'000'000'000 + now_ns};
    acceptor.now = now;
    const std::int64_t action = uni(0, 99);
    if (action < 35) {
      venues::OrderCommand c;
      c.kind = venues::OrderCommandKind::New;
      c.instrument = kInst;
      c.cl_ord_id = make_cl_ord_id(7, next_seq++);
      c.side = uni(0, 1) == 0 ? Side::Buy : Side::Sell;
      const std::int64_t off = uni(-3, 15);
      c.price = ticks(c.side == Side::Buy ? kMid - off : kMid + off);
      c.qty = Qty::from_int(uni(1, 30));
      const std::int64_t kind = uni(0, 99);
      if (kind < 8)
        c.tif = TimeInForce::Ioc;
      else if (kind < 14)
        c.tif = TimeInForce::Fok;
      else if (kind < 22)
        c.type = OrderType::PostOnly;
      const std::size_t n = cs->enc.encode(c, buf);
      REQUIRE(n != 0);
      REQUIRE(client.send(std::span<const std::byte>(buf.data(), n)));
      sent_new.insert(c.cl_ord_id.value);
      candidates.push_back(c.cl_ord_id);
    } else if (action < 55 && !candidates.empty()) {
      const std::size_t i =
          static_cast<std::size_t>(uni(0, static_cast<std::int64_t>(candidates.size()) - 1));
      const ClientOrderId id = candidates[i];
      if (V::open(*cs, id) != nullptr) {
        venues::OrderCommand c;
        c.kind = venues::OrderCommandKind::Cancel;
        c.cl_ord_id = id;
        const std::size_t n = cs->enc.encode(c, buf);
        REQUIRE(n != 0);
        REQUIRE(client.send(std::span<const std::byte>(buf.data(), n)));
        ++cancels_sent;
      }
      candidates[i] = candidates.back();
      candidates.pop_back();
    } else if (action < 70 && !candidates.empty()) {
      const std::size_t i =
          static_cast<std::size_t>(uni(0, static_cast<std::int64_t>(candidates.size()) - 1));
      const ClientOrderId id = candidates[i];
      const ouch::OrderEntry* e = V::open(*cs, id);
      // Replace Shares are "total shares liable inclusive of previous executions": the
      // acceptor hands them to the engine as the new order size, so replace unfilled orders.
      if (e != nullptr && e->cum.is_zero()) {
        venues::OrderCommand c;
        c.kind = venues::OrderCommandKind::Replace;
        c.orig_cl_ord_id = id;
        c.cl_ord_id = make_cl_ord_id(7, next_seq++);
        const std::int64_t off = uni(-3, 15);
        c.price = ticks(e->side == Side::Buy ? kMid - off : kMid + off);
        c.qty = Qty::from_int(uni(1, 30));
        const std::size_t n = cs->enc.encode(c, buf);
        REQUIRE(n != 0);
        REQUIRE(client.send(std::span<const std::byte>(buf.data(), n)));
        candidates[i] = c.cl_ord_id;
        ++replaces_sent;
      }
    } else {
      NewOrder o;
      o.account = kStreet;
      o.cl_ord_id = make_cl_ord_id(9, street_seq++);
      o.instrument = kInst;
      o.side = uni(0, 1) == 0 ? Side::Buy : Side::Sell;
      const std::int64_t off = uni(-6, 12);
      o.price = ticks(o.side == Side::Buy ? kMid - off : kMid + off);
      o.qty = Qty::from_int(uni(1, 40));
      if (uni(0, 3) == 0) o.tif = TimeInForce::Ioc;
      eng->submit(o, now);
    }
    pump();
    client.on_timer(now_ns);
    server.on_timer(now_ns);
    pump();
  }

  // Engine ledger == what the client learnt through OUCH.
  const AccountLedger& ledger = eng->ledger(kClient);
  CHECK(tally.fills == ledger.fills);
  CHECK(tally.bought == ledger.bought);
  CHECK(tally.sold == ledger.sold);
  CHECK(tally.buy_notional == ledger.buy_notional);
  CHECK(tally.sell_notional == ledger.sell_notional);
  CHECK(ledger.fills > 0);
  CHECK(tally.acks > 0);
  CHECK(tally.cancel_acks > 0);
  CHECK(cancels_sent > 0);
  CHECK(replaces_sent > 0);
  CHECK(tally.acks + tally.rejects >= sent_new.size());

  // Open orders: engine (client account) == client decoder table, ids and leaves.
  std::size_t engine_open = 0;
  eng->for_each_open_order([&](const SimOrder& o) {
    if (o.account != kClient) return;
    ++engine_open;
    const ClientOrderId id = V::client_id(*cs, o.cl_ord_id);
    REQUIRE(id.valid());
    const ouch::OrderEntry* e = V::open(*cs, id);
    REQUIRE(e != nullptr);
    CHECK(e->leaves == o.leaves());
    CHECK(e->side == o.side);
  });
  CHECK(cs->dec.open_orders() == engine_open);
  CHECK(cs->dec.stats().unknown_order == 0);
  CHECK(cs->dec.stats().foreign_id == 0);
  CHECK(cs->dec.stats().malformed == 0);
  CHECK(client.next_sequence() == server.messages() + 1);
  CHECK(client.state() == SessionState::Up);
  CHECK(server.state() == SessionState::Up);
  CHECK(client.stats().protocol_errors == 0);
}

}  // namespace

TEST_CASE("codecs.ouch: 4.2 client over SoupBinTCP against a simulated OUCH port") {
  for (std::uint64_t seed = 1; seed <= 12; ++seed) run_session<V42>(seed);
}

TEST_CASE("codecs.ouch: 5.0 client over SoupBinTCP against a simulated OUCH port") {
  for (std::uint64_t seed = 101; seed <= 112; ++seed) run_session<V50>(seed);
}
