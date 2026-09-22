// UserTcp against a scripted peer: handshake, send and retransmission, receive ordering, FIN and
// RST handling, windows, sequence wrap-around.
#include "user_tcp_test_util.hpp"

#include <cerrno>
#include <string>

using namespace fastmm;
using namespace fastmm::net;
using namespace fastmm::net::test;

namespace {

constexpr std::int64_t kMs = 1'000'000;

struct Fixture {
  Peer peer;
  CaptureTx tx;
  RecordingHandler h;
  UserTcpConfig cfg;
  std::unique_ptr<UserTcp> tcp;
  std::int64_t now = 1'000 * kMs;
  std::uint32_t iss = 1000;
  std::uint32_t peer_seq = 50'000;  // the peer's next sequence number

  explicit Fixture(std::uint32_t isn = 1000) : iss(isn) {
    cfg.local_mac = peer.our_mac;
    cfg.local_ip = peer.our_ip;
    cfg.remote_ip = peer.ip;
    cfg.remote_port = peer.port;
    cfg.local_port = 40000;
    cfg.rto_initial_ns = 100 * kMs;
    cfg.rto_min_ns = 10 * kMs;
    cfg.max_retransmits = 4;
    cfg.rx_buffer = 8192;
    tcp = std::make_unique<UserTcp>(tx, h, cfg);
  }

  void feed(const std::vector<std::byte>& f) { tcp->on_frame(f, now); }
  std::vector<std::byte> seg(std::uint8_t flags,
                             std::string_view data = {},
                             std::uint16_t wnd = 65535) {
    auto f = peer.tcp(40000, peer_seq, tcp_ack(), flags, wnd, data);
    return f;
  }
  std::uint32_t acked = 0;  // what the peer acknowledges
  [[nodiscard]] std::uint32_t tcp_ack() const { return acked; }

  // ARP, SYN, SYN-ACK, ACK.
  void establish(std::uint16_t mss = 1460, std::uint16_t wnd = 65535) {
    REQUIRE(tcp->connect(now, iss));
    auto out = tx.take();
    REQUIRE(out.size() == 1);
    CHECK(out[0].arp);
    CHECK(out[0].arp_op == 1);
    CHECK(out[0].arp_tpa == peer.ip);
    CHECK(tcp->state() == TcpState::Arp);
    feed(peer.arp(2, peer.our_ip));
    out = tx.take();
    REQUIRE(out.size() == 1);
    CHECK(out[0].has(kSyn));
    CHECK(out[0].seq == iss);
    CHECK(out[0].mss == 1460);
    CHECK(out[0].csum_ok);
    CHECK(std::memcmp(out[0].dst_mac, peer.mac.data(), 6) == 0);
    acked = iss + 1;
    feed(peer.tcp(40000, peer_seq, acked, kSyn | kAck, wnd, {}, mss));
    ++peer_seq;
    out = tx.take();
    REQUIRE(out.size() == 1);
    CHECK(out[0].flags == kAck);
    CHECK(out[0].ack == peer_seq);
    CHECK(h.connected);
    CHECK(tcp->state() == TcpState::Established);
  }
};

}  // namespace

TEST_CASE("UserTcp: ARP, handshake, MSS and ARP replies") {
  Fixture fx;
  fx.establish(1000);
  CHECK(fx.tcp->mss() == 1000);
  // Someone asks for our address: we answer.
  fx.feed(fx.peer.arp(1, fx.peer.our_ip));
  auto out = fx.tx.take();
  REQUIRE(out.size() == 1);
  CHECK(out[0].arp);
  CHECK(out[0].arp_op == 2);
  // ARP for another address: silence.
  fx.feed(fx.peer.arp(1, ip4("10.9.0.77")));
  CHECK(fx.tx.take().empty());
}

TEST_CASE("UserTcp: send is segmented at the MSS and acknowledged") {
  Fixture fx;
  fx.establish(100);
  const std::string payload(250, 'x');
  REQUIRE(fx.tcp->send(bytes_of(payload)));
  auto out = fx.tx.take();
  REQUIRE(out.size() == 3);
  CHECK(out[0].seq == fx.iss + 1);
  CHECK(out[0].data.size() == 100);
  CHECK(out[1].seq == fx.iss + 101);
  CHECK(out[2].data.size() == 50);
  CHECK(out[2].has(kPsh));
  for (const auto& f : out) CHECK(f.csum_ok);
  CHECK(fx.tcp->unacked() == 250);
  fx.acked = fx.iss + 251;
  fx.feed(fx.seg(kAck));
  CHECK(fx.tcp->unacked() == 0);
  CHECK(fx.tx.take().empty());  // a pure ACK is not answered
}

