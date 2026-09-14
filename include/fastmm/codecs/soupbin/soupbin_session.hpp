#pragma once
// SoupBinTCP session layers (fastmm::codecs::soupbin). See soupbin.hpp for the packets.
//
// ClientSession<W> (satisfies SessionLayer)
//   login(now)      sends a Login Request with the requested session and sequence number;
//                   after a disconnect the next login resumes the accepted session at the
//                   next expected sequence number (spec 1.2 "Protocol Flow").
//   on_frame(f)     consumes session packets (A J H Z +) and returns false for Sequenced (S)
//                   and server Unsequenced (U, 4.00+) data so the Decoder sees them; every
//                   S packet advances the implied sequence number.
//   on_timer(now)   sends a Client Heartbeat when nothing was sent for heartbeat_interval
//                   (spec 1.3: "more than 1 second"), and drops the session when nothing was
//                   received for server_timeout or the login is not answered in time.
//
// ServerSession<W, H> (tests, simulation): validates Login Requests (username and password
// case-insensitive, blank or matching session), answers Login Accepted / Rejected, replays
// its sequenced history from the requested sequence number, sends Server Heartbeats, hands
// client Unsequenced Data to the handler and closes on logout or client timeout (4.10: the
// Login Request's Heartbeat Timeout overrides the configured one).
//
// Both write through W::send(bytes) (in-memory pipe, socket wrapper). A session never
// allocates after construction; send() is given a 3-byte header and the payload separately.
// Timestamps are whatever monotonic nanosecond clock the caller drives on_timer() with;
// packet arrival is attributed to the next on_timer() tick.
#include "fastmm/codecs/codec.hpp"
#include "fastmm/codecs/soupbin/soupbin.hpp"
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/fixed_string.hpp"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>

namespace fastmm::codecs::soupbin {

template <class W>
concept ByteWriter = requires(W& w, std::span<const std::byte> bytes) {
  { w.send(bytes) } noexcept -> std::same_as<bool>;
};

enum class CloseReason : std::uint8_t {
  None = 0,
  LoginRejected = 1,
  EndOfSession = 2,
  PeerTimeout = 3,
  LoginTimeout = 4,
  ProtocolError = 5,
  Logout = 6,
  Disconnected = 7,
  WriteFailed = 8,
};

// ---- client ------------------------------------------------------------------------------

struct ClientConfig {
  FixedString<16> username;
  FixedString<16> password;
  FixedString<16> session;     // blank: the currently active session
  std::uint64_t sequence = 1;  // next sequenced message wanted (0: most recent)
  Version version = Version::V400;
  std::uint32_t heartbeat_timeout_ms = 15'000;  // sent in the 4.10 Login Request only
  std::int64_t heartbeat_interval_ns = 1'000'000'000;
  std::int64_t server_timeout_ns = 15'000'000'000;
  std::int64_t login_timeout_ns = 10'000'000'000;
};

struct ClientStats {
  std::uint64_t packets = 0;
  std::uint64_t sequenced = 0;
  std::uint64_t unsequenced = 0;
  std::uint64_t server_heartbeats = 0;
  std::uint64_t heartbeats_sent = 0;
  std::uint64_t debug = 0;
  std::uint64_t logins = 0;
  std::uint64_t protocol_errors = 0;
  std::uint64_t timeouts = 0;
};

template <ByteWriter W>
class ClientSession {
 public:
  ClientSession(W& writer, const ClientConfig& cfg) noexcept
      : w_(writer), cfg_(cfg), session_(cfg.session), next_sequence_(cfg.sequence) {}

  bool login(std::int64_t now_ns) noexcept {
    std::array<std::byte, sizeof(LoginRequestPacket41)> buf{};
    now_ns_ = now_ns;
    const std::size_t n = write_login_request(buf,
                                              cfg_.username.view(),
                                              cfg_.password.view(),
                                              session_.view(),
                                              next_sequence_,
                                              cfg_.version,
                                              cfg_.heartbeat_timeout_ms);
    if (n == 0 || !write({buf.data(), n})) return false;
    state_ = SessionState::LoggingOn;
    reason_ = CloseReason::None;
    end_of_session_ = false;
    login_sent_ns_ = now_ns;
    last_rx_ns_ = now_ns;
    return true;
  }

