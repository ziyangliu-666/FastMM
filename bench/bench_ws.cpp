// WebSocket frame codec micro-benchmarks: header decode, full message assembly through the
// RecvBuffer path (what the receive loop does per frame), and client-side encode + mask.
#include "fastmm/net/recv_buffer.hpp"
#include "fastmm/net/wire_buffer.hpp"
#include "fastmm/net/ws_frame.hpp"

#include <benchmark/benchmark.h>

#include <cstring>
#include <string>
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

}  // namespace
