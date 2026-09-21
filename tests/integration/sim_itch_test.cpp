// fastmm-sim-itch in-process (ADR-0015, section 6), in a user + network namespace whose lo carries
// multicast (tests/net/netns_test_util.hpp). The clients are the library pieces a venue uses:
// KernelDatagramSource -> moldudp::Receiver (A/B, re-requests) -> ITCH, GlimpseClient over
// SoupBinTCP, and an OUCH 5.0 SoupBinTCP ClientSession.
//
//   * both lines with drops: the stream recovers through re-requests and the ITCH book equals
//     the simulator's books;
//   * a client that joins mid-stream builds its book from GLIMPSE plus the stream from End of
//     Snapshot's sequence number, and it equals the simulator's books;
//   * OUCH orders: Accepted (with the ITCH order reference), Executed and E on the feed, Replaced,
//     Canceled and D on the feed, and the wire-to-wire histogram counts the order whose ClOrdID
//     names a sequence number.
#include "../net/netns_test_util.hpp"
#include "test_support.hpp"

#include "fastmm/codecs/itch/glimpse.hpp"
#include "fastmm/codecs/itch/itch_messages.hpp"
#include "fastmm/codecs/itch/nasdaq_fields.hpp"
#include "fastmm/codecs/moldudp/moldudp64.hpp"
#include "fastmm/codecs/ouch/ouch50.hpp"
#include "fastmm/codecs/soupbin/soupbin_session.hpp"
#include "fastmm/net/kernel_datagram_source.hpp"
#include "fastmm/net/reactor.hpp"
#include "fastmm/net/tcp_socket.hpp"
#include "fastmm/net/udp_socket.hpp"
#include "fastmm/sim/itch/sim_itch_server.hpp"

#include <doctest/doctest.h>

#include <poll.h>

#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <vector>

using namespace fastmm;
namespace ci = fastmm::codecs::itch;
namespace moldudp = fastmm::codecs::moldudp;
namespace ouch50 = fastmm::codecs::ouch50;
namespace soupbin = fastmm::codecs::soupbin;
namespace nasdaq = fastmm::codecs::nasdaq;
using fastmm::sim::itch::SimItchConfig;
using fastmm::sim::itch::SimItchServer;

namespace {

using Bytes = std::vector<std::byte>;

std::int64_t now_ns() {
  return net::Reactor::now_ns();
}

template <class M>
M view(std::span<const std::byte> b) {
  M m{};
  REQUIRE(b.size() >= sizeof(M));
  std::memcpy(&m, b.data(), sizeof m);
  return m;
}

SimItchConfig test_config() {
  SimItchConfig c = SimItchConfig::defaults();
  c.line_a = {"239.192.10.1", 31501, 0.0};
  c.line_b = {"239.192.10.2", 31502, 0.0};
  c.interface = "lo";
  c.rerequest_port = 0;
  c.glimpse_port = 0;
  c.ouch_port = 0;
  c.heartbeat_ms = 50;
  c.generator.limit_rate_per_s = 1500.0;
  c.generator.market_rate_per_s = 80.0;
  c.generator.cancel_rate_per_order_s = 2.0;
  c.history_messages = 1U << 18;
  c.history_bytes = 1U << 24;
  c.stamp_ring = 1U << 16;
  return c;
}

// Market-by-order book rebuilt from ITCH messages.
struct Book {
  struct Order {
    std::uint16_t locate = 0;
    char side = ' ';
    std::uint32_t price4 = 0;
    std::uint64_t shares = 0;
  };
  std::unordered_map<std::uint64_t, Order> orders;
  std::set<std::uint16_t> directory;
  std::uint64_t errors = 0;
  std::uint64_t executions = 0;
  std::uint64_t deletes = 0;

