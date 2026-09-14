#pragma once
// Shared helpers for the FIX codec tests: '|' <-> SOH, fixtures, a ring-backed recording sink, a
// manual clock and an in-memory byte pipe joining an initiator and an acceptor FixSession.
#include "test_support.hpp"

#include "fastmm/codecs/fix/fix.hpp"
#include "fastmm/core/msg_ring.hpp"
#include "fastmm/core/rng.hpp"
#include "fastmm/core/time.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm::codecs::fix::test {

inline std::string soh(std::string_view s) {
  std::string out(s);
  std::replace(out.begin(), out.end(), '|', kSoh);
  return out;
}
inline std::string bars(std::string_view s) {
  std::string out(s);
  std::replace(out.begin(), out.end(), kSoh, '|');
  return out;
}

// "35=...|...|" (everything between BodyLength and CheckSum) -> complete message with real
// BodyLength and CheckSum. `convert` maps '|' to SOH first.
inline std::string wrap(std::string_view fields,
                        std::string_view begin_string = kBeginString44,
                        bool convert = true) {
  const std::string body = convert ? soh(fields) : std::string(fields);
  std::string m =
      "8=" + std::string(begin_string) + kSoh + "9=" + std::to_string(body.size()) + kSoh + body;
  unsigned sum = 0;
  for (const char c : m) sum += static_cast<unsigned char>(c);
  char cks[8];
  std::snprintf(cks, sizeof cks, "10=%03u", sum % 256);
  return m + cks + kSoh;
}

inline std::vector<std::string> fixture_messages(const std::string& name) {
  std::istringstream in(fastmm::test::fixture("fix/" + name));
  std::vector<std::string> out;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    out.push_back(soh(line));
  }
  return out;
}

inline std::span<const std::byte> bytes_of(std::string_view s) {
  return std::as_bytes(std::span<const char>(s.data(), s.size()));
}
inline FrameView frame_of(std::string_view m) {
  return FrameView{bytes_of(m), m.size(), FixFramer::kMessage};
}

// Value of the first `tag` in a raw message ("" when absent).
inline std::string field_of(std::string_view m, std::uint32_t tag) {
  const std::string key = std::to_string(tag) + "=";
  std::size_t pos = 0;
  while (pos < m.size()) {
    if (m.compare(pos, key.size(), key) == 0) {
      const std::size_t end = m.find(kSoh, pos);
      return std::string(m.substr(pos + key.size(), end - pos - key.size()));
    }
    pos = m.find(kSoh, pos);
    if (pos == std::string_view::npos) break;
    ++pos;
  }
  return {};
}
inline std::vector<std::string> of_type(const std::vector<std::string>& log, std::string_view t) {
  std::vector<std::string> out;
  for (const auto& m : log) {
    if (field_of(m, tag::kMsgType) == t) out.push_back(m);
  }
  return out;
}

inline FixSymbolTable test_symbols() {
  FixSymbolTable t;
  REQUIRE(t.add("BTCUSDT", InstrumentId{0}));
  REQUIRE(t.add("ETHUSDT", InstrumentId{1}));
  return t;
}

// Copies every committed message out of a MsgRing-backed sink.
struct RecordingSink {
  MsgRing ring;
  venues::EventSink sink;
  explicit RecordingSink(std::size_t bytes = 1U << 20)
      : ring(bytes), sink(&ring, venues::SinkPolicy::Drop, 10) {}

  std::vector<std::vector<std::byte>> drain() {
    std::vector<std::vector<std::byte>> out;
    while (const std::byte* p = ring.try_peek()) {
      const auto* h = reinterpret_cast<const EventHeader*>(p);
      out.emplace_back(p, p + h->len);
      ring.release();
    }
    return out;
  }
  template <class M>
  static const M& as(const std::vector<std::byte>& raw) {
    return *reinterpret_cast<const M*>(raw.data());
  }
  static EventType type_of(const std::vector<std::byte>& raw) {
    return reinterpret_cast<const EventHeader*>(raw.data())->type;
  }
};

struct ManualClock {
  std::int64_t ns = 1'789'344'000'000'000'000;  // 2026-09-14T00:00:00Z
  static std::int64_t read(void* self) noexcept { return static_cast<ManualClock*>(self)->ns; }
};