TEST_CASE("UserTcp: cork holds segments until uncork") {
  Fixture fx;
  fx.establish();
  fx.tcp->cork();
  REQUIRE(fx.tcp->send(bytes_of("abc")));
  REQUIRE(fx.tcp->send(bytes_of("def")));
  CHECK(fx.tx.take().empty());
  REQUIRE(fx.tcp->uncork());
  auto out = fx.tx.take();
  REQUIRE(out.size() == 1);
  CHECK(out[0].data == "abcdef");
}

TEST_CASE("UserTcp: RTO retransmits from snd_una with backoff, then gives up") {
  Fixture fx;
  fx.establish();
  REQUIRE(fx.tcp->send(bytes_of("hello")));
  fx.tx.take();
  // Nothing acknowledged: the segment is resent after the RTO, doubled each time.
  std::int64_t rto = fx.tcp->stats().rto_ns;
  REQUIRE(rto > 0);
  for (int i = 0; i < 4; ++i) {
    fx.now = fx.tcp->next_timer_ns();
    fx.tcp->on_timer(fx.now);
    auto out = fx.tx.take();
    REQUIRE(out.size() == 1);
    CHECK(out[0].seq == fx.iss + 1);
    CHECK(out[0].data == "hello");
    CHECK(fx.tcp->stats().rto_ns >= std::min<std::int64_t>(2 * rto, fx.cfg.rto_max_ns));
    rto = fx.tcp->stats().rto_ns;
  }
  CHECK(fx.tcp->stats().retransmits == 4);
  fx.now = fx.tcp->next_timer_ns();
  fx.tcp->on_timer(fx.now);
  auto out = fx.tx.take();
  REQUIRE(out.size() == 1);
  CHECK(out[0].has(kRst));
  CHECK(fx.h.closed == ETIMEDOUT);
  CHECK(fx.tcp->state() == TcpState::Closed);
}

TEST_CASE("UserTcp: an ACK after the RTO resumes the go-back-N send") {
  Fixture fx;
  fx.establish(100);
  const std::string payload(300, 'y');
  REQUIRE(fx.tcp->send(bytes_of(payload)));
  CHECK(fx.tx.take().size() == 3);
  fx.now = fx.tcp->next_timer_ns();
  fx.tcp->on_timer(fx.now);
  auto out = fx.tx.take();
  REQUIRE(out.size() == 1);  // cwnd = 1 MSS
  CHECK(out[0].seq == fx.iss + 1);
  // The peer had the first two segments; the ACK covers them.
  fx.acked = fx.iss + 201;
  fx.feed(fx.seg(kAck));
  out = fx.tx.take();
  REQUIRE(out.size() == 1);
  CHECK(out[0].seq == fx.iss + 201);
  CHECK(out[0].data.size() == 100);
  fx.acked = fx.iss + 301;
  fx.feed(fx.seg(kAck));
  CHECK(fx.tcp->unacked() == 0);
  CHECK(fx.tcp->next_timer_ns() == INT64_MAX);
}

TEST_CASE("UserTcp: three duplicate ACKs trigger a fast retransmit") {
  Fixture fx;
  fx.establish(100);
  const std::string payload(500, 'z');
  REQUIRE(fx.tcp->send(bytes_of(payload)));
  CHECK(fx.tx.take().size() == 5);
  fx.acked = fx.iss + 101;
  fx.feed(fx.seg(kAck));
  for (int i = 0; i < 3; ++i) fx.feed(fx.seg(kAck));
  auto out = fx.tx.take();
  REQUIRE(out.size() == 1);
  CHECK(out[0].seq == fx.iss + 101);
  CHECK(out[0].data.size() == 100);
  CHECK(fx.tcp->stats().fast_retransmits == 1);
  CHECK(fx.tcp->stats().dup_acks == 3);
}

