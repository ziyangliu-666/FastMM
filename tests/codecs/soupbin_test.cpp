// SoupBinTCP 3.00 / 4.00 / 4.10: packets against the spec-derived fixtures, framer, and the
// client / server sessions over an in-memory byte pipe.
#include "fastmm/codecs/soupbin/soupbin.hpp"

#include "nasdaq_test_util.hpp"

#include "fastmm/codecs/soupbin/soupbin_session.hpp"

#include <array>

using namespace fastmm;
using namespace fastmm::codecs;
using namespace fastmm::codecs::soupbin;
using fastmm::codecs::test::BytePipe;
using fastmm::codecs::test::Bytes;
using fastmm::codecs::test::to_bytes;
using fastmm::codecs::test::to_string;

namespace {
constexpr std::int64_t kSec = 1'000'000'000;

const std::map<std::string, Bytes>& fixtures() {
  static const std::map<std::string, Bytes> f =
      codecs::test::load_hex_fixture("soupbin_packets.hex");
  return f;
}
const Bytes& fx(const char* name) {
  const auto it = fixtures().find(name);
  REQUIRE_MESSAGE(it != fixtures().end(), name);
  return it->second;
}

struct ServerApp {
  std::vector<std::string> received;
  void on_unsequenced(std::span<const std::byte> msg) noexcept {
    received.push_back(to_string(msg));
  }
};
static_assert(ByteWriter<BytePipe>);
static_assert(ServerHandler<ServerApp>);
static_assert(SessionLayer<ClientSession<BytePipe>>);

ClientConfig client_config() {
  ClientConfig c;
  c.username = "FMUSER";
  c.password = "secret";
  return c;
}
ServerConfig server_config() {
  ServerConfig s;
  s.username = "fmuser";  // case-insensitive
  s.password = "SECRET";
  s.session = "SESS01";
  s.history_bytes = 1U << 16;
  s.history_messages = 1024;
  return s;
}

// Client, server and the two pipes between them.
struct Link {
  BytePipe c2s;
  BytePipe s2c;
  ServerApp app;
  ClientSession<BytePipe> client;
  ServerSession<BytePipe, ServerApp> server;
  std::vector<std::pair<std::uint64_t, std::string>> sequenced;
  std::vector<std::string> unsequenced;

  explicit Link(const ClientConfig& cc = client_config(), const ServerConfig& sc = server_config())
      : client(c2s, cc), server(s2c, app, sc) {}

  void pump_server() {
    SoupBinFramer f;
    for (;;) {
      const FrameView v = f.next(c2s.pending());
      if (!v.complete()) break;
      REQUIRE(server.on_frame(v));
      c2s.consume(v.consumed);
    }
  }
  // At most `limit` packets for the client.
  void pump_client(std::size_t limit = SIZE_MAX) {
    SoupBinFramer f;
    for (std::size_t i = 0; i < limit; ++i) {
      const FrameView v = f.next(s2c.pending());
      if (!v.complete()) break;
      if (!client.on_frame(v)) {
        if (v.kind == 'S') {
          sequenced.emplace_back(client.last_sequence(), to_string(v.payload));
        } else {
          unsequenced.push_back(to_string(v.payload));
        }
      }
      s2c.consume(v.consumed);
    }
  }
  void pump() {
    pump_server();
    pump_client();
  }
  void tick(std::int64_t now) {
    client.on_timer(now);
    server.on_timer(now);
    pump();
  }
  void connect_and_login(std::int64_t now) {
    server.on_connect(now);
    REQUIRE(client.login(now));
    CHECK(client.state() == SessionState::LoggingOn);
    pump();
  }
};
}  // namespace

