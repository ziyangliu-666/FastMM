#include "fix_test_util.hpp"

using namespace fastmm;
using namespace fastmm::codecs;
using namespace fastmm::codecs::fix;
using namespace fastmm::codecs::fix::test;

namespace {

static_assert(SessionLayer<FixSession>);

std::string heartbeat_from_venue(std::uint64_t seq, std::string_view extra = {}) {
  return wrap("35=0|49=VENUE|56=CLIENT|34=" + std::to_string(seq) + "|52=20260914-08:30:15.000|" +
              std::string(extra));
}

std::string exec_report_fields(std::string_view exec_id) {
  return "37=1|11=fm000100000001|17=" + std::string(exec_id) +
         "|150=0|39=0|55=BTCUSDT|54=1|151=1|14=0|6=0|";
}

}  // namespace

TEST_CASE("codecs.fix.session: logon, heartbeat, test request and logout") {
  Link link;
  FixSession& ini = link.initiator->session;
  FixSession& acc = link.acceptor->session;
  CHECK(ini.state() == SessionState::Down);
  CHECK(acc.state() == SessionState::Down);
  CHECK_FALSE(acc.logon(link.clock.ns));  // acceptors wait

  REQUIRE(ini.logon(link.clock.ns));
  CHECK(ini.state() == SessionState::LoggingOn);
  link.pump();
  CHECK(ini.state() == SessionState::Up);
  CHECK(acc.state() == SessionState::Up);
  REQUIRE(link.to_acceptor.log.size() == 1);
  const std::string& logon = link.to_acceptor.log[0];
  CHECK(field_of(logon, tag::kMsgType) == "A");
  CHECK(field_of(logon, tag::kEncryptMethod) == "0");
  CHECK(field_of(logon, tag::kHeartBtInt) == "30");
  CHECK(field_of(logon, tag::kResetSeqNumFlag) == "Y");
  CHECK(field_of(logon, tag::kMsgSeqNum) == "1");
  REQUIRE(link.to_initiator.log.size() == 1);
  CHECK(field_of(link.to_initiator.log[0], tag::kMsgType) == "A");
  CHECK(field_of(link.to_initiator.log[0], tag::kResetSeqNumFlag) == "Y");
  CHECK(acc.heartbeat_interval_s() == 30);
  CHECK(ini.next_sender_seq() == 2);
  CHECK(ini.next_target_seq() == 2);
  CHECK(acc.next_target_seq() == 2);

  // Heartbeats after HeartBtInt without sending.
  link.advance(seconds(29));
  CHECK(ini.stats().heartbeats_sent == 0);
  link.advance(seconds(1));
  link.pump();
  CHECK(ini.stats().heartbeats_sent == 1);
  CHECK(acc.stats().heartbeats_sent == 1);
  CHECK(ini.stats().test_requests_sent == 0);
  CHECK(ini.next_target_seq() == 3);
  CHECK(acc.next_target_seq() == 3);

  // Nothing received for HeartBtInt + 20%: TestRequest, answered by Heartbeat(TestReqID).
  link.clock.ns += seconds(35).ns;
  ini.on_timer(link.clock.ns);
  CHECK(ini.stats().test_requests_sent == 0);
  link.clock.ns += seconds(1).ns;
  ini.on_timer(link.clock.ns);
  CHECK(ini.stats().test_requests_sent == 1);
  CHECK(ini.test_request_pending());
  CHECK(field_of(of_type(link.to_acceptor.log, "1").back(), tag::kTestReqID) == "TEST1");
  link.pump();
  const auto beats = of_type(link.to_initiator.log, "0");
  REQUIRE_FALSE(beats.empty());
  CHECK(field_of(beats.back(), tag::kTestReqID) == "TEST1");
  CHECK_FALSE(ini.test_request_pending());
  CHECK(ini.state() == SessionState::Up);

  // Logout handshake.
  ini.logout("bye");
  CHECK(ini.logout_pending());
  link.pump();
  CHECK(acc.state() == SessionState::Down);
  CHECK(acc.last_disconnect_reason() == FixDisconnectReason::LogoutReceived);
  CHECK(ini.state() == SessionState::Down);
  CHECK(ini.last_disconnect_reason() == FixDisconnectReason::LogoutComplete);
  CHECK(field_of(of_type(link.to_acceptor.log, "5").back(), tag::kText) == "bye");

  // A new logon with ResetSeqNumFlag starts both sides at 1 again.
  link.logon();
  CHECK(ini.next_sender_seq() == 2);
  CHECK(acc.next_sender_seq() == 2);
  CHECK(acc.next_target_seq() == 2);
}

