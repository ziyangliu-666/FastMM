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

TEST_CASE("codecs.moldudp: the receiver delivers in order, detects gaps and recovers") {
  const SessionId s = make_session("SESS");
  Recorder h;
  ReceiverConfig cfg;
  cfg.request_timeout_ns = 100;
  Receiver<Recorder> rx(h, cfg);
  CHECK(rx.state() == SessionState::Down);

  rx.on_packet(codecs::test::span_of(packet(s, 1, {"m1", "m2"})));
  CHECK(rx.state() == SessionState::Up);
  CHECK(rx.next_sequence() == 3);
  rx.on_packet(codecs::test::span_of(packet(s, 1, {"m1", "m2"})));  // duplicate
  CHECK(rx.stats().duplicate_packets == 1);

  rx.on_timer(1'000);
  rx.on_packet(codecs::test::span_of(packet(s, 5, {"m5"})));  // 3 and 4 missing
  CHECK(rx.state() == SessionState::Recovering);
  CHECK(rx.stats().gaps == 1);
  REQUIRE(h.requests.size() == 1);
  RequestView r;
  REQUIRE(parse_request(codecs::test::span_of(h.requests[0]), r));
  CHECK(r.session == s);
  CHECK(r.sequence == 3);
  CHECK(r.count == 3);  // 3, 4 and the dropped 5
  CHECK(rx.stats().ahead_packets == 1);

  rx.on_timer(1'050);  // not yet timed out
  CHECK(h.requests.size() == 1);
  rx.on_timer(1'100);
  CHECK(h.requests.size() == 2);  // re-sent

  rx.on_packet(
      codecs::test::span_of(packet(s, 2, {"m2", "m3", "m4", "m5"})));  // overlapping retransmission
  CHECK(rx.state() == SessionState::Up);
  CHECK(rx.next_sequence() == 6);

  rx.on_packet(codecs::test::span_of(packet(s, 6, {"m6"})));
  std::array<std::byte, 20> hb{};
  write_heartbeat(hb, s, 9);  // a heartbeat announcing 7 and 8 exist
  rx.on_packet(hb);
  CHECK(rx.state() == SessionState::Recovering);
  REQUIRE(h.requests.size() == 3);
  REQUIRE(parse_request(codecs::test::span_of(h.requests.back()), r));
  CHECK(r.sequence == 7);
  CHECK(r.count == 2);
  rx.on_packet(codecs::test::span_of(packet(s, 7, {"m7", "m8"})));
  write_end_of_session(hb, s, 9);
  rx.on_packet(hb);
  CHECK(rx.state() == SessionState::Up);
  CHECK(rx.end_of_session());
  CHECK(h.eos == 1);

  rx.on_packet(codecs::test::span_of(packet(make_session("OTHER"), 9, {"x"})));
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
  Receiver<Recorder> rx(h, cfg);
  rx.on_packet(codecs::test::span_of(packet(make_session("WRONG"), 1, {"a"})));
  CHECK(rx.stats().session_mismatch == 1);
  CHECK(rx.state() == SessionState::Down);
  rx.on_packet(codecs::test::span_of(packet(s, 10, {"j"})));  // joined late: 1..9 missing
  REQUIRE(h.requests.size() == 1);
  RequestView r;
  REQUIRE(parse_request(codecs::test::span_of(h.requests[0]), r));
  CHECK(r.sequence == 1);
  CHECK(r.count == 2);
  rx.on_packet(codecs::test::span_of(packet(s, 1, {"a", "b"})));  // progress -> next chunk
  REQUIRE(h.requests.size() == 2);
  REQUIRE(parse_request(codecs::test::span_of(h.requests[1]), r));
  CHECK(r.sequence == 3);

  Recorder live;
  Receiver<Recorder> joiner(live);  // next_sequence 0: live join, no history recovery
  joiner.on_packet(codecs::test::span_of(packet(s, 100, {"x", "y"})));
  CHECK(joiner.state() == SessionState::Up);
  CHECK(live.requests.empty());
  CHECK(live.messages.size() == 2);
  CHECK(live.messages[0].first == 100);
  joiner.on_packet(std::span<const std::byte>{});
  CHECK(joiner.stats().malformed == 1);
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