  void reduce(std::uint64_t ref, std::uint64_t n) {
    const auto it = orders.find(ref);
    if (it == orders.end() || it->second.shares < n) {
      ++errors;
      return;
    }
    it->second.shares -= n;
    if (it->second.shares == 0) orders.erase(it);
  }

  void apply(std::span<const std::byte> msg) {
    REQUIRE_FALSE(msg.empty());
    switch (static_cast<char>(msg[0])) {
      case 'R':
        directory.insert(view<ci::StockDirectory>(msg).hdr.stock_locate.get());
        break;
      case 'A':
      case 'F': {
        const auto m = view<ci::AddOrder>(msg);
        const Order o{
            m.hdr.stock_locate.get(), m.buy_sell_indicator, m.price.get(), m.shares.get()};
        if (!orders.emplace(m.order_reference_number.get(), o).second) ++errors;
        break;
      }
      case 'E': {
        const auto m = view<ci::OrderExecuted>(msg);
        ++executions;
        reduce(m.order_reference_number.get(), m.executed_shares.get());
        break;
      }
      case 'C': {
        const auto m = view<ci::OrderExecutedWithPrice>(msg);
        ++executions;
        reduce(m.order_reference_number.get(), m.executed_shares.get());
        break;
      }
      case 'X': {
        const auto m = view<ci::OrderCancel>(msg);
        reduce(m.order_reference_number.get(), m.cancelled_shares.get());
        break;
      }
      case 'D': {
        ++deletes;
        if (orders.erase(view<ci::OrderDelete>(msg).order_reference_number.get()) == 0) ++errors;
        break;
      }
      case 'U': {
        const auto m = view<ci::OrderReplace>(msg);
        const auto it = orders.find(m.original_order_reference_number.get());
        if (it == orders.end()) {
          ++errors;
          break;
        }
        Order o = it->second;
        orders.erase(it);
        o.shares = m.shares.get();
        o.price4 = m.price.get();
        orders.emplace(m.new_order_reference_number.get(), o);
        break;
      }
      default:
        break;
    }
  }
};

// Every resting order of the simulator is in the book, under its ITCH reference, and nothing else.
void check_book(const SimItchServer& sim, const Book& book) {
  CHECK(book.errors == 0);
  std::size_t total = 0;
  for (std::size_t sym = 0; sym < sim.symbol_count(); ++sym) {
    const auto locate = static_cast<std::uint16_t>(sym + 1);
    sim.engine(sym).for_each_open_order([&](const sim::SimOrder& o) {
      const std::uint64_t ref = sim.order_ref(sym, o.order_id);
      REQUIRE(ref != 0);
      const auto it = book.orders.find(ref);
      REQUIRE(it != book.orders.end());
      std::uint32_t p4 = 0;
      std::uint64_t shares = 0;
      REQUIRE(nasdaq::price_to_price4(o.price, p4));
      REQUIRE(nasdaq::qty_to_shares(o.leaves(), shares));
      CHECK(it->second.locate == locate);
      CHECK(it->second.side == (o.side == Side::Buy ? 'B' : 'S'));
      CHECK(it->second.price4 == p4);
      CHECK(it->second.shares == shares);
      ++total;
    });
  }
  CHECK(total == book.orders.size());
  CHECK(total > 0);
}

// The moldudp::ReceiverHandler side of FeedClient: messages in sequence order.
class FeedState {
 public:
  // Messages below `seq` are kept (not applied) until apply_from(seq).
  void hold_until_set() { apply_from_ = UINT64_MAX; }
  void apply_from(std::uint64_t seq) {
    apply_from_ = seq;
    for (const auto& [s, m] : held_) {
      if (s >= seq) book.apply(m);
    }
    held_.clear();
  }

  void on_message(std::uint64_t seq, std::span<const std::byte> msg) noexcept {
    last_seq = seq;
    if (first_seq == 0) first_seq = seq;
    if (seq >= apply_from_) {
      book.apply(msg);
    } else {
      held_.emplace_back(seq, Bytes(msg.begin(), msg.end()));
    }
  }
  void send_request(std::span<const std::byte> bytes) noexcept {
    static_cast<void>(req_.send(bytes));
  }
  void on_end_of_session() noexcept { eos = true; }