TEST_CASE("codecs.soupbin: the framer splits every fixture packet") {
  SoupBinFramer f;
  const std::pair<const char*, char> kinds[] = {{"L_login_request_v3", 'L'},
                                                {"L_login_request_v41", 'L'},
                                                {"A_login_accepted", 'A'},
                                                {"J_login_rejected", 'J'},
                                                {"S_sequenced_data", 'S'},
                                                {"U_unsequenced_data", 'U'},
                                                {"H_server_heartbeat", 'H'},
                                                {"R_client_heartbeat", 'R'},
                                                {"Z_end_of_session", 'Z'},
                                                {"O_logout_request", 'O'},
                                                {"plus_debug", '+'}};
  Bytes stream;
  for (const auto& [name, kind] : kinds) {
    const Bytes& raw = fx(name);
    const FrameView v = f.next(codecs::test::span_of(raw));
    REQUIRE(v.complete());
    CHECK(v.consumed == raw.size());
    CHECK(v.kind == static_cast<std::uint8_t>(kind));
    CHECK(v.payload.size() == raw.size() - 3);
    CHECK(payload_size_valid(kind, v.payload.size()));
    stream.insert(stream.end(), raw.begin(), raw.end());
  }
  CHECK(to_string(f.next(codecs::test::span_of(fx("S_sequenced_data"))).payload) == "hello");

  // Byte-at-a-time delivery: never a frame before it is complete, all 11 in the end.
  Bytes acc;
  std::size_t frames = 0;
  for (std::byte b : stream) {
    acc.push_back(b);
    const FrameView v = f.next(codecs::test::span_of(acc));
    if (v.complete()) {
      ++frames;
      acc.erase(acc.begin(), acc.begin() + static_cast<std::ptrdiff_t>(v.consumed));
    }
  }
  CHECK(frames == 11);
  CHECK(acc.empty());

  const std::array<std::byte, 2> zero{};
  const FrameView bad = f.next(zero);
  CHECK(bad.complete());
  CHECK(bad.kind == 0);
  CHECK_FALSE(payload_size_valid('A', 29));
  CHECK_FALSE(payload_size_valid('H', 1));
  CHECK_FALSE(payload_size_valid('Q', 0));
}

