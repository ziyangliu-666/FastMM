// The Nasdaq codecs' hot paths do not allocate after construction: ITCH decode, MoldUDP64
// framing, A/B arbitration, the reorder buffer and gap recovery, SoupBinTCP framing and session
// traffic both ways (including heartbeats), OUCH 4.2 / 5.0 encode and decode (including order-table
// inserts and erases).
#include "alloc_counter.hpp"
#include "test_support.hpp"

#include "fastmm/codecs/itch/itch_decoder.hpp"
#include "fastmm/codecs/itch/itch_encoder.hpp"
#include "fastmm/codecs/moldudp/moldudp64.hpp"
#include "fastmm/codecs/ouch/ouch42.hpp"
#include "fastmm/codecs/ouch/ouch50.hpp"
#include "fastmm/codecs/soupbin/soupbin.hpp"
#include "fastmm/codecs/soupbin/soupbin_session.hpp"
#include "fastmm/core/msg_ring.hpp"

#include <array>
#include <cstring>
#include <memory>
#include <vector>

using namespace fastmm;
using namespace fastmm::codecs;
using fastmm::test::NoAllocScope;

namespace {
struct FixedWriter {
  std::array<std::byte, 1U << 16> buf{};
  std::size_t size = 0;
  bool send(std::span<const std::byte> b) noexcept {
    if (buf.size() - size < b.size()) size = 0;
    std::memcpy(buf.data() + size, b.data(), b.size());
    size += b.size();
    return true;
  }
  [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return {buf.data(), size}; }
};
struct NullApp {
  std::uint64_t n = 0;
  void on_unsequenced(std::span<const std::byte>) noexcept { ++n; }
};
struct MoldMeta {
  std::int64_t t0 = 0;
};
struct MoldHandler {
  std::uint64_t messages = 0;
  std::uint64_t requests = 0;
  std::int64_t last_t0 = 0;
  void on_message(std::uint64_t, std::span<const std::byte>, const MoldMeta& m) noexcept {
    ++messages;
    last_t0 = m.t0;
  }
  void send_request(std::span<const std::byte>) noexcept { ++requests; }
  void on_end_of_session() noexcept {}
};
void drain(MsgRing& ring) noexcept {
  while (ring.try_peek() != nullptr) ring.release();
}
// Hands every complete packet in `w` to `on`, then empties the writer.
template <class F>
void deliver(FixedWriter& w, F&& on) {
  soupbin::SoupBinFramer framer;
  std::span<const std::byte> rest = w.bytes();
  for (FrameView v = framer.next(rest); v.complete(); v = framer.next(rest)) {
    on(v);
    rest = rest.subspan(v.consumed);
  }
  w.size = 0;
}
}  // namespace