TEST_CASE("UserTcp: out-of-order data waits for the hole and is ACKed as a duplicate") {
  Fixture fx;
  fx.establish();
  const std::uint32_t base = fx.peer_seq;
  const std::string text = "the quick brown fox jumps";  // 25 bytes
  const auto piece = [&](std::size_t from, std::size_t n) {
    fx.peer_seq = base + static_cast<std::uint32_t>(from);
    fx.feed(fx.seg(kAck | kPsh, text.substr(from, n)));
    return fx.tx.take();
  };
  auto out = piece(15, 5);  // "fox j"
  REQUIRE(out.size() == 1);
  CHECK(out[0].ack == base);
  CHECK(fx.h.data.empty());
  out = piece(4, 6);  // "quick "
  CHECK(out[0].ack == base);
  out = piece(8, 9);  // overlaps both ranges: "ck brown " + "fo"
  CHECK(out[0].ack == base);
  CHECK(fx.tcp->stats().out_of_order == 3);
  out = piece(0, 4);  // "the " fills the hole up to 20
  REQUIRE(out.size() == 1);
  CHECK(out[0].ack == base + 20);
  CHECK(fx.h.data == text.substr(0, 20));
  out = piece(22, 3);  // "mps"
  CHECK(out[0].ack == base + 20);
  out = piece(0, 22);  // retransmission overlapping everything delivered
  CHECK(out[0].ack == base + 25);
  CHECK(fx.h.data == text);
  out = piece(0, 5);  // full duplicate: acknowledged only
  REQUIRE(out.size() == 1);
  CHECK(out[0].ack == base + 25);
  CHECK(fx.h.data == text);
}

TEST_CASE("UserTcp: out-of-order bytes survive the handler consuming part of the buffer") {
  Fixture fx;
  fx.establish();
  const std::uint32_t base = fx.peer_seq;
  fx.h.consume_limit = 2;
  fx.peer_seq = base + 6;
  fx.feed(fx.seg(kAck | kPsh, "ghij"));
  fx.peer_seq = base;
  fx.feed(fx.seg(kAck | kPsh, "abc"));  // handler takes "ab", leaves "c"
  CHECK(fx.h.data == "ab");
  fx.h.consume_limit = SIZE_MAX;
  fx.peer_seq = base + 3;
  fx.feed(fx.seg(kAck | kPsh, "def"));
  CHECK(fx.h.data == "abcdefghij");
}

TEST_CASE("UserTcp: bytes the handler leaves are offered again with the next segment") {
  Fixture fx;
  fx.establish();
  fx.h.consume_limit = 3;
  fx.feed(fx.seg(kAck | kPsh, "abcdef"));
  fx.peer_seq += 6;
  CHECK(fx.h.data == "abc");
  fx.h.consume_limit = SIZE_MAX;
  fx.feed(fx.seg(kAck | kPsh, "gh"));
  CHECK(fx.h.data == "abcdefgh");
}

TEST_CASE("UserTcp: peer FIN reports EOF, answers FIN and closes on its ACK") {
  Fixture fx;
  fx.establish();
  fx.feed(fx.seg(kAck | kPsh | kFin, "bye"));
  fx.peer_seq += 4;
  auto out = fx.tx.take();
  REQUIRE(!out.empty());
  CHECK(out.back().has(kFin));
  CHECK(out.back().ack == fx.peer_seq);
  CHECK(fx.h.data == "bye");
  CHECK(fx.h.closed == 0);
  CHECK(fx.tcp->state() == TcpState::LastAck);
  CHECK_FALSE(fx.tcp->send(bytes_of("late")));
  // Our FIN lost: retransmitted by the RTO.
  fx.now = fx.tcp->next_timer_ns();
  fx.tcp->on_timer(fx.now);
  out = fx.tx.take();
  REQUIRE(out.size() == 1);
  CHECK(out[0].has(kFin));
  fx.acked = fx.iss + 2;
  fx.feed(fx.seg(kAck));
  CHECK(fx.tcp->state() == TcpState::Closed);
}

TEST_CASE("UserTcp: our close goes FIN_WAIT_1, FIN_WAIT_2, TIME_WAIT, CLOSED") {
  Fixture fx;
  fx.establish();
  REQUIRE(fx.tcp->send(bytes_of("last")));
  fx.tx.take();
  fx.tcp->close();
  auto out = fx.tx.take();
  REQUIRE(out.size() == 1);
  CHECK(out[0].has(kFin));
  CHECK(out[0].seq == fx.iss + 5);
  CHECK(fx.tcp->state() == TcpState::FinWait1);
  fx.acked = fx.iss + 6;
  fx.feed(fx.seg(kAck));
  CHECK(fx.tcp->state() == TcpState::FinWait2);
  fx.feed(fx.seg(kAck | kFin));
  fx.peer_seq += 1;
  out = fx.tx.take();
  REQUIRE(out.size() == 1);
  CHECK(out[0].ack == fx.peer_seq);
  CHECK(fx.tcp->state() == TcpState::TimeWait);
  // A retransmitted FIN is acknowledged again.
  fx.peer_seq -= 1;
  fx.feed(fx.seg(kAck | kFin));
  fx.peer_seq += 1;
  out = fx.tx.take();
  REQUIRE(out.size() == 1);
  CHECK(out[0].ack == fx.peer_seq);
  fx.now += fx.cfg.time_wait_ns;
  fx.tcp->on_timer(fx.now);
  CHECK(fx.tcp->state() == TcpState::Closed);
  CHECK(fx.h.closed == -1);  // close() reports nothing
}