  bool on_frame(const FrameView& f) noexcept {
    ++stats_.packets;
    rx_activity_ = true;
    const auto type = static_cast<char>(f.kind);
    switch (type) {
      case 'S':
        if (FASTMM_UNLIKELY(state_ != SessionState::Up)) return protocol_error();
        last_sequence_ = next_sequence_++;
        ++stats_.sequenced;
        return false;
      case 'U':
        if (FASTMM_UNLIKELY(state_ != SessionState::Up)) return protocol_error();
        ++stats_.unsequenced;
        return false;
      case 'H':
        ++stats_.server_heartbeats;
        return true;
      case '+':
        ++stats_.debug;
        return true;
      case 'A': {
        LoginAcceptedView v;
        if (state_ != SessionState::LoggingOn || !parse_login_accepted(f.payload, v))
          return protocol_error();
        session_.assign(v.session);
        next_sequence_ = v.sequence;
        state_ = SessionState::Up;
        last_rx_ns_ = now_ns_;
        ++stats_.logins;
        return true;
      }
      case 'J':
        reject_code_ = f.payload.size() == 1 ? static_cast<char>(f.payload[0]) : '?';
        close(CloseReason::LoginRejected);
        return true;
      case 'Z':
        end_of_session_ = true;
        close(CloseReason::EndOfSession);
        return true;
      default:
        return protocol_error();
    }
  }

  void on_timer(std::int64_t now_ns) noexcept {
    now_ns_ = now_ns;
    if (rx_activity_) {
      last_rx_ns_ = now_ns;
      rx_activity_ = false;
    }
    if (state_ == SessionState::LoggingOn) {
      if (now_ns - login_sent_ns_ >= cfg_.login_timeout_ns) {
        ++stats_.timeouts;
        close(CloseReason::LoginTimeout);
      }
      return;
    }
    if (state_ != SessionState::Up) return;
    if (now_ns - last_rx_ns_ >= cfg_.server_timeout_ns) {
      ++stats_.timeouts;
      close(CloseReason::PeerTimeout);
      return;
    }
    if (now_ns - last_tx_ns_ >= cfg_.heartbeat_interval_ns) {
      std::array<std::byte, 3> hb{};
      write_control(hb, PacketType::ClientHeartbeat);
      if (write(hb)) ++stats_.heartbeats_sent;
    }
  }

  [[nodiscard]] SessionState state() const noexcept { return state_; }

  // Unsequenced Data to the server (the higher-level protocol's inbound messages).
  bool send(std::span<const std::byte> msg) noexcept {
    if (FASTMM_UNLIKELY(state_ != SessionState::Up)) return false;
    std::array<std::byte, 3> hdr{};
    if (write_header(hdr, PacketType::UnsequencedData, msg.size()) == 0) return false;
    return write(hdr) && (msg.empty() || write(msg));
  }

  bool logout() noexcept {
    if (state_ == SessionState::Down) return false;
    std::array<std::byte, 3> pkt{};
    write_control(pkt, PacketType::LogoutRequest);
    const bool ok = write(pkt);
    close(CloseReason::Logout);
    return ok;
  }

  // The transport closed. The accepted session and next sequence number are kept so the next
  // login() resumes where the stream stopped.
  void on_disconnect() noexcept {
    if (state_ != SessionState::Down) close(CloseReason::Disconnected);
  }

  [[nodiscard]] std::uint64_t next_sequence() const noexcept { return next_sequence_; }
  // Sequence number of the last Sequenced Data packet returned by on_frame().
  [[nodiscard]] std::uint64_t last_sequence() const noexcept { return last_sequence_; }
  [[nodiscard]] std::string_view session() const noexcept { return session_.view(); }
  [[nodiscard]] char reject_code() const noexcept { return reject_code_; }
  [[nodiscard]] CloseReason close_reason() const noexcept { return reason_; }
  [[nodiscard]] bool end_of_session() const noexcept { return end_of_session_; }
  [[nodiscard]] const ClientStats& stats() const noexcept { return stats_; }

 private:
  bool write(std::span<const std::byte> bytes) noexcept {
    if (FASTMM_UNLIKELY(!w_.send(bytes))) {
      close(CloseReason::WriteFailed);
      return false;
    }
    last_tx_ns_ = now_ns_;
    return true;
  }
  bool protocol_error() noexcept {
    ++stats_.protocol_errors;
    close(CloseReason::ProtocolError);
    return true;
  }
  void close(CloseReason r) noexcept {
    state_ = SessionState::Down;
    reason_ = r;
  }

