#pragma once
// FixSession: the FIX 4.4 session layer (satisfies codecs::SessionLayer) for initiator and
// acceptor roles.
//
//   Logon(A)          initiator sends EncryptMethod(98)=0, HeartBtInt(108), ResetSeqNumFlag(141)=Y
//                     when configured; the acceptor adopts HeartBtInt, resets both sequence
//                     numbers on 141=Y and answers with its own Logon.
//   Heartbeat(0)      sent after HeartBtInt seconds without sending; echoes TestReqID(112).
//   TestRequest(1)    sent after HeartBtInt + grace (default 20%) without receiving; a second
//                     silent interval disconnects.
//   ResendRequest(2)  sent on MsgSeqNum(34) gaps with BeginSeqNo(7)=expected, EndSeqNo(16)=0
//                     (infinity). Received ones are served from the MessageStore: application
//                     messages go out again with their original MsgSeqNum, PossDupFlag(43)=Y and
//                     OrigSendingTime(122); administrative or unavailable messages are replaced by
//                     SequenceReset-GapFill(123=Y, NewSeqNo 36).
//   Reject(3)         RefSeqNum(45), RefTagID(371), RefMsgType(372), SessionRejectReason(373).
//   SequenceReset(4)  GapFill mode obeys MsgSeqNum checks; Reset mode ignores MsgSeqNum and only
//                     moves the expected number forward (a lower NewSeqNo is rejected, 373=5).
//   Logout(5)         the initiator of a logout waits for the confirming Logout.
//
// Inbound MsgSeqNum rules: equal -> process; higher -> ResendRequest, message discarded (it is
// covered by EndSeqNo=0), state Recovering until the gap is filled; lower with PossDupFlag=Y ->
// ignored duplicate; lower without it -> Logout naming expected and received, then disconnect.
// PossDupFlag=Y without OrigSendingTime -> Reject 373=1; OrigSendingTime later than SendingTime ->
// Reject 373=10, Logout, disconnect. Messages failing BodyLength/CheckSum are ignored.
//
// Sources: OnixS FIX 4.4 dictionary message pages Logon, Heartbeat, TestRequest, ResendRequest,
// Reject, SequenceReset, Logout and the StandardHeader component
// (https://www.onixs.biz/fix-dictionary/4.4/msgType_A_65.html and siblings); the FIX session-level
// test cases and expected behaviours (FIX Trading Community) for the MsgSeqNum and PossDup rules.
// See docs/reference/codecs/fix.md for what could not be checked against the primary PDF.
//
// I/O is by callbacks: SendFn transmits one complete message (it must not call back into the
// session), ClockFn supplies SendingTime (defaults to the time of the last on_timer()). on_frame()
// returns false for application messages the Decoder should see; everything is noexcept and
// allocation-free after construction.
#include "fastmm/codecs/codec.hpp"
#include "fastmm/codecs/fix/fix_builder.hpp"
#include "fastmm/codecs/fix/fix_tags.hpp"
#include "fastmm/codecs/fix/fix_view.hpp"
#include "fastmm/codecs/fix/message_store.hpp"
#include "fastmm/core/fixed_string.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace fastmm::codecs::fix {

enum class FixRole : std::uint8_t { Initiator = 0, Acceptor = 1 };

enum class FixSessionEvent : std::uint8_t {
  LoggedOn = 0,
  Disconnected = 1,
  GapDetected = 2,
  GapFilled = 3,
};

enum class FixDisconnectReason : std::uint8_t {
  None = 0,
  LogoutComplete = 1,  // we sent Logout, peer confirmed
  LogoutReceived = 2,  // peer sent Logout, we confirmed
  LogonTimeout = 3,
  LogoutTimeout = 4,
  TestRequestTimeout = 5,
  SeqNumTooLow = 6,
  BadBeginString = 7,
  CompIdProblem = 8,
  SendingTimeAccuracy = 9,
  NotLoggedOn = 10,  // first message was not a Logon
  BadLogon = 11,
  MissingSeqNum = 12,
};
[[nodiscard]] constexpr std::string_view to_string(FixDisconnectReason r) noexcept {
  switch (r) {
    case FixDisconnectReason::None:
      return "None";
    case FixDisconnectReason::LogoutComplete:
      return "LogoutComplete";
    case FixDisconnectReason::LogoutReceived:
      return "LogoutReceived";
    case FixDisconnectReason::LogonTimeout:
      return "LogonTimeout";
    case FixDisconnectReason::LogoutTimeout:
      return "LogoutTimeout";
    case FixDisconnectReason::TestRequestTimeout:
      return "TestRequestTimeout";
    case FixDisconnectReason::SeqNumTooLow:
      return "SeqNumTooLow";
    case FixDisconnectReason::BadBeginString:
      return "BadBeginString";
    case FixDisconnectReason::CompIdProblem:
      return "CompIdProblem";
    case FixDisconnectReason::SendingTimeAccuracy:
      return "SendingTimeAccuracy";
    case FixDisconnectReason::NotLoggedOn:
      return "NotLoggedOn";
    case FixDisconnectReason::BadLogon:
      return "BadLogon";
    case FixDisconnectReason::MissingSeqNum:
      return "MissingSeqNum";
  }
  return "?";
}

