#include "fastmm/core/status_segment.hpp"

#include "test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

using namespace fastmm;

namespace {
std::string tmp_path(const char* name) {
  return std::string(FASTMM_TEST_TMP_DIR) + "/" + name;
}
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
  s.realized_pnl_raw = 150'000'000;  // 1.5
  s.latency[static_cast<std::size_t>(LatencyInterval::TickToTrade)] = {
      10, 106'495, 216'053, 300'000};
  s.venue_count = 1;
  set_status_name(s.venues[0].name, "binance");
  s.venues[0].md = 2;
  s.venues[0].user = 3;
  s.venues[0].order = 0;
  s.venues[0].books_synced = 1;
  s.venues[0].books_total = 1;
  return s;
}
}  // namespace

TEST_CASE("core.status_segment: writer and reader round trip") {
  const std::string path = tmp_path("roundtrip.status");
  StatusWriter w;
  std::string err;
  REQUIRE_MESSAGE(w.open(path, &err), err);
  StatusReader r;
  REQUIRE_MESSAGE(r.open(path, &err), err);
  StatusSnapshot got;
  CHECK_FALSE(r.read(got));  // nothing published yet
  const StatusSnapshot s = sample();
  w.publish(s);
  REQUIRE(r.read(got));
  CHECK(got.pid == 4242);
  CHECK(got.session_id == 17);
  CHECK(std::string(got.engine_name) == "binance-demo");
  CHECK(got.orders_sent == 7);
  CHECK(got.venues[0].md == 2);
  CHECK(got.latency[static_cast<std::size_t>(LatencyInterval::TickToTrade)].p99_ns == 216'053);
  StatusSnapshot next = s;
  next.state = StatusRunState::Stopped;
  w.publish(next);
  w.close();  // the file stays for monitors
  REQUIRE(r.read(got));
  CHECK(got.state == StatusRunState::Stopped);
  std::remove(path.c_str());
}

TEST_CASE("core.status_segment: missing, short and foreign files are rejected") {
  StatusReader r;
  std::string err;
  CHECK_FALSE(r.open(tmp_path("does-not-exist.status"), &err));
  CHECK_FALSE(err.empty());
  const std::string small = tmp_path("small.status");
  std::FILE* f = std::fopen(small.c_str(), "wb");
  REQUIRE(f != nullptr);
  std::fputs("not a status segment", f);
  std::fclose(f);
  CHECK_FALSE(r.open(small, &err));
  std::remove(small.c_str());
  const std::string foreign = tmp_path("foreign.status");
  {
    StatusWriter w;
    REQUIRE(w.open(foreign, &err));
  }
  f = std::fopen(foreign.c_str(), "r+b");
  REQUIRE(f != nullptr);
  // An even sequence, as if published, but the magic number right after it wiped: a file some
  // other program or an incompatible build wrote.
  const unsigned char header[16] = {2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  std::fwrite(header, 1, sizeof header, f);
  std::fclose(f);
  REQUIRE(r.open(foreign, &err));
  StatusSnapshot got;
  CHECK_FALSE(r.read(got));
  std::remove(foreign.c_str());
}

TEST_CASE("core.status_segment: the dashboard shows state, engine, latency and venues") {
  StatusSnapshot s = sample();
  std::string frame = format_status(s, s.updated_ns + 500'000'000, false);
  CHECK(frame.find("engine=binance-demo") != std::string::npos);
  CHECK(frame.find("running") != std::string::npos);
  CHECK(frame.find("uptime=1m00s") != std::string::npos);
  CHECK(frame.find("orders=7") != std::string::npos);
  CHECK(frame.find("realized=1.5") != std::string::npos);
  CHECK(frame.find("106.5 us") != std::string::npos);
  CHECK(frame.find("binance") != std::string::npos);
  CHECK(frame.find("stale") != std::string::npos);  // the user channel
  CHECK(frame.find('\x1b') == std::string::npos);   // no colour requested
  frame = format_status(s, s.updated_ns + 5'000'000'000, true);
  CHECK(frame.find("STALE") != std::string::npos);  // running but not updated for 5 s
  CHECK(frame.find('\x1b') != std::string::npos);
  CHECK(default_status_path("mm") == "/dev/shm/fastmm-mm.status");
  char tiny[4];
  set_status_name(tiny, "abcdef");
  CHECK(std::string(tiny) == "abc");
}

TEST_CASE("core.status_segment: reject reasons round trip and show on the dashboard") {
  StatusSnapshot s = sample();
  std::string frame = format_status(s, s.updated_ns, false);
  CHECK(frame.find("risk_rejects=0 venue_rejects=0") != std::string::npos);

  RejectCounts risk;
  for (int i = 0; i < 5; ++i) risk.add(RejectReason::RateLimit);
  for (int i = 0; i < 12; ++i) risk.add(RejectReason::MaxPosition);
  RejectCounts venue;
  for (int i = 0; i < 3; ++i) venue.add(RejectReason::PostOnlyWouldCross);
  s.risk_rejects = risk.total();
  s.venue_rejects = venue.total();
  set_status_rejects(s.risk_reject_reasons, risk);
  set_status_rejects(s.venue_reject_reasons, venue);

  const std::string path = tmp_path("rejects.status");
  StatusWriter w;
  std::string err;
  REQUIRE_MESSAGE(w.open(path, &err), err);
  StatusReader r;
  REQUIRE_MESSAGE(r.open(path, &err), err);
  w.publish(s);
  StatusSnapshot got;
  REQUIRE(r.read(got));
  CHECK(got.risk_rejects == 17);
  CHECK(got.venue_rejects == 3);
  CHECK(got.risk_reject_reasons[0].reason == static_cast<std::uint8_t>(RejectReason::MaxPosition));
  CHECK(got.risk_reject_reasons[0].count == 12);
  CHECK(got.risk_reject_reasons[1].reason == static_cast<std::uint8_t>(RejectReason::RateLimit));
  CHECK(got.risk_reject_reasons[1].count == 5);
  CHECK(got.risk_reject_reasons[2].count == 0);
  CHECK(got.venue_reject_reasons[0].count == 3);
  frame = format_status(got, got.updated_ns, false);
  INFO(frame);
  CHECK(frame.find("risk_rejects=17 (MaxPosition 12, RateLimit 5) venue_rejects=3 "
                   "(PostOnlyWouldCross 3)") != std::string::npos);
  w.close();
  std::remove(path.c_str());

  // More reasons than entries: the least frequent are summed as "other".
  RejectCounts many;
  const RejectReason reasons[] = {RejectReason::InvalidTick,
                                  RejectReason::InvalidLot,
                                  RejectReason::StaleMarketData,
                                  RejectReason::PriceCollar,
                                  RejectReason::FatFinger,
                                  RejectReason::MaxOrderQty,
                                  RejectReason::MaxPosition,
                                  RejectReason::RateLimit};
  std::uint64_t n = 10;
  for (const RejectReason reason : reasons) {
    for (std::uint64_t i = 0; i < n; ++i) many.add(reason);
    --n;  // 10, 9, ..., 3
  }
  s.risk_rejects = many.total();
  set_status_rejects(s.risk_reject_reasons, many);
  CHECK(s.risk_reject_reasons[kStatusMaxRejectReasons - 1].reason ==
        static_cast<std::uint8_t>(RejectReason::MaxOrderQty));
  CHECK(format_status_rejects(s.risk_reject_reasons, s.risk_rejects) ==
        "InvalidTick 10, InvalidLot 9, StaleMarketData 8, PriceCollar 7, FatFinger 6, "
        "MaxOrderQty 5, other 7");
  CHECK(format_status_rejects(s.risk_reject_reasons, 0).empty());
}

TEST_CASE("core.status_segment: a segment of another version is refused, not misread") {
  // Same layout size, other version: what a reader of another build sees, since every version
  // keeps the magic and the version at the same offsets.
  const std::string path = tmp_path("other-version.status");
  std::string err;
  {
    StatusWriter w;
    REQUIRE_MESSAGE(w.open(path, &err), err);
    w.publish(sample());
  }
  StatusReader r;
  REQUIRE_MESSAGE(r.open(path, &err), err);
  StatusSnapshot got;
  REQUIRE(r.read(got));
  CHECK(r.segment_version() == kStatusVersion);
  std::FILE* f = std::fopen(path.c_str(), "r+b");
  REQUIRE(f != nullptr);
  const std::uint32_t other = kStatusVersion + 1;
  REQUIRE(std::fseek(f, sizeof(std::uint64_t) + offsetof(StatusSnapshot, version), SEEK_SET) == 0);
  REQUIRE(std::fwrite(&other, sizeof other, 1, f) == 1);
  std::fclose(f);
  CHECK_FALSE(r.read(got));
  CHECK(r.segment_version() == other);
  r.close();

  // A smaller file with the header of version 1 (the layout before per-reason rejects): open
  // names both versions instead of reporting a short file.
  const std::string old_path = tmp_path("v1.status");
  f = std::fopen(old_path.c_str(), "wb");
  REQUIRE(f != nullptr);
  const std::uint64_t seq = 2;
  const std::uint64_t magic = kStatusMagic;
  const std::uint32_t v1 = 1;
  std::fwrite(&seq, sizeof seq, 1, f);
  std::fwrite(&magic, sizeof magic, 1, f);
  std::fwrite(&v1, sizeof v1, 1, f);
  const std::string rest(512, '\0');
  std::fwrite(rest.data(), 1, rest.size(), f);
  std::fclose(f);
  CHECK_FALSE(r.open(old_path, &err));
  INFO(err);
  CHECK(err == status_version_mismatch(1));
  CHECK(err.find("version 1 ") != std::string::npos);
  CHECK(err.find("(version 2)") != std::string::npos);
  CHECK_FALSE(r.is_open());
  CHECK(r.segment_version() == 1);  // fastmm-top reports the refused version on its own
  CHECK_FALSE(r.open(tmp_path("no-such.status"), &err));
  CHECK(r.segment_version() == 0);
  std::remove(old_path.c_str());
  std::remove(path.c_str());
}
