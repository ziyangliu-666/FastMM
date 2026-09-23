// nasdaq_itch without a network: [venues.<name>] keys, the recovery buffer, order_entry = "none"
// and the API-key check of resolve_venue_env. The feed itself runs against fastmm-sim-itch in
// tests/integration/nasdaq_itch_venue_test.cpp.
#include "fastmm/venues/nasdaq/nasdaq_itch_venue.hpp"
#include "fastmm/venues/nasdaq/recovery_buffer.hpp"
#include "fastmm/venues/registry.hpp"

#include <doctest/doctest.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::nasdaq;

namespace {

VenueSection section(std::initializer_list<std::pair<const char*, const char*>> kv) {
  VenueSection v;
  v.name = "itch";
  v.kind = "nasdaq_itch";
  for (const auto& [k, val] : kv) v.extra[k] = val;
  return v;
}

std::span<const std::byte> bytes(std::string_view s) {
  return std::as_bytes(std::span<const char>(s.data(), s.size()));
}

}  // namespace

TEST_CASE("venues.nasdaq_itch: config keys and defaults") {
  const NasdaqItchVenueConfig c =
      make_nasdaq_itch_config(section({{"line_a", "239.1.1.1:31001"}}), false, false);
  CHECK(c.rx_backend == RxBackend::Kernel);
  CHECK(c.lines[0].group == "239.1.1.1");
  CHECK(c.lines[0].port == 31001);
  CHECK(c.lines[1].group.empty());
  CHECK(c.reorder_packets == 256);
  CHECK(c.gap_timeout_ns == 2'000'000);
  CHECK(c.max_request_attempts == 4);
  CHECK(c.recovery_buffer_packets == 65536);
  CHECK(c.depth == 20);
  CHECK(c.order_entry == OrderEntry::None);
  CHECK(c.hw_clock == HwClock::None);
  CHECK(c.glimpse_url.empty());

  const NasdaqItchVenueConfig x =
      make_nasdaq_itch_config(section({{"rx_backend", "af_xdp"},
                                       {"interface", "eth1"},
                                       {"line_a", "233.54.12.111:26477"},
                                       {"line_b", "233.49.196.111:26477"},
                                       {"line_b_interface", "eth2"},
                                       {"line_a_source", "10.0.0.1"},
                                       {"queues", "[2, 3]"},
                                       {"xdp_mode", "native_copy"},
                                       {"batch", "64"},
                                       {"rerequest", "10.0.0.5:18000"},
                                       {"glimpse_url", "10.0.0.6:18001"},
                                       {"glimpse_username", "user01"},
                                       {"reorder_packets", "1024"},
                                       {"gap_timeout_ns", "500000"},
                                       {"max_request_attempts", "8"},
                                       {"recovery_buffer_packets", "4096"},
                                       {"depth", "5"},
                                       {"price_window_ticks", "4096"},
                                       {"max_orders", "100000"},
                                       {"hw_timestamps", "true"},
                                       {"hw_clock", "phc_synced"},
                                       {"order_entry", "sim_ouch"},
                                       {"ouch_url", "10.0.0.6:18002"}}),
                              false,
                              true);
  CHECK(x.rx_backend == RxBackend::AfXdp);
  CHECK(x.lines[0].interface == "eth1");
  CHECK(x.lines[1].interface == "eth2");
  CHECK(x.lines[0].source == "10.0.0.1");
  CHECK(x.queues == std::vector<std::uint32_t>{2, 3});
  CHECK(x.xdp_mode == net::XdpMode::NativeCopy);
  CHECK(x.batch == 64);
  CHECK(x.busy_poll);
  CHECK(x.glimpse_username == "user01");
  CHECK(x.reorder_packets == 1024);
  CHECK(x.gap_timeout_ns == 500'000);
  CHECK(x.recovery_buffer_packets == 4096);
  CHECK(x.depth == 5);
  CHECK(x.price_window_ticks == 4096);
  CHECK(x.max_orders == 100'000);
  CHECK(x.hw_timestamps);
  CHECK(x.hw_clock == HwClock::PhcSynced);
  CHECK(x.order_entry == OrderEntry::SimOuch);
  // --dry-run: no order entry whatever the file says.
  const NasdaqItchVenueConfig d = make_nasdaq_itch_config(
      section({{"line_a", "239.1.1.1:1"}, {"order_entry", "sim_ouch"}, {"ouch_url", "1.2.3.4:5"}}),
      true,
      false);
  CHECK(d.order_entry == OrderEntry::None);
}

TEST_CASE("venues.nasdaq_itch: dpdk backend and user_tcp order transport keys") {
  const NasdaqItchVenueConfig c = make_nasdaq_itch_config(
      section({{"rx_backend", "dpdk"},
               {"interface", "eth1"},
               {"line_a", "239.1.1.1:31001"},
               {"dpdk_eal_args", "--no-huge --vdev=net_af_packet0,iface=eth1"},
               {"dpdk_port", "net_af_packet0"},
               {"order_entry", "sim_ouch"},
               {"ouch_url", "10.0.0.6:18002"},
               {"order_transport", "user_tcp"},
               {"user_tcp_ip", "10.0.0.9"},
               {"user_tcp_gateway", "10.0.0.1"}}),
      false,
      true);
  CHECK(c.rx_backend == RxBackend::Dpdk);
  CHECK(c.dpdk_eal_args == "--no-huge --vdev=net_af_packet0,iface=eth1");
  CHECK(c.dpdk_port == "net_af_packet0");
  CHECK(c.order_transport == OrderTransport::UserTcp);
  CHECK(c.user_tcp_interface == "eth1");
  CHECK(c.user_tcp_ip == "10.0.0.9");
  CHECK(c.user_tcp_gateway == "10.0.0.1");
  const NasdaqItchVenueConfig k =
      make_nasdaq_itch_config(section({{"line_a", "239.1.1.1:31001"}}), false, false);
  CHECK(k.order_transport == OrderTransport::Kernel);
  // Without DPDK in the build the source says so at open.
  if (!net::dpdk_available()) {
    net::DpdkDatagramSource src;
    CHECK(src.open(net::DpdkConfig{}) == -ENOTSUP);
    CHECK(src.error().find("FASTMM_WITH_DPDK") != std::string::npos);
  }
}

TEST_CASE("venues.nasdaq_itch: bad config values are refused with the key") {
  const auto refused = [](std::initializer_list<std::pair<const char*, const char*>> kv,
                          const char* key) {
    try {
      static_cast<void>(make_nasdaq_itch_config(section(kv), false, false));
    } catch (const std::invalid_argument& e) {
      const std::string what = e.what();
      INFO(what);
      CHECK(what.find(std::string("venues.itch.") + key) != std::string::npos);
      return;
    }
    FAIL("accepted: " << key);
  };
  refused({}, "line_a");
  refused({{"line_a", "0.0.0.0:31001"}}, "line_a");  // no address (unicast is fine)
  refused({{"line_a", "239.1.1.1"}}, "line_a");
  refused({{"line_a", "239.1.1.1:1"}, {"rx_backend", "dpdk"}}, "rx_backend");
  refused({{"line_a", "239.1.1.1:1"}, {"rx_backend", "af_xdp"}}, "line_a_interface");
  refused({{"line_a", "239.1.1.1:1"}, {"xdp_mode", "fast"}}, "xdp_mode");
  refused({{"line_a", "239.1.1.1:1"}, {"queues", "0,x"}}, "queues");
  refused({{"line_a", "239.1.1.1:1"}, {"rerequest", "localhost"}}, "rerequest");
  refused({{"line_a", "239.1.1.1:1"}, {"glimpse_url", "host:1"}}, "glimpse_url");
  refused({{"line_a", "239.1.1.1:1"}, {"depth", "0"}}, "depth");
  refused({{"line_a", "239.1.1.1:1"}, {"price_window_ticks", "100"}}, "price_window_ticks");
  refused({{"line_a", "239.1.1.1:1"}, {"hw_clock", "tai"}}, "hw_clock");
  refused({{"line_a", "239.1.1.1:1"}, {"order_entry", "ouch"}}, "order_entry");
  refused({{"line_a", "239.1.1.1:1"}, {"order_entry", "sim_ouch"}}, "ouch_url");
  refused({{"line_a", "239.1.1.1:1"}, {"glimpse_username", "toolong"}}, "glimpse_username");
  refused({{"line_a", "239.1.1.1:1"}, {"hw_timestamps", "yes"}}, "hw_timestamps");
  refused({{"line_a", "239.1.1.1:1"}, {"rx_backend", "rdma"}}, "rx_backend");
  refused({{"line_a", "239.1.1.1:1"}, {"order_transport", "onload"}}, "order_transport");
  refused({{"line_a", "239.1.1.1:1"}, {"order_transport", "user_tcp"}}, "user_tcp_interface");
  refused({{"line_a", "239.1.1.1:1"}, {"interface", "eth1"}, {"order_transport", "user_tcp"}},
          "user_tcp_ip");
  refused({{"line_a", "239.1.1.1:1"},
           {"interface", "eth1"},
           {"order_transport", "user_tcp"},
           {"user_tcp_ip", "10.0.0.9"},
           {"user_tcp_gateway", "gw"}},
          "user_tcp_gateway");

  net::SockAddr a;
  CHECK(parse_ip_port("127.0.0.1:31000", a));
  CHECK(a.port() == 31000);
  CHECK_FALSE(parse_ip_port("127.0.0.1", a));
  CHECK_FALSE(parse_ip_port("127.0.0.1:0", a));
  CHECK_FALSE(parse_ip_port("127.0.0.1:70000", a));
  CHECK_FALSE(parse_ip_port("[::1]:5", a));
  register_builtin_venues();
  REQUIRE(VenueRegistry::instance().find("nasdaq_itch") != nullptr);
}

TEST_CASE("venues.nasdaq_itch: the recovery buffer groups messages by datagram and fills up") {
  RecoveryBuffer b;
  b.allocate(2, 64);
  CHECK(b.empty());
  ItchRxMeta m1;
  m1.datagram = 1;
  m1.t0_cycles = Cycles{111};
  ItchRxMeta m2;
  m2.datagram = 2;
  m2.t0_cycles = Cycles{222};
  REQUIRE(b.append(10, bytes("a"), m1));
  REQUIRE(b.append(11, bytes("bb"), m1));
  REQUIRE(b.append(12, bytes(""), m2));
  CHECK(b.packets() == 2);
  CHECK(b.messages() == 3);
  CHECK(b.first_seq() == 10);
  ItchRxMeta m3;
  m3.datagram = 3;
  CHECK_FALSE(b.append(13, bytes("c"), m3));  // a third datagram does not fit
  CHECK(b.append(13, bytes("c"), m2));        // but the open one still takes messages
  CHECK_FALSE(b.append(14, bytes(std::string(200, 'x')), m2));  // bytes exhausted

  std::vector<std::pair<std::uint64_t, std::string>> seen;
  std::vector<std::uint64_t> t0s;
  int ends = 0;
  b.for_each(
      [&](std::uint64_t seq, std::span<const std::byte> msg, const ItchRxMeta& meta) {
        seen.emplace_back(seq, std::string(reinterpret_cast<const char*>(msg.data()), msg.size()));
        t0s.push_back(meta.t0_cycles.v);
      },
      [&] { ++ends; });
  REQUIRE(seen.size() == 4);
  CHECK(seen[0] == std::pair<std::uint64_t, std::string>{10, "a"});
  CHECK(seen[1] == std::pair<std::uint64_t, std::string>{11, "bb"});
  CHECK(seen[2] == std::pair<std::uint64_t, std::string>{12, ""});
  CHECK(seen[3] == std::pair<std::uint64_t, std::string>{13, "c"});
  CHECK(t0s == std::vector<std::uint64_t>{111, 111, 222, 222});
  CHECK(ends == 2);
  b.clear();
  CHECK(b.empty());
  CHECK(b.first_seq() == 0);
  CHECK(b.append(20, bytes("z"), m3));
}

TEST_CASE("venues.nasdaq_itch: order_entry none rejects every order with VenueReject") {
  NasdaqItchVenueConfig cfg;
  cfg.lines[0] = ItchLine{"lo", "239.1.1.1", 31001, ""};
  NasdaqItchVenue venue(VenueId{0}, cfg);
  CHECK_FALSE(venue.caps().supports_replace);
  InstrumentTable instruments;
  Instrument inst;
  inst.venue = VenueId{0};
  inst.symbol.assign("FMAA");
  inst.tick = Price::from_decimal("0.01").value();
  inst.lot = Qty::from_int(1);
  REQUIRE(instruments.add(inst));
  SymbolTable symbols;
  REQUIRE(symbols.build(instruments));
  MsgRing md(1U << 16);
  MsgRing orders(1U << 16);
  MsgRing outbound(1U << 16);
  EventSink md_sink(&md, SinkPolicy::Drop);
  EventSink order_sink(&orders, SinkPolicy::Spin);
  venue.attach(symbols, instruments, md_sink, order_sink, &outbound);
  const InstrumentId ids[] = {InstrumentId{0}};
  venue.subscribe(ids);
  CHECK(venue.status().books_total == 1);

  OutNewOrderMsg m{};
  init_header(m, EventType::OutNewOrder, InstrumentId{0}, VenueId{0});
  m.cl_ord_id = ClientOrderId{7};
  m.price = Price::from_int(100);
  m.qty = Qty::from_int(10);
  REQUIRE(outbound.try_push(&m, m.hdr.len));
  venue.on_wake();
  const std::byte* p = orders.try_peek();
  REQUIRE(p != nullptr);
  const auto* h = reinterpret_cast<const EventHeader*>(p);
  REQUIRE(h->type == EventType::OrderReject);
  const auto& r = msg_cast<OrderRejectMsg>(h);
  CHECK(r.cl_ord_id == ClientOrderId{7});
  CHECK(r.reason == RejectReason::VenueReject);
  CHECK(r.text.view() == "order_entry = none");
  orders.release();
  CHECK(venue.cancel_all());
  CHECK(venue.feed_state() == FeedState::Down);
}