struct FixSessionConfig {
  FixRole role = FixRole::Initiator;
  FixedString<32> sender_comp_id;
  FixedString<32> target_comp_id;
  FixedString<16> begin_string{kBeginString44};
  std::int32_t heartbeat_interval_s = 30;  // initiator's HeartBtInt; the acceptor adopts the peer's
  bool reset_seq_num_on_logon = true;      // initiator sends ResetSeqNumFlag(141)=Y
  std::uint32_t transmission_grace_pct = 20;  // "reasonable transmission time" over HeartBtInt
  std::int64_t logon_timeout_ns = 10'000'000'000;
  std::int64_t logout_timeout_ns = 5'000'000'000;
  bool validate_comp_ids = true;
  std::size_t store_max_messages = 1U << 16;
  std::size_t store_max_bytes = 16U << 20;
};

struct FixSessionStats {
  std::uint64_t rx_messages = 0;
  std::uint64_t rx_app = 0;
  std::uint64_t tx_messages = 0;
  std::uint64_t tx_app = 0;
  std::uint64_t garbled = 0;
  std::uint64_t duplicates_ignored = 0;
  std::uint64_t dropped_not_logged_on = 0;
  std::uint64_t heartbeats_sent = 0;
  std::uint64_t test_requests_sent = 0;
  std::uint64_t resend_requests_sent = 0;
  std::uint64_t resend_requests_received = 0;
  std::uint64_t messages_resent = 0;
  std::uint64_t gap_fills_sent = 0;
  std::uint64_t gap_fills_received = 0;
  std::uint64_t sequence_resets_received = 0;
  std::uint64_t gaps_detected = 0;
  std::uint64_t rejects_sent = 0;
  std::uint64_t rejects_received = 0;
  std::uint64_t logouts_sent = 0;
  std::uint64_t send_failures = 0;
};

class FixSession {
 public:
  using SendFn = void (*)(void* ctx, std::span<const char> bytes) noexcept;
  using ClockFn = std::int64_t (*)(void* ctx) noexcept;
  using EventFn = void (*)(void* ctx, FixSessionEvent ev, FixDisconnectReason why) noexcept;
  static constexpr std::size_t kMaxMessageBytes = 1U << 16;

  FixSession(const FixSessionConfig& cfg, SendFn send, void* send_ctx);
  FixSession(const FixSession&) = delete;
  FixSession& operator=(const FixSession&) = delete;

  void set_clock(ClockFn fn, void* ctx) noexcept {
    clock_fn_ = fn;
    clock_ctx_ = ctx;
  }
  void set_event_callback(EventFn fn, void* ctx) noexcept {
    event_fn_ = fn;
    event_ctx_ = ctx;
  }

  // Initiator: sends Logon and waits for the acceptor's. False unless Down.
  bool logon(std::int64_t now_ns) noexcept;
  // Sends Logout and waits (logout_timeout_ns) for the confirmation.
  void logout(std::string_view text = {}) noexcept;

  // SessionLayer.
  bool on_frame(const FrameView& frame) noexcept;
  void on_timer(std::int64_t now_ns) noexcept;
  [[nodiscard]] SessionState state() const noexcept { return state_; }

  // Transmits an application message built with this session's header (see FixEncoder or
  // begin_app) and MsgSeqNum == next_sender_seq(), and keeps it for resends.
  bool send_app(std::span<const char> wire) noexcept;
  // Starts an application message in `buf` with the standard header for the next MsgSeqNum.
  [[nodiscard]] FixBuilder begin_app(std::span<char> buf, std::string_view msg_type) noexcept {
    FixBuilder b(buf);
    b.begin_header(msg_type,
                   cfg_.sender_comp_id.view(),
                   cfg_.target_comp_id.view(),
                   next_sender_seq_,
                   now_ns(),
                   false,
                   0,
                   cfg_.begin_string.view());
    return b;
  }