  W& w_;
  ClientConfig cfg_;
  FixedString<16> session_;
  std::uint64_t next_sequence_;
  std::uint64_t last_sequence_ = 0;
  SessionState state_ = SessionState::Down;
  CloseReason reason_ = CloseReason::None;
  char reject_code_ = ' ';
  bool end_of_session_ = false;
  bool rx_activity_ = false;
  std::int64_t now_ns_ = 0;
  std::int64_t last_rx_ns_ = 0;
  std::int64_t last_tx_ns_ = 0;
  std::int64_t login_sent_ns_ = 0;
  ClientStats stats_{};
};

// ---- server (tests / simulation) ---------------------------------------------------------

template <class H>
concept ServerHandler = requires(H& h, std::span<const std::byte> msg) {
  { h.on_unsequenced(msg) } noexcept;
};

struct ServerConfig {
  FixedString<16> username;
  FixedString<16> password;
  FixedString<16> session;
  std::int64_t heartbeat_interval_ns = 1'000'000'000;
  std::int64_t client_timeout_ns = 15'000'000'000;
  std::int64_t login_timeout_ns = 30'000'000'000;
  std::size_t history_bytes = 1U << 22;
  std::size_t history_messages = 1U << 16;
};

struct ServerStats {
  std::uint64_t packets = 0;
  std::uint64_t logins_accepted = 0;
  std::uint64_t logins_rejected = 0;
  std::uint64_t unsequenced = 0;
  std::uint64_t client_heartbeats = 0;
  std::uint64_t heartbeats_sent = 0;
  std::uint64_t replayed = 0;
  std::uint64_t protocol_errors = 0;
  std::uint64_t timeouts = 0;
};

template <ByteWriter W, ServerHandler H>
class ServerSession {
 public:
  ServerSession(W& writer, H& handler, const ServerConfig& cfg)
      : w_(writer),
        h_(handler),
        cfg_(cfg),
        client_timeout_ns_(cfg.client_timeout_ns),
        bytes_(new std::byte[cfg.history_bytes]),
        offsets_(new std::uint64_t[cfg.history_messages + 1]()) {}

  // A client connected: wait for its Login Request.
  void on_connect(std::int64_t now_ns) noexcept {
    now_ns_ = now_ns;
    connected_ns_ = now_ns;
    last_rx_ns_ = now_ns;
    last_tx_ns_ = now_ns;
    state_ = SessionState::LoggingOn;
    reason_ = CloseReason::None;
    client_timeout_ns_ = cfg_.client_timeout_ns;
  }

  bool on_frame(const FrameView& f) noexcept {
    ++stats_.packets;
    rx_activity_ = true;
    const auto type = static_cast<char>(f.kind);
    switch (type) {
      case 'L':
        return on_login(f.payload);
      case 'R':
        ++stats_.client_heartbeats;
        return true;
      case '+':
        return true;
      case 'U':
        if (FASTMM_UNLIKELY(state_ != SessionState::Up)) return protocol_error();
        ++stats_.unsequenced;
        h_.on_unsequenced(f.payload);
        return true;
      case 'O':
        close(CloseReason::Logout);
        return true;
      default:
        return protocol_error();
    }
  }

  void on_timer(std::int64_t now_ns) noexcept {
    now_ns_ = now_ns;
    if (rx_activity_) {
      last_rx_ns_ = now_ns;
      rx_activity_ = false;
    }
    if (state_ == SessionState::LoggingOn) {
      if (now_ns - connected_ns_ >= cfg_.login_timeout_ns) {
        ++stats_.timeouts;
        close(CloseReason::LoginTimeout);
      }
      return;
    }
    if (state_ != SessionState::Up) return;
    if (now_ns - last_rx_ns_ >= client_timeout_ns_) {
      ++stats_.timeouts;
      close(CloseReason::PeerTimeout);
      return;
    }
    if (now_ns - last_tx_ns_ >= cfg_.heartbeat_interval_ns) {
      std::array<std::byte, 3> hb{};
      write_control(hb, PacketType::ServerHeartbeat);
      if (write(hb)) ++stats_.heartbeats_sent;
    }
  }

  // Appends to the session history (sequence number = previous + 1) and sends it when a
  // client is logged in. False when the history is full or the message too large.
  bool send_sequenced(std::span<const std::byte> msg) noexcept {
    if (msg.size() > kMaxPayload || count_ >= cfg_.history_messages ||
        cfg_.history_bytes - used_ < msg.size())
      return false;
    if (!msg.empty()) std::memcpy(bytes_.get() + used_, msg.data(), msg.size());
    used_ += msg.size();
    ++count_;
    offsets_[count_] = used_;
    if (state_ == SessionState::Up) return send_data(PacketType::SequencedData, msg);
    return true;
  }
  // Best-effort data outside the sequence (SoupBinTCP 4.00+).
  bool send_unsequenced(std::span<const std::byte> msg) noexcept {
    if (state_ != SessionState::Up) return false;
    return send_data(PacketType::UnsequencedData, msg);
  }
  bool send_debug(std::string_view text) noexcept {
    std::array<std::byte, 3> hdr{};
    if (write_header(hdr, PacketType::Debug, text.size()) == 0) return false;
    return write(hdr) && (text.empty() || write(std::as_bytes(std::span<const char>(text))));
  }
  bool end_session() noexcept {
    std::array<std::byte, 3> z{};
    write_control(z, PacketType::EndOfSession);
    const bool ok = write(z);
    ended_ = true;
    close(CloseReason::EndOfSession);
    return ok;
  }
  void on_disconnect() noexcept {
    if (state_ != SessionState::Down) close(CloseReason::Disconnected);
  }

