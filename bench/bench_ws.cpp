// WebSocket frame codec micro-benchmarks: header decode, full message assembly through the
// RecvBuffer path (what the receive loop does per frame), and client-side encode + mask.
//
//   BM_WsClientReceive/<backend>/<tls>  a server session on the same reactor writes one 64-byte
//                                       text frame; the client's busy-polled reactor runs until
//                                       the client delivered it (epoll_wait, the client's reads,
//                                       TLS decryption with tls = 1, the frame decode)
#include "fastmm/net/byte_stream.hpp"
#include "fastmm/net/http_server.hpp"
#include "fastmm/net/reactor.hpp"
#include "fastmm/net/recv_buffer.hpp"
#include "fastmm/net/tls_stream.hpp"
#include "fastmm/net/wire_buffer.hpp"
#include "fastmm/net/ws_client.hpp"
#include "fastmm/net/ws_frame.hpp"
#include "fastmm/net/ws_server.hpp"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

using namespace fastmm::net;

namespace {

struct NullSink {
  std::size_t delivered = 0;
  void on_message(WsOpcode, std::span<std::byte> p) { delivered += p.size(); }
  void on_control(WsOpcode, std::span<std::byte>) {}
};

std::vector<std::byte> make_frame(std::size_t payload_len, const std::uint8_t* mask) {
  WireBuffer w(payload_len + kWsMaxHeaderSize);
  std::string payload(payload_len, 'x');
  ws_encode_frame(w,
                  WsOpcode::Text,
                  true,
                  std::span<const std::byte>(reinterpret_cast<const std::byte*>(payload.data()),
                                             payload.size()),
                  mask);
  return std::vector<std::byte>(w.pending().begin(), w.pending().end());
}

void BM_WsDecodeHeader_Small(benchmark::State& state) {
  const auto frame = make_frame(64, nullptr);
  for (auto _ : state) {
    WsFrameHeader h;
    parse_frame_header(frame, h);
    benchmark::DoNotOptimize(h);
  }
}
BENCHMARK(BM_WsDecodeHeader_Small);

// Feeds one complete frame through the assembler each iteration: parse + deliver + consume.
void BM_WsAssemble(benchmark::State& state) {
  const auto payload_len = static_cast<std::size_t>(state.range(0));
  const auto frame = make_frame(payload_len, nullptr);
  RecvBuffer rx(1 << 20);
  detail::WsMessageAssembler assembler(rx, 1 << 20, false, /*validate_utf8=*/false);
  NullSink sink;
  for (auto _ : state) {
    auto w = rx.writable();
    std::memcpy(w.data(), frame.data(), frame.size());
    rx.commit(frame.size());
    assembler.process(sink);
    benchmark::DoNotOptimize(sink.delivered);
  }
  state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(frame.size()));
}
BENCHMARK(BM_WsAssemble)->Arg(64)->Arg(16 * 1024);

// The same with UTF-8 validation of the text payload (WsClientConfig::validate_utf8, the
// default outside Connection).
void BM_WsAssembleUtf8(benchmark::State& state) {
  const auto payload_len = static_cast<std::size_t>(state.range(0));
  const auto frame = make_frame(payload_len, nullptr);
  RecvBuffer rx(1 << 20);
  detail::WsMessageAssembler assembler(rx, 1 << 20, false, /*validate_utf8=*/true);
  NullSink sink;
  for (auto _ : state) {
    auto w = rx.writable();
    std::memcpy(w.data(), frame.data(), frame.size());
    rx.commit(frame.size());
    assembler.process(sink);
    benchmark::DoNotOptimize(sink.delivered);
  }
  state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(frame.size()));
}
BENCHMARK(BM_WsAssembleUtf8)->Arg(64)->Arg(1024)->Arg(16 * 1024);

// Server-side path: masked frame must be unmasked in place.
void BM_WsAssembleMasked16K(benchmark::State& state) {
  const std::uint8_t mask[4] = {0x37, 0xfa, 0x21, 0x3d};
  const auto frame = make_frame(16 * 1024, mask);
  RecvBuffer rx(1 << 20);
  detail::WsMessageAssembler assembler(rx, 1 << 20, true, /*validate_utf8=*/false);
  NullSink sink;
  for (auto _ : state) {
    auto w = rx.writable();
    std::memcpy(w.data(), frame.data(), frame.size());
    rx.commit(frame.size());
    assembler.process(sink);
    benchmark::DoNotOptimize(sink.delivered);
  }
  state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(frame.size()));
}
BENCHMARK(BM_WsAssembleMasked16K);