TEST_CASE("codecs.fix.session: an unanswered test request disconnects") {
  Link link;
  link.logon();
  FixSession& ini = link.initiator->session;
  std::vector<FixSessionEvent> events;
  ini.set_event_callback(
      [](void* ctx, FixSessionEvent ev, FixDisconnectReason) noexcept {
        static_cast<std::vector<FixSessionEvent>*>(ctx)->push_back(ev);
      },
      &events);
  link.to_initiator.hold = true;
  link.clock.ns += seconds(36).ns;
  ini.on_timer(link.clock.ns);
  REQUIRE(ini.test_request_pending());
  link.clock.ns += seconds(35).ns;
  ini.on_timer(link.clock.ns);
  CHECK(ini.state() == SessionState::Up);
  link.clock.ns += seconds(1).ns;
  ini.on_timer(link.clock.ns);
  CHECK(ini.state() == SessionState::Down);
  CHECK(ini.last_disconnect_reason() == FixDisconnectReason::TestRequestTimeout);
  CHECK(events == std::vector<FixSessionEvent>{FixSessionEvent::Disconnected});
}

TEST_CASE("codecs.fix.session: logon timeout and messages before logon") {
  Link link;
  FixSession& ini = link.initiator->session;
  FixSession& acc = link.acceptor->session;
  REQUIRE(ini.logon(link.clock.ns));
  link.clock.ns += seconds(9).ns;
  ini.on_timer(link.clock.ns);
  CHECK(ini.state() == SessionState::LoggingOn);
  link.clock.ns += seconds(1).ns;
  ini.on_timer(link.clock.ns);
  CHECK(ini.state() == SessionState::Down);
  CHECK(ini.last_disconnect_reason() == FixDisconnectReason::LogonTimeout);

  // An acceptor ignores anything but Logon while down.
  link.acceptor->receive(wrap("35=0|49=CLIENT|56=VENUE|34=1|52=20260914-08:30:15.000|"));
  CHECK(acc.state() == SessionState::Down);
  CHECK(acc.stats().dropped_not_logged_on == 1);
  CHECK(link.to_initiator.log.empty());
  // A Logon without HeartBtInt is refused.
  link.acceptor->receive(wrap("35=A|49=CLIENT|56=VENUE|34=1|52=20260914-08:30:15.000|98=0|"));
  CHECK(acc.state() == SessionState::Down);
  CHECK(acc.last_disconnect_reason() == FixDisconnectReason::BadLogon);
}

