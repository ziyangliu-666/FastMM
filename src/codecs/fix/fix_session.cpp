#include "fastmm/codecs/fix/fix_session.hpp"

#include "fastmm/codecs/fix/fix_framer.hpp"

namespace fastmm::codecs::fix {

namespace {

constexpr std::int64_t kNsPerSec = 1'000'000'000;

// StandardHeader tags (FIX 4.4): used to find where the body of an outbound message starts.
constexpr bool is_header_tag(std::int64_t t) noexcept {
  switch (t) {
    case 34:
    case 35:
    case 43:
    case 49:
    case 50:
    case 52:
    case 56:
    case 57:
    case 90:
    case 91:
    case 97:
    case 115:
    case 116:
    case 122:
    case 128:
    case 129:
    case 142:
    case 143:
    case 144:
    case 145:
    case 212:
    case 213:
    case 347:
    case 369:
    case 627:
      return true;
    default:
      return false;
  }
}

std::string_view sub(std::string_view s, std::size_t pos, std::size_t len) noexcept {
  return {s.data() + pos, len};
}

// MsgType, MsgSeqNum, SendingTime and the body range of a message this process built.
struct OutboundHeader {
  std::string_view msg_type;
  std::uint64_t seq = 0;
  std::int64_t sending_ns = 0;
  std::size_t body_begin = 0;
  std::size_t body_end = 0;
  bool ok = false;
};

OutboundHeader scan_outbound(std::string_view m) noexcept {
  OutboundHeader h;
  if (m.size() < 20) return h;
  const std::size_t trailer = m.size() - 7;
  if (m[trailer] != '1' || m[trailer + 1] != '0' || m[trailer + 2] != '=') return h;
  const std::size_t first = m.find(kSoh);
  if (first == std::string_view::npos) return h;
  const std::size_t second = m.find(kSoh, first + 1);
  if (second == std::string_view::npos || second >= trailer) return h;
  std::size_t k = second + 1;
  while (k < trailer) {
    const std::size_t eq = m.find('=', k);
    if (eq == std::string_view::npos || eq >= trailer) return h;
    const std::size_t soh = m.find(kSoh, eq);
    if (soh == std::string_view::npos || soh > trailer) return h;
    const auto t = parse_fix_int(sub(m, k, eq - k));
    if (!t) return h;
    if (!is_header_tag(*t)) break;
    const std::string_view v = sub(m, eq + 1, soh - eq - 1);
    if (*t == tag::kMsgType) {
      h.msg_type = v;
    } else if (*t == tag::kMsgSeqNum) {
      h.seq = static_cast<std::uint64_t>(parse_fix_int(v).value_or(0));
    } else if (*t == tag::kSendingTime) {
      h.sending_ns = parse_utc_timestamp(v).value_or(0);
    }
    k = soh + 1;
  }
  h.body_begin = k;
  h.body_end = trailer;
  h.ok = !h.msg_type.empty() && h.seq != 0;
  return h;
}

void append_uint(FixedString<96>& s, std::uint64_t v) noexcept {
  char tmp[20];
  int n = 0;
  do {
    tmp[n++] = static_cast<char>('0' + v % 10);
    v /= 10;
  } while (v != 0);
  while (n > 0) s.push_back(tmp[--n]);
}

}  // namespace

FixSession::FixSession(const FixSessionConfig& cfg, SendFn send, void* send_ctx)
    : cfg_(cfg),
      send_(send),
      send_ctx_(send_ctx),
      heartbeat_s_(cfg.heartbeat_interval_s),
      store_(cfg.store_max_messages, cfg.store_max_bytes),
      out_(std::make_unique<char[]>(kMaxMessageBytes)) {}

// ---- outbound -------------------------------------------------------------------------------

FixBuilder FixSession::admin(std::string_view msg_type) noexcept {
  FixBuilder b(std::span<char>(out_.get(), kMaxMessageBytes));
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

void FixSession::finish_admin(FixBuilder& b) noexcept {
  const std::size_t n = b.finish();
  if (n == 0) {
    ++stats_.send_failures;
    return;
  }
  ++next_sender_seq_;
  transmit(std::string_view(out_.get(), n));
}

void FixSession::transmit(std::string_view bytes) noexcept {
  send_(send_ctx_, std::span<const char>(bytes.data(), bytes.size()));
  last_tx_ns_ = now_ns();
  ++stats_.tx_messages;
}

bool FixSession::send_app(std::span<const char> wire) noexcept {
  const std::string_view m(wire.data(), wire.size());
  const OutboundHeader h = scan_outbound(m);
  if (!logged_on() || !h.ok || h.seq != next_sender_seq_ || is_admin_msg_type(h.msg_type)) {
    ++stats_.send_failures;
    return false;
  }
  store_.append(h.seq, h.msg_type, h.sending_ns, sub(m, h.body_begin, h.body_end - h.body_begin));
  ++next_sender_seq_;
  ++stats_.tx_app;
  transmit(m);
  return true;
}

void FixSession::send_logon(bool reset) noexcept {
  FixBuilder b = admin(msg::kLogon);
  b.field_int(tag::kEncryptMethod, 0).field_int(tag::kHeartBtInt, heartbeat_s_);
  if (reset) b.field_char(tag::kResetSeqNumFlag, 'Y');
  finish_admin(b);
}

void FixSession::send_heartbeat(std::string_view test_req_id) noexcept {
  FixBuilder b = admin(msg::kHeartbeat);
  if (!test_req_id.empty()) b.field(tag::kTestReqID, test_req_id);
  finish_admin(b);
  ++stats_.heartbeats_sent;
}

void FixSession::send_test_request() noexcept {
  FixedString<96> id("TEST");
  append_uint(id, ++test_req_counter_);
  test_req_id_.assign(id.view());
  FixBuilder b = admin(msg::kTestRequest);
  b.field(tag::kTestReqID, test_req_id_.view());
  finish_admin(b);
  test_req_pending_ = true;
  test_req_sent_ns_ = now_ns();
  ++stats_.test_requests_sent;
}

void FixSession::send_resend_request(std::uint64_t begin, std::uint64_t end) noexcept {
  FixBuilder b = admin(msg::kResendRequest);
  b.field_uint(tag::kBeginSeqNo, begin).field_uint(tag::kEndSeqNo, end);
  finish_admin(b);
  ++stats_.resend_requests_sent;
}

void FixSession::send_reject(std::uint64_t ref_seq,
                             std::uint32_t ref_tag,
                             std::string_view ref_msg_type,
                             std::int64_t reason,
                             std::string_view text) noexcept {
  FixBuilder b = admin(msg::kReject);
  b.field_uint(tag::kRefSeqNum, ref_seq);
  if (ref_tag != 0) b.field_uint(tag::kRefTagID, ref_tag);
  if (!ref_msg_type.empty()) b.field(tag::kRefMsgType, ref_msg_type);
  b.field_int(tag::kSessionRejectReason, reason);
  if (!text.empty()) b.field(tag::kText, text);
  finish_admin(b);
  ++stats_.rejects_sent;
}

void FixSession::send_logout(std::string_view text) noexcept {
  FixBuilder b = admin(msg::kLogout);
  if (!text.empty()) b.field(tag::kText, text);
  finish_admin(b);
  logout_sent_ = true;
  logout_sent_ns_ = now_ns();
  ++stats_.logouts_sent;
}

// ---- lifecycle ------------------------------------------------------------------------------

bool FixSession::logon(std::int64_t now_ns) noexcept {
  if (cfg_.role != FixRole::Initiator || state_ != SessionState::Down) return false;
  now_ns_ = now_ns;
  if (cfg_.reset_seq_num_on_logon) {
    next_sender_seq_ = 1;
    next_target_seq_ = 1;
    store_.reset();
  }
  heartbeat_s_ = cfg_.heartbeat_interval_s;
  test_req_pending_ = false;
  logout_sent_ = false;
  send_logon(cfg_.reset_seq_num_on_logon);
  state_ = SessionState::LoggingOn;
  logon_sent_ns_ = this->now_ns();
  last_rx_ns_ = logon_sent_ns_;
  return true;
}

void FixSession::logout(std::string_view text) noexcept {
  if (!logged_on() || logout_sent_) return;
  send_logout(text);
}

void FixSession::disconnect(FixDisconnectReason why) noexcept {
  state_ = SessionState::Down;
  logout_sent_ = false;
  test_req_pending_ = false;
  last_disconnect_ = why;
  emit(FixSessionEvent::Disconnected, why);
}

void FixSession::emit(FixSessionEvent ev, FixDisconnectReason why) noexcept {
  if (event_fn_ != nullptr) event_fn_(event_ctx_, ev, why);
}

void FixSession::on_timer(std::int64_t now) noexcept {
  now_ns_ = now;
  switch (state_) {
    case SessionState::Down:
      return;
    case SessionState::LoggingOn:
      if (now - logon_sent_ns_ >= cfg_.logon_timeout_ns)
        disconnect(FixDisconnectReason::LogonTimeout);
      return;
    case SessionState::Up:
    case SessionState::Recovering:
      break;
  }
  if (logout_sent_) {
    if (now - logout_sent_ns_ >= cfg_.logout_timeout_ns)
      disconnect(FixDisconnectReason::LogoutTimeout);
    return;
  }
  if (heartbeat_s_ <= 0) return;
  const std::int64_t hb = static_cast<std::int64_t>(heartbeat_s_) * kNsPerSec;
  const std::int64_t limit = hb + hb * static_cast<std::int64_t>(cfg_.transmission_grace_pct) / 100;
  if (test_req_pending_) {
    if (now - test_req_sent_ns_ >= limit) {
      disconnect(FixDisconnectReason::TestRequestTimeout);
      return;
    }
  } else if (now - last_rx_ns_ >= limit) {
    send_test_request();
  }
  if (now - last_tx_ns_ >= hb) send_heartbeat({});
}

// ---- inbound --------------------------------------------------------------------------------

bool FixSession::on_frame(const FrameView& frame) noexcept {
  if (frame.kind != FixFramer::kMessage || frame.payload.empty()) return true;
  ++stats_.rx_messages;
  const FixError err = view_.parse(frame.payload, cfg_.begin_string.view());
  if (err != FixError::None) {
    ++stats_.garbled;
    if (err == FixError::BadBeginString && logged_on()) {
      send_logout("Incorrect BeginString");
      disconnect(FixDisconnectReason::BadBeginString);
    }
    return true;
  }
  last_rx_ns_ = now_ns();
  test_req_pending_ = false;  // any message proves the line is alive
  const std::string_view type = view_.msg_type();
  const auto seq_opt = view_.get_int(tag::kMsgSeqNum);
  if (!seq_opt || *seq_opt <= 0) {
    if (logged_on()) {
      send_logout("MsgSeqNum(34) missing");
      disconnect(FixDisconnectReason::MissingSeqNum);
    }
    return true;
  }
  const auto seq = static_cast<std::uint64_t>(*seq_opt);
  if (cfg_.validate_comp_ids && (view_.get(tag::kSenderCompID) != cfg_.target_comp_id.view() ||
                                 view_.get(tag::kTargetCompID) != cfg_.sender_comp_id.view())) {
    if (logged_on()) {
      send_reject(seq, tag::kSenderCompID, type, session_reject::kCompIdProblem, "CompID problem");
      send_logout("CompID problem");
    }
    disconnect(FixDisconnectReason::CompIdProblem);
    return true;
  }
  if (!logged_on()) return on_frame_not_logged_on(type, seq);

  if (type == msg::kSequenceReset && !view_.get_bool(tag::kGapFillFlag).value_or(false)) {
    handle_sequence_reset(seq);  // Reset mode: MsgSeqNum ignored
    return true;
  }
  const bool poss_dup = view_.get_bool(tag::kPossDupFlag).value_or(false);
  if (seq > next_target_seq_) {
    if (type == msg::kResendRequest) handle_resend_request(seq);
    if (type == msg::kLogout) {
      handle_logout();
      return true;
    }
    begin_recovery(seq);
    return true;
  }
  if (seq < next_target_seq_) {
    if (poss_dup) {
      ++stats_.duplicates_ignored;
      return true;
    }
    FixedString<96> text("MsgSeqNum too low, expecting ");
    append_uint(text, next_target_seq_);
    text.append(" but received ");
    append_uint(text, seq);
    send_logout(text.view());
    disconnect(FixDisconnectReason::SeqNumTooLow);
    return true;
  }
  if (poss_dup) {
    const auto orig = view_.get_timestamp_ns(tag::kOrigSendingTime);
    if (!orig) {
      ++next_target_seq_;
      send_reject(seq,
                  tag::kOrigSendingTime,
                  type,
                  session_reject::kRequiredTagMissing,
                  "OrigSendingTime(122) required with PossDupFlag(43)=Y");
      check_recovered();
      return true;
    }
    const auto sending = view_.get_timestamp_ns(tag::kSendingTime);
    if (sending && *orig > *sending) {
      ++next_target_seq_;
      send_reject(seq,
                  tag::kOrigSendingTime,
                  type,
                  session_reject::kSendingTimeAccuracyProblem,
                  "OrigSendingTime(122) later than SendingTime(52)");
      send_logout("SendingTime accuracy problem");
      disconnect(FixDisconnectReason::SendingTimeAccuracy);
      return true;
    }
  }

  ++next_target_seq_;
  bool app = false;
  if (type.size() != 1) {
    app = true;
  } else {
    switch (type[0]) {
      case '0':  // Heartbeat
        break;
      case '1': {  // TestRequest
        const std::string_view id = view_.get(tag::kTestReqID);
        if (id.empty()) {
          send_reject(seq,
                      tag::kTestReqID,
                      type,
                      session_reject::kRequiredTagMissing,
                      "TestReqID(112) missing");
        } else {
          send_heartbeat(id);
        }
        break;
      }
      case '2':  // ResendRequest
        handle_resend_request(seq);
        break;
      case '3':  // Reject
        ++stats_.rejects_received;
        break;
      case '4': {  // SequenceReset-GapFill
        ++stats_.gap_fills_received;
        const auto nsn = view_.get_int(tag::kNewSeqNo);
        if (!nsn || static_cast<std::uint64_t>(*nsn) <= seq) {
          send_reject(seq,
                      tag::kNewSeqNo,
                      type,
                      session_reject::kValueIncorrect,
                      "NewSeqNo(36) must exceed MsgSeqNum(34)");
        } else {
          next_target_seq_ = static_cast<std::uint64_t>(*nsn);
        }
        break;
      }
      case '5':  // Logout
        handle_logout();
        return true;
      case 'A':  // Logon while logged on
        send_logout("Unexpected Logon");
        disconnect(FixDisconnectReason::BadLogon);
        return true;
      default:
        app = true;
        break;
    }
  }
  check_recovered();
  if (app) ++stats_.rx_app;
  return !app;
}

bool FixSession::on_frame_not_logged_on(std::string_view type, std::uint64_t seq) noexcept {
  if (type == msg::kLogon) {
    if (cfg_.role == FixRole::Acceptor && state_ == SessionState::Down) {
      accept_logon(seq);
    } else if (cfg_.role == FixRole::Initiator && state_ == SessionState::LoggingOn) {
      confirm_logon(seq);
    } else {
      ++stats_.dropped_not_logged_on;
    }
    return true;
  }
  ++stats_.dropped_not_logged_on;
  if (state_ == SessionState::LoggingOn) {
    disconnect(type == msg::kLogout ? FixDisconnectReason::LogoutReceived
                                    : FixDisconnectReason::NotLoggedOn);
  }
  return true;
}

void FixSession::accept_logon(std::uint64_t seq) noexcept {
  const auto hb = view_.get_int(tag::kHeartBtInt);
  if (!hb || *hb < 0 || *hb > 86'400) {
    disconnect(FixDisconnectReason::BadLogon);
    return;
  }
  heartbeat_s_ = static_cast<std::int32_t>(*hb);
  const bool reset = view_.get_bool(tag::kResetSeqNumFlag).value_or(false);
  if (reset) {
    next_target_seq_ = 1;
    next_sender_seq_ = 1;
    store_.reset();
  }
  test_req_pending_ = false;
  logout_sent_ = false;
  if (seq < next_target_seq_) {
    FixedString<96> text("MsgSeqNum too low, expecting ");
    append_uint(text, next_target_seq_);
    text.append(" but received ");
    append_uint(text, seq);
    send_logout(text.view());
    disconnect(FixDisconnectReason::SeqNumTooLow);
    return;
  }
  send_logon(reset);
  after_logon(seq);
}

void FixSession::confirm_logon(std::uint64_t seq) noexcept {
  if (seq < next_target_seq_) {
    FixedString<96> text("MsgSeqNum too low, expecting ");
    append_uint(text, next_target_seq_);
    text.append(" but received ");
    append_uint(text, seq);
    send_logout(text.view());
    disconnect(FixDisconnectReason::SeqNumTooLow);
    return;
  }
  after_logon(seq);
}

void FixSession::after_logon(std::uint64_t seq) noexcept {
  state_ = SessionState::Up;
  last_tx_ns_ = now_ns();
  emit(FixSessionEvent::LoggedOn);
  if (seq > next_target_seq_) {
    begin_recovery(seq);
  } else {
    next_target_seq_ = seq + 1;
  }
}

void FixSession::handle_sequence_reset(std::uint64_t seq) noexcept {
  ++stats_.sequence_resets_received;
  const auto nsn = view_.get_int(tag::kNewSeqNo);
  if (!nsn || *nsn <= 0) {
    send_reject(seq,
                tag::kNewSeqNo,
                msg::kSequenceReset,
                session_reject::kRequiredTagMissing,
                "NewSeqNo(36) missing");
    return;
  }
  const auto n = static_cast<std::uint64_t>(*nsn);
  if (n > next_target_seq_) {
    next_target_seq_ = n;
    check_recovered();
  } else if (n < next_target_seq_) {
    send_reject(seq,
                tag::kNewSeqNo,
                msg::kSequenceReset,
                session_reject::kValueIncorrect,
                "NewSeqNo(36) lower than expected MsgSeqNum");
  }
}

void FixSession::handle_resend_request(std::uint64_t seq) noexcept {
  ++stats_.resend_requests_received;
  const auto begin = view_.get_int(tag::kBeginSeqNo);
  const auto end = view_.get_int(tag::kEndSeqNo);
  if (!begin || !end || *begin <= 0 || *end < 0) {
    send_reject(seq,
                !begin ? tag::kBeginSeqNo : tag::kEndSeqNo,
                msg::kResendRequest,
                session_reject::kValueIncorrect,
                "BeginSeqNo(7)/EndSeqNo(16) invalid");
    return;
  }
  serve_resend(static_cast<std::uint64_t>(*begin), static_cast<std::uint64_t>(*end));
}

void FixSession::serve_resend(std::uint64_t begin, std::uint64_t end) noexcept {
  const std::uint64_t last = next_sender_seq_ - 1;
  if (begin > last) return;
  const std::uint64_t stop = (end == 0 || end > last) ? last : end;
  std::size_t idx = store_.lower_bound(begin);
  std::uint64_t cur = begin;
  const std::int64_t now = now_ns();
  while (cur <= stop) {
    if (idx < store_.size() && store_.at(idx).seq == cur) {
      const MessageStore::Stored s = store_.at(idx);
      FixBuilder b(std::span<char>(out_.get(), kMaxMessageBytes));
      b.begin_header(s.msg_type,
                     cfg_.sender_comp_id.view(),
                     cfg_.target_comp_id.view(),
                     cur,
                     now,
                     true,
                     s.sending_ns,
                     cfg_.begin_string.view());
      b.raw(s.body);
      const std::size_t n = b.finish();
      if (n == 0) {
        ++stats_.send_failures;
      } else {
        transmit(std::string_view(out_.get(), n));
        ++stats_.messages_resent;
      }
      ++idx;
      ++cur;
      continue;
    }
    const std::uint64_t next =
        (idx < store_.size() && store_.at(idx).seq <= stop) ? store_.at(idx).seq : stop + 1;
    FixBuilder b(std::span<char>(out_.get(), kMaxMessageBytes));
    b.begin_header(msg::kSequenceReset,
                   cfg_.sender_comp_id.view(),
                   cfg_.target_comp_id.view(),
                   cur,
                   now,
                   true,
                   now,
                   cfg_.begin_string.view());
    b.field_char(tag::kGapFillFlag, 'Y').field_uint(tag::kNewSeqNo, next);
    const std::size_t n = b.finish();
    if (n == 0) {
      ++stats_.send_failures;
    } else {
      transmit(std::string_view(out_.get(), n));
      ++stats_.gap_fills_sent;
    }
    cur = next;
  }
}

void FixSession::handle_logout() noexcept {
  if (logout_sent_) {
    disconnect(FixDisconnectReason::LogoutComplete);
    return;
  }
  send_logout({});
  disconnect(FixDisconnectReason::LogoutReceived);
}

void FixSession::begin_recovery(std::uint64_t seq) noexcept {
  if (state_ != SessionState::Recovering) {
    state_ = SessionState::Recovering;
    recovery_target_ = seq;
    ++stats_.gaps_detected;
    resend_begin_ = next_target_seq_;
    send_resend_request(next_target_seq_, 0);
    emit(FixSessionEvent::GapDetected);
    return;
  }
  if (seq > recovery_target_) recovery_target_ = seq;
  // Still recovering, yet the resend already moved us forward: this message was sent after the
  // resend was served, so a new gap opened (e.g. a message lost mid-recovery). Ask again from here;
  // without progress it is just an original overtaken by the pending resend.
  if (next_target_seq_ != resend_begin_) {
    ++stats_.gaps_detected;
    resend_begin_ = next_target_seq_;
    send_resend_request(next_target_seq_, 0);
  }
}

void FixSession::check_recovered() noexcept {
  if (state_ == SessionState::Recovering && next_target_seq_ > recovery_target_) {
    state_ = SessionState::Up;
    emit(FixSessionEvent::GapFilled);
  }
}

}  // namespace fastmm::codecs::fix
