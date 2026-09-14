#pragma once
// Shared helpers for the SBE / CME MDP 3.0 tests: hex fixtures, a ring-backed sink that records
// every event, packet construction and engine-side L2 books fed from the event stream.
#include "test_support.hpp"

#include "fastmm/codecs/mdp3/mdp3_encoder.hpp"
#include "fastmm/codecs/mdp3/mdp3_feed.hpp"
#include "fastmm/core/book/l2_book.hpp"
#include "fastmm/core/msg_ring.hpp"
#include "fastmm/venues/event_sink.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fastmm::codecs::mdp3::test {

using Bytes = std::vector<std::byte>;
using Raw = std::vector<std::byte>;

// Hex text: pairs of hex digits, whitespace ignored, '#' starts a comment to the end of line.
inline Bytes hex_fixture(const std::string& rel) {
  const std::string text = fastmm::test::fixture(rel);
  Bytes out;
  int high = -1;
  bool comment = false;
  for (const char c : text) {
    if (c == '\n') {
      comment = false;
      continue;
    }
    if (comment) continue;
    if (c == '#') {
      comment = true;
      continue;
    }
    int v = -1;
    if (c >= '0' && c <= '9') v = c - '0';
    if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
    if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    if (v < 0) continue;
    if (high < 0) {
      high = v;
    } else {
      out.push_back(static_cast<std::byte>(high * 16 + v));
      high = -1;
    }
  }
  REQUIRE(high < 0);
  return out;
}

struct RecordingRing {
  std::unique_ptr<MsgRing> ring;
  venues::EventSink sink;
  explicit RecordingRing(std::size_t bytes = std::size_t{1} << 22)
      : ring(std::make_unique<MsgRing>(bytes)), sink(ring.get(), venues::SinkPolicy::Drop) {}

  std::vector<Raw> drain() {
    std::vector<Raw> out;
    while (const std::byte* p = ring->try_peek()) {
      const auto* h = reinterpret_cast<const EventHeader*>(p);
      out.emplace_back(p, p + h->len);
      ring->release();
    }
    return out;
  }
};

inline const EventHeader& header_of(const Raw& raw) {
  return *reinterpret_cast<const EventHeader*>(raw.data());
}
template <class M>
const M& as(const Raw& raw) {
  return *reinterpret_cast<const M*>(raw.data());
}

inline Price px(const char* s) {
  return Price::from_decimal(s).value();
}

inline schema::MatchEventIndicator end_of_event() {
  schema::MatchEventIndicator m{};
  m.set_end_of_event(true);
  return m;
}

// One packet; `fill(PacketBuilder&)` adds the messages.
template <class F>
Bytes make_packet(std::uint32_t seq, F&& fill, std::uint64_t sending_time = 0) {
  Bytes buf(kMaxPacketBytes);
  PacketBuilder p(buf);
  REQUIRE(p.begin(seq, sending_time));
  fill(p);
  buf.resize(p.size());
  return buf;
}

inline MbpEntry book_entry(std::int32_t security_id,
                           std::uint32_t rpt,
                           schema::MDUpdateAction action,
                           schema::MDEntryTypeBook type,
                           std::uint8_t level,
                           const char* price,
                           std::int32_t qty) {
  MbpEntry e;
  e.security_id = security_id;
  e.rpt_seq = rpt;
  e.action = action;
  e.type = type;
  e.level = level;
  e.price = px(price);
  e.qty = qty;
  return e;
}

inline Bytes book_packet(std::uint32_t seq, const std::vector<MbpEntry>& entries) {
  return make_packet(seq, [&](PacketBuilder& p) {
    REQUIRE(encode_book46(p, 1'000 + seq, end_of_event(), entries));
  });
}

// Engine-side view: one L2Book per InstrumentId, rebuilt from BookDelta / BookSnapshot events.
struct EngineBooks {
  std::vector<std::unique_ptr<L2Book<64>>> books;
  std::vector<ConnState> states;
  std::vector<std::int32_t> reasons;
  std::size_t deltas = 0;
  std::size_t snapshots = 0;
  std::size_t trades = 0;

  explicit EngineBooks(std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) books.push_back(std::make_unique<L2Book<64>>());
  }
  void apply(const std::vector<Raw>& events) {
    for (const Raw& raw : events) {
      const EventHeader& h = header_of(raw);
      switch (h.type) {
        case EventType::BookDelta:
        case EventType::BookSnapshot:
          REQUIRE(h.instrument.value < books.size());
          books[h.instrument.value]->apply_delta(as<BookDeltaMsg>(raw));
          if (h.type == EventType::BookDelta) {
            ++deltas;
          } else {
            ++snapshots;
          }
          break;
        case EventType::ConnectionState:
          states.push_back(as<ConnectionStateMsg>(raw).state);
          reasons.push_back(as<ConnectionStateMsg>(raw).reason_code);
          break;
        case EventType::Trade:
          ++trades;
          break;
        default:
          break;
      }
    }
  }
};

// Levels of an L2Book side, best first.
inline std::vector<Level> l2_levels(const L2Book<64>& b, Side side) {
  std::vector<Level> out;
  b.for_each_level(side, [&](const Level& l) { out.push_back(l); });
  return out;
}
inline std::vector<Level> to_vector(std::span<const Level> s) {
  return {s.begin(), s.end()};
}

}  // namespace fastmm::codecs::mdp3::test