TEST_CASE("codecs.fix.session: gap triggers ResendRequest, resend with PossDup and GapFill") {
  Link link;
  link.logon();
  FixSession& ini = link.initiator->session;
  FixSession& acc = link.acceptor->session;
  std::vector<std::string> received;
  link.initiator->on_app = [&](const FixView& v) {
    received.emplace_back(v.get(tag::kExecID));
    CHECK(v.get_bool(tag::kPossDupFlag) == true);  // every one arrives as a retransmission
  };

  const std::uint64_t s1 = acc.next_sender_seq();
  REQUIRE(send_app_fields(acc, msg::kExecutionReport, exec_report_fields("E1")));  // s1
  link.clock.ns += seconds(30).ns;
  acc.on_timer(link.clock.ns);  // Heartbeat, s1 + 1
  REQUIRE(acc.stats().heartbeats_sent == 1);
  REQUIRE(send_app_fields(acc, msg::kExecutionReport, exec_report_fields("E2")));  // s1 + 2
  REQUIRE(send_app_fields(acc, msg::kExecutionReport, exec_report_fields("E3")));  // s1 + 3
  link.to_initiator.drop = [&](const std::string& m) {
    const std::uint64_t seq = std::stoull(field_of(m, tag::kMsgSeqNum));
    return field_of(m, tag::kPossDupFlag).empty() && (seq == s1 || seq == s1 + 1);
  };
  link.pump();

  CHECK(received == std::vector<std::string>{"E1", "E2", "E3"});
  CHECK(link.to_initiator.dropped == 2);
  CHECK(ini.stats().gaps_detected == 1);
  CHECK(ini.stats().resend_requests_sent == 1);
  const auto rr = of_type(link.to_acceptor.log, "2");
  REQUIRE(rr.size() == 1);
  CHECK(field_of(rr[0], tag::kBeginSeqNo) == std::to_string(s1));
  CHECK(field_of(rr[0], tag::kEndSeqNo) == "0");
  CHECK(acc.stats().resend_requests_received == 1);
  CHECK(acc.stats().messages_resent == 3);
  CHECK(acc.stats().gap_fills_sent == 1);

  std::string original;
  std::string resent;
  for (const auto& m : link.to_initiator.log) {
    if (field_of(m, tag::kExecID) != "E1") continue;
    (field_of(m, tag::kPossDupFlag) == "Y" ? resent : original) = m;
  }
  REQUIRE_FALSE(original.empty());
  REQUIRE_FALSE(resent.empty());
  CHECK(field_of(resent, tag::kMsgSeqNum) == std::to_string(s1));
  CHECK(field_of(resent, tag::kOrigSendingTime) == field_of(original, tag::kSendingTime));
  CHECK(field_of(resent, tag::kSendingTime) != field_of(original, tag::kSendingTime));
  // Identical body: everything after the standard header.
  CHECK(original.substr(original.find("37="))
            .substr(0, original.find("10=") - original.find("37=")) ==
        resent.substr(resent.find("37=")).substr(0, resent.find("10=") - resent.find("37=")));

  const auto gap_fills = of_type(link.to_initiator.log, "4");
  REQUIRE(gap_fills.size() == 1);
  CHECK(field_of(gap_fills[0], tag::kMsgSeqNum) == std::to_string(s1 + 1));
  CHECK(field_of(gap_fills[0], tag::kGapFillFlag) == "Y");
  CHECK(field_of(gap_fills[0], tag::kNewSeqNo) == std::to_string(s1 + 2));
  CHECK(field_of(gap_fills[0], tag::kPossDupFlag) == "Y");
  CHECK_FALSE(field_of(gap_fills[0], tag::kOrigSendingTime).empty());

  CHECK(ini.state() == SessionState::Up);
  CHECK(ini.next_target_seq() == acc.next_sender_seq());
  CHECK(ini.stats().gap_fills_received == 1);
  CHECK(ini.stats().duplicates_ignored == 0);

  // A late duplicate of an already processed message is ignored.
  link.initiator->receive(resent);
  CHECK(ini.stats().duplicates_ignored == 1);
  CHECK(received.size() == 3);
}

TEST_CASE("codecs.fix.session: a full MessageStore gap-fills what it did not keep") {
  FixSessionConfig acfg = acceptor_config();
  acfg.store_max_messages = 2;
  Link link(initiator_config(), acfg);
  link.logon();
  FixSession& ini = link.initiator->session;
  FixSession& acc = link.acceptor->session;
  std::vector<std::string> received;
  link.initiator->on_app = [&](const FixView& v) { received.emplace_back(v.get(tag::kExecID)); };

  const std::uint64_t s1 = acc.next_sender_seq();
  for (int i = 1; i <= 5; ++i) {
    REQUIRE(
        send_app_fields(acc, msg::kExecutionReport, exec_report_fields("E" + std::to_string(i))));
  }
  CHECK(acc.store().size() == 2);
  CHECK(acc.store().dropped() == 3);
  link.to_initiator.drop = [&](const std::string& m) {
    return field_of(m, tag::kPossDupFlag).empty() &&
           std::stoull(field_of(m, tag::kMsgSeqNum)) < s1 + 4;
  };
  link.pump();
  CHECK(received == std::vector<std::string>{"E1", "E2"});
  CHECK(acc.stats().messages_resent == 2);
  CHECK(acc.stats().gap_fills_sent == 1);
  const auto gap_fills = of_type(link.to_initiator.log, "4");
  REQUIRE(gap_fills.size() == 1);
  CHECK(field_of(gap_fills[0], tag::kMsgSeqNum) == std::to_string(s1 + 2));
  CHECK(field_of(gap_fills[0], tag::kNewSeqNo) == std::to_string(s1 + 5));
  CHECK(ini.state() == SessionState::Up);
  CHECK(ini.next_target_seq() == s1 + 5);
}

