#include "fastmm/core/status_segment.hpp"

#include "test_support.hpp"

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