inline FixSessionConfig initiator_config() {
  FixSessionConfig c;
  c.role = FixRole::Initiator;
  c.sender_comp_id = "CLIENT";
  c.target_comp_id = "VENUE";
  c.heartbeat_interval_s = 30;
  c.store_max_messages = 1U << 14;
  c.store_max_bytes = 4U << 20;
  return c;
}
inline FixSessionConfig acceptor_config() {
  FixSessionConfig c = initiator_config();
  c.role = FixRole::Acceptor;
  c.sender_comp_id = "VENUE";
  c.target_comp_id = "CLIENT";
  return c;
}

// One direction of the pipe. Every message a session sends is queued and logged; delivery can be
// held, and a drop filter can discard whole messages on the way.
struct Wire {
  std::deque<std::string> queue;
  std::vector<std::string> log;
  std::function<bool(const std::string&)> drop;
  bool hold = false;
  std::size_t dropped = 0;
  static void send(void* self, std::span<const char> bytes) noexcept {
    auto* w = static_cast<Wire*>(self);
    w->queue.emplace_back(bytes.data(), bytes.size());
    w->log.emplace_back(bytes.data(), bytes.size());
  }
};

// A session behind a FixFramer; application messages go to on_app.
struct Endpoint {
  FixSession session;
  FixFramer framer;
  std::string rx;
  std::vector<std::string> app_log;
  std::function<void(const FixView&)> on_app;

  Endpoint(const FixSessionConfig& cfg, Wire& out, ManualClock& clock)
      : session(cfg, &Wire::send, &out) {
    session.set_clock(&ManualClock::read, &clock);
  }
  void receive(std::string_view chunk) {
    rx.append(chunk);
    for (;;) {
      const FrameView f = framer.next(bytes_of(rx));
      if (!f.complete()) break;
      if (!session.on_frame(f)) {
        app_log.emplace_back(session.view().raw());
        if (on_app) on_app(session.view());
      }
      rx.erase(0, f.consumed);
    }
  }
};

// OrderCommand for an outbound message. Not inlined: when gcc inlines OrderCommand::from() for
// a 128-byte OutNewOrderMsg it also analyses the OutReplaceMsg branch and reports -Warray-bounds.
FASTMM_NOINLINE inline venues::OrderCommand command_of(const EventHeader& h) {
  return *venues::OrderCommand::from(h);
}

struct Link {
  ManualClock clock;
  Wire to_acceptor;
  Wire to_initiator;
  std::unique_ptr<Endpoint> initiator;
  std::unique_ptr<Endpoint> acceptor;
  Xoshiro256ss rng{1};
  bool chunked = true;  // deliver in random 1..64 byte chunks

  explicit Link(const FixSessionConfig& icfg = initiator_config(),
                const FixSessionConfig& acfg = acceptor_config())
      : initiator(std::make_unique<Endpoint>(icfg, to_acceptor, clock)),
        acceptor(std::make_unique<Endpoint>(acfg, to_initiator, clock)) {}

  bool deliver(Wire& w, Endpoint& e) {
    if (w.hold || w.queue.empty()) return false;
    const std::string m = std::move(w.queue.front());
    w.queue.pop_front();
    if (w.drop && w.drop(m)) {
      ++w.dropped;
      return true;
    }
    if (!chunked) {
      e.receive(m);
      return true;
    }
    std::size_t i = 0;
    while (i < m.size()) {
      const std::uint64_t room = std::min<std::uint64_t>(m.size() - i, 64);
      const std::size_t n = 1 + rng.uniform(room);
      e.receive(std::string_view(m).substr(i, n));
      i += n;
    }
    return true;
  }
  void pump() {
    for (int step = 0; step < 10'000'000; ++step) {
      const bool a = deliver(to_acceptor, *acceptor);
      const bool b = deliver(to_initiator, *initiator);
      if (!a && !b) return;
    }
    FAIL("pump did not settle");
  }
  void advance(Duration d) {
    clock.ns += d.ns;
    initiator->session.on_timer(clock.ns);
    acceptor->session.on_timer(clock.ns);
  }
  void logon() {
    REQUIRE(initiator->session.logon(clock.ns));
    pump();
    REQUIRE(initiator->session.state() == SessionState::Up);
    REQUIRE(acceptor->session.state() == SessionState::Up);
  }
};

// Sends an application message ("tag=value|..." after the standard header) from `s`.
inline bool send_app_fields(FixSession& s, std::string_view msg_type, std::string_view fields) {
  char buf[4096];
  FixBuilder b = s.begin_app(std::span<char>(buf), msg_type);
  const std::string body = soh(fields);
  b.raw(body);
  const std::size_t n = b.finish();
  return n != 0 && s.send_app(std::span<const char>(buf, n));
}

}  // namespace fastmm::codecs::fix::test