  Book book;
  std::uint64_t first_seq = 0;
  std::uint64_t last_seq = 0;
  bool eos = false;

 protected:
  net::UdpSocket req_;

 private:
  std::uint64_t apply_from_ = 0;
  std::vector<std::pair<std::uint64_t, Bytes>> held_;
};

// Lines A and B through KernelDatagramSource, re-requests over unicast UDP.
class FeedClient : public FeedState {
 public:
  FeedClient(const SimItchConfig& cfg, std::uint16_t rerequest_port) : rx_(*this, rx_config()) {
    net::KernelSourceConfig kc;
    kc.subscriptions = {
        {.interface = "lo", .group = cfg.line_a.group, .port = cfg.line_a.port, .source = ""},
        {.interface = "lo", .group = cfg.line_b.group, .port = cfg.line_b.port, .source = ""}};
    kc.timestamps = false;
    const net::KernelSourceOpen r = src_.open(kc);
    INFO("open failed at " << r.step << ": " << r.err);
    REQUIRE(r.ok());
    req_ = net::UdpSocket::open();
    REQUIRE(req_.valid());
    const auto to = net::SockAddr::from_ip("127.0.0.1", rerequest_port);
    REQUIRE(to.has_value());
    REQUIRE(req_.connect(*to) == 0);
  }

  void poll() {
    const std::int64_t now = now_ns();
    src_.poll([&](std::span<const std::byte> p, const net::RxMeta& m) noexcept {
      rx_.on_packet(m.line, p, now);
    });
    std::array<std::byte, 2048> buf{};
    for (;;) {
      const net::IoResult r = req_.recv_from(buf);
      if (!r.ok() || r.would_block()) break;
      rx_.on_packet(2, std::span<const std::byte>(buf.data(), r.bytes), now);
    }
    rx_.on_timer(now);
  }

  [[nodiscard]] const moldudp::ReceiverStats& stats() const noexcept { return rx_.stats(); }
  [[nodiscard]] std::uint64_t next_sequence() const noexcept { return rx_.next_sequence(); }

 private:
  static moldudp::ReceiverConfig rx_config() {
    moldudp::ReceiverConfig c;
    c.gap_timeout_ns = 1'000'000;
    c.request_timeout_ns = 20'000'000;
    c.max_request_attempts = 50;
    c.max_packet_bytes = 1500;
    c.reorder_packets = 4096;
    return c;
  }

  net::KernelDatagramSource src_;
  moldudp::Receiver<FeedState> rx_;
};

// A blocking-enough TCP client for SoupBinTCP sessions (the ByteWriter).
class TcpClient {
 public:
  explicit TcpClient(std::uint16_t port) {
    const auto a = net::SockAddr::from_ip("127.0.0.1", port);
    REQUIRE(a.has_value());
    const net::ConnectStatus st = sock_.connect(*a);
    REQUIRE(st != net::ConnectStatus::Error);
  }
  bool send(std::span<const std::byte> b) noexcept {
    while (!b.empty()) {
      const net::IoResult r = sock_.write(b);
      if (r.failed() || r.closed) return false;
      b = b.subspan(r.bytes);
      if (r.would_block()) {
        pollfd p{sock_.fd(), POLLOUT, 0};
        static_cast<void>(::poll(&p, 1, 10));
      }
    }
    return true;
  }
  [[nodiscard]] bool connected() {
    pollfd p{sock_.fd(), POLLOUT, 0};
    return ::poll(&p, 1, 0) == 1 && (p.revents & POLLOUT) != 0 && sock_.finish_connect() == 0;
  }
  // Appends whatever arrived; false once the peer closed.
  bool read() {
    std::array<std::byte, 65536> buf{};
    for (;;) {
      const net::IoResult r = sock_.read(buf);
      if (r.bytes > 0) in.insert(in.end(), buf.begin(), buf.begin() + static_cast<long>(r.bytes));
      if (r.closed || r.failed()) return false;
      if (r.would_block() || r.bytes == 0) return true;
    }
  }
  // SoupBinTCP packets received so far, consumed.
  template <class F>
  void frames(F&& f) {
    const soupbin::SoupBinFramer framer;
    std::size_t used = 0;
    for (codecs::FrameView v = framer.next(in); v.complete();
         v = framer.next(std::span<const std::byte>(in).subspan(used))) {
      used += v.consumed;
      f(v);
    }
    in.erase(in.begin(), in.begin() + static_cast<long>(used));
  }

