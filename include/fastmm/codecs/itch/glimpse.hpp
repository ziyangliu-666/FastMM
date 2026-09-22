#pragma once
// Nasdaq GLIMPSE 5.0 (fastmm::codecs::itch::glimpse): the point-to-point snapshot of the
// TotalView-ITCH book over SoupBinTCP.
//
// Source: "GLIMPSE 5.0" (NQGlimpseSpecification.pdf). After a SoupBinTCP login for sequence 1
// the server sends, as Sequenced Data, one ITCH message per packet:
//
//   Stock Directory ('R') for every symbol, Stock Trading Action ('H') with the current state,
//   Add Order ('A' / 'F') for every displayable order at the time of the login request, then
//   End of Snapshot ('G') | Sequence Number(20, ASCII numeric): the TotalView-ITCH sequence
//   number to continue from ("process real-time TotalView-ITCH messages beginning with the
//   message sequence number reflected in this snapshot message").
//
// The message layouts other than 'G' are the ITCH 5.0 ones (itch_messages.hpp).
//
// GlimpseClient<W, H> drives a soupbin::ClientSession<W> over a byte stream: login() asks for
// sequence 1, on_bytes() consumes SoupBinTCP packets and hands every snapshot message to
// H::on_snapshot_message(msg) and the End of Snapshot sequence number to
// H::on_snapshot_end(next_itch_seq). With logout_after_snapshot the client logs out once the
// snapshot is complete. Nothing allocates after construction.
#include "fastmm/codecs/codec.hpp"
#include "fastmm/codecs/soupbin/soupbin.hpp"
#include "fastmm/codecs/soupbin/soupbin_session.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>

namespace fastmm::codecs::itch::glimpse {

inline constexpr char kEndOfSnapshot = 'G';
inline constexpr std::size_t kSequenceChars = 20;
inline constexpr std::size_t kEndOfSnapshotLength = 1 + kSequenceChars;

#pragma pack(push, 1)
struct EndOfSnapshot {  // 'G' 1.7
  char type;
  char sequence_number[20];  // right-aligned, left padded with spaces (as SoupBinTCP numerics)
};
#pragma pack(pop)
static_assert(sizeof(EndOfSnapshot) == kEndOfSnapshotLength);

// Bytes written (21), or 0 when `out` is too small.
std::size_t write_end_of_snapshot(std::span<std::byte> out, std::uint64_t next_sequence) noexcept;
// False unless `msg` is a 21-byte 'G' message with a numeric sequence (spaces or zeros padding).
[[nodiscard]] bool parse_end_of_snapshot(std::span<const std::byte> msg,
                                         std::uint64_t& next_sequence) noexcept;

template <class H>
concept SnapshotHandler = requires(H& h, std::span<const std::byte> msg, std::uint64_t seq) {
  { h.on_snapshot_message(msg) } noexcept;
  { h.on_snapshot_end(seq) } noexcept;
};

enum class GlimpseState : std::uint8_t {
  Idle = 0,       // before login() or after a failure (see close_reason())
  LoggingIn = 1,  // Login Request sent
  Receiving = 2,  // logged in, snapshot in progress
  Complete = 3,   // End of Snapshot received
};

struct GlimpseConfig {
  soupbin::ClientConfig session;  // sequence is forced to 1
  bool logout_after_snapshot = true;
};

struct GlimpseStats {
  std::uint64_t messages = 0;  // snapshot messages handed to the handler ('G' excluded)
  std::uint64_t malformed = 0;
  std::uint64_t snapshots = 0;
};

template <soupbin::ByteWriter W, SnapshotHandler H>
class GlimpseClient {
 public:
  GlimpseClient(W& writer, H& handler, const GlimpseConfig& cfg) noexcept
      : h_(handler), cfg_(cfg), session_(writer, with_sequence_one(cfg.session)) {}

  bool login(std::int64_t now_ns) noexcept {
    if (!session_.login(now_ns)) return false;
    state_ = GlimpseState::LoggingIn;
    end_sequence_ = 0;
    return true;
  }

  // Consumes whole SoupBinTCP packets from `in`; returns the bytes consumed (a partial packet is
  // left for the next call).
  std::size_t on_bytes(std::span<const std::byte> in) noexcept {
    std::size_t used = 0;
    const soupbin::SoupBinFramer framer;
    while (used < in.size()) {
      const FrameView f = framer.next(in.subspan(used));
      if (!f.complete()) break;
      used += f.consumed;
      on_packet(f);
    }
    return used;
  }

  void on_timer(std::int64_t now_ns) noexcept {
    session_.on_timer(now_ns);
    fail_if_down();
  }
  void on_disconnect() noexcept {
    session_.on_disconnect();
    fail_if_down();
  }

  [[nodiscard]] GlimpseState state() const noexcept { return state_; }
  [[nodiscard]] bool complete() const noexcept { return state_ == GlimpseState::Complete; }
  // Sequence number of End of Snapshot (0 until it arrived).
  [[nodiscard]] std::uint64_t end_sequence() const noexcept { return end_sequence_; }
  [[nodiscard]] soupbin::CloseReason close_reason() const noexcept {
    return session_.close_reason();
  }
  [[nodiscard]] const soupbin::ClientSession<W>& session() const noexcept { return session_; }
  [[nodiscard]] const GlimpseStats& stats() const noexcept { return stats_; }

 private:
  static soupbin::ClientConfig with_sequence_one(soupbin::ClientConfig c) noexcept {
    c.sequence = 1;  // the spec: "users must login to SoupBinTCP for sequence 1"
    return c;
  }

  void on_packet(const FrameView& f) noexcept {
    if (session_.on_frame(f)) {  // session packet
      if (state_ == GlimpseState::LoggingIn && session_.state() == SessionState::Up)
        state_ = GlimpseState::Receiving;
      fail_if_down();
      return;
    }
    if (static_cast<char>(f.kind) != 'S' || state_ != GlimpseState::Receiving) return;
    if (!f.payload.empty() && static_cast<char>(f.payload[0]) == kEndOfSnapshot) {
      std::uint64_t seq = 0;
      if (!parse_end_of_snapshot(f.payload, seq)) {
        ++stats_.malformed;
        return;
      }
      end_sequence_ = seq;
      state_ = GlimpseState::Complete;
      ++stats_.snapshots;
      h_.on_snapshot_end(seq);
      if (cfg_.logout_after_snapshot) session_.logout();
      return;
    }
    ++stats_.messages;
    h_.on_snapshot_message(f.payload);
  }

  void fail_if_down() noexcept {
    if (session_.state() == SessionState::Down && state_ != GlimpseState::Complete)
      state_ = GlimpseState::Idle;
  }

  H& h_;
  GlimpseConfig cfg_;
  soupbin::ClientSession<W> session_;
  GlimpseState state_ = GlimpseState::Idle;
  std::uint64_t end_sequence_ = 0;
  GlimpseStats stats_{};
};

}  // namespace fastmm::codecs::itch::glimpse