TEST_CASE("codecs.fix.session: MsgSeqNum lower than expected without PossDup logs out") {
  Link link;
  link.logon();
  FixSession& ini = link.initiator->session;
  FixSession& acc = link.acceptor->session;
  acc.set_next_sender_seq(1);
  REQUIRE(send_app_fields(acc, msg::kExecutionReport, exec_report_fields("E1")));
  link.pump();
  CHECK(ini.state() == SessionState::Down);
  CHECK(ini.last_disconnect_reason() == FixDisconnectReason::SeqNumTooLow);
  const auto logouts = of_type(link.to_acceptor.log, "5");
  REQUIRE(logouts.size() == 1);
  CHECK(field_of(logouts[0], tag::kText) == "MsgSeqNum too low, expecting 2 but received 1");
  CHECK(link.initiator->app_log.empty());
  CHECK(acc.state() == SessionState::Down);
}

TEST_CASE("codecs.fix.session: PossDupFlag rules for OrigSendingTime") {
  Link link;
  link.logon();
  FixSession& ini = link.initiator->session;
  Endpoint& ep = *link.initiator;
  const std::uint64_t exp = ini.next_target_seq();

  // A duplicate below the expected number is ignored.
  ep.receive(heartbeat_from_venue(1, "43=Y|122=20260914-08:30:14.000|"));
  CHECK(ini.stats().duplicates_ignored == 1);
  CHECK(ini.next_target_seq() == exp);

  // PossDupFlag=Y without OrigSendingTime: Reject 373=1 on tag 122, sequence consumed.
  ep.receive(heartbeat_from_venue(exp, "43=Y|"));
  auto rejects = of_type(link.to_acceptor.log, "3");
  REQUIRE(rejects.size() == 1);
  CHECK(field_of(rejects[0], tag::kRefSeqNum) == std::to_string(exp));
  CHECK(field_of(rejects[0], tag::kRefTagID) == "122");
  CHECK(field_of(rejects[0], tag::kRefMsgType) == "0");
  CHECK(field_of(rejects[0], tag::kSessionRejectReason) == "1");
  CHECK(ini.next_target_seq() == exp + 1);
  CHECK(ini.state() == SessionState::Up);

  // OrigSendingTime later than SendingTime: Reject 373=10, Logout, disconnect.
  ep.receive(heartbeat_from_venue(exp + 1, "43=Y|122=20260914-08:30:16.000|"));
  rejects = of_type(link.to_acceptor.log, "3");
  REQUIRE(rejects.size() == 2);
  CHECK(field_of(rejects[1], tag::kSessionRejectReason) == "10");
  CHECK(of_type(link.to_acceptor.log, "5").size() == 1);
  CHECK(ini.state() == SessionState::Down);
  CHECK(ini.last_disconnect_reason() == FixDisconnectReason::SendingTimeAccuracy);
}

TEST_CASE("codecs.fix.session: SequenceReset in Reset and GapFill modes") {
  Link link;
  link.logon();
  FixSession& ini = link.initiator->session;
  Endpoint& ep = *link.initiator;
  const std::string head = "35=4|49=VENUE|56=CLIENT|52=20260914-08:30:15.000|";

  ep.receive(wrap(head + "34=999|36=50|"));  // Reset mode: MsgSeqNum ignored
  CHECK(ini.next_target_seq() == 50);
  CHECK(ini.stats().sequence_resets_received == 1);

  ep.receive(wrap(head + "34=1|36=10|"));  // lower NewSeqNo: Reject 373=5
  CHECK(ini.next_target_seq() == 50);
  auto rejects = of_type(link.to_acceptor.log, "3");
  REQUIRE(rejects.size() == 1);
  CHECK(field_of(rejects[0], tag::kSessionRejectReason) == "5");
  CHECK(field_of(rejects[0], tag::kRefTagID) == "36");

  ep.receive(wrap(head + "34=50|123=Y|36=60|"));  // GapFill obeys MsgSeqNum
  CHECK(ini.next_target_seq() == 60);
  CHECK(ini.stats().gap_fills_received == 1);

  ep.receive(wrap(head + "34=60|123=Y|36=60|"));  // NewSeqNo must exceed MsgSeqNum
  CHECK(ini.next_target_seq() == 61);
  rejects = of_type(link.to_acceptor.log, "3");
  REQUIRE(rejects.size() == 2);
  CHECK(field_of(rejects[1], tag::kSessionRejectReason) == "5");

  ep.receive(wrap(head + "34=70|123=Y|36=80|"));  // GapFill above expected is a gap
  CHECK(ini.state() == SessionState::Recovering);
  CHECK(ini.stats().resend_requests_sent == 1);
  ep.receive(wrap(head + "34=61|123=Y|36=71|43=Y|122=20260914-08:30:14.000|"));
  CHECK(ini.state() == SessionState::Up);
  CHECK(ini.next_target_seq() == 71);
}