  Bytes in;

 private:
  net::TcpSocket sock_;
};

// Polls the simulator and `also` until `done` or the timeout.
bool pump(SimItchServer& sim,
          int timeout_ms,
          const std::function<bool()>& done,
          const std::function<void()>& also = {}) {
  const std::int64_t end = now_ns() + std::int64_t{timeout_ms} * 1'000'000;
  while (now_ns() < end) {
    sim.poll(0);
    if (also) also();
    if (done()) return true;
  }
  return false;
}

bool caught_up(const SimItchServer& sim, const FeedClient& c) {
  return sim.idle() && c.next_sequence() == sim.published() + 1;
}

struct SnapshotBook {
  Book book;
  std::uint64_t end = 0;
  std::uint64_t messages = 0;
  void on_snapshot_message(std::span<const std::byte> m) noexcept {
    ++messages;
    book.apply(m);
  }
  void on_snapshot_end(std::uint64_t seq) noexcept { end = seq; }
};

}  // namespace

TEST_CASE("sim_itch: lines A and B with drops recover through re-requests") {
  if (!net::test::in_multicast_netns()) return;
  SimItchConfig cfg = test_config();
  cfg.line_a.drop_rate = 0.05;
  cfg.line_b.drop_rate = 0.05;
  cfg.drop_seed = 11;
  SimItchServer sim(cfg);
  REQUIRE_MESSAGE(sim.open(), sim.last_error());
  FeedClient feed(cfg, sim.rerequest_port());

  pump(sim, 800, [] { return false; }, [&] { feed.poll(); });
  sim.set_generator_enabled(false);
  REQUIRE(pump(sim, 5000, [&] { return caught_up(sim, feed); }, [&] { feed.poll(); }));

  const moldudp::ReceiverStats& st = feed.stats();
  const auto ss = sim.stats();
  CHECK(feed.first_seq == 1);
  CHECK(st.lines[0].packets > 0);
  CHECK(st.lines[1].packets > 0);
  CHECK(st.lines[2].packets > 0);  // retransmissions
  CHECK(st.duplicate_packets > 0);
  CHECK(st.requests > 0);
  CHECK(st.unrecoverable_gaps == 0);
  CHECK(ss.datagrams_dropped[0] > 0);
  CHECK(ss.datagrams_dropped[1] > 0);
  CHECK(ss.requests_answered > 0);
  CHECK(ss.send_errors == 0);
  CHECK(ss.publish_failures == 0);
  CHECK(ss.messages > 500);
  CHECK(feed.book.directory == std::set<std::uint16_t>{1, 2});
  CHECK(feed.book.executions > 0);
  CHECK(feed.book.deletes > 0);
  check_book(sim, feed.book);

  sim.end_session();
  REQUIRE(pump(sim, 2000, [&] { return feed.eos; }, [&] { feed.poll(); }));
}

