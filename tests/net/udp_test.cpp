#include "net_test_util.hpp"
#include "netns_test_util.hpp"

#include "fastmm/net/kernel_datagram_source.hpp"
#include "fastmm/net/udp_socket.hpp"

#include <arpa/inet.h>
#include <net/if.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::net;
using namespace fastmm::net::test;

namespace {

std::uint32_t ip(const char* text) {
  std::uint32_t v = 0;
  REQUIRE(parse_ipv4(text, v));
  return v;
}

SockAddr addr(const char* text, std::uint16_t port) {
  auto a = SockAddr::from_ip(text, port);
  REQUIRE(a.has_value());
  return *a;
}

// Multicast sender on lo, bound to `src` so the receiver sees that source address.
UdpSocket mcast_sender(const char* src = "127.0.0.1") {
  UdpSocket s = UdpSocket::open();
  REQUIRE(s.valid());
  REQUIRE(s.bind(addr(src, 0)) == 0);
  McastInterface lo;
  REQUIRE(McastInterface::resolve("lo", lo) == 0);
  REQUIRE(s.set_multicast_if(lo) == 0);
  REQUIRE(s.set_multicast_ttl(1) == 0);
  REQUIRE(s.set_multicast_loop(true) == 0);
  return s;
}

void send(UdpSocket& s, const char* group, std::uint16_t port, std::string_view payload) {
  const IoResult r = s.send_to(bytes(payload), addr(group, port));
  REQUIRE(r.ok());
  REQUIRE(r.bytes == payload.size());
}

struct Received {
  std::string payload;
  RxMeta meta;
};

// Polls until `want` datagrams arrived or `timeout_ms` passed.
std::vector<Received> drain(KernelDatagramSource& src,
                            std::size_t want,
                            std::vector<std::size_t>* poll_sizes = nullptr,
                            int timeout_ms = 2000) {
  std::vector<Received> out;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (out.size() < want && std::chrono::steady_clock::now() < deadline) {
    const std::size_t n = src.poll([&](std::span<const std::byte> p, const RxMeta& m) noexcept {
      out.push_back(Received{std::string(sv(p)), m});
    });
    if (n > 0 && poll_sizes != nullptr) poll_sizes->push_back(n);
  }
  return out;
}

KernelSourceConfig two_line_config() {
  KernelSourceConfig cfg;
  // Line A by interface name, line B by interface address; both on one port.
  cfg.subscriptions = {
      {.interface = "lo", .group = "239.1.1.1", .port = 30001, .source = ""},
      {.interface = "127.0.0.1", .group = "239.1.1.2", .port = 30001, .source = ""}};
  return cfg;
}

}  // namespace

TEST_CASE("UdpSocket: parse_ipv4 and McastInterface::resolve") {
  std::uint32_t v = 0;
  CHECK(parse_ipv4("239.1.2.3", v));
  CHECK(v == htonl(0xEF010203U));
  CHECK(is_ipv4_multicast(v));
  CHECK(parse_ipv4("224.0.0.1", v));
  CHECK(is_ipv4_multicast(v));
  CHECK(parse_ipv4("10.0.0.1", v));
  CHECK_FALSE(is_ipv4_multicast(v));
  CHECK_FALSE(parse_ipv4("", v));
  CHECK_FALSE(parse_ipv4("239.1.2", v));
  CHECK_FALSE(parse_ipv4("::1", v));
  CHECK_FALSE(parse_ipv4("239.1.2.3.4.5.6.7.8.9", v));

  McastInterface ifc;
  CHECK(McastInterface::resolve("", ifc) == 0);
  CHECK(ifc.addr_be == 0);
  CHECK(ifc.index == 0);
  CHECK(McastInterface::resolve("127.0.0.1", ifc) == 0);
  CHECK(ifc.addr_be == htonl(INADDR_LOOPBACK));
  CHECK(ifc.index == 0);
  CHECK(McastInterface::resolve("lo", ifc) == 0);
  CHECK(ifc.index == ::if_nametoindex("lo"));
  CHECK(ifc.addr_be == 0);
  CHECK(McastInterface::resolve("nosuchif0", ifc) == -ENODEV);
  CHECK(McastInterface::resolve("a-name-longer-than-ifnamsiz", ifc) == -ENODEV);
}

