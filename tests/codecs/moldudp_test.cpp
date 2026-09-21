// MoldUDP64 1.00: packets against the spec-derived fixtures, builders, receiver state machine,
// transmitter retransmission.
#include "nasdaq_test_util.hpp"

#include "fastmm/codecs/moldudp/moldudp64.hpp"

#include <array>

using namespace fastmm;
using namespace fastmm::codecs;
using namespace fastmm::codecs::moldudp;
using fastmm::codecs::test::Bytes;
using fastmm::codecs::test::bytes_of;
using fastmm::codecs::test::to_bytes;
using fastmm::codecs::test::to_string;

namespace {
const std::map<std::string, Bytes>& fixtures() {
  static const std::map<std::string, Bytes> f =
      codecs::test::load_hex_fixture("moldudp64_packets.hex");
  return f;
}
const Bytes& fx(const char* name) {
  const auto it = fixtures().find(name);
  REQUIRE_MESSAGE(it != fixtures().end(), name);
  return it->second;
}

struct Recorder {
  std::vector<std::pair<std::uint64_t, std::string>> messages;
  std::vector<Bytes> requests;
  int eos = 0;
  void on_message(std::uint64_t seq, std::span<const std::byte> msg) noexcept {
    messages.emplace_back(seq, to_string(msg));
  }
  void send_request(std::span<const std::byte> packet) noexcept {
    requests.push_back(bytes_of(packet));
  }
  void on_end_of_session() noexcept { ++eos; }
};
static_assert(ReceiverHandler<Recorder>);
static_assert(SessionLayer<Receiver<Recorder>>);

Bytes packet(const SessionId& s, std::uint64_t seq, std::initializer_list<std::string_view> msgs) {
  std::array<std::byte, 1500> buf{};
  PacketBuilder b(buf, s, seq);
  for (std::string_view m : msgs) REQUIRE(b.add(std::as_bytes(std::span<const char>(m))));
  const std::size_t n = b.finish();
  return Bytes(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n));
}
}  // namespace

TEST_CASE("codecs.moldudp: fixtures parse at the spec offsets") {
  PacketView p;
  REQUIRE(parse_packet(codecs::test::span_of(fx("data_seq5_two_messages")), p));
  CHECK(p.session == make_session("SESSION001"));
  CHECK(p.kind == PacketKind::Data);
  CHECK(p.sequence == 5);
  CHECK(p.count == 2);
  CHECK(p.next_sequence() == 7);
  MessageIterator it(p);
  std::uint64_t seq = 0;
  std::span<const std::byte> msg;
  REQUIRE(it.next(seq, msg));
  CHECK(seq == 5);
  CHECK(to_string(msg) == "abc");
  REQUIRE(it.next(seq, msg));
  CHECK(seq == 6);
  CHECK(msg.empty());  // zero-length messages are legal
  CHECK_FALSE(it.next(seq, msg));

  REQUIRE(parse_packet(codecs::test::span_of(fx("heartbeat_next7")), p));
  CHECK(p.kind == PacketKind::Heartbeat);
  CHECK(p.sequence == 7);
  CHECK(p.count == 0);
  REQUIRE(parse_packet(codecs::test::span_of(fx("end_of_session_next7")), p));
  CHECK(p.kind == PacketKind::EndOfSession);
  CHECK(p.next_sequence() == 7);

  RequestView r;
  REQUIRE(parse_request(codecs::test::span_of(fx("request_seq3_count4")), r));
  CHECK(r.sequence == 3);
  CHECK(r.count == 4);
  CHECK(r.session == make_session("SESSION001"));
}