TEST_CASE("hotpath.noalloc: Nasdaq codecs (ITCH, MoldUDP64, SoupBinTCP, OUCH 4.2 and 5.0)") {
  auto ring = std::make_unique<MsgRing>(1U << 20);
  venues::EventSink sink(ring.get(), venues::SinkPolicy::Drop);

  // ITCH messages (built before the guard).
  auto itch_dec = std::make_unique<itch::ItchDecoder>();
  itch::ItchEncoder itch_enc;
  const Price p = Price::from_decimal("100.25").value();
  const Qty q = Qty::from_int(100);
  std::vector<std::array<std::byte, 64>> itch_msgs(8);
  std::vector<std::size_t> itch_len(8);
  itch_len[0] = itch_enc.stock_directory(itch_msgs[0], 7, 1, "FMHOT");
  itch_len[1] = itch_enc.add_order(itch_msgs[1], 7, 2, 1, Side::Buy, q, "FMHOT", p);
  itch_len[2] = itch_enc.order_executed(itch_msgs[2], 7, 3, 1, Qty::from_int(10), 9);
  itch_len[3] =
      itch_enc.order_executed_with_price(itch_msgs[3], 7, 4, 1, Qty::from_int(10), 10, p, true);
  itch_len[4] = itch_enc.order_cancel(itch_msgs[4], 7, 5, 1, Qty::from_int(5));
  itch_len[5] = itch_enc.order_replace(itch_msgs[5], 7, 6, 1, 2, q, p);
  itch_len[6] = itch_enc.order_delete(itch_msgs[6], 7, 7, 2);
  itch_len[7] = itch_enc.trade(itch_msgs[7], 7, 8, Side::Buy, q, "FMHOT", p, 11);
  REQUIRE(itch_dec->add_symbol("FMHOT", InstrumentId{0}));

  // MoldUDP64: 30 messages per iteration on lines A and B; 11..20 of each iteration are lost
  // on both and recovered from a retransmission on line 2.
  moldudp::TransmitterConfig tc;
  tc.history_bytes = 1U << 23;
  tc.history_messages = 1U << 18;
  auto tx = std::make_unique<moldudp::Transmitter>("HOTPATH", tc);
  for (int i = 0; i < 5'000 * 30; ++i)
    REQUIRE(tx->publish(std::span<const std::byte>(itch_msgs[1].data(), itch_len[1])) != 0);
  std::array<std::byte, 1500> d1{};
  std::array<std::byte, 1500> d2{};
  std::array<std::byte, 1500> d3{};
  std::array<std::byte, 20> req{};
  MoldHandler mh;
  moldudp::ReceiverConfig rc;
  rc.next_sequence = 1;
  rc.gap_timeout_ns = 50'000;
  auto rx = std::make_unique<moldudp::Receiver<MoldHandler, MoldMeta>>(mh, rc);

  // SoupBinTCP sessions over fixed buffers, logged in before the guard.
  auto c2s = std::make_unique<FixedWriter>();
  auto s2c = std::make_unique<FixedWriter>();
  NullApp app;
  soupbin::ClientConfig cc;
  cc.username = "u";
  cc.password = "p";
  soupbin::ServerConfig sc;
  sc.username = "u";
  sc.password = "p";
  sc.session = "S1";
  sc.history_bytes = 1U << 24;
  sc.history_messages = 1U << 18;
  auto server = std::make_unique<soupbin::ServerSession<FixedWriter, NullApp>>(*s2c, app, sc);
  auto client = std::make_unique<soupbin::ClientSession<FixedWriter>>(*c2s, cc);
  server->on_connect(0);
  REQUIRE(client->login(0));
  deliver(*c2s, [&](const FrameView& v) { server->on_frame(v); });
  deliver(*s2c, [&](const FrameView& v) { client->on_frame(v); });
  REQUIRE(client->state() == SessionState::Up);
  REQUIRE(server->state() == SessionState::Up);

  // OUCH.
  auto enc42 = std::make_unique<ouch42::OuchEncoder>();
  auto dec42 = std::make_unique<ouch42::OuchDecoder>();
  auto ids = std::make_unique<ouch50::UserRefMap>();
  auto enc50 = std::make_unique<ouch50::OuchEncoder>(*ids);
  auto dec50 = std::make_unique<ouch50::OuchDecoder>(ids.get());
  REQUIRE(enc42->add_symbol("FMHOT", InstrumentId{0}));
  REQUIRE(enc50->add_symbol("FMHOT", InstrumentId{0}));
  std::array<std::byte, 128> in{};
  std::array<std::byte, 128> out{};
  std::uint64_t events = 0;
  std::uint64_t app_messages = 0;

  {
    NoAllocScope guard;
    for (std::uint32_t i = 1; i <= 5'000; ++i) {
      for (std::size_t k = 0; k < itch_msgs.size(); ++k) {
        const FrameView f{
            std::span<const std::byte>(itch_msgs[k].data(), itch_len[k]), itch_len[k], 0};
        if (itch_dec->decode(f, 0, sink) == venues::ParseStatus::Ok) ++events;
      }
      drain(*ring);

      const std::uint64_t base = (i - 1) * 30ULL;
      const std::int64_t t = static_cast<std::int64_t>(i) * 1'000'000;
      const std::size_t n1 = tx->packet_at(d1, base + 1, 10);
      const std::size_t n2 = tx->packet_at(d2, base + 21, 10);
      rx->on_packet(0, std::span<const std::byte>(d1.data(), n1), t, MoldMeta{t});
      rx->on_packet(1, std::span<const std::byte>(d1.data(), n1), t + 1'000, MoldMeta{t});
      rx->on_packet(0, std::span<const std::byte>(d2.data(), n2), t + 2'000, MoldMeta{t});
      rx->on_packet(1, std::span<const std::byte>(d2.data(), n2), t + 3'000, MoldMeta{t});
      rx->on_timer(t + 52'000);  // gap declared: request
      moldudp::write_request(req, tx->session(), base + 11, 10);
      const std::size_t n3 = tx->answer_request(req, d3);
      rx->on_packet(2, std::span<const std::byte>(d3.data(), n3), t + 60'000, MoldMeta{t});

      // OUCH 4.2 over SoupBinTCP: client Unsequenced -> server, server Sequenced -> client.
      venues::OrderCommand cmd;
      cmd.kind = venues::OrderCommandKind::New;
      cmd.cl_ord_id = make_cl_ord_id(1, i);
      cmd.instrument = InstrumentId{0};
      cmd.price = p;
      cmd.qty = q;
      std::size_t n = enc42->encode(cmd, in);
      client->send(std::span<const std::byte>(in.data(), n));
      deliver(*c2s, [&](const FrameView& v) { server->on_frame(v); });
      const ouch42::EnterOrder eo = ouch::view_as<ouch42::EnterOrder>(in.data());
      n = ouch42::host::accepted(out, i, eo, 100, i, 'L');
      server->send_sequenced(std::span<const std::byte>(out.data(), n));
      n = ouch42::host::executed(out, i, eo.order_token, 100, 1'002'500, 'A', i);
      server->send_sequenced(std::span<const std::byte>(out.data(), n));
      deliver(*s2c, [&](const FrameView& v) {
        if (!client->on_frame(v)) {
          ++app_messages;
          if (dec42->decode(FrameView{v.payload, v.payload.size(), 0}, 0, sink) ==
              venues::ParseStatus::Ok)
            ++events;
        }
      });
      cmd.kind = venues::OrderCommandKind::Cancel;
      n = enc42->encode(cmd, in);

      // OUCH 5.0 (UserRefNum assigned, then forgotten when the order is done).
      cmd.kind = venues::OrderCommandKind::New;
      n = enc50->encode(cmd, in);
      const ouch50::EnterOrder e50 = ouch::view_as<ouch50::EnterOrder>(in.data());
      n = ouch50::host::accepted(out, i, e50, 100, i, 'L');
      if (dec50->decode(FrameView{{out.data(), n}, n, 0}, 0, sink) == venues::ParseStatus::Ok)
        ++events;
      cmd.kind = venues::OrderCommandKind::Cancel;
      n = enc50->encode(cmd, in);
      n = ouch50::host::canceled(out, i, e50.user_ref_num.get(), 100, 'U');
      if (dec50->decode(FrameView{{out.data(), n}, n, 0}, 0, sink) == venues::ParseStatus::Ok)
        ++events;
      drain(*ring);

      const auto now =
          static_cast<std::int64_t>(i) * 1'000'000;  // heartbeats every 1000 iterations
      client->on_timer(now);
      server->on_timer(now);
      deliver(*c2s, [&](const FrameView& v) { server->on_frame(v); });
      deliver(*s2c, [&](const FrameView& v) { client->on_frame(v); });
    }
    // An idle link: both sides heartbeat (spec 1.3) and keep each other alive.
    for (std::int64_t t = 5'100'000'000; t <= 10'000'000'000; t += 100'000'000) {
      client->on_timer(t);
      server->on_timer(t);
      deliver(*c2s, [&](const FrameView& v) { server->on_frame(v); });
      deliver(*s2c, [&](const FrameView& v) { client->on_frame(v); });
    }
    CHECK(guard.allocations_so_far() == 0);
  }
  CHECK(events == 55'000);  // 7 ITCH + 2 OUCH 4.2 + 2 OUCH 5.0 events per iteration
  CHECK(app_messages == 10'000);
  CHECK(mh.messages == 5'000 * 30);
  CHECK(mh.requests == 5'000);
  CHECK(rx->stats().lines[1].duplicate_packets == 10'000);
  CHECK(rx->stats().held_packets == 5'000);
  CHECK(client->state() == SessionState::Up);
  CHECK(client->stats().heartbeats_sent >= 4);
  CHECK(server->stats().heartbeats_sent >= 4);
  CHECK(app.n == 5'000);
  CHECK(dec42->open_orders() == 0);
  CHECK(dec50->open_orders() == 0);
  CHECK(ids->size() == 0);
}