TEST_CASE("UdpSocket: unicast send_to and recv_from report the full length") {
  UdpSocket rx = UdpSocket::open();
  REQUIRE(rx.valid());
  REQUIRE(rx.bind(SockAddr::loopback_v4(0)) == 0);
  const auto local = rx.local_addr();
  REQUIRE(local.has_value());
  UdpSocket tx = UdpSocket::open();
  REQUIRE(tx.valid());

  std::array<std::byte, 8> buf{};
  CHECK(rx.recv_from(buf).want_read);

  REQUIRE(tx.send_to(bytes("hello"), *local).bytes == 5);
  SockAddr from;
  IoResult r = rx.recv_from(buf, &from);
  REQUIRE(r.ok());
  CHECK(r.bytes == 5);
  CHECK(sv(std::span<const std::byte>(buf.data(), r.bytes)) == "hello");
  CHECK(from.to_string().starts_with("127.0.0.1:"));

  // Longer than the buffer: the rest is discarded and `bytes` is the datagram's length.
  REQUIRE(tx.send_to(bytes("0123456789abcdef"), *local).bytes == 16);
  r = rx.recv_from(buf);
  CHECK(r.bytes == 16);
  CHECK(sv(buf) == "01234567");

  // Connected send.
  REQUIRE(tx.connect(*local) == 0);
  REQUIRE(tx.send(bytes("xy")).bytes == 2);
  r = rx.recv_from(buf);
  CHECK(r.bytes == 2);

  UdpSocket moved(std::move(rx));
  CHECK_FALSE(rx.valid());  // NOLINT(bugprone-use-after-move)
  CHECK(moved.valid());
  CHECK(moved.recv_from(buf).want_read);
}

TEST_CASE("UdpSocket: send_batch and recv_batch") {
  UdpSocket rx = UdpSocket::open();
  REQUIRE(rx.bind(SockAddr::loopback_v4(0)) == 0);
  UdpSocket tx = UdpSocket::open();
  REQUIRE(tx.connect(*rx.local_addr()) == 0);

  constexpr std::size_t kN = 5;
  std::array<std::string, kN> payloads{"a", "bb", "ccc", "dddd", "eeeee"};
  std::array<iovec, kN> iov{};
  std::array<mmsghdr, kN> out{};
  for (std::size_t i = 0; i < kN; ++i) {
    iov[i] = {payloads[i].data(), payloads[i].size()};
    out[i].msg_hdr.msg_iov = &iov[i];
    out[i].msg_hdr.msg_iovlen = 1;
  }
  REQUIRE(tx.send_batch(out) == static_cast<int>(kN));

  std::array<std::array<char, 16>, 8> bufs{};
  std::array<iovec, 8> riov{};
  std::array<mmsghdr, 8> in{};
  for (std::size_t i = 0; i < in.size(); ++i) {
    riov[i] = {bufs[i].data(), bufs[i].size()};
    in[i].msg_hdr.msg_iov = &riov[i];
    in[i].msg_hdr.msg_iovlen = 1;
  }
  REQUIRE(rx.recv_batch(in) == static_cast<int>(kN));
  for (std::size_t i = 0; i < kN; ++i) {
    CHECK(std::string_view(bufs[i].data(), in[i].msg_len) == payloads[i]);
  }
  CHECK(rx.recv_batch(in) == 0);  // EAGAIN
  UdpSocket closed;
  CHECK(closed.recv_batch(in) == -EBADF);
  CHECK(closed.last_error() == EBADF);
}