TEST_CASE("codecs.moldudp: builders reproduce the fixture bytes") {
  const SessionId s = make_session("SESSION001");
  CHECK(packet(s, 5, {"abc", ""}) == fx("data_seq5_two_messages"));
  std::array<std::byte, 20> buf{};
  REQUIRE(write_heartbeat(buf, s, 7) == 20);
  CHECK(Bytes(buf.begin(), buf.end()) == fx("heartbeat_next7"));
  REQUIRE(write_end_of_session(buf, s, 7) == 20);
  CHECK(Bytes(buf.begin(), buf.end()) == fx("end_of_session_next7"));
  REQUIRE(write_request(buf, s, 3, 4) == 20);
  CHECK(Bytes(buf.begin(), buf.end()) == fx("request_seq3_count4"));
  CHECK(write_request(std::span<std::byte>(buf).first(19), s, 3, 4) == 0);
}

TEST_CASE("codecs.moldudp: malformed datagrams are refused") {
  PacketView p;
  const Bytes& good = fx("data_seq5_two_messages");
  CHECK_FALSE(parse_packet(std::span<const std::byte>(good).first(19), p));
  CHECK_FALSE(
      parse_packet(std::span<const std::byte>(good).first(good.size() - 1), p));  // truncated
  Bytes trailing = good;
  trailing.push_back(std::byte{0});
  CHECK_FALSE(parse_packet(codecs::test::span_of(trailing), p));
  Bytes hb = fx("heartbeat_next7");
  hb.push_back(std::byte{1});
  CHECK_FALSE(parse_packet(codecs::test::span_of(hb), p));

  MessageFramer f;
  const std::array<std::byte, 4> partial{
      std::byte{0}, std::byte{3}, std::byte{'a'}, std::byte{'b'}};
  CHECK_FALSE(f.next(partial).complete());
  CHECK_FALSE(f.next(std::span<const std::byte>(partial).first(1)).complete());

  std::array<std::byte, 24> small{};
  PacketBuilder b(small, make_session("S"), 1);
  CHECK(b.add(to_bytes("ab")));
  CHECK_FALSE(b.add(to_bytes("c")));  // 20 + 4 used, 2 + 1 needed
  CHECK(b.finish() == 24);
}

namespace {
struct GapRecorder : Recorder {
  std::vector<std::pair<std::uint64_t, std::uint64_t>> lost;
  void on_gap_unrecoverable(std::uint64_t from, std::uint64_t count) noexcept {
    lost.emplace_back(from, count);
  }
};
static_assert(UnrecoverableGapHandler<GapRecorder>);
static_assert(!UnrecoverableGapHandler<Recorder>);

struct Stamp {
  std::int64_t t = 0;
};
struct StampRecorder {
  std::vector<std::pair<std::uint64_t, std::int64_t>> messages;
  void on_message(std::uint64_t seq, std::span<const std::byte>, const Stamp& m) noexcept {
    messages.emplace_back(seq, m.t);
  }
  void send_request(std::span<const std::byte>) noexcept {}
  void on_end_of_session() noexcept {}
};
static_assert(ReceiverHandler<StampRecorder, Stamp>);
static_assert(!ReceiverHandler<StampRecorder>);

std::span<const std::byte> sp(const Bytes& b) {
  return codecs::test::span_of(b);
}
Bytes heartbeat(const SessionId& s, std::uint64_t next) {
  Bytes b(kHeaderLength);
  write_heartbeat(b, s, next);
  return b;
}
Bytes end_of_session(const SessionId& s, std::uint64_t next) {
  Bytes b(kHeaderLength);
  write_end_of_session(b, s, next);
  return b;
}
RequestView parsed(const Bytes& b) {
  RequestView r;
  REQUIRE(parse_request(sp(b), r));
  return r;
}
std::vector<std::uint64_t> seqs(const Recorder& h) {
  std::vector<std::uint64_t> out;
  out.reserve(h.messages.size());
  for (const auto& m : h.messages) out.push_back(m.first);
  return out;
}
}  // namespace