TEST_CASE("codecs.soupbin: builders reproduce the fixture bytes and parsers read them") {
  std::array<std::byte, 128> buf{};
  auto same = [&](std::size_t n, const char* name) {
    REQUIRE_MESSAGE(n == fx(name).size(), name);
    CHECK_MESSAGE(Bytes(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n)) == fx(name),
                  name);
  };
  same(write_login_request(buf, "FMUSER", "secret", "", 1, Version::V300, 0), "L_login_request_v3");
  same(write_login_request(buf, "FMUSER", "secret", "", 1, Version::V400, 0), "L_login_request_v3");
  same(write_login_request(buf, "FMUSER", "secret", "", 1, Version::V410, 15'000),
       "L_login_request_v41");
  same(write_login_accepted(buf, "SESS01", 42), "A_login_accepted");
  same(write_login_rejected(buf, kRejectNotAuthorized), "J_login_rejected");
  same(write_packet(buf, PacketType::SequencedData, to_bytes("hello")), "S_sequenced_data");
  same(write_packet(buf, PacketType::UnsequencedData, to_bytes("world")), "U_unsequenced_data");
  same(write_control(buf, PacketType::ServerHeartbeat), "H_server_heartbeat");
  same(write_control(buf, PacketType::ClientHeartbeat), "R_client_heartbeat");
  same(write_control(buf, PacketType::EndOfSession), "Z_end_of_session");
  same(write_control(buf, PacketType::LogoutRequest), "O_logout_request");
  same(write_debug(buf, "dbg"), "plus_debug");
  CHECK(write_login_request(buf, "TOOLONG", "x", "", 1, Version::V300, 0) == 0);
  CHECK(write_login_request(buf, "u", "p", "", 1, Version::V410, 100'000) == 0);  // 6 digits

  SoupBinFramer f;
  LoginRequestView lr;
  REQUIRE(parse_login_request(f.next(codecs::test::span_of(fx("L_login_request_v3"))).payload, lr));
  CHECK(lr.username == "FMUSER");
  CHECK(lr.password == "secret");
  CHECK(lr.session.empty());
  CHECK(lr.sequence == 1);
  CHECK(lr.heartbeat_timeout_ms == 0);
  REQUIRE(
      parse_login_request(f.next(codecs::test::span_of(fx("L_login_request_v41"))).payload, lr));
  CHECK(lr.heartbeat_timeout_ms == 15'000);
  LoginAcceptedView la;
  REQUIRE(parse_login_accepted(f.next(codecs::test::span_of(fx("A_login_accepted"))).payload, la));
  CHECK(la.session == "SESS01");
  CHECK(la.sequence == 42);
  CHECK(equal_ignore_case("FmUser", "FMUSER"));
  CHECK_FALSE(equal_ignore_case("FmUser", "FMUSE"));
}

TEST_CASE("codecs.soupbin: login, sequenced and unsequenced data, heartbeats and timeouts") {
  Link l;
  l.connect_and_login(0);
  REQUIRE(l.client.state() == SessionState::Up);
  CHECK(l.server.state() == SessionState::Up);
  CHECK(l.client.session() == "SESS01");
  CHECK(l.client.next_sequence() == 1);

  for (int i = 1; i <= 5; ++i)
    REQUIRE(l.server.send_sequenced(to_bytes("seq" + std::to_string(i))));
  REQUIRE(l.server.send_unsequenced(to_bytes("best-effort")));  // 4.00+
  REQUIRE(l.server.send_debug("hello from the host"));
  REQUIRE(l.client.send(to_bytes("order-1")));
  REQUIRE(l.client.send(to_bytes("order-2")));
  l.pump();
  REQUIRE(l.sequenced.size() == 5);
  for (std::size_t i = 0; i < 5; ++i) {
    CHECK(l.sequenced[i].first == i + 1);
    CHECK(l.sequenced[i].second == "seq" + std::to_string(i + 1));
  }
  CHECK(l.client.next_sequence() == 6);
  CHECK(l.unsequenced == std::vector<std::string>{"best-effort"});
  CHECK(l.app.received == std::vector<std::string>{"order-1", "order-2"});
  CHECK(l.client.stats().debug == 1);

  // Idle: both sides heartbeat after the interval, nobody times out.
  for (std::int64_t t = kSec / 2; t <= 10 * kSec; t += kSec / 2) l.tick(t);
  CHECK(l.client.stats().heartbeats_sent >= 9);
  CHECK(l.server.stats().heartbeats_sent >= 9);
  CHECK(l.client.stats().server_heartbeats >= 9);
  CHECK(l.server.stats().client_heartbeats >= 9);
  CHECK(l.client.state() == SessionState::Up);
  CHECK(l.server.state() == SessionState::Up);

  // The server goes silent: the client gives up after server_timeout (15 s).
  std::int64_t t = 10 * kSec;
  while (l.client.state() == SessionState::Up && t < 40 * kSec) {
    t += kSec / 2;
    l.client.on_timer(t);
    l.s2c.drop_all();
  }
  CHECK(l.client.state() == SessionState::Down);
  CHECK(l.client.close_reason() == CloseReason::PeerTimeout);
  CHECK(t >= 25 * kSec);
  CHECK(t <= 26 * kSec);

  // And the server drops the silent client after client_timeout.
  while (l.server.state() == SessionState::Up && t < 60 * kSec) {
    t += kSec / 2;
    l.server.on_timer(t);
  }
  CHECK(l.server.close_reason() == CloseReason::PeerTimeout);
}

TEST_CASE("codecs.soupbin: rejected logins") {
  {
    ClientConfig cc = client_config();
    cc.password = "wrong";
    Link l(cc);
    l.connect_and_login(0);
    CHECK(l.client.state() == SessionState::Down);
    CHECK(l.client.close_reason() == CloseReason::LoginRejected);
    CHECK(l.client.reject_code() == kRejectNotAuthorized);
    CHECK(l.server.stats().logins_rejected == 1);
  }
  {
    ClientConfig cc = client_config();
    cc.session = "SESS99";
    Link l(cc);
    l.connect_and_login(0);
    CHECK(l.client.reject_code() == kRejectSessionNotAvailable);
  }
  {
    Link l;
    l.server.on_connect(0);
    REQUIRE(l.client.login(0));
    for (std::int64_t t = kSec; l.client.state() == SessionState::LoggingOn && t < 20 * kSec;
         t += kSec)
      l.client.on_timer(t);  // the server never answers
    CHECK(l.client.close_reason() == CloseReason::LoginTimeout);
  }
  {
    Link l;
    l.server.on_connect(0);
    for (std::int64_t t = kSec; l.server.state() == SessionState::LoggingOn && t < 40 * kSec;
         t += kSec)
      l.server.on_timer(t);  // no Login Request within 30 s
    CHECK(l.server.close_reason() == CloseReason::LoginTimeout);
  }
  {
    Link l;  // sequenced data before login is a protocol error
    std::array<std::byte, 16> buf{};
    const std::size_t n = write_packet(buf, PacketType::SequencedData, to_bytes("x"));
    l.s2c.send(std::span<const std::byte>(buf.data(), n));
    REQUIRE(l.client.login(0));
    l.pump_client();
    CHECK(l.client.close_reason() == CloseReason::ProtocolError);
    CHECK(l.client.stats().protocol_errors == 1);
  }
}

TEST_CASE("codecs.soupbin: a reconnect resumes the session at the next expected sequence number") {
  Link l;
  l.connect_and_login(0);
  for (int i = 1; i <= 5; ++i) REQUIRE(l.server.send_sequenced(to_bytes("m" + std::to_string(i))));
  l.pump_server();
  l.pump_client(3);  // the connection breaks after three packets
  REQUIRE(l.sequenced.size() == 3);
  l.s2c.drop_all();
  l.c2s.drop_all();
  l.client.on_disconnect();
  l.server.on_disconnect();
  CHECK(l.client.close_reason() == CloseReason::Disconnected);
  CHECK(l.client.next_sequence() == 4);
  REQUIRE(l.server.send_sequenced(to_bytes("m6")));  // generated while nobody is connected
  REQUIRE(l.server.send_sequenced(to_bytes("m7")));

  l.connect_and_login(5 * kSec);  // Login Request: session SESS01, sequence 4
  REQUIRE(l.client.state() == SessionState::Up);
  REQUIRE(l.sequenced.size() == 7);
  for (std::size_t i = 0; i < 7; ++i) {
    CHECK(l.sequenced[i].first == i + 1);
    CHECK(l.sequenced[i].second == "m" + std::to_string(i + 1));
  }
  CHECK(l.server.stats().replayed == 4);

  REQUIRE(l.client.logout());
  l.pump_server();
  CHECK(l.server.close_reason() == CloseReason::Logout);
}

TEST_CASE("codecs.soupbin: 4.10 heartbeat timeout and end of session") {
  ClientConfig cc = client_config();
  cc.version = Version::V410;
  cc.heartbeat_timeout_ms = 2'500;
  Link l(cc);
  l.connect_and_login(0);
  REQUIRE(l.server.state() == SessionState::Up);
  CHECK(l.server.client_timeout_ns() == 2'500'000'000);
  std::int64_t t = 0;
  while (l.server.state() == SessionState::Up && t < 10 * kSec) {
    t += kSec / 10;
    l.server.on_timer(t);  // the client never sends anything
  }
  CHECK(l.server.close_reason() == CloseReason::PeerTimeout);
  CHECK(t == 26 * kSec / 10);  // arrival is attributed to the first tick (0.1 s) + 2.5 s

  Link e;
  e.connect_and_login(0);
  REQUIRE(e.server.send_sequenced(to_bytes("last")));
  REQUIRE(e.server.end_session());
  e.pump_client();
  CHECK(e.sequenced.size() == 1);
  CHECK(e.client.end_of_session());
  CHECK(e.client.close_reason() == CloseReason::EndOfSession);
  e.server.on_connect(kSec);
  REQUIRE(e.client.login(kSec));
  e.pump();
  CHECK(e.client.reject_code() == kRejectSessionNotAvailable);
}
