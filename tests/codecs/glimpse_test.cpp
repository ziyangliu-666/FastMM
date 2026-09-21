// GLIMPSE 5.0: End of Snapshot and the GlimpseClient session over a SoupBinTCP ServerSession.
#include "fastmm/codecs/itch/glimpse.hpp"

#include "nasdaq_test_util.hpp"

#include "fastmm/codecs/itch/itch_encoder.hpp"
#include "fastmm/codecs/soupbin/soupbin_session.hpp"

#include <array>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::codecs;
using namespace fastmm::codecs::itch::glimpse;
using fastmm::codecs::test::BytePipe;
using fastmm::codecs::test::Bytes;
using fastmm::codecs::test::span_of;
using fastmm::codecs::test::to_bytes;

namespace {

struct Snapshot {
  std::vector<Bytes> messages;
  std::uint64_t end = 0;
  int ends = 0;
  void on_snapshot_message(std::span<const std::byte> m) noexcept {
    messages.emplace_back(m.begin(), m.end());
  }
  void on_snapshot_end(std::uint64_t seq) noexcept {
    end = seq;
    ++ends;
  }
};

struct NoInbound {
  void on_unsequenced(std::span<const std::byte>) noexcept {}
};

}  // namespace

TEST_CASE("codecs.glimpse: End of Snapshot carries a 20-character sequence number") {
  std::array<std::byte, 32> buf{};
  REQUIRE(write_end_of_snapshot(buf, 123456789) == kEndOfSnapshotLength);
  CHECK(fastmm::codecs::test::to_string(std::span<const std::byte>(
            buf.data(), kEndOfSnapshotLength)) == "G           123456789");
  std::uint64_t seq = 0;
  REQUIRE(parse_end_of_snapshot(std::span<const std::byte>(buf.data(), kEndOfSnapshotLength), seq));
  CHECK(seq == 123456789);
  CHECK(parse_end_of_snapshot(span_of(to_bytes("G00000000000000000042")), seq));
  CHECK(seq == 42);
  CHECK(parse_end_of_snapshot(span_of(to_bytes("G42                  ")), seq));
  CHECK(seq == 42);
  CHECK_FALSE(parse_end_of_snapshot(span_of(to_bytes("G0000000000000000042")), seq));   // 20 bytes
  CHECK_FALSE(parse_end_of_snapshot(span_of(to_bytes("H00000000000000000042")), seq));  // type
  CHECK_FALSE(parse_end_of_snapshot(span_of(to_bytes("G0000000000000000004x")), seq));
  CHECK(write_end_of_snapshot(std::span<std::byte>(buf.data(), 20), 1) == 0);
}

TEST_CASE("codecs.glimpse: the client logs in for sequence 1 and hands over the snapshot") {
  BytePipe c2s;
  BytePipe s2c;
  NoInbound inbound;
  soupbin::ServerConfig scfg;
  scfg.username = "glimps";
  scfg.password = "pw";
  scfg.session = "GLIMPSE";
  soupbin::ServerSession<BytePipe, NoInbound> server(s2c, inbound, scfg);
  server.on_connect(0);

  Snapshot snap;
  GlimpseConfig gcfg;
  gcfg.session.username = "glimps";
  gcfg.session.password = "pw";
  gcfg.session.sequence = 99;  // ignored: GLIMPSE is always read from sequence 1
  GlimpseClient<BytePipe, Snapshot> client(c2s, snap, gcfg);
  REQUIRE(client.state() == GlimpseState::Idle);
  REQUIRE(client.login(0));
  CHECK(client.state() == GlimpseState::LoggingIn);

  auto to_server = [&] {
    const soupbin::SoupBinFramer f;
    for (FrameView v = f.next(c2s.pending()); v.complete(); v = f.next(c2s.pending())) {
      server.on_frame(v);
      c2s.consume(v.consumed);
    }
  };
  to_server();
  REQUIRE(server.state() == SessionState::Up);

  itch::ItchEncoder enc;
  std::array<std::byte, 64> buf{};
  auto send = [&](std::size_t n) {
    REQUIRE(n != 0);
    REQUIRE(server.send_sequenced(std::span<const std::byte>(buf.data(), n)));
  };
  send(enc.stock_directory(buf, 1, 10, "FMAA"));
  send(enc.add_order(buf, 1, 11, 7, Side::Buy, Qty::from_int(100), "FMAA", Price::from_int(10)));
  send(enc.add_order(buf, 1, 12, 8, Side::Sell, Qty::from_int(50), "FMAA", Price::from_int(11)));

  // Deliver the stream in two pieces split inside a packet.
  const Bytes all(s2c.pending().begin(), s2c.pending().end());
  s2c.drop_all();
  std::size_t used = client.on_bytes(std::span<const std::byte>(all.data(), 40));
  CHECK(client.state() == GlimpseState::Receiving);
  used += client.on_bytes(std::span<const std::byte>(all.data() + used, all.size() - used));
  CHECK(used == all.size());
  REQUIRE(snap.messages.size() == 3);
  CHECK(static_cast<char>(snap.messages[0][0]) == 'R');
  CHECK(static_cast<char>(snap.messages[2][0]) == 'A');
  CHECK(snap.ends == 0);

  send(write_end_of_snapshot(buf, 5001));
  const Bytes tail(s2c.pending().begin(), s2c.pending().end());
  s2c.drop_all();
  CHECK(client.on_bytes(span_of(tail)) == tail.size());
  CHECK(client.complete());
  CHECK(client.end_sequence() == 5001);
  CHECK(snap.end == 5001);
  CHECK(snap.ends == 1);
  CHECK(client.stats().messages == 3);
  // logout_after_snapshot: the server sees the Logout Request.
  to_server();
  CHECK(server.state() == SessionState::Down);
  CHECK(server.close_reason() == soupbin::CloseReason::Logout);
}

TEST_CASE("codecs.glimpse: a rejected login or a lost connection leaves the client idle") {
  BytePipe c2s;
  BytePipe s2c;
  NoInbound inbound;
  soupbin::ServerConfig scfg;
  scfg.username = "glimps";
  scfg.password = "right";
  soupbin::ServerSession<BytePipe, NoInbound> server(s2c, inbound, scfg);
  server.on_connect(0);
  Snapshot snap;
  GlimpseConfig gcfg;
  gcfg.session.username = "glimps";
  gcfg.session.password = "wrong";
  GlimpseClient<BytePipe, Snapshot> client(c2s, snap, gcfg);
  REQUIRE(client.login(0));
  const soupbin::SoupBinFramer f;
  const FrameView v = f.next(c2s.pending());
  REQUIRE(v.complete());
  server.on_frame(v);
  const Bytes reply(s2c.pending().begin(), s2c.pending().end());
  client.on_bytes(span_of(reply));
  CHECK(client.state() == GlimpseState::Idle);
  CHECK(client.close_reason() == soupbin::CloseReason::LoginRejected);

  GlimpseClient<BytePipe, Snapshot> other(c2s, snap, gcfg);
  REQUIRE(other.login(0));
  other.on_disconnect();
  CHECK(other.state() == GlimpseState::Idle);
  CHECK(other.close_reason() == soupbin::CloseReason::Disconnected);
}