TEST_CASE("codecs.moldudp: the receiver delivers in order, detects gaps and recovers") {
  const SessionId s = make_session("SESS");
  Recorder h;
  ReceiverConfig cfg;
  cfg.request_timeout_ns = 100;
  cfg.gap_timeout_ns = 0;
  Receiver<Recorder> rx(h, cfg);
  CHECK(rx.state() == SessionState::Down);

  rx.on_packet(sp(packet(s, 1, {"m1", "m2"})), 0);
  CHECK(rx.state() == SessionState::Up);
  CHECK(rx.next_sequence() == 3);
  rx.on_packet(sp(packet(s, 1, {"m1", "m2"})), 0);  // duplicate
  CHECK(rx.stats().duplicate_packets == 1);

  rx.on_packet(sp(packet(s, 5, {"m5"})), 1'000);  // 3 and 4 missing: 5 is held
  CHECK(rx.state() == SessionState::Recovering);
  CHECK(rx.stats().gaps == 1);
  CHECK(rx.stats().ahead_packets == 1);
  CHECK(rx.stats().held_packets == 1);
  CHECK(rx.held() == 1);
  REQUIRE(h.requests.size() == 1);
  const RequestView r = parsed(h.requests[0]);
  CHECK(r.session == s);
  CHECK(r.sequence == 3);
  CHECK(r.count == 2);  // up to the held packet

  rx.on_timer(1'050);  // not yet timed out
  CHECK(h.requests.size() == 1);
  rx.on_timer(1'100);
  CHECK(h.requests.size() == 2);  // re-sent

  rx.on_packet(sp(packet(s, 2, {"m2", "m3", "m4"})), 1'200);  // overlapping retransmission
  CHECK(rx.state() == SessionState::Up);
  CHECK(rx.next_sequence() == 6);  // 5 drained from the reorder buffer
  CHECK(rx.held() == 0);

  rx.on_packet(sp(packet(s, 6, {"m6"})), 1'300);
  rx.on_packet(sp(heartbeat(s, 9)), 1'300);  // announces 7 and 8
  CHECK(rx.state() == SessionState::Recovering);
  REQUIRE(h.requests.size() == 3);
  CHECK(parsed(h.requests.back()).sequence == 7);
  CHECK(parsed(h.requests.back()).count == 2);
  rx.on_packet(sp(packet(s, 7, {"m7", "m8"})), 1'400);
  rx.on_packet(sp(end_of_session(s, 9)), 1'400);
  CHECK(rx.state() == SessionState::Up);
  CHECK(rx.end_of_session());
  CHECK(h.eos == 1);

  rx.on_packet(sp(packet(make_session("OTHER"), 9, {"x"})), 1'500);
  CHECK(rx.stats().session_mismatch == 1);

  REQUIRE(h.messages.size() == 8);
  for (std::size_t i = 0; i < h.messages.size(); ++i) {
    CHECK(h.messages[i].first == i + 1);
    CHECK(h.messages[i].second == "m" + std::to_string(i + 1));
  }
}

TEST_CASE("codecs.moldudp: receiver configuration: expected session, start sequence, request cap") {
  const SessionId s = make_session("WANTED");
  Recorder h;
  ReceiverConfig cfg;
  cfg.session = s;
  cfg.next_sequence = 1;
  cfg.max_request_count = 2;
  cfg.gap_timeout_ns = 0;
  Receiver<Recorder> rx(h, cfg);
  rx.on_packet(sp(packet(make_session("WRONG"), 1, {"a"})), 0);
  CHECK(rx.stats().session_mismatch == 1);
  CHECK(rx.state() == SessionState::Down);
  rx.on_packet(sp(packet(s, 10, {"j"})), 0);  // joined late: 1..9 missing
  REQUIRE(h.requests.size() == 1);
  CHECK(parsed(h.requests[0]).sequence == 1);
  CHECK(parsed(h.requests[0]).count == 2);
  rx.on_packet(sp(packet(s, 1, {"a", "b"})), 0);  // progress -> next chunk
  REQUIRE(h.requests.size() == 2);
  CHECK(parsed(h.requests[1]).sequence == 3);

  Recorder live;
  Receiver<Recorder> joiner(live);  // next_sequence 0: live join, no history recovery
  joiner.on_packet(sp(packet(s, 100, {"x", "y"})), 0);
  CHECK(joiner.state() == SessionState::Up);
  CHECK(live.requests.empty());
  CHECK(live.messages.size() == 2);
  CHECK(live.messages[0].first == 100);
  joiner.on_packet(std::span<const std::byte>{}, 0);
  CHECK(joiner.stats().malformed == 1);
}

TEST_CASE("codecs.moldudp: a request is stamped with the packet time, not the last timer tick") {
  const SessionId s = make_session("STAMP");
  Recorder h;
  ReceiverConfig cfg;
  cfg.gap_timeout_ns = 0;
  cfg.request_timeout_ns = 250'000'000;
  Receiver<Recorder> rx(h, cfg);
  rx.on_timer(0);
  rx.on_packet(sp(packet(s, 1, {"a"})), 1'000'000'000);
  rx.on_packet(sp(packet(s, 3, {"c"})), 1'000'000'000);
  REQUIRE(h.requests.size() == 1);
  rx.on_timer(1'000'000'001);  // the request is 1 ns old
  CHECK(h.requests.size() == 1);
  rx.on_timer(1'249'999'999);
  CHECK(h.requests.size() == 1);
  rx.on_timer(1'250'000'000);
  CHECK(h.requests.size() == 2);

  // on_frame() has no time argument: it uses the latest time seen (2 s, when the timer re-sent
  // the request), so the frame does not trigger another re-send.
  rx.on_timer(2'000'000'000);
  CHECK(h.requests.size() == 3);
  CHECK(rx.on_frame(codecs::test::frame_of(packet(s, 5, {"e"}))));
  CHECK(h.requests.size() == 3);
  CHECK(rx.held() == 2);
}

TEST_CASE("codecs.moldudp: line B fills a gap within gap_timeout_ns without a request") {
  const SessionId s = make_session("AB");
  Recorder h;
  ReceiverConfig cfg;
  cfg.gap_timeout_ns = 50'000;
  Receiver<Recorder> rx(h, cfg);
  rx.on_packet(0, sp(packet(s, 1, {"m1"})), 0);
  rx.on_packet(1, sp(packet(s, 1, {"m1"})), 3'000);   // B trails A by 3 us
  rx.on_packet(0, sp(packet(s, 3, {"m3"})), 10'000);  // 2 lost on A
  CHECK(rx.held() == 1);
  rx.on_timer(40'000);
  CHECK(h.requests.empty());
  CHECK(rx.state() == SessionState::Up);
  rx.on_packet(1, sp(packet(s, 2, {"m2"})), 45'000);  // B delivers it in time
  CHECK(rx.next_sequence() == 4);
  rx.on_packet(1, sp(packet(s, 3, {"m3"})), 46'000);
  rx.on_packet(0, sp(packet(s, 2, {"m2"})), 47'000);  // late copy on A
  CHECK(h.requests.empty());
  CHECK(rx.stats().gaps == 0);

  const ReceiverLineStats& a = rx.stats().lines[0];
  const ReceiverLineStats& b = rx.stats().lines[1];
  CHECK(a.packets == 3);
  CHECK(b.packets == 3);
  CHECK(a.duplicate_packets == 1);
  CHECK(b.duplicate_packets == 2);
  CHECK(b.skew_samples == 2);
  CHECK(b.skew_max_ns == 36'000);  // m3: A at 10 us, B at 46 us
  CHECK(b.skew_last_ns == 36'000);
  CHECK(b.skew_sum_ns == 39'000);
  CHECK(a.skew_samples == 1);
  CHECK(a.skew_max_ns == 2'000);

  // Lost on both lines: declared once the hole is gap_timeout_ns old.
  rx.on_packet(0, sp(packet(s, 5, {"m5"})), 100'000);
  rx.on_packet(1, sp(packet(s, 5, {"m5"})), 101'000);
  rx.on_timer(149'999);
  CHECK(h.requests.empty());
  rx.on_timer(150'000);
  REQUIRE(h.requests.size() == 1);
  CHECK(rx.state() == SessionState::Recovering);
  CHECK(rx.stats().gaps == 1);
  CHECK(parsed(h.requests[0]).sequence == 4);
  CHECK(parsed(h.requests[0]).count == 1);
  rx.on_packet(2, sp(packet(s, 4, {"m4"})), 400'000);  // retransmission
  CHECK(rx.state() == SessionState::Up);
  CHECK(seqs(h) == std::vector<std::uint64_t>{1, 2, 3, 4, 5});
}

TEST_CASE("codecs.moldudp: a full reorder buffer drops packets and recovery requests them") {
  const SessionId s = make_session("FULL");
  Recorder h;
  ReceiverConfig cfg;
  cfg.reorder_packets = 2;
  cfg.gap_timeout_ns = 1'000'000;
  cfg.max_request_count = 100;
  Receiver<Recorder> rx(h, cfg);
  rx.on_packet(sp(packet(s, 1, {"m1"})), 0);
  rx.on_packet(sp(packet(s, 3, {"m3"})), 10);
  rx.on_packet(sp(packet(s, 4, {"m4"})), 20);
  CHECK(h.requests.empty());                   // within gap_timeout_ns
  rx.on_packet(sp(packet(s, 5, {"m5"})), 30);  // no room: dropped, gap declared at once
  CHECK(rx.stats().reorder_overflow == 1);
  CHECK(rx.stats().reorder_high_water == 2);
  CHECK(rx.state() == SessionState::Recovering);
  REQUIRE(h.requests.size() == 1);
  CHECK(parsed(h.requests[0]).sequence == 2);
  CHECK(parsed(h.requests[0]).count == 1);
  rx.on_packet(sp(packet(s, 6, {"m6"})), 40);
  CHECK(rx.stats().reorder_overflow == 2);
  CHECK(h.requests.size() == 1);

  // The retransmission of 2 drains 3 and 4; 5 and 6 were dropped, so they are requested
  // without waiting for gap_timeout_ns.
  rx.on_packet(2, sp(packet(s, 2, {"m2"})), 50);
  CHECK(rx.next_sequence() == 5);
  REQUIRE(h.requests.size() == 2);
  CHECK(parsed(h.requests[1]).sequence == 5);
  CHECK(parsed(h.requests[1]).count == 2);
  rx.on_packet(2, sp(packet(s, 5, {"m5", "m6"})), 60);
  CHECK(rx.state() == SessionState::Up);
  CHECK(seqs(h) == std::vector<std::uint64_t>{1, 2, 3, 4, 5, 6});
}

TEST_CASE("codecs.moldudp: without a request path gaps are skipped and reported") {
  const SessionId s = make_session("NOREQ");
  GapRecorder h;
  ReceiverConfig cfg;
  cfg.can_request = false;
  cfg.gap_timeout_ns = 1'000;
  cfg.reorder_packets = 2;
  Receiver<GapRecorder> rx(h, cfg);
  rx.on_packet(sp(packet(s, 1, {"m1"})), 0);
  rx.on_packet(sp(packet(s, 3, {"m3"})), 0);
  rx.on_timer(999);
  CHECK(h.lost.empty());
  rx.on_timer(1'000);
  REQUIRE(h.lost.size() == 1);
  CHECK(h.lost[0] == std::pair<std::uint64_t, std::uint64_t>{2, 1});
  CHECK(rx.next_sequence() == 4);

  // Overflow: the gap before the earliest held packet is given up at once.
  rx.on_packet(sp(packet(s, 5, {"m5"})), 2'000);
  rx.on_packet(sp(packet(s, 7, {"m7"})), 2'000);
  rx.on_packet(sp(packet(s, 9, {"m9"})), 2'000);
  REQUIRE(h.lost.size() == 2);
  CHECK(h.lost[1] == std::pair<std::uint64_t, std::uint64_t>{4, 1});
  CHECK(rx.next_sequence() == 6);
  CHECK(rx.held() == 2);  // 7 and 9
  rx.on_packet(sp(packet(s, 6, {"m6"})), 2'100);
  CHECK(rx.next_sequence() == 8);
  rx.on_timer(2'999);
  CHECK(h.lost.size() == 2);
  rx.on_timer(3'000);  // 8 has been missing since 9 arrived at 2 000
  REQUIRE(h.lost.size() == 3);
  CHECK(h.lost[2] == std::pair<std::uint64_t, std::uint64_t>{8, 1});
  rx.on_packet(sp(end_of_session(s, 10)), 3'100);
  CHECK(h.eos == 1);
  CHECK(h.requests.empty());
  CHECK(rx.state() == SessionState::Up);
  CHECK(rx.stats().unrecoverable_gaps == 3);
  CHECK(rx.stats().lost_messages == 3);
  CHECK(seqs(h) == std::vector<std::uint64_t>{1, 3, 5, 6, 7, 9});

  // A handler without on_gap_unrecoverable() still gets the rest of the stream.
  Recorder plain;
  Receiver<Recorder> rx2(plain, cfg);
  rx2.on_packet(sp(packet(s, 1, {"m1"})), 0);
  rx2.on_packet(sp(packet(s, 3, {"m3"})), 0);
  rx2.on_timer(1'000);
  CHECK(seqs(plain) == std::vector<std::uint64_t>{1, 3});
}

TEST_CASE("codecs.moldudp: a gap is unrecoverable after max_request_attempts requests") {
  const SessionId s = make_session("ATTEMPTS");
  GapRecorder h;
  ReceiverConfig cfg;
  cfg.gap_timeout_ns = 0;
  cfg.request_timeout_ns = 100;
  cfg.max_request_attempts = 2;
  Receiver<GapRecorder> rx(h, cfg);
  rx.on_packet(sp(packet(s, 1, {"m1"})), 0);
  rx.on_packet(sp(packet(s, 3, {"m3"})), 0);
  CHECK(h.requests.size() == 1);
  rx.on_timer(100);
  CHECK(h.requests.size() == 2);
  CHECK(h.lost.empty());
  rx.on_timer(200);
  CHECK(h.requests.size() == 2);
  REQUIRE(h.lost.size() == 1);
  CHECK(h.lost[0] == std::pair<std::uint64_t, std::uint64_t>{2, 1});
  CHECK(rx.state() == SessionState::Up);
  CHECK(seqs(h) == std::vector<std::uint64_t>{1, 3});
  CHECK(rx.stats().unrecoverable_gaps == 1);
}

TEST_CASE(
    "codecs.moldudp: a new session after End of Session is adopted only with follow_session") {
  const SessionId a = make_session("DAY1");
  const SessionId b = make_session("DAY2");
  ReceiverConfig cfg;
  cfg.next_sequence = 1;
  cfg.gap_timeout_ns = 1'000'000;

  Recorder off;
  Receiver<Recorder> rx_off(off, cfg);
  rx_off.on_packet(sp(packet(a, 1, {"a1"})), 0);
  rx_off.on_packet(sp(end_of_session(a, 2)), 0);
  CHECK(off.eos == 1);
  rx_off.on_packet(sp(packet(b, 1, {"b1"})), 0);
  CHECK(rx_off.stats().session_mismatch == 1);
  CHECK(rx_off.session() == a);
  CHECK(off.messages.size() == 1);

  cfg.follow_session = true;
  Recorder on;
  Receiver<Recorder> rx(on, cfg);
  rx.on_packet(sp(packet(a, 1, {"a1"})), 0);
  rx.on_packet(sp(packet(a, 3, {"a3"})), 0);
  rx.on_packet(sp(end_of_session(a, 4)), 0);
  CHECK(on.eos == 0);  // 2 is still missing
  rx.on_packet(sp(packet(b, 1, {"b1"})), 0);
  CHECK(rx.stats().session_mismatch == 1);  // not before the old session is complete
  rx.on_packet(1, sp(packet(a, 2, {"a2"})), 0);
  CHECK(on.eos == 1);
  rx.on_packet(sp(packet(b, 1, {"b1"})), 0);
  CHECK(rx.stats().sessions == 1);
  CHECK(rx.session() == b);
  CHECK_FALSE(rx.end_of_session());
  rx.on_packet(sp(packet(b, 2, {"b2"})), 0);
  rx.on_packet(1, sp(end_of_session(a, 4)), 0);  // late copy of the old session
  CHECK(rx.stats().session_mismatch == 2);
  REQUIRE(on.messages.size() == 5);
  CHECK(on.messages[2].second == "a3");
  CHECK(on.messages[3] == std::pair<std::uint64_t, std::string>{1, "b1"});
  CHECK(on.messages[4] == std::pair<std::uint64_t, std::string>{2, "b2"});
}

TEST_CASE("codecs.moldudp: packet metadata reaches on_message, also from the reorder buffer") {
  const SessionId s = make_session("META");
  StampRecorder h;
  ReceiverConfig cfg;
  cfg.gap_timeout_ns = 1'000'000;
  Receiver<StampRecorder, Stamp> rx(h, cfg);
  rx.on_packet(0, sp(packet(s, 1, {"a"})), 10, Stamp{10});
  rx.on_packet(0, sp(packet(s, 3, {"c", "d"})), 30, Stamp{30});
  rx.on_packet(1, sp(packet(s, 2, {"b"})), 40, Stamp{40});
  REQUIRE(h.messages.size() == 4);
  CHECK(h.messages[0] == std::pair<std::uint64_t, std::int64_t>{1, 10});
  CHECK(h.messages[1] == std::pair<std::uint64_t, std::int64_t>{2, 40});
  CHECK(h.messages[2] == std::pair<std::uint64_t, std::int64_t>{3, 30});
  CHECK(h.messages[3] == std::pair<std::uint64_t, std::int64_t>{4, 30});
}

TEST_CASE("codecs.moldudp: the transmitter packs, sequences and answers requests") {
  TransmitterConfig cfg;
  cfg.max_datagram = 20 + 3 * 12;  // three 10-byte messages per packet
  cfg.history_messages = 16;
  Transmitter tx("TXSESSION", cfg);
  for (int i = 1; i <= 7; ++i) {
    std::string m = "message" + std::to_string(100 + i);
    CHECK(tx.publish(to_bytes(m)) == static_cast<std::uint64_t>(i));
  }
  std::array<std::byte, 1500> buf{};
  PacketView p;
  std::size_t n = tx.next_packet(buf);
  REQUIRE(parse_packet(std::span<const std::byte>(buf.data(), n), p));
  CHECK(p.sequence == 1);
  CHECK(p.count == 3);
  n = tx.next_packet(buf);
  REQUIRE(parse_packet(std::span<const std::byte>(buf.data(), n), p));
  CHECK(p.sequence == 4);
  CHECK(tx.next_unsent() == 7);

  std::array<std::byte, 20> req{};
  write_request(req, tx.session(), 2, 2);
  n = tx.answer_request(req, buf);
  REQUIRE(parse_packet(std::span<const std::byte>(buf.data(), n), p));
  CHECK(p.sequence == 2);
  CHECK(p.count == 2);
  write_request(req, tx.session(), 8, 2);
  CHECK(tx.answer_request(req, buf) == 0);  // not published yet
  write_request(req, make_session("OTHER"), 2, 2);
  CHECK(tx.answer_request(req, buf) == 0);

  n = tx.heartbeat(buf);
  REQUIRE(parse_packet(std::span<const std::byte>(buf.data(), n), p));
  CHECK(p.kind == PacketKind::Heartbeat);
  CHECK(p.sequence == 7);
  n = tx.end_of_session(buf);
  REQUIRE(parse_packet(std::span<const std::byte>(buf.data(), n), p));
  CHECK(p.kind == PacketKind::EndOfSession);

  for (int i = 8; i <= 16; ++i) CHECK(tx.publish(to_bytes("x")) != 0);
  CHECK(tx.publish(to_bytes("x")) == 0);  // history full
}