TEST_CASE("UserTcp: simultaneous close through CLOSING") {
  Fixture fx;
  fx.establish();
  fx.tcp->close();
  fx.tx.take();
  fx.feed(fx.seg(kAck | kFin));  // FIN that does not acknowledge ours
  fx.peer_seq += 1;
  CHECK(fx.tcp->state() == TcpState::Closing);
  fx.acked = fx.iss + 2;
  fx.feed(fx.seg(kAck));
  CHECK(fx.tcp->state() == TcpState::TimeWait);
}

TEST_CASE("UserTcp: RST at rcv_nxt resets, elsewhere in the window draws a challenge ACK") {
  Fixture fx;
  fx.establish();
  fx.peer_seq += 10;
  fx.feed(fx.seg(kRst));
  auto out = fx.tx.take();
  REQUIRE(out.size() == 1);
  CHECK(out[0].flags == kAck);
  CHECK(fx.tcp->stats().challenge_acks == 1);
  CHECK(fx.h.closed == -1);
  fx.peer_seq -= 10;
  fx.feed(fx.seg(kRst));
  CHECK(fx.h.closed == ECONNRESET);
  CHECK(fx.tcp->state() == TcpState::Closed);
}

TEST_CASE("UserTcp: a SYN in ESTABLISHED draws a challenge ACK") {
  Fixture fx;
  fx.establish();
  fx.feed(fx.seg(kSyn));
  auto out = fx.tx.take();
  REQUIRE(out.size() == 1);
  CHECK(out[0].flags == kAck);
  CHECK(fx.tcp->state() == TcpState::Established);
}

TEST_CASE("UserTcp: refused connect and a SYN-ACK with a bad ACK") {
  Fixture fx;
  REQUIRE(fx.tcp->connect(fx.now, fx.iss));
  fx.feed(fx.peer.arp(2, fx.peer.our_ip));
  fx.tx.take();
  fx.feed(fx.peer.tcp(40000, 7, fx.iss + 5, kSyn | kAck, 65535));
  auto out = fx.tx.take();
  REQUIRE(out.size() == 1);
  CHECK(out[0].has(kRst));
  CHECK(out[0].seq == fx.iss + 5);
  CHECK(fx.tcp->state() == TcpState::SynSent);
  fx.feed(fx.peer.tcp(40000, 0, fx.iss + 1, kRst | kAck, 0));
  CHECK(fx.h.closed == ECONNREFUSED);
}

TEST_CASE("UserTcp: SYN retries then ETIMEDOUT") {
  Fixture fx;
  fx.cfg.syn_retries = 2;
  fx.tcp = std::make_unique<UserTcp>(fx.tx, fx.h, fx.cfg);
  REQUIRE(fx.tcp->connect(fx.now, fx.iss));
  fx.feed(fx.peer.arp(2, fx.peer.our_ip));
  fx.tx.take();
  for (int i = 0; i < 2; ++i) {
    fx.now = fx.tcp->next_timer_ns();
    fx.tcp->on_timer(fx.now);
    auto out = fx.tx.take();
    REQUIRE(out.size() == 1);
    CHECK(out[0].has(kSyn));
  }
  fx.now = fx.tcp->next_timer_ns();
  fx.tcp->on_timer(fx.now);
  CHECK(fx.h.closed == ETIMEDOUT);
}

TEST_CASE("UserTcp: ARP gives up after arp_retries") {
  Fixture fx;
  fx.cfg.arp_retries = 3;
  fx.tcp = std::make_unique<UserTcp>(fx.tx, fx.h, fx.cfg);
  REQUIRE(fx.tcp->connect(fx.now, fx.iss));
  for (int i = 0; i < 3; ++i) {
    fx.now = fx.tcp->next_timer_ns();
    fx.tcp->on_timer(fx.now);
  }
  CHECK(fx.h.closed == ETIMEDOUT);
  CHECK(fx.tcp->stats().arp_requests == 3);
}