TEST_CASE("UdpSocket: busy-poll setters report failure and change nothing") {
  UdpSocket s = UdpSocket::open();
  REQUIRE(s.valid());
  auto get = [&](int name) {
    int v = -1;
    socklen_t len = sizeof(v);
    return ::getsockopt(s.fd(), SOL_SOCKET, name, &v, &len) == 0 ? v : -1;
  };
  // Without CAP_NET_ADMIN: -EPERM. On kernels before 5.11 the last two are -ENOPROTOOPT.
  const int busy = s.set_busy_poll(50);
  CHECK((busy == 0 || busy == -EPERM));
  CHECK(get(SO_BUSY_POLL) == (busy == 0 ? 50 : 0));
  const int prefer = s.set_prefer_busy_poll(true);
  CHECK((prefer == 0 || prefer == -EPERM || prefer == -ENOPROTOOPT));
  if (prefer != 0) CHECK(s.last_error() == -prefer);
  const int budget = s.set_busy_poll_budget(64);
  CHECK((budget == 0 || budget == -EPERM || budget == -ENOPROTOOPT));
  MESSAGE("SO_BUSY_POLL " << busy << ", SO_PREFER_BUSY_POLL " << prefer << ", SO_BUSY_POLL_BUDGET "
                          << budget);
  // Lowering needs no capability.
  CHECK(s.set_busy_poll(0) == 0);
}

TEST_CASE("UdpSocket: enable_hw_timestamps reports an error without NIC support") {
  // lo has no PHC: -EOPNOTSUPP (or -EPERM without CAP_NET_ADMIN, -EINVAL on some kernels).
  const int rc = enable_hw_timestamps("lo");
  CHECK(rc < 0);
  CHECK(enable_hw_timestamps("nosuchif0") < 0);
  CHECK(enable_hw_timestamps("") == -ENODEV);
}

TEST_CASE("KernelDatagramSource: open rejects bad configuration") {
  KernelDatagramSource src;
  KernelSourceConfig cfg;
  KernelSourceOpen r = src.open(cfg);
  CHECK(r.err == -EINVAL);
  CHECK(r.step == "config");

  cfg.subscriptions = {{.interface = "", .group = "10.0.0.1", .port = 30001, .source = ""}};
  r = src.open(cfg);
  CHECK(r.err == -EINVAL);
  CHECK(r.step == "group");

  cfg.subscriptions = {{.interface = "", .group = "239.1.1.1", .port = 30001, .source = ""},
                       {.interface = "", .group = "239.1.1.2", .port = 0, .source = ""}};
  r = src.open(cfg);
  CHECK(r.step == "group");
  CHECK(r.line == 1);
  CHECK_FALSE(src.is_open());  // line 0 was opened and closed again

  cfg.subscriptions = {
      {.interface = "nosuchif0", .group = "239.1.1.1", .port = 30001, .source = ""}};
  r = src.open(cfg);
  CHECK(r.err == -ENODEV);
  CHECK(r.step == "interface");

  cfg.subscriptions = {{.interface = "", .group = "239.1.1.1", .port = 30001, .source = "x"}};
  r = src.open(cfg);
  CHECK(r.step == "source");

  cfg.subscriptions[0].source = "";
  cfg.batch = 0;
  CHECK(src.open(cfg).step == "config");
  cfg.batch = 32;
  cfg.max_datagram = 0;
  CHECK(src.open(cfg).step == "config");

  CHECK(src.line_count() == 0);
  CHECK(src.fd(0) == -1);
  CHECK(src.poll([](std::span<const std::byte>, const RxMeta&) noexcept {}) == 0);
}