void BM_WsEncodeMask1K(benchmark::State& state) {
  const std::string payload(1024, 'o');
  const std::uint8_t mask[4] = {0x37, 0xfa, 0x21, 0x3d};
  WireBuffer tx(4096);
  for (auto _ : state) {
    tx.clear();
    ws_encode_frame(tx,
                    WsOpcode::Text,
                    true,
                    std::span<const std::byte>(reinterpret_cast<const std::byte*>(payload.data()),
                                               payload.size()),
                    mask);
    benchmark::DoNotOptimize(tx.pending().data());
  }
  state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(payload.size()));
}
BENCHMARK(BM_WsEncodeMask1K);

struct CountingClient {
  std::uint64_t texts = 0;
  bool open = false;
  bool failed = false;
  void on_ws_progress(WsProgress) {}
  void on_ws_open() { open = true; }
  void on_ws_text(std::string_view, std::int64_t) { ++texts; }
  void on_ws_binary(std::span<const std::byte>, std::int64_t) {}
  void on_ws_ping(std::span<const std::byte>) {}
  void on_ws_pong(std::span<const std::byte>) {}
  void on_ws_close(std::uint16_t, std::string_view) { failed = true; }
  void on_ws_error(NetError, std::string_view) { failed = true; }
};

class SessionHolder final : public WsSessionHandler {
 public:
  void on_open(WsSession& s) override { session = &s; }
  void on_close(WsSession&, std::uint16_t, std::string_view) override { session = nullptr; }
  void on_error(WsSession&, NetError, std::string_view) override { session = nullptr; }
  WsSession* session = nullptr;
};

template <class Stream, class MakeServerStream, class MakeClientStream>
void ws_client_receive(benchmark::State& state,
                       Reactor& reactor,
                       MakeServerStream make_server_stream,
                       MakeClientStream make_client_stream) {
  SessionHolder holder;
  HttpServer<Stream> server(reactor, make_server_stream);
  server.set_ws_handler(&holder);
  if (!server.listen(SockAddr::loopback_v4(0))) {
    state.SkipWithError("listen failed");
    return;
  }
  TcpSocket sock;
  if (sock.connect(SockAddr::loopback_v4(server.port())) == ConnectStatus::Error) {
    state.SkipWithError("connect failed");
    return;
  }
  CountingClient events;
  WsClient<Stream, CountingClient> client(
      reactor, make_client_stream(PlainStream(std::move(sock))), events);
  if (!client.start("localhost", server.port(), false, "/ws")) {
    state.SkipWithError("start failed");
    return;
  }
  for (int i = 0; i < 1'000'000 && !(events.open && holder.session != nullptr); ++i)
    reactor.run_once(0);
  if (!events.open || holder.session == nullptr) {
    state.SkipWithError("upgrade failed");
    return;
  }
  const std::string payload(64, 'x');
  std::uint64_t expected = events.texts;
  for (auto _ : state) {
    if (!holder.session->send_text(payload)) {
      state.SkipWithError("send failed");
      break;
    }
    ++expected;
    while (events.texts < expected && !events.failed) reactor.run_once(0);
  }
  if (events.failed) state.SkipWithError("connection failed");
}

void BM_WsClientReceive(benchmark::State& state) {
  const ReactorBackend backend =
      state.range(0) == 0 ? ReactorBackend::Epoll : ReactorBackend::IoUring;
  const bool tls = state.range(1) != 0;
  state.SetLabel(std::string(to_string(backend)) + (tls ? "/tls" : "/plain"));
  if (backend == ReactorBackend::IoUring && !Reactor::io_uring_supported()) {
    state.SkipWithMessage("io_uring is not available on this kernel");
    return;
  }
  Reactor reactor(backend);
  reactor.set_busy_poll(true);
  if (!tls) {
    ws_client_receive<PlainStream>(
        state,
        reactor,
        [](TcpSocket&& s) { return PlainStream(std::move(s)); },
        [](PlainStream&& s) { return std::move(s); });
    return;
  }
  const std::string dir = FASTMM_TLS_FIXTURES_DIR;
  TlsContext server_ctx = TlsContext::server(dir + "/cert.pem", dir + "/key.pem");
  TlsContext client_ctx;
  client_ctx.set_ca_file(dir + "/cert.pem");
  using Tls = TlsStream<PlainStream>;
  ws_client_receive<Tls>(
      state,
      reactor,
      [&](TcpSocket&& s) { return Tls(server_ctx, PlainStream(std::move(s))); },
      [&](PlainStream&& s) { return Tls(client_ctx, std::move(s), "localhost"); });
}
BENCHMARK(BM_WsClientReceive)->ArgsProduct({{0, 1}, {0, 1}});

}  // namespace
