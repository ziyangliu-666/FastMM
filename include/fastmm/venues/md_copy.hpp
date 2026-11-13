#pragma once
// Redundant market-data connections ([venues.<name>] md_copies). A copy is one more connection
// carrying streams the venue's own market-data connection carries too, for the updates whose id
// increases per instrument (top of book, trades): whichever connection delivers an update first
// pushes it, the others drop it. One push server stalling, or one network thread waiting for its
// core, then costs nothing while another delivers. The copies, and the venue's own feed, ask an
// UpdateFilter; it is lock-free, so the copies can run on network threads of their own
// (md_copy_cpus) and push into rings of their own.
//
//   UpdateFilter  per instrument, the highest update id pushed so far, for tops and for trades
//   MdCopy        one copy: connect(reactor) on the thread that runs it, disconnect(), stats()
//   WsMdCopy<P>   an MdCopy over a WebSocket connection whose frames the connector's parser P
//                 decodes; what a connector returns from Venue::make_md_copy()
//
// Only BookTicker and Trade events pass a copy; anything else the connection carries is ignored.
// After connect() nothing allocates.
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/log.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/net/connection.hpp"
#include "fastmm/net/reactor.hpp"
#include "fastmm/venues/connection_slot.hpp"
#include "fastmm/venues/event_sink.hpp"
#include "fastmm/venues/feed.hpp"
#include "fastmm/venues/symbology.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fastmm::venues {

// A copy's parser buffer: copies carry small frames (a bookTicker is about 200 bytes).
inline constexpr std::size_t kMdCopyParserBytes = 1U << 16;

class UpdateFilter {
 public:
  // True when `seq` is newer than every update of this kind and instrument passed before: the
  // caller pushes it. False for a copy already pushed, an older update, an instrument out of range
  // and a kind the filter does not keep (only BookTicker and Trade).
  bool first(MdKind kind, InstrumentId id, std::uint64_t seq) noexcept {
    std::atomic<std::uint64_t>* slot = at(kind, id);
    if (slot == nullptr) return true;  // not deduplicated: pass
    std::uint64_t cur = slot->load(std::memory_order_relaxed);
    while (seq > cur) {
      if (slot->compare_exchange_weak(cur, seq, std::memory_order_relaxed)) return true;
    }
    return false;
  }
  [[nodiscard]] static constexpr bool keeps(MdKind kind) noexcept {
    return kind == MdKind::BookTicker || kind == MdKind::Trade;
  }

 private:
  std::atomic<std::uint64_t>* at(MdKind kind, InstrumentId id) noexcept {
    if (id.value >= kMaxInstruments) return nullptr;
    if (kind == MdKind::BookTicker) return &tops_[id.value];
    if (kind == MdKind::Trade) return &trades_[id.value];
    return nullptr;
  }
  alignas(64) std::array<std::atomic<std::uint64_t>, kMaxInstruments> tops_{};
  alignas(64) std::array<std::atomic<std::uint64_t>, kMaxInstruments> trades_{};
};

struct MdCopyStats {
  std::uint64_t messages = 0;   // frames received
  std::uint64_t first = 0;      // updates this copy delivered first (pushed)
  std::uint64_t late = 0;       // updates another connection had delivered already
  std::uint64_t dropped = 0;    // the ring was full
  std::uint64_t malformed = 0;  // frames the parser refused
  std::uint64_t reconnects = 0;
};

// One redundant connection. Control path: virtual, called on the thread that runs the copy.
class MdCopy {
 public:
  virtual ~MdCopy() = default;
  virtual void connect(net::Reactor& reactor) = 0;
  virtual void disconnect() = 0;
  // Safe from any thread (relaxed counters): a snapshot for logs and status.
  [[nodiscard]] virtual MdCopyStats stats() const noexcept = 0;
  [[nodiscard]] virtual std::string_view name() const noexcept = 0;
};

