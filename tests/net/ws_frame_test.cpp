#include "fastmm/net/ws_frame.hpp"

#include "net_test_util.hpp"

#include "fastmm/net/ws_client.hpp"

#include <cstring>
#include <string>
#include <utility>
#include <vector>

using namespace fastmm::net;
using namespace fastmm::net::test;

namespace {

std::vector<std::byte> bytes_of(std::initializer_list<int> v) {
  std::vector<std::byte> out;
  for (int b : v) out.push_back(static_cast<std::byte>(b));
  return out;
}

// Collects assembler output.
struct Sink {
  std::vector<std::pair<WsOpcode, std::string>> messages;
  std::vector<std::pair<WsOpcode, std::string>> controls;
  void on_message(WsOpcode op, std::span<std::byte> p) {
    messages.emplace_back(op, std::string(sv(p)));
  }
  void on_control(WsOpcode op, std::span<std::byte> p) {
    controls.emplace_back(op, std::string(sv(p)));
  }
};

void feed(RecvBuffer& rx, std::span<const std::byte> data) {
  auto w = rx.writable();
  REQUIRE(w.size() >= data.size());
  std::memcpy(w.data(), data.data(), data.size());
  rx.commit(data.size());
}

}  // namespace

TEST_CASE("ws_frame: RFC 6455 section 5.7 header vectors") {
  WsFrameHeader h;

  SUBCASE("single-frame unmasked text 'Hello'") {
    auto f = bytes_of({0x81, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f});
    REQUIRE(parse_frame_header(f, h) == WsParseStatus::Ok);
    CHECK(h.fin);
    CHECK_FALSE(h.masked);
    CHECK(h.opcode == WsOpcode::Text);
    CHECK(h.payload_len == 5);
    CHECK(h.header_len == 2);
  }
  SUBCASE("single-frame masked text 'Hello'") {
    auto f = bytes_of({0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58});
    REQUIRE(parse_frame_header(f, h) == WsParseStatus::Ok);
    CHECK(h.masked);
    CHECK(h.header_len == 6);
    CHECK(h.mask[0] == 0x37);
    CHECK(h.mask[3] == 0x3d);
    std::vector<std::byte> payload(f.begin() + 6, f.end());
    ws_mask_inplace(payload, h.mask);
    CHECK(sv(payload) == "Hello");
  }
  SUBCASE("fragmented unmasked text 'Hel' + 'lo'") {
    auto f1 = bytes_of({0x01, 0x03, 0x48, 0x65, 0x6c});
    auto f2 = bytes_of({0x80, 0x02, 0x6c, 0x6f});
    REQUIRE(parse_frame_header(f1, h) == WsParseStatus::Ok);
    CHECK_FALSE(h.fin);
    CHECK(h.opcode == WsOpcode::Text);
    REQUIRE(parse_frame_header(f2, h) == WsParseStatus::Ok);
    CHECK(h.fin);
    CHECK(h.opcode == WsOpcode::Continuation);
  }
  SUBCASE("unmasked ping / masked pong") {
    auto ping = bytes_of({0x89, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f});
    REQUIRE(parse_frame_header(ping, h) == WsParseStatus::Ok);
    CHECK(h.opcode == WsOpcode::Ping);
    CHECK(ws_is_control(h.opcode));
    auto pong = bytes_of({0x8a, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58});
    REQUIRE(parse_frame_header(pong, h) == WsParseStatus::Ok);
    CHECK(h.opcode == WsOpcode::Pong);
    CHECK(h.masked);
  }
  SUBCASE("256 bytes binary: 16-bit length") {
    auto f = bytes_of({0x82, 0x7E, 0x01, 0x00});
    REQUIRE(parse_frame_header(f, h) == WsParseStatus::Ok);
    CHECK(h.opcode == WsOpcode::Binary);
    CHECK(h.payload_len == 256);
    CHECK(h.header_len == 4);
  }
  SUBCASE("64 KiB binary: 64-bit length") {
    auto f = bytes_of({0x82, 0x7F, 0, 0, 0, 0, 0, 1, 0, 0});
    REQUIRE(parse_frame_header(f, h) == WsParseStatus::Ok);
    CHECK(h.payload_len == 65536);
    CHECK(h.header_len == 10);
  }
  SUBCASE("incomplete headers") {
    CHECK(parse_frame_header(bytes_of({0x81}), h) == WsParseStatus::Incomplete);
    CHECK(parse_frame_header(bytes_of({0x82, 0x7E, 0x01}), h) == WsParseStatus::Incomplete);
    CHECK(parse_frame_header(bytes_of({0x82, 0x7F, 0, 0, 0, 0}), h) == WsParseStatus::Incomplete);
    CHECK(parse_frame_header(bytes_of({0x81, 0x85, 0x37, 0xfa}), h) == WsParseStatus::Incomplete);
  }
  SUBCASE("invalid headers") {
    CHECK(parse_frame_header(bytes_of({0x83, 0x00}), h) ==
          WsParseStatus::Invalid);  // reserved opcode 3
    CHECK(parse_frame_header(bytes_of({0x8F, 0x00}), h) == WsParseStatus::Invalid);
    CHECK(parse_frame_header(bytes_of({0x82, 0x7E, 0x00, 0x05}), h) ==
          WsParseStatus::Invalid);  // non-minimal
    CHECK(parse_frame_header(bytes_of({0x82, 0x7F, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF}), h) ==
          WsParseStatus::Invalid);
    CHECK(parse_frame_header(bytes_of({0x82, 0x7F, 0x80, 0, 0, 0, 0, 1, 0, 0}), h) ==
          WsParseStatus::Invalid);  // top bit
    auto rsv = bytes_of({0xC1, 0x00});
    REQUIRE(parse_frame_header(rsv, h) == WsParseStatus::Ok);
    CHECK(h.rsv);
  }
}

