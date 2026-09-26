#include "fastmm/core/status_prometheus.hpp"

#include "test_support.hpp"

#include "fastmm/core/enums.hpp"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace fastmm;

namespace {

StatusSnapshot sample() {
  StatusSnapshot s;
  s.pid = 4242;
  s.session_id = 17;
  s.started_ns = 1'000'000'000;
  s.updated_ns = 61'000'000'000;
  s.state = StatusRunState::Running;
  set_status_name(s.engine_name, "binance-demo");
  set_status_name(s.strategy, "basic_mm");
  s.events = 897;
  s.orders_sent = 7;
  s.fills = 2;
  s.risk_rejects = 5;
  s.risk_reject_reasons[0] = {5, static_cast<std::uint8_t>(RejectReason::MaxPosition), {}};
  s.realized_pnl_raw = 150'000'000;  // 1.5
  s.fees_raw = -25'000'000;          // -0.25, a rebate
  s.latency[static_cast<std::size_t>(LatencyInterval::TickToTrade)] = {
      10, 106'495, 216'053, 250'000, 300'000};
  s.venue_count = 1;
  set_status_name(s.venues[0].name, "binance");
  s.venues[0].md = 2;
  s.venues[0].books_synced = 1;
  s.venues[0].books_total = 1;
  s.venues[0].md_messages = 963;
  return s;
}

std::vector<std::string> lines(const std::string& text) {
  std::vector<std::string> out;
  std::size_t pos = 0;
  while (pos < text.size()) {
    const std::size_t end = text.find('\n', pos);
    out.emplace_back(text, pos, end == std::string::npos ? end : end - pos);
    if (end == std::string::npos) break;
    pos = end + 1;
  }
  return out;
}

bool has(const std::string& text, std::string_view needle) {
  return text.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("core.status_prometheus: the snapshot becomes metrics in base units") {
  const std::string text = format_status_prometheus(sample(), 61'500'000'000);
  CHECK(has(text, "fastmm_up 1\n"));
  CHECK(has(text,
            "fastmm_info{engine=\"binance-demo\",strategy=\"basic_mm\",pid=\"4242\","
            "session_id=\"17\"} 1\n"));
  CHECK(has(text, "fastmm_state 1\n"));
  CHECK(has(text, "fastmm_events_total 897\n"));
  CHECK(has(text, "fastmm_orders_sent_total 7\n"));
  CHECK(has(text, "fastmm_realized_pnl 1.5\n"));
  CHECK(has(text, "fastmm_fees -0.25\n"));
  CHECK(has(text, "fastmm_uptime_seconds 60\n"));
  CHECK(has(text, "fastmm_status_age_seconds 0.5\n"));
  CHECK(has(text, "fastmm_rejects_by_reason_total{kind=\"risk\",reason=\"MaxPosition\"} 5\n"));
  // Nanoseconds in the snapshot, seconds on the wire.
  CHECK(has(text,
            "fastmm_latency_quantile_seconds{interval=\"tick_to_trade\",quantile=\"0.5\"} "
            "0.000106495\n"));
  CHECK(has(text, "fastmm_latency_samples_total{interval=\"tick_to_trade\"} 10\n"));
  CHECK(has(text, "fastmm_venue_channel_state{venue=\"binance\",channel=\"md\"} 2\n"));
  CHECK(has(text, "fastmm_venue_md_messages_total{venue=\"binance\"} 963\n"));
  CHECK_FALSE(has(text, "fastmm_feed_"));  // no multicast venue in this snapshot
}

TEST_CASE("core.status_prometheus: an engine exports the max_loss it applies") {
  StatusSnapshot s = sample();
  s.max_loss_raw = 25'000'000'000;  // 250
  CHECK(has(format_status_prometheus(s, s.updated_ns), "fastmm_max_loss 250\n"));
}

TEST_CASE("core.status_prometheus: every sample belongs to a declared family") {
  StatusSnapshot s = sample();
  s.venue_count = 2;
  set_status_name(s.venues[1].name, "itch");
  s.venues[1].feed.state = 3;
  s.venues[1].feed.packets = 1234;
  s.venues[1].feed.line_duplicates[1] = 7;
  const std::string text = format_status_prometheus(s, s.updated_ns);
  std::set<std::string> declared;
  for (const std::string& line : lines(text)) {
    if (line.empty()) continue;
    if (line.rfind("# TYPE ", 0) == 0) {
      const std::size_t end = line.find(' ', 7);
      declared.insert(line.substr(7, end - 7));
      continue;
    }
    if (line.rfind("# HELP ", 0) == 0) continue;
    const std::size_t brace = line.find('{');
    const std::size_t space = line.find(' ');
    const std::string name = line.substr(0, std::min(brace, space));
    CAPTURE(line);
    CHECK(declared.count(name) == 1);
    CHECK(space != std::string::npos);  // "<name>[{labels}] <value>"
  }
  CHECK(has(text, "fastmm_feed_state{venue=\"itch\"} 3\n"));
  CHECK(has(text, "fastmm_feed_packets_total{venue=\"itch\"} 1234\n"));
  CHECK(has(text, "fastmm_feed_line_duplicates_total{venue=\"itch\",line=\"b\"} 7\n"));
}

TEST_CASE("core.status_prometheus: a label value is escaped") {
  StatusSnapshot s = sample();
  set_status_name(s.engine_name, "od\"d\\name");
  const std::string text = format_status_prometheus(s, s.updated_ns);
  CHECK(has(text, "engine=\"od\\\"d\\\\name\""));
}

TEST_CASE("core.status_prometheus: a gateway exports its account, positions and attachments") {
  StatusSnapshot s;
  s.kind = StatusKind::Gateway;
  s.pid = 99;
  s.updated_ns = 61'000'000'000;
  s.state = StatusRunState::Running;
  set_status_name(s.engine_name, "gw");
  s.realized_pnl_raw = 150'000'000;
  s.fees_raw = 25'000'000;
  s.kill_flags = 1;
  s.kill_reason = static_cast<std::uint8_t>(KillReason::GatewayOperator);
  s.venue_count = 1;
  set_status_name(s.venues[0].name, "sim");
  s.venues[0].md = 2;
  StatusGateway& g = s.gateway;
  g.kill_active = 1;
  g.net_pnl_raw = 125'000'000;
  g.gross_raw = 12'000'000'000;
  g.venues[0].refused[3] = 2;  // GatewayGrossNotional
  g.venues[0].gateway_cancels = 4;
  g.attachment_count = 1;
  set_status_name(g.attachments[0].engine, "mm-a");
  g.attachments[0].pid = 1001;
  g.attachments[0].id = 1;
  g.attachments[0].epoch = 7;
  g.attachments[0].md_dropped = 12;
  g.attachments[0].refused[3] = 2;
  g.position_count = 2;
  set_status_name(g.positions[0].symbol, "BTCUSDT");
  g.positions[0].owner_epoch = 7;
  g.positions[0].qty_raw = 400'000;
  set_status_name(g.positions[1].symbol, "ETHUSDT");
  g.positions[1].qty_raw = -200'000;
  const std::string text = format_status_prometheus(s, s.updated_ns);
  INFO(text);
  CHECK(has(text, "fastmm_info{gateway=\"gw\",pid=\"99\"} 1\n"));
  CHECK(has(text, "fastmm_kill_active 1\n"));
  CHECK(has(text, "fastmm_kill_reason 14\n"));
  CHECK(has(text, "fastmm_account_net_pnl 1.25\n"));
  CHECK(has(text, "fastmm_account_realized_pnl 1.5\n"));
  CHECK(has(text, "fastmm_account_fees 0.25\n"));
  CHECK(has(text, "fastmm_account_gross_exposure 120\n"));
  CHECK(has(text, "fastmm_account_position{venue=\"sim\",instrument=\"BTCUSDT\"} 0.004\n"));
  CHECK(has(text, "fastmm_account_position{venue=\"sim\",instrument=\"ETHUSDT\"} -0.002\n"));
  CHECK(has(text, "fastmm_gateway_instrument_owner{venue=\"sim\",instrument=\"BTCUSDT\"} 7\n"));
  CHECK_FALSE(has(text, "fastmm_gateway_instrument_owner{venue=\"sim\",instrument=\"ETHUSDT\"}"));
  CHECK(has(text, "fastmm_gateway_attachments 1\n"));
  CHECK(has(text,
            "fastmm_gateway_attachment_info{epoch=\"7\",engine=\"mm-a\",pid=\"1001\","
            "attachment=\"1\"} 1\n"));
  CHECK(has(text, "fastmm_gateway_attachment_md_dropped_total{epoch=\"7\",engine=\"mm-a\"} 12\n"));
  CHECK(has(text,
            "fastmm_gateway_attachment_refused_total{epoch=\"7\",engine=\"mm-a\","
            "reason=\"GatewayGrossNotional\"} 2\n"));
  CHECK(
      has(text, "fastmm_gateway_refused_total{venue=\"sim\",reason=\"GatewayGrossNotional\"} 2\n"));
  CHECK(has(text, "fastmm_gateway_cancels_total{venue=\"sim\"} 4\n"));
  CHECK(has(text, "fastmm_venue_channel_state{venue=\"sim\",channel=\"md\"} 2\n"));
  // No engine counters, and no gateway families in an engine's export.
  CHECK_FALSE(has(text, "fastmm_events_total"));
  CHECK_FALSE(has(text, "fastmm_latency_quantile_seconds"));
  CHECK_FALSE(has(format_status_prometheus(sample(), 0), "fastmm_account_"));
  CHECK_FALSE(has(format_status_prometheus(sample(), 0), "fastmm_gateway_"));
  // Every sample belongs to a declared family.
  std::set<std::string> declared;
  for (const std::string& line : lines(text)) {
    if (line.rfind("# TYPE ", 0) == 0) {
      declared.insert(line.substr(7, line.find(' ', 7) - 7));
      continue;
    }
    if (line.empty() || line[0] == '#') continue;
    CAPTURE(line);
    CHECK(declared.count(line.substr(0, std::min(line.find('{'), line.find(' ')))) == 1);
  }
}

// deploy/prometheus/fastmm-alerts.yml is only useful while every metric it names is exported: a
// renamed or removed family would leave a rule that never fires.
TEST_CASE("core.status_prometheus: every metric the shipped alert rules use is exported") {
  const std::string rules = fastmm::test::read_file(std::string(FASTMM_FIXTURES_DIR) +
                                                    "/../../deploy/prometheus/fastmm-alerts.yml");
  REQUIRE_FALSE(rules.empty());
  StatusSnapshot gw;
  gw.kind = StatusKind::Gateway;
  gw.updated_ns = 1;
  gw.venue_count = 1;
  set_status_name(gw.venues[0].name, "sim");
  gw.gateway.attachment_count = 1;
  set_status_name(gw.gateway.attachments[0].engine, "mm");
  const std::string exported =
      format_status_prometheus(sample(), 61'500'000'000) + format_status_prometheus(gw, 1);
  std::set<std::string> names;
  for (std::size_t at = rules.find("fastmm_"); at != std::string::npos;
       at = rules.find("fastmm_", at + 1)) {
    if (at > 0 && (std::isalnum(static_cast<unsigned char>(rules[at - 1])) ||
                   rules[at - 1] == '_' || rules[at - 1] == '/' || rules[at - 1] == '-'))
      continue;  // part of a file name or another word
    std::size_t end = at;
    while (end < rules.size() &&
           (std::islower(static_cast<unsigned char>(rules[end])) || rules[end] == '_' ||
            std::isdigit(static_cast<unsigned char>(rules[end]))))
      ++end;
    names.insert(rules.substr(at, end - at));
  }
  CHECK(names.size() >= 10);
  for (const std::string& n : names) {
    INFO(n);
    CHECK(has(exported, "# TYPE " + n + " "));
  }
}