// Parser: decode(json, recv, t0, out) -> DecodeResult, as the connectors' market-data parsers.
template <class Parser>
class WsMdCopy final : public MdCopy {
 public:
  // `cfg`: the connection (url with the streams, timeouts, TLS); `subscriptions`: frames sent after
  // the upgrade when cfg.manual_subscribe is set (venues that subscribe by message); the rest
  // construct the parser.
  template <class... ParserArgs>
  WsMdCopy(std::string name,
           net::ConnectionConfig cfg,
           EventSink& sink,
           UpdateFilter& filter,
           std::vector<std::string> subscriptions,
           ParserArgs&&... parser_args)
      : name_(std::move(name)),
        cfg_(std::move(cfg)),
        parser_(std::forward<ParserArgs>(parser_args)...),
        sink_(sink),
        filter_(filter),
        subscriptions_(std::move(subscriptions)) {}
  ~WsMdCopy() override { conn_.reset(); }
  WsMdCopy(const WsMdCopy&) = delete;
  WsMdCopy& operator=(const WsMdCopy&) = delete;

  void connect(net::Reactor& reactor) override {
    conn_.open(reactor, cfg_, handler_);
    conn_.connect();
  }
  void disconnect() override { conn_.close(); }
  [[nodiscard]] MdCopyStats stats() const noexcept override {
    MdCopyStats s;
    s.messages = messages_.load(std::memory_order_relaxed);
    s.first = first_.load(std::memory_order_relaxed);
    s.late = late_.load(std::memory_order_relaxed);
    s.dropped = dropped_.load(std::memory_order_relaxed);
    s.malformed = malformed_.load(std::memory_order_relaxed);
    s.reconnects = reconnects_.load(std::memory_order_relaxed);
    return s;
  }
  [[nodiscard]] std::string_view name() const noexcept override { return name_; }

  // One text frame (public for tests).
  ParseStatus on_text(std::string_view json) noexcept {
    bump(messages_);
    const Cycles t0 = rdtscp();
    const DecodeResult r = parser_.decode(json, wall_now(), t0, scratch_);
    if (r.status != ParseStatus::Ok) {
      if (r.status == ParseStatus::Malformed) bump(malformed_);
      return r.status;
    }
    if (!UpdateFilter::keeps(r.kind)) return ParseStatus::Ignored;
    auto* h = reinterpret_cast<EventHeader*>(scratch_);
    if (!filter_.first(r.kind, h->instrument, h->venue_seq)) {
      bump(late_);
      return ParseStatus::Ok;
    }
    h->t1_delta = static_cast<std::uint32_t>(rdtscp() - t0);
    if (!sink_.push(*h)) {
      bump(dropped_);
      return ParseStatus::Overflow;
    }
    bump(first_);
    return ParseStatus::Ok;
  }

 private:
  struct Handler {
    WsMdCopy* c;
    void on_state(net::ConnState s) { c->on_state(s); }
    void on_text(std::string_view t, std::int64_t) { static_cast<void>(c->on_text(t)); }
    void on_binary(std::span<const std::byte>, std::int64_t) {}
    void on_connected_send_subscriptions() { c->send_subscriptions(); }
  };
  static_assert(net::ConnectionHandler<Handler>);

  // Single writer (the copy's thread): a plain load and store, no locked instruction.
  static void bump(std::atomic<std::uint64_t>& n) noexcept {
    n.store(n.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
  }
  void on_state(net::ConnState s) {
    if (s == net::ConnState::Live) {
      FASTMM_LOG_INFO("{}: -> Live", name_);
    } else if (s == net::ConnState::Backoff) {
      bump(reconnects_);
      FASTMM_LOG_WARN("{}: -> reconnecting", name_);
    }
  }
  void send_subscriptions() {
    for (const std::string& f : subscriptions_) static_cast<void>(conn_.send_text(f));
    conn_.subscribe_done();
  }

  std::string name_;
  net::ConnectionConfig cfg_;
  Parser parser_;
  EventSink& sink_;
  UpdateFilter& filter_;
  std::vector<std::string> subscriptions_;
  Handler handler_{this};
  ConnectionSlot<Handler> conn_;
  std::atomic<std::uint64_t> messages_{0};
  std::atomic<std::uint64_t> first_{0};
  std::atomic<std::uint64_t> late_{0};
  std::atomic<std::uint64_t> dropped_{0};
  std::atomic<std::uint64_t> malformed_{0};
  std::atomic<std::uint64_t> reconnects_{0};
  alignas(64) std::byte scratch_[kDecoderScratchBytes];
};

}  // namespace fastmm::venues