TEST_CASE("sim_itch: GLIMPSE snapshot plus the stream from End of Snapshot rebuilds the books") {
  if (!net::test::in_multicast_netns()) return;
  SimItchConfig cfg = test_config();
  cfg.line_a.drop_rate = 0.02;
  cfg.drop_seed = 5;
  SimItchServer sim(cfg);
  REQUIRE_MESSAGE(sim.open(), sim.last_error());
  pump(sim, 300, [] { return false; });

  // Join mid-stream and hold the messages until the snapshot says where to start.
  FeedClient feed(cfg, sim.rerequest_port());
  feed.hold_until_set();
  REQUIRE(pump(sim, 2000, [&] { return feed.first_seq != 0; }, [&] { feed.poll(); }));
  CHECK(feed.first_seq > 1);

  TcpClient tcp(sim.glimpse_port());
  REQUIRE(pump(sim, 2000, [&] { return tcp.connected(); }, [&] { feed.poll(); }));
  SnapshotBook snap;
  ci::glimpse::GlimpseConfig gcfg;
  gcfg.session.username = std::string_view(cfg.glimpse_username);
  gcfg.session.password = std::string_view(cfg.glimpse_password);
  ci::glimpse::GlimpseClient<TcpClient, SnapshotBook> glimpse(tcp, snap, gcfg);
  REQUIRE(glimpse.login(now_ns()));
  REQUIRE(pump(
      sim,
      3000,
      [&] { return glimpse.complete(); },
      [&] {
        feed.poll();
        tcp.read();
        const std::size_t used = glimpse.on_bytes(tcp.in);
        tcp.in.erase(tcp.in.begin(), tcp.in.begin() + static_cast<long>(used));
      }));
  CHECK(sim.stats().snapshots == 1);
  CHECK(snap.book.directory == std::set<std::uint16_t>{1, 2});
  CHECK(snap.book.orders.size() > 10);
  CHECK(snap.book.errors == 0);
  REQUIRE(snap.end >= feed.first_seq);

  feed.book = snap.book;
  feed.apply_from(snap.end);
  pump(sim, 300, [] { return false; }, [&] { feed.poll(); });
  sim.set_generator_enabled(false);
  REQUIRE(pump(sim, 5000, [&] { return caught_up(sim, feed); }, [&] { feed.poll(); }));
  CHECK(feed.stats().unrecoverable_gaps == 0);
  check_book(sim, feed.book);
}