TEST_CASE("ws_frame: encode round-trips through parse") {
  const std::uint8_t mask[4] = {0x37, 0xfa, 0x21, 0x3d};
  for (std::uint64_t len : {0ull, 1ull, 125ull, 126ull, 65535ull, 65536ull, 1ull << 40}) {
    for (const std::uint8_t* m : {static_cast<const std::uint8_t*>(nullptr), mask}) {
      std::byte hdr[kWsMaxHeaderSize];
      const std::size_t n = encode_frame_header(hdr, WsOpcode::Binary, true, len, m);
      CHECK(n == ws_header_size(len, m != nullptr));
      WsFrameHeader h;
      REQUIRE(parse_frame_header(std::span<const std::byte>(hdr, n), h) == WsParseStatus::Ok);
      CHECK(h.payload_len == len);
      CHECK(h.header_len == n);
      CHECK(h.masked == (m != nullptr));
      CHECK(h.fin);
      CHECK(h.opcode == WsOpcode::Binary);
    }
  }
  std::byte small[3];
  CHECK(encode_frame_header(small, WsOpcode::Text, true, 300, nullptr) == 0);

  SUBCASE("ws_encode_frame masks the payload and matches the RFC vector") {
    WireBuffer w(64);
    REQUIRE(ws_encode_frame(w, WsOpcode::Text, true, bytes("Hello"), mask));
    auto expect = bytes_of({0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58});
    CHECK(sv(w.pending()) == sv(expect));
    WireBuffer tiny(4);
    CHECK_FALSE(ws_encode_frame(tiny, WsOpcode::Text, true, bytes("Hello"), mask));
    CHECK(tiny.empty());
  }
  SUBCASE("close payload codec") {
    std::byte buf[kWsMaxControlPayload];
    const std::size_t n = ws_encode_close_payload(buf, 1001, "bye");
    CHECK(n == 5);
    CHECK(ws_decode_close_code(std::span<const std::byte>(buf, n)) == 1001);
    CHECK(ws_decode_close_code({}) == 1005);
    CHECK(ws_encode_close_payload(buf, 1000, std::string(124, 'x')) == 0);  // > 125
  }
  SUBCASE("mask with offset continues the key rotation") {
    std::string data = "0123456789abcdef";
    auto whole = mutable_bytes(data);
    std::string copy = data;
    auto part = mutable_bytes(copy);
    ws_mask_inplace(whole, mask);
    ws_mask_inplace(part.subspan(0, 5), mask, 0);
    ws_mask_inplace(part.subspan(5), mask, 5);
    CHECK(data == copy);
  }
}