TEST_CASE("KernelDatagramSource: two groups on one port deliver with their line index") {
  if (!in_multicast_netns()) return;
  KernelDatagramSource src;
  const KernelSourceOpen o = src.open(two_line_config());
  INFO("step " << o.step << " err " << o.err);
  REQUIRE(o.ok());
  CHECK(src.line_count() == 2);
  CHECK(src.fd(0) >= 0);
  CHECK(src.fd(1) >= 0);
  CHECK(src.fd(2) == -1);
  CHECK(src.poll([](std::span<const std::byte>, const RxMeta&) noexcept {}) == 0);

  UdpSocket tx = mcast_sender();
  for (int i = 0; i < 10; ++i) {
    send(tx, "239.1.1.1", 30001, "A" + std::to_string(i));
    send(tx, "239.1.1.2", 30001, "B" + std::to_string(i));
  }
  send(tx, "239.1.1.3", 30001, "not joined");
  send(tx, "239.1.1.1", 30002, "other port");
  const auto got = drain(src, 20);
  REQUIRE(got.size() == 20);
  int a = 0;
  int b = 0;
  for (const Received& r : got) {
    INFO(r.payload);
    const bool line_a = r.payload[0] == 'A';
    CHECK(r.meta.line == (line_a ? 0 : 1));
    CHECK(r.meta.dst_ip == ip(line_a ? "239.1.1.1" : "239.1.1.2"));
    CHECK(r.meta.dst_port == 30001);
    CHECK(r.meta.src_ip == ip("127.0.0.1"));
    CHECK(r.payload.substr(1) == std::to_string(line_a ? a++ : b++));  // in order per line
  }
  CHECK(src.poll([](std::span<const std::byte>, const RxMeta&) noexcept {}) == 0);
  CHECK(src.stats().datagrams == 20);
  CHECK(src.stats().bytes == 40);
  CHECK(src.stats().truncated == 0);
  CHECK(src.stats().errors == 0);

  // Moving keeps the preallocated buffers valid.
  KernelDatagramSource moved(std::move(src));
  send(tx, "239.1.1.2", 30001, "after move");
  const auto more = drain(moved, 1);
  REQUIRE(more.size() == 1);
  CHECK(more[0].payload == "after move");
  CHECK(more[0].meta.line == 1);
}

TEST_CASE("KernelDatagramSource: source-specific join drops other sources") {
  if (!in_multicast_netns()) return;
  KernelDatagramSource src;
  KernelSourceConfig cfg;
  cfg.subscriptions = {
      {.interface = "lo", .group = "232.1.1.1", .port = 30010, .source = "127.0.0.1"},
      {.interface = "127.0.0.1", .group = "232.1.1.2", .port = 30010, .source = "127.0.0.2"},
      {.interface = "lo", .group = "239.1.1.9", .port = 30010, .source = ""}};
  const KernelSourceOpen o = src.open(cfg);
  INFO("step " << o.step << " err " << o.err);
  REQUIRE(o.ok());

  UdpSocket from1 = mcast_sender("127.0.0.1");
  UdpSocket from2 = mcast_sender("127.0.0.2");
  for (const char* group : {"232.1.1.1", "232.1.1.2", "239.1.1.9"}) {
    send(from1, group, 30010, std::string("1@") + group);
    send(from2, group, 30010, std::string("2@") + group);
  }
  const auto got = drain(src, 4);
  std::vector<std::string> payloads;
  for (const Received& r : got) {
    payloads.push_back(r.payload);
    CHECK(r.meta.src_ip == ip(r.payload[0] == '1' ? "127.0.0.1" : "127.0.0.2"));
  }
  std::sort(payloads.begin(), payloads.end());
  CHECK(payloads ==
        std::vector<std::string>{"1@232.1.1.1", "1@239.1.1.9", "2@232.1.1.2", "2@239.1.1.9"});
  CHECK(drain(src, 1, nullptr, 200).empty());
}

TEST_CASE("KernelDatagramSource: batch boundaries") {
  if (!in_multicast_netns()) return;
  KernelDatagramSource src;
  KernelSourceConfig cfg = two_line_config();
  cfg.batch = 4;
  REQUIRE(src.open(cfg).ok());
  UdpSocket tx = mcast_sender();
  constexpr int kA = 37;
  constexpr int kB = 6;
  for (int i = 0; i < kA; ++i) send(tx, "239.1.1.1", 30001, "A" + std::to_string(i));
  for (int i = 0; i < kB; ++i) send(tx, "239.1.1.2", 30001, "B" + std::to_string(i));

  std::vector<std::size_t> sizes;
  const auto got = drain(src, kA + kB, &sizes);
  REQUIRE(got.size() == kA + kB);
  for (const std::size_t n : sizes) CHECK(n <= 8);  // one batch of 4 per line and poll
  CHECK(sizes.size() >= (kA + 3) / 4);
  int a = 0;
  int b = 0;
  for (const Received& r : got) {
    CHECK(r.payload.substr(1) == std::to_string(r.payload[0] == 'A' ? a++ : b++));
  }
  CHECK(a == kA);
  CHECK(b == kB);
}

