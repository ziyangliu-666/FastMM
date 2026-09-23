#include "fastmm/core/status_prometheus.hpp"

#include "test_support.hpp"

#include "fastmm/core/enums.hpp"

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