TEST_CASE("ws_frame: assembler delivers messages and compacts fragments in place") {
  RecvBuffer rx(4096);
  detail::WsMessageAssembler asm_(rx, 4000, /*require_masked=*/false, /*validate_utf8=*/true);
  Sink sink;

  SUBCASE("two whole frames in one read") {
    auto f = bytes_of({0x81, 0x05, 'H', 'e', 'l', 'l', 'o', 0x82, 0x02, 'h', 'i'});
    feed(rx, f);
    CHECK_FALSE(asm_.process(sink));
    REQUIRE(sink.messages.size() == 2);
    CHECK(sink.messages[0] == std::make_pair(WsOpcode::Text, std::string("Hello")));
    CHECK(sink.messages[1] == std::make_pair(WsOpcode::Binary, std::string("hi")));
    CHECK(rx.empty());
  }
  SUBCASE("byte-at-a-time delivery of fragments with an interleaved ping") {
    auto stream = bytes_of({0x01,
                            0x03,
                            'H',
                            'e',
                            'l',  // text fragment 1
                            0x89,
                            0x02,
                            'p',
                            'g',  // ping in the middle
                            0x00,
                            0x01,
                            'l',  // continuation
                            0x8A,
                            0x00,  // pong
                            0x80,
                            0x01,
                            'o'});  // final continuation
    for (std::size_t i = 0; i < stream.size(); ++i) {
      feed(rx, std::span<const std::byte>(&stream[i], 1));
      CHECK_FALSE(asm_.process(sink));
      if (i + 1 < stream.size()) CHECK(sink.messages.empty());
    }
    REQUIRE(sink.messages.size() == 1);
    CHECK(sink.messages[0] == std::make_pair(WsOpcode::Text, std::string("Hello")));
    REQUIRE(sink.controls.size() == 2);
    CHECK(sink.controls[0] == std::make_pair(WsOpcode::Ping, std::string("pg")));
    CHECK(sink.controls[1] == std::make_pair(WsOpcode::Pong, std::string("")));
    CHECK(rx.empty());
    CHECK_FALSE(asm_.fragment_active());
  }
  SUBCASE("fragmented message is contiguous even after buffer compaction") {
    // Push the read cursor deep into the buffer first, so the compaction path moves the
    // partially assembled message while a fragment is pending.
    RecvBuffer big(RecvBuffer::kCompactBelow + 4096);
    detail::WsMessageAssembler a2(big, 1 << 20, false, true);
    std::string junk(2000, 'j');
    feed(big, bytes(junk));
    big.consume(2000);  // rd_ = 2000
    auto f1 = bytes_of({0x01, 0x03, 'a', 'b', 'c'});
    feed(big, f1);
    CHECK_FALSE(a2.process(sink));
    CHECK(a2.fragment_active());
    // Fill until the tail is under the threshold: next writable() compacts.
    std::string filler(big.capacity() - big.write_offset() - 1000, 'x');
    // A continuation frame containing the filler as payload (fits within capacity).
    WireBuffer enc(filler.size() + 16);
    ws_encode_frame(enc, WsOpcode::Continuation, false, bytes(filler), nullptr);
    feed(big, enc.pending());
    CHECK_FALSE(a2.process(sink));
    CHECK(big.read_offset() == 0);  // compacted, fragment still valid
    auto f3 = bytes_of({0x80, 0x01, 'z'});
    feed(big, f3);
    CHECK_FALSE(a2.process(sink));
    REQUIRE(sink.messages.size() == 1);
    CHECK(sink.messages[0].second == "abc" + filler + "z");
    CHECK(big.empty());
  }
  SUBCASE("masked frames are rejected when not expected, and vice versa") {
    auto masked = bytes_of({0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58});
    feed(rx, masked);
    auto err = asm_.process(sink);
    CHECK(err.code == WsCloseCode::ProtocolError);
    CHECK(std::string_view(err.detail) == "masked server frame");

    RecvBuffer rx2(256);
    detail::WsMessageAssembler server_side(rx2, 200, /*require_masked=*/true, true);
    feed(rx2, bytes_of({0x81, 0x01, 'x'}));
    err = server_side.process(sink);
    CHECK(err.code == WsCloseCode::ProtocolError);
    CHECK(std::string_view(err.detail) == "unmasked client frame");
    rx2.clear();
    feed(rx2, masked);
    CHECK_FALSE(server_side.process(sink));
    CHECK(sink.messages.back().second == "Hello");
  }
  SUBCASE("protocol errors") {
    feed(rx, bytes_of({0x80, 0x01, 'x'}));  // continuation without start
    CHECK(asm_.process(sink).code == WsCloseCode::ProtocolError);
    rx.clear();
    asm_.reset();
    feed(rx, bytes_of({0x09, 0x00}));  // fragmented ping
    CHECK(asm_.process(sink).code == WsCloseCode::ProtocolError);
    rx.clear();
    asm_.reset();
    feed(rx, bytes_of({0x01, 0x01, 'a', 0x01, 0x01, 'b'}));  // new text inside fragment
    CHECK(asm_.process(sink).code == WsCloseCode::ProtocolError);
    rx.clear();
    asm_.reset();
    feed(rx, bytes_of({0xC1, 0x01, 'a'}));  // RSV1 (permessage-deflate) not negotiated
    CHECK(asm_.process(sink).code == WsCloseCode::ProtocolError);
  }
  SUBCASE("oversized messages are rejected before the payload arrives") {
    feed(rx, bytes_of({0x82, 0x7E, 0x0F, 0xC8}));  // 4040 > max 4000
    auto err = asm_.process(sink);
    CHECK(err.code == WsCloseCode::MessageTooBig);
    rx.clear();
    asm_.reset();
    // Fragments that add up beyond the limit.
    std::string part(3000, 'p');
    WireBuffer enc(8192);
    ws_encode_frame(enc, WsOpcode::Binary, false, bytes(part), nullptr);
    ws_encode_frame(enc, WsOpcode::Continuation, true, bytes(part), nullptr);
    feed(rx, enc.pending().subspan(0, 3004));
    CHECK_FALSE(asm_.process(sink));
    feed(rx, enc.pending().subspan(3004, 4));  // the second header alone is enough to reject
    CHECK(asm_.process(sink).code == WsCloseCode::MessageTooBig);
  }
  SUBCASE("64 KiB binary frame") {
    RecvBuffer rx64(70000);
    detail::WsMessageAssembler a64(rx64, 69000, false, true);
    std::string payload(65536, '\0');
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(i);
    WireBuffer enc(70000);
    ws_encode_frame(enc, WsOpcode::Binary, true, bytes(payload), nullptr);
    CHECK(enc.size() == 65536 + 10);
    feed(rx64, enc.pending().subspan(0, 30000));
    CHECK_FALSE(a64.process(sink));
    CHECK(sink.messages.empty());
    feed(rx64, enc.pending().subspan(30000));
    CHECK_FALSE(a64.process(sink));
    REQUIRE(sink.messages.size() == 1);
    CHECK(sink.messages[0].second == payload);
  }
  SUBCASE("text messages must be UTF-8, binary messages need not be") {
    feed(rx, bytes_of({0x81, 0x02, 0xC3, 0xA9}));  // "é"
    CHECK_FALSE(asm_.process(sink));
    CHECK(sink.messages.back().second == "\xC3\xA9");
    feed(rx, bytes_of({0x82, 0x02, 0xC3, 0x28}));  // binary: no check
    CHECK_FALSE(asm_.process(sink));
    feed(rx, bytes_of({0x81, 0x02, 0xC3, 0x28}));  // truncated two-byte sequence
    auto err = asm_.process(sink);
    CHECK(err.code == WsCloseCode::InvalidPayload);
    rx.clear();
    asm_.reset();
    feed(rx, bytes_of({0x81, 0x03, 0xED, 0xA0, 0x80}));  // UTF-16 surrogate U+D800
    CHECK(asm_.process(sink).code == WsCloseCode::InvalidPayload);
    rx.clear();
    asm_.reset();
    // A code point split across fragments is valid once the message is complete.
    const std::size_t before = sink.messages.size();
    feed(rx, bytes_of({0x01, 0x01, 0xE2, 0x00, 0x01, 0x82, 0x80, 0x01, 0xAC}));  // "€"
    CHECK_FALSE(asm_.process(sink));
    REQUIRE(sink.messages.size() == before + 1);
    CHECK(sink.messages.back().second == "\xE2\x82\xAC");
    feed(rx, bytes_of({0x01, 0x01, 0xE2, 0x80, 0x01, 0x82}));  // ends mid code point
    CHECK(asm_.process(sink).code == WsCloseCode::InvalidPayload);
  }
  SUBCASE("text is not checked with validate_utf8 off") {
    RecvBuffer rx2(256);
    detail::WsMessageAssembler lax(rx2, 200, false, /*validate_utf8=*/false);
    feed(rx2, bytes_of({0x81, 0x02, 0xC3, 0x28}));
    CHECK_FALSE(lax.process(sink));
    CHECK(sink.messages.back().second == "\xC3\x28");
  }
  SUBCASE("close frames: payload length, code and reason are checked") {
    const auto close_err = [&](std::initializer_list<int> frame) {
      rx.clear();
      asm_.reset();
      feed(rx, bytes_of(frame));
      return asm_.process(sink).code;
    };
    CHECK(close_err({0x88, 0x00}) == WsCloseCode::Normal);              // empty: no code
    CHECK(close_err({0x88, 0x02, 0x03, 0xE8}) == WsCloseCode::Normal);  // 1000
    CHECK(close_err({0x88, 0x02, 0x0B, 0xB8}) == WsCloseCode::Normal);  // 3000
    CHECK(close_err({0x88, 0x01, 0x03}) == WsCloseCode::ProtocolError);
    CHECK(close_err({0x88, 0x02, 0x03, 0xE7}) == WsCloseCode::ProtocolError);  // 999
    CHECK(close_err({0x88, 0x02, 0x03, 0xED}) == WsCloseCode::ProtocolError);  // 1005
    CHECK(close_err({0x88, 0x02, 0x03, 0xF7}) == WsCloseCode::ProtocolError);  // 1015
    CHECK(close_err({0x88, 0x02, 0x13, 0x88}) == WsCloseCode::ProtocolError);  // 5000
    CHECK(close_err({0x88, 0x04, 0x03, 0xE8, 0xC3, 0x28}) == WsCloseCode::InvalidPayload);
    CHECK(ws_close_code_sendable(1014));
    CHECK_FALSE(ws_close_code_sendable(1004));
    CHECK_FALSE(ws_close_code_sendable(2999));
    CHECK(ws_close_code_sendable(4999));
  }
}