TEST_CASE("codecs.fix.session: garbled messages are ignored and a CompID mismatch disconnects") {
  Link link;
  link.logon();
  FixSession& ini = link.initiator->session;
  Endpoint& ep = *link.initiator;
  std::string m = heartbeat_from_venue(2);
  char& digit = m[m.size() - 2];
  digit = digit == '9' ? '8' : '9';
  ep.receive(m);
  CHECK(ini.stats().garbled == 1);
  CHECK(ini.next_target_seq() == 2);
  CHECK(link.to_acceptor.log.size() == 1);  // just the Logon

  ep.receive(wrap("35=0|49=OTHER|56=CLIENT|34=2|52=20260914-08:30:15.000|"));
  CHECK(ini.state() == SessionState::Down);
  CHECK(ini.last_disconnect_reason() == FixDisconnectReason::CompIdProblem);
  const auto rejects = of_type(link.to_acceptor.log, "3");
  REQUIRE(rejects.size() == 1);
  CHECK(field_of(rejects[0], tag::kSessionRejectReason) == "9");
}

TEST_CASE("codecs.fix.session: application messages are only accepted when logged on") {
  Link link;
  FixSession& ini = link.initiator->session;
  char buf[512];
  FixBuilder b = ini.begin_app(std::span<char>(buf), msg::kNewOrderSingle);
  b.field(tag::kClOrdID, "x");
  const std::size_t n = b.finish();
  REQUIRE(n > 0);
  CHECK_FALSE(ini.send_app(std::span<const char>(buf, n)));
  link.logon();
  FixBuilder stale = ini.begin_app(std::span<char>(buf), msg::kNewOrderSingle);
  stale.field(tag::kClOrdID, "x");
  const std::size_t m = stale.finish();
  CHECK(ini.send_app(std::span<const char>(buf, m)));
  CHECK_FALSE(ini.send_app(std::span<const char>(buf, m)));  // MsgSeqNum already used
  CHECK(ini.stats().send_failures == 2);
  CHECK(ini.store().size() == 1);
  CHECK(ini.store().at(0).msg_type == "D");
  CHECK(ini.store().at(0).body == soh("11=x|"));
}

TEST_CASE("codecs.fix.session: MessageStore is append-only and bounded") {
  MessageStore s(3, 16);
  CHECK(s.append(5, "8", 1, "a=1|"));
  CHECK_FALSE(s.append(5, "8", 1, "a=1|"));  // not increasing
  CHECK(s.append(9, "AE", 2, "b=22|"));
  CHECK_FALSE(s.append(10, "8", 3, std::string(16, 'x')));  // bytes
  CHECK(s.append(11, "8", 3, ""));
  CHECK_FALSE(s.append(12, "8", 4, "c=3|"));  // entries
  CHECK(s.size() == 3);
  CHECK(s.dropped() == 3);
  CHECK(s.lower_bound(1) == 0);
  CHECK(s.lower_bound(6) == 1);
  CHECK(s.lower_bound(12) == 3);
  CHECK(s.at(1).seq == 9);
  CHECK(s.at(1).msg_type == "AE");
  CHECK(s.at(1).body == "b=22|");
  CHECK(s.at(1).sending_ns == 2);
  s.reset();
  CHECK(s.size() == 0);
  CHECK(s.append(1, "8", 1, "a=1|"));
}
