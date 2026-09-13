// HTTP/1.1 keep-alive round trip over loopback: HttpClient<PlainStream> -> HttpServer on the
// same reactor, one request per iteration. Measures the full stack (encode, syscalls, epoll,
// parse, callback) rather than the parser alone. Also the response parser in isolation.
#include "fastmm/net/http_client.hpp"
#include "fastmm/net/http_server.hpp"
#include "fastmm/net/reactor.hpp"

#include <benchmark/benchmark.h>

#include <cstring>
#include <string>

using namespace fastmm::net;

namespace {

void BM_HttpRoundTripLoopback(benchmark::State& state) {
  Reactor reactor;
  HttpServer<PlainStream> server(reactor, [](TcpSocket&& s) { return PlainStream(std::move(s)); });
  server.route("GET", "/api/v3/time", [](const HttpRequest&) {
    return HttpServerResponse::json(200, "{\"serverTime\":1700000000000}");
  });
  if (!server.listen(SockAddr::loopback_v4(0))) {
    state.SkipWithError("listen failed");
    return;
  }
  TcpSocket sock;
  sock.connect(SockAddr::loopback_v4(server.port()));
  HttpClient<PlainStream> client(
      reactor, PlainStream(std::move(sock)), "localhost", server.port(), false);
  client.start();
  while (!client.is_ready()) reactor.run_once(10);

  reactor.set_busy_poll(true);
  for (auto _ : state) {
    bool done = false;
    client.request("GET", "/api/v3/time", "", "", [&](const HttpResponse& r) {
      int status = r.status;
      benchmark::DoNotOptimize(status);
      done = true;
    });
    while (!done) reactor.run_once(0);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_HttpRoundTripLoopback)->UseRealTime();

void BM_HttpParseResponse(benchmark::State& state) {
  const std::string wire =
      "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nX-MBX-USED-WEIGHT-1M: 12\r\n"
      "Date: Tue, 01 Jan 2030 00:00:00 GMT\r\nContent-Length: "
      "28\r\n\r\n{\"serverTime\":1700000000000}";
  RecvBuffer rx(64 * 1024);
  detail::HttpResponseParser parser;
  for (auto _ : state) {
    auto w = rx.writable();
    std::memcpy(w.data(), wire.data(), wire.size());
    rx.commit(wire.size());
    parser.begin(false);
    HttpResponse resp;
    std::size_t consume = 0;
    auto r = parser.parse(rx, resp, consume);
    if (r != detail::HttpResponseParser::Result::Complete) {
      state.SkipWithError("parser did not complete");
      break;
    }
    benchmark::DoNotOptimize(r);
    benchmark::DoNotOptimize(resp.body.data());
    rx.consume(consume);
  }
  state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(wire.size()));
}
BENCHMARK(BM_HttpParseResponse);

}  // namespace