TEST_CASE("ws_handshake: accept key known answer and request format") {
  char accept[detail::kWsAcceptLen];
  detail::ws_compute_accept_key("dGhlIHNhbXBsZSBub25jZQ==", accept);
  CHECK(std::string_view(accept, sizeof(accept)) == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");

  char key[detail::kWsKeyLen];
  detail::ws_generate_client_key(key);
  CHECK(key[21] != '\0');
  CHECK(key[22] == '=');
  CHECK(key[23] == '=');

  WireBuffer w(512);
  REQUIRE(detail::ws_build_upgrade_request(w,
                                           "stream.binance.com",
                                           9443,
                                           true,
                                           "/stream?streams=x",
                                           "KEYKEYKEYKEYKEYKEYKEY==",
                                           "X-Extra: 1\r\n"));
  CHECK(sv(w.pending()) ==
        "GET /stream?streams=x HTTP/1.1\r\nHost: stream.binance.com:9443\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Key: "
        "KEYKEYKEYKEYKEYKEYKEY==\r\nSec-WebSocket-Version: 13\r\n"
        "X-Extra: 1\r\n\r\n");
  w.clear();
  REQUIRE(detail::ws_build_upgrade_request(w, "h", 443, true, "/", "k", ""));
  CHECK(sv(w.pending()).find("Host: h\r\n") != std::string_view::npos);
  WireBuffer tiny(16);
  CHECK_FALSE(detail::ws_build_upgrade_request(tiny, "h", 443, true, "/", "k", ""));

  SUBCASE("response validation") {
    HttpResponseHead head;
    const std::string good =
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: WebSocket\r\nConnection: keep-alive, "
        "Upgrade\r\n"
        "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
    REQUIRE(parse_response_head(good, head) == HttpParseStatus::Ok);
    CHECK(detail::ws_check_upgrade_response(head, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == nullptr);
    CHECK(detail::ws_check_upgrade_response(head, "wrong") != nullptr);

    const std::string forbidden = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n";
    REQUIRE(parse_response_head(forbidden, head) == HttpParseStatus::Ok);
    CHECK(std::string_view(detail::ws_check_upgrade_response(head, "x")).find("non-101") !=
          std::string_view::npos);

    const std::string deflate =
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\nSec-WebSocket-Extensions: "
        "permessage-deflate\r\n\r\n";
    REQUIRE(parse_response_head(deflate, head) == HttpParseStatus::Ok);
    CHECK(detail::ws_check_upgrade_response(head, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != nullptr);
  }
}

TEST_CASE("ws_handshake: mask generator produces varying masks") {
  detail::MaskGenerator g;
  std::uint8_t a[4];
  std::uint8_t b[4];
  g.next(a);
  g.next(b);
  CHECK(std::memcmp(a, b, 4) != 0);
}
