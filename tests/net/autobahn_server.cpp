// Autobahn|Testsuite testee for WsServerConnection: an echo server on 127.0.0.1:<port> that
// `wstest -m fuzzingclient` drives. Runs until SIGTERM or SIGINT. scripts/autobahn.sh runs it.
//
//   fastmm_autobahn_server <port>
#include "fastmm/net/byte_stream.hpp"
#include "fastmm/net/http_server.hpp"
#include "fastmm/net/reactor.hpp"
#include "fastmm/net/tcp_socket.hpp"
#include "fastmm/net/ws_server.hpp"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>

using namespace fastmm::net;

namespace {

std::atomic<bool> g_stop{false};

extern "C" void on_signal(int) {
  g_stop.store(true);
}

class Echo final : public WsSessionHandler {
 public:
  void on_text(WsSession& s, std::string_view t) override { s.send_text(t); }
  void on_binary(WsSession& s, std::span<const std::byte> b) override { s.send_binary(b); }
};

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <port>\n", argv[0]);
    return 2;
  }
  const auto port = static_cast<std::uint16_t>(std::atoi(argv[1]));
  std::signal(SIGTERM, on_signal);
  std::signal(SIGINT, on_signal);
  std::signal(SIGPIPE, SIG_IGN);

  // Case 9.* sends messages of up to 16 MiB, fragmented into frames as small as 64 bytes.
  HttpServerConfig cfg;
  cfg.ws.recv_capacity = std::size_t{64} << 20;
  cfg.ws.send_capacity = std::size_t{64} << 20;
  Reactor reactor;
  HttpServer<PlainStream> server(
      reactor, [](TcpSocket&& s) { return PlainStream(std::move(s)); }, cfg);
  Echo echo;
  server.set_ws_handler(&echo);
  if (!server.listen(SockAddr::loopback_v4(port))) {
    std::fprintf(stderr, "listen on 127.0.0.1:%u failed\n", port);
    return 2;
  }
  std::fprintf(stderr, "echo server on 127.0.0.1:%u\n", port);
  while (!g_stop.load()) reactor.run_once(100);
  return 0;
}
