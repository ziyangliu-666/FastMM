// Autobahn|Testsuite testee for WsClient: connects to `wstest -m fuzzingserver`, asks for the case
// count, runs every case (echoing each text and binary message back), then asks the server to
// write its reports. scripts/autobahn.sh runs it; the reports carry the verdicts.
//
//   fastmm_autobahn_client <host> <port> <agent>
#include "fastmm/net/byte_stream.hpp"
#include "fastmm/net/reactor.hpp"
#include "fastmm/net/tcp_socket.hpp"
#include "fastmm/net/ws_client.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

using namespace fastmm::net;

namespace {

struct Testee {
  // Set once the client exists; its type needs Testee complete.
  std::function<void(WsOpcode, std::span<const std::byte>)> send;
  bool done = false;
  std::string first_text;
  std::uint16_t close_code = 0;
  std::string error;

  void on_ws_progress(WsProgress) {}
  void on_ws_open() {}
  void on_ws_text(std::string_view s, std::int64_t) {
    if (first_text.empty()) first_text = std::string(s);
    if (send) send(WsOpcode::Text, std::as_bytes(std::span(s.data(), s.size())));
  }
  void on_ws_binary(std::span<const std::byte> b, std::int64_t) {
    if (send) send(WsOpcode::Binary, b);
  }
  void on_ws_ping(std::span<const std::byte>) {}
  void on_ws_pong(std::span<const std::byte>) {}
  void on_ws_close(std::uint16_t code, std::string_view) {
    close_code = code;
    done = true;
  }
  void on_ws_error(NetError, std::string_view detail) {
    error = std::string(detail);
    done = true;
  }
};

// One connection to `target`, run until the server closes it or `timeout` passes. Returns the
// first text message received (the case count for /getCaseCount).
std::string run_session(Reactor& reactor,
                        const SockAddr& addr,
                        std::string_view host,
                        std::uint16_t port,
                        const std::string& target,
                        bool echo,
                        std::chrono::seconds timeout) {
  TcpSocket sock;
  if (sock.connect(addr) == ConnectStatus::Error) {
    std::fprintf(stderr, "connect to %.*s:%u failed\n", static_cast<int>(host.size()), host.data(),
                 port);
    std::exit(2);
  }
  // Case 9.* sends messages of up to 16 MiB, fragmented into frames as small as 64 bytes; the
  // fragment headers stay in the buffer until the message completes.
  WsClientConfig cfg;
  cfg.recv_capacity = std::size_t{64} << 20;
  cfg.send_capacity = std::size_t{64} << 20;
  Testee testee;
  auto client = std::make_unique<WsClient<PlainStream, Testee>>(
      reactor, PlainStream(std::move(sock)), testee, cfg);
  if (echo) {
    testee.send = [c = client.get()](WsOpcode op, std::span<const std::byte> payload) {
      if (op == WsOpcode::Text) {
        c->send_text(std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size()));
      } else {
        c->send_binary(payload);
      }
    };
  }
  if (!client->start(host, port, false, target)) {
    std::fprintf(stderr, "%s: start failed\n", target.c_str());
    std::exit(2);
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!testee.done && std::chrono::steady_clock::now() < deadline) reactor.run_once(100);
  if (!testee.done) std::fprintf(stderr, "%s: timed out\n", target.c_str());
  return testee.first_text;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: %s <host> <port> <agent>\n", argv[0]);
    return 2;
  }
  const std::string host = argv[1];
  const auto port = static_cast<std::uint16_t>(std::atoi(argv[2]));
  const std::string agent = argv[3];
  const auto addr = SockAddr::from_ip(host, port);
  if (!addr) {
    std::fprintf(stderr, "%s: not a numeric address\n", host.c_str());
    return 2;
  }
  Reactor reactor;
  const std::string count_text =
      run_session(reactor, *addr, host, port, "/getCaseCount", false, std::chrono::seconds(10));
  const int count = std::atoi(count_text.c_str());
  if (count <= 0) {
    std::fprintf(stderr, "no cases (getCaseCount answered '%s')\n", count_text.c_str());
    return 1;
  }
  std::fprintf(stderr, "running %d cases as %s\n", count, agent.c_str());
  for (int i = 1; i <= count; ++i) {
    run_session(reactor, *addr, host, port,
                "/runCase?case=" + std::to_string(i) + "&agent=" + agent, true,
                std::chrono::seconds(120));
  }
  run_session(reactor, *addr, host, port, "/updateReports?agent=" + agent, false,
              std::chrono::seconds(60));
  return 0;
}