TEST_CASE("UserTcp: segments of an unknown connection get a RST") {
  Fixture fx;
  fx.establish();
  Peer other = fx.peer;
  other.port = 5001;
  fx.tcp->on_frame(other.tcp(40000, 1, 99, kAck, 100, "x"), fx.now);
  auto out = fx.tx.take();
  REQUIRE(out.size() == 1);
  CHECK(out[0].flags == kRst);
  CHECK(out[0].seq == 99);
  CHECK(out[0].dport == 5001);
  fx.tcp->on_frame(other.tcp(40000, 1, 0, kSyn, 100), fx.now);
  out = fx.tx.take();
  REQUIRE(out.size() == 1);
  CHECK(out[0].flags == (kRst | kAck));
  CHECK(out[0].ack == 2);
  // A RST is never answered.
  fx.tcp->on_frame(other.tcp(40000, 1, 0, kRst, 0), fx.now);
  CHECK(fx.tx.take().empty());
}

TEST_CASE("UserTcp: bad checksums are dropped unless the device says they were not computed") {
  Fixture fx;
  fx.establish();
  const auto bad = fx.peer.tcp(40000, fx.peer_seq, fx.acked, kAck | kPsh, 65535, "data", 0, true);
  fx.tcp->on_frame(bad, fx.now);
  CHECK(fx.h.data.empty());
  CHECK(fx.tcp->stats().bad_frames == 1);
  fx.tcp->on_frame(bad, fx.now, true);
  CHECK(fx.h.data == "data");
}

TEST_CASE("UserTcp: zero window stops sending and probes until it opens") {
  Fixture fx;
  fx.establish(1460, 0);
  REQUIRE(fx.tcp->send(bytes_of("queued")));
  CHECK(fx.tx.take().empty());
  fx.now = fx.tcp->next_timer_ns();
  fx.tcp->on_timer(fx.now);
  auto out = fx.tx.take();
  REQUIRE(out.size() == 1);
  CHECK(out[0].seq == fx.iss);  // snd_una - 1
  CHECK(out[0].data.empty());
  CHECK(fx.tcp->stats().zero_window_probes == 1);
  fx.feed(fx.seg(kAck, {}, 1000));
  out = fx.tx.take();
  REQUIRE(out.size() == 1);
  CHECK(out[0].data == "queued");
}

TEST_CASE("UserTcp: the peer's window limits what is in flight") {
  Fixture fx;
  fx.establish(100, 150);
  const std::string payload(400, 'w');
  REQUIRE(fx.tcp->send(bytes_of(payload)));
  auto out = fx.tx.take();
  REQUIRE(out.size() == 2);
  CHECK(out[1].data.size() == 50);
  fx.acked = fx.iss + 151;
  fx.feed(fx.seg(kAck, {}, 150));
  out = fx.tx.take();
  REQUIRE(out.size() == 2);
  CHECK(out[0].seq == fx.iss + 151);
}

TEST_CASE("UserTcp: sequence numbers wrap around") {
  Fixture fx(0xFFFF'FFF0U);
  fx.peer_seq = 0xFFFF'FFFEU;
  fx.establish(100);
  std::string sent;
  for (int i = 0; i < 5; ++i) {
    const std::string chunk(40, static_cast<char>('a' + i));
    REQUIRE(fx.tcp->send(bytes_of(chunk)));
    sent += chunk;
  }
  auto out = fx.tx.take();
  std::string got;
  for (const auto& f : out) got += f.data;
  CHECK(got == sent);
  fx.acked = 0xFFFF'FFF1U + 200;  // wraps
  fx.feed(fx.seg(kAck | kPsh, "0123456789"));
  fx.peer_seq += 10;
  CHECK(fx.tcp->unacked() == 0);
  CHECK(fx.h.data == "0123456789");
  out = fx.tx.take();
  REQUIRE(out.size() == 1);
  CHECK(out[0].ack == fx.peer_seq);
}

TEST_CASE("UserTcp: an ACK for unsent data is answered and ignored") {
  Fixture fx;
  fx.establish();
  fx.acked = fx.iss + 100;
  fx.feed(fx.seg(kAck | kPsh, "data"));
  auto out = fx.tx.take();
  REQUIRE(out.size() == 1);
  CHECK(fx.h.data.empty());
  CHECK(fx.tcp->state() == TcpState::Established);
}

TEST_CASE("UserTcp: a full receive buffer aborts with ENOBUFS") {
  Fixture fx;
  fx.establish();
  fx.h.consume_limit = 0;
  const std::string chunk(1000, 'q');
  for (int i = 0; i < 9 && fx.h.closed == -1; ++i) {
    fx.feed(fx.seg(kAck | kPsh, chunk));
    fx.peer_seq += 1000;
  }
  CHECK(fx.h.closed == ENOBUFS);
}