  [[nodiscard]] SessionState state() const noexcept { return state_; }
  [[nodiscard]] CloseReason close_reason() const noexcept { return reason_; }
  [[nodiscard]] std::uint64_t messages() const noexcept { return count_; }
  [[nodiscard]] std::int64_t client_timeout_ns() const noexcept { return client_timeout_ns_; }
  [[nodiscard]] const ServerStats& stats() const noexcept { return stats_; }

 private:
  bool on_login(std::span<const std::byte> payload) noexcept {
    LoginRequestView v;
    if (state_ != SessionState::LoggingOn || !parse_login_request(payload, v))
      return protocol_error();
    std::array<std::byte, sizeof(LoginAcceptedPacket)> buf{};
    if (!equal_ignore_case(v.username, cfg_.username.view()) ||
        !equal_ignore_case(v.password, cfg_.password.view()) || ended_) {
      reject(ended_ ? kRejectSessionNotAvailable : kRejectNotAuthorized);
      return true;
    }
    if (!v.session.empty() && v.session != cfg_.session.view()) {
      reject(kRejectSessionNotAvailable);
      return true;
    }
    // 0 ("the most recently generated message") and anything past the end start with the
    // next message generated.
    std::uint64_t next = v.sequence;
    if (next == 0 || next > count_ + 1) next = count_ + 1;
    if (v.heartbeat_timeout_ms != 0)
      client_timeout_ns_ = static_cast<std::int64_t>(v.heartbeat_timeout_ms) * 1'000'000;
    const std::size_t n = write_login_accepted(buf, cfg_.session.view(), next);
    if (n == 0 || !write({buf.data(), n})) return true;
    state_ = SessionState::Up;
    ++stats_.logins_accepted;
    for (std::uint64_t s = next; s <= count_; ++s) {
      const std::uint64_t b = offsets_[s - 1];
      const std::uint64_t e = offsets_[s];
      if (!send_data(PacketType::SequencedData, {bytes_.get() + b, e - b})) return true;
      ++stats_.replayed;
    }
    return true;
  }
  void reject(char code) noexcept {
    std::array<std::byte, sizeof(LoginRejectedPacket)> buf{};
    write_login_rejected(buf, code);
    write(buf);
    ++stats_.logins_rejected;
    close(CloseReason::LoginRejected);
  }
  bool send_data(PacketType type, std::span<const std::byte> msg) noexcept {
    std::array<std::byte, 3> hdr{};
    if (write_header(hdr, type, msg.size()) == 0) return false;
    return write(hdr) && (msg.empty() || write(msg));
  }
  bool write(std::span<const std::byte> bytes) noexcept {
    if (FASTMM_UNLIKELY(!w_.send(bytes))) {
      close(CloseReason::WriteFailed);
      return false;
    }
    last_tx_ns_ = now_ns_;
    return true;
  }
  bool protocol_error() noexcept {
    ++stats_.protocol_errors;
    close(CloseReason::ProtocolError);
    return true;
  }
  void close(CloseReason r) noexcept {
    state_ = SessionState::Down;
    reason_ = r;
  }

  W& w_;
  H& h_;
  ServerConfig cfg_;
  std::int64_t client_timeout_ns_;
  std::unique_ptr<std::byte[]> bytes_;
  std::unique_ptr<std::uint64_t[]> offsets_;
  std::uint64_t count_ = 0;
  std::uint64_t used_ = 0;
  SessionState state_ = SessionState::Down;
  CloseReason reason_ = CloseReason::None;
  bool rx_activity_ = false;
  bool ended_ = false;
  std::int64_t now_ns_ = 0;
  std::int64_t connected_ns_ = 0;
  std::int64_t last_rx_ns_ = 0;
  std::int64_t last_tx_ns_ = 0;
  ServerStats stats_{};
};

}  // namespace fastmm::codecs::soupbin