TEST_CASE("KernelDatagramSource: software timestamps and T0") {
  if (!in_multicast_netns()) return;
  KernelDatagramSource src;
  REQUIRE(src.open(two_line_config()).ok());
  UdpSocket tx = mcast_sender();
  Cycles last{};
  for (int round = 0; round < 5; ++round) {
    const std::int64_t before = wall_now().ns;
    for (int i = 0; i < 3; ++i) send(tx, "239.1.1.1", 30001, "x");
    const auto got = drain(src, 3);
    REQUIRE(got.size() == 3);
    for (const Received& r : got) {
      CHECK(r.meta.t0_cycles.v != 0);
      CHECK(r.meta.t0_cycles >= last);
      last = r.meta.t0_cycles;
      CHECK(r.meta.hw_ts_ns == 0);  // lo has no PHC
      CHECK(r.meta.sw_ts_ns >= before);
      CHECK(r.meta.sw_ts_ns <= r.meta.t0_wall_ns);  // kernel stamp, then T0; one clock
      CHECK(r.meta.t0_wall_ns - r.meta.sw_ts_ns < 1'000'000'000);
    }
  }

  KernelSourceConfig cfg = two_line_config();
  cfg.timestamps = false;
  REQUIRE(src.open(cfg).ok());
  send(tx, "239.1.1.1", 30001, "y");
  const auto got = drain(src, 1);
  REQUIRE(got.size() == 1);
  CHECK(got[0].meta.sw_ts_ns == 0);
  CHECK(got[0].meta.t0_wall_ns != 0);
}

TEST_CASE("KernelDatagramSource: oversized datagrams are counted and dropped") {
  if (!in_multicast_netns()) return;
  KernelDatagramSource src;
  KernelSourceConfig cfg = two_line_config();
  cfg.max_datagram = 64;
  REQUIRE(src.open(cfg).ok());
  UdpSocket tx = mcast_sender();
  send(tx, "239.1.1.1", 30001, std::string(65, 'L'));
  send(tx, "239.1.1.1", 30001, std::string(64, 'F'));
  send(tx, "239.1.1.1", 30001, "small");
  send(tx, "239.1.1.1", 30001, std::string(1500, 'M'));
  const auto got = drain(src, 2);
  REQUIRE(got.size() == 2);
  CHECK(got[0].payload == std::string(64, 'F'));
  CHECK(got[1].payload == "small");
  CHECK(drain(src, 1, nullptr, 200).empty());
  CHECK(src.stats().truncated == 2);
  CHECK(src.stats().datagrams == 2);
}

TEST_CASE("KernelDatagramSource: rcvbuf and busy poll are reported") {
  if (!in_multicast_netns()) return;
  KernelDatagramSource src;
  KernelSourceConfig cfg = two_line_config();
  cfg.rcvbuf_bytes = 1 << 20;
  cfg.busy_poll = true;
  cfg.busy_poll_budget = 64;
  const KernelSourceOpen o = src.open(cfg);
  REQUIRE(o.ok());
  CHECK(o.rcvbuf > 0);
  // In a user namespace CAP_NET_ADMIN does not reach the initial namespace: SO_PREFER_BUSY_POLL
  // fails, and open() still succeeds.
  CHECK((o.prefer_busy_poll == -EPERM || o.prefer_busy_poll == -ENOPROTOOPT));
  CHECK_FALSE(o.busy_poll_applied());
  UdpSocket tx = mcast_sender();
  send(tx, "239.1.1.2", 30001, "z");
  CHECK(drain(src, 1).size() == 1);
}