  [[nodiscard]] bool logged_on() const noexcept {
    return state_ == SessionState::Up || state_ == SessionState::Recovering;
  }
  [[nodiscard]] std::int64_t now_ns() const noexcept {
    return clock_fn_ != nullptr ? clock_fn_(clock_ctx_) : now_ns_;
  }
  [[nodiscard]] std::uint64_t next_sender_seq() const noexcept { return next_sender_seq_; }
  [[nodiscard]] std::uint64_t next_target_seq() const noexcept { return next_target_seq_; }
  [[nodiscard]] std::string_view sender_comp_id() const noexcept {
    return cfg_.sender_comp_id.view();
  }
  [[nodiscard]] std::string_view target_comp_id() const noexcept {
    return cfg_.target_comp_id.view();
  }
  [[nodiscard]] std::string_view begin_string() const noexcept { return cfg_.begin_string.view(); }
  [[nodiscard]] std::int32_t heartbeat_interval_s() const noexcept { return heartbeat_s_; }
  [[nodiscard]] bool test_request_pending() const noexcept { return test_req_pending_; }
  [[nodiscard]] bool logout_pending() const noexcept { return logout_sent_; }
  [[nodiscard]] FixDisconnectReason last_disconnect_reason() const noexcept {
    return last_disconnect_;
  }
  // The last message on_frame() parsed (valid until the next call); lets a Decoder skip a parse.
  [[nodiscard]] const FixView& view() const noexcept { return view_; }
  [[nodiscard]] const FixSessionStats& stats() const noexcept { return stats_; }
  [[nodiscard]] const MessageStore& store() const noexcept { return store_; }
  [[nodiscard]] const FixSessionConfig& config() const noexcept { return cfg_; }

  // Recovery / test hooks: skip or rewind sequence numbers without sending anything.
  void set_next_sender_seq(std::uint64_t s) noexcept { next_sender_seq_ = s; }
  void set_next_target_seq(std::uint64_t s) noexcept { next_target_seq_ = s; }

 private:
  bool on_frame_not_logged_on(std::string_view type, std::uint64_t seq) noexcept;
  void accept_logon(std::uint64_t seq) noexcept;
  void confirm_logon(std::uint64_t seq) noexcept;
  void after_logon(std::uint64_t seq) noexcept;
  void handle_sequence_reset(std::uint64_t seq) noexcept;
  void handle_resend_request(std::uint64_t seq) noexcept;
  void handle_logout() noexcept;
  void begin_recovery(std::uint64_t seq) noexcept;
  void check_recovered() noexcept;
  void serve_resend(std::uint64_t begin, std::uint64_t end) noexcept;
  void disconnect(FixDisconnectReason why) noexcept;
  void emit(FixSessionEvent ev, FixDisconnectReason why = FixDisconnectReason::None) noexcept;

  FixBuilder admin(std::string_view msg_type) noexcept;
  void finish_admin(FixBuilder& b) noexcept;
  void transmit(std::string_view bytes) noexcept;
  void send_logon(bool reset) noexcept;
  void send_heartbeat(std::string_view test_req_id) noexcept;
  void send_test_request() noexcept;
  void send_resend_request(std::uint64_t begin, std::uint64_t end) noexcept;
  void send_reject(std::uint64_t ref_seq,
                   std::uint32_t ref_tag,
                   std::string_view ref_msg_type,
                   std::int64_t reason,
                   std::string_view text) noexcept;
  void send_logout(std::string_view text) noexcept;

  FixSessionConfig cfg_;
  SendFn send_;
  void* send_ctx_;
  ClockFn clock_fn_ = nullptr;
  void* clock_ctx_ = nullptr;
  EventFn event_fn_ = nullptr;
  void* event_ctx_ = nullptr;

  SessionState state_ = SessionState::Down;
  FixDisconnectReason last_disconnect_ = FixDisconnectReason::None;
  std::uint64_t next_sender_seq_ = 1;
  std::uint64_t next_target_seq_ = 1;
  std::uint64_t recovery_target_ = 0;  // highest MsgSeqNum seen beyond the gap
  std::uint64_t resend_begin_ = 0;     // BeginSeqNo of the last ResendRequest
  std::int32_t heartbeat_s_ = 30;
  std::int64_t now_ns_ = 0;
  std::int64_t last_rx_ns_ = 0;
  std::int64_t last_tx_ns_ = 0;
  std::int64_t logon_sent_ns_ = 0;
  std::int64_t logout_sent_ns_ = 0;
  std::int64_t test_req_sent_ns_ = 0;
  std::uint64_t test_req_counter_ = 0;
  bool test_req_pending_ = false;
  bool logout_sent_ = false;
  FixedString<24> test_req_id_;

  MessageStore store_;
  std::unique_ptr<char[]> out_;
  FixView view_;
  FixSessionStats stats_{};
};

}  // namespace fastmm::codecs::fix