TEST_CASE("sim_itch: OUCH orders trade against the book and are timed wire to wire") {
  if (!net::test::in_multicast_netns()) return;
  SimItchConfig cfg = test_config();
  SimItchServer sim(cfg);
  REQUIRE_MESSAGE(sim.open(), sim.last_error());
  FeedClient feed(cfg, sim.rerequest_port());
  REQUIRE(pump(sim, 2000, [&] { return feed.last_seq > 100; }, [&] { feed.poll(); }));

  TcpClient tcp(sim.ouch_port());
  REQUIRE(pump(sim, 2000, [&] { return tcp.connected(); }));
  soupbin::ClientConfig ccfg;
  ccfg.username = std::string_view(cfg.ouch_username);
  ccfg.password = std::string_view(cfg.ouch_password);
  soupbin::ClientSession<TcpClient> ouch(tcp, ccfg);
  std::vector<Bytes> inbox;  // outbound OUCH messages from the simulator
  auto read_ouch = [&] {
    feed.poll();
    tcp.read();
    tcp.frames([&](const codecs::FrameView& f) {
      if (!ouch.on_frame(f)) inbox.emplace_back(f.payload.begin(), f.payload.end());
    });
  };
  REQUIRE(ouch.login(now_ns()));
  REQUIRE(pump(sim, 2000, [&] { return ouch.state() == codecs::SessionState::Up; }, read_ouch));

  auto enter = [&](std::uint32_t urn,
                   char side,
                   const char* price,
                   std::uint32_t qty,
                   char tif,
                   std::uint64_t token) {
    ouch50::EnterOrder m{};
    m.type = 'O';
    m.user_ref_num.set(urn);
    m.side = side;
    m.quantity.set(qty);
    nasdaq::put_alpha(m.symbol, sizeof m.symbol, "FMAA");
    std::uint64_t p4 = 0;
    REQUIRE(nasdaq::price_to_price4(Price::from_decimal(price).value(), p4));
    m.price.set(p4);
    m.time_in_force = tif;
    m.display = 'Y';
    m.capacity = 'P';
    m.intermarket_sweep_eligibility = 'N';
    m.cross_type = 'N';
    if (token != 0) {
      REQUIRE(ouch50::put_seq_token(m.cl_ord_id, token));
    } else {
      nasdaq::put_alpha(m.cl_ord_id, sizeof m.cl_ord_id, "plain");
    }
    m.appendage_length.set(0);
    REQUIRE(ouch.send(std::as_bytes(std::span<const ouch50::EnterOrder>(&m, 1))));
  };
  auto next_of = [&](char type) -> std::function<bool()> {
    return [&inbox, type] {
      for (const Bytes& b : inbox) {
        if (static_cast<char>(b[0]) == type) return true;
      }
      return false;
    };
  };
  auto take = [&](char type) {
    for (auto it = inbox.begin(); it != inbox.end(); ++it) {
      if (static_cast<char>((*it)[0]) == type) {
        Bytes b = *it;
        inbox.erase(it);
        return b;
      }
    }
    FAIL("no OUCH message of type " << type);
    return Bytes{};
  };

  // 1. A resting buy far below the market, triggered by the last message the feed delivered.
  const std::uint64_t trigger = feed.last_seq;
  enter(1, 'B', "50.00", 100, ouch50::kTifDay, trigger);
  REQUIRE(pump(sim, 2000, next_of('A'), read_ouch));
  const auto acc = view<ouch50::OrderAccepted>(take('A'));
  CHECK(acc.user_ref_num.get() == 1);
  CHECK(acc.quantity.get() == 100);
  CHECK(ouch50::parse_seq_token(acc.cl_ord_id) == trigger);
  const std::uint64_t ref = acc.order_reference_number.get();
  REQUIRE(ref != 0);
  REQUIRE(pump(sim, 2000, [&] { return feed.book.orders.contains(ref); }, read_ouch));
  CHECK(feed.book.orders.at(ref).price4 == 500'000);
  CHECK(sim.wire_to_wire().count() == 1);
  CHECK(sim.stats().w2w_tokens == 1);
  CHECK(sim.stats().w2w_misses == 0);
  CHECK(sim.wire_to_wire().max() < 1'000'000'000ULL);

  // 2. An aggressive IOC buy: Executed (removed liquidity), E on the feed with the same match.
  const std::uint64_t execs_before = feed.book.executions;
  enter(2, 'B', "150.00", 30, ouch50::kTifIoc, 0);
  REQUIRE(pump(sim, 2000, next_of('E'), read_ouch));
  const auto ex = view<ouch50::OrderExecuted>(take('E'));
  CHECK(ex.user_ref_num.get() == 2);
  CHECK(ex.liquidity_flag == 'R');
  CHECK(ex.quantity.get() > 0);
  REQUIRE(pump(sim, 2000, [&] { return feed.book.executions > execs_before; }, read_ouch));

  // 3. Replace the resting order to a new price: Replaced, then cancel it: Canceled and D.
  ouch50::ReplaceOrder rep{};
  rep.type = 'U';
  rep.orig_user_ref_num.set(1);
  rep.user_ref_num.set(3);
  rep.quantity.set(80);
  rep.price.set(510'000);
  rep.time_in_force = ouch50::kTifDay;
  rep.display = 'Y';
  rep.intermarket_sweep_eligibility = 'N';
  nasdaq::put_alpha(rep.cl_ord_id, sizeof rep.cl_ord_id, "repl");
  rep.appendage_length.set(0);
  REQUIRE(ouch.send(std::as_bytes(std::span<const ouch50::ReplaceOrder>(&rep, 1))));
  REQUIRE(pump(sim, 2000, next_of('U'), read_ouch));
  const auto rd = view<ouch50::OrderReplaced>(take('U'));
  CHECK(rd.orig_user_ref_num.get() == 1);
  CHECK(rd.user_ref_num.get() == 3);
  CHECK(rd.quantity.get() == 80);
  const std::uint64_t ref3 = rd.order_reference_number.get();
  REQUIRE(pump(sim, 2000, [&] { return feed.book.orders.contains(ref3); }, read_ouch));
  CHECK_FALSE(feed.book.orders.contains(ref));

  ouch50::CancelOrder cx{};
  cx.type = 'X';
  cx.user_ref_num.set(3);
  cx.quantity.set(0);
  cx.appendage_length.set(0);
  REQUIRE(ouch.send(std::as_bytes(std::span<const ouch50::CancelOrder>(&cx, 1))));
  REQUIRE(pump(sim, 2000, next_of('C'), read_ouch));
  const auto cd = view<ouch50::OrderCanceled>(take('C'));
  CHECK(cd.user_ref_num.get() == 3);
  CHECK(cd.quantity.get() == 80);
  REQUIRE(pump(sim, 2000, [&] { return !feed.book.orders.contains(ref3); }, read_ouch));

  // 4. An unknown symbol is rejected.
  ouch50::EnterOrder bad{};
  bad.type = 'O';
  bad.user_ref_num.set(4);
  bad.side = 'S';
  bad.quantity.set(1);
  nasdaq::put_alpha(bad.symbol, sizeof bad.symbol, "NOPE");
  bad.price.set(1'000'000);
  bad.time_in_force = ouch50::kTifDay;
  nasdaq::put_alpha(bad.cl_ord_id, sizeof bad.cl_ord_id, "bad");
  REQUIRE(ouch.send(std::as_bytes(std::span<const ouch50::EnterOrder>(&bad, 1))));
  REQUIRE(pump(sim, 2000, next_of('J'), read_ouch));
  CHECK(view<ouch50::Rejected>(take('J')).reason.get() == 0x0017);

  sim.set_generator_enabled(false);
  REQUIRE(pump(sim, 5000, [&] { return caught_up(sim, feed); }, read_ouch));
  check_book(sim, feed.book);
  CHECK(sim.wire_to_wire().count() == 1);
  const std::string json = sim.summary_json();
  CHECK(json.find("\"count\": 1,") != std::string::npos);
  CHECK(json.find("\"tokens\": 1,") != std::string::npos);
}

TEST_CASE("sim_itch: configs/sim-itch.toml loads and bad values are refused") {
  const std::filesystem::path path =
      (fastmm::test::fixtures_dir() / ".." / ".." / "configs" / "sim-itch.toml").lexically_normal();
  const SimItchConfig c = SimItchConfig::load(path.string());
  REQUIRE(c.symbols.size() == 2);
  CHECK(c.symbols[0].symbol == "FMAA");
  CHECK(c.symbols[1].locate == 2);
  CHECK(c.symbols[1].start_mid == Price::from_int(50));
  CHECK(c.line_a.group == "239.192.0.1");
  CHECK(c.line_b.port == 31002);
  CHECK(c.ouch_port == 31020);

  SimItchConfig bad = SimItchConfig::defaults();
  bad.line_a.drop_rate = 1.0;
  CHECK_THROWS_AS(bad.validate(), std::invalid_argument);
  bad = SimItchConfig::defaults();
  bad.symbols[1].locate = 1;  // FMAA already has locate 1
  CHECK_THROWS_AS(bad.validate(), std::invalid_argument);
  bad = SimItchConfig::defaults();
  bad.symbols[0].lot = Qty::from_decimal("0.5").value();
  CHECK_THROWS_AS(bad.validate(), std::invalid_argument);
}
