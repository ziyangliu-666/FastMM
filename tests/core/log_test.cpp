#include "fastmm/core/log.hpp"

#include "test_support.hpp"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

using namespace fastmm;

namespace {
std::string read_all(std::FILE* f) {
  std::fflush(f);
  std::fseek(f, 0, SEEK_SET);
  std::string s;
  char buf[4096];
  std::size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
  return s;
}
std::size_t count_lines(const std::string& s) {
  std::size_t c = 0;
  for (char ch : s) c += ch == '\n' ? 1 : 0;
  return c;
}
}  // namespace

TEST_CASE("core.log: formatting of every supported argument type") {
  std::FILE* f = std::tmpfile();
  REQUIRE(f != nullptr);
  auto& lg = Logger::instance();
  lg.set_level(LogLevel::Trace);
  lg.start(f, LogLevel::Off);
  const Price px = Price::from_decimal("50000.5").value();
  const Qty q = Qty::from_decimal("0.002").value();
  FASTMM_LOG_INFO("int={} uint={} dbl={:.2f} bool={} char={} px={} qty={} n={} side={} state={}",
                  -5,
                  7U,
                  1.5,
                  true,
                  'x',
                  px,
                  q,
                  mul(px, q),
                  Side::Sell,
                  OrderState::Live);
  FASTMM_LOG_WARN("str={} lit={} fs={} key={} ts={}",
                  std::string_view("view"),
                  "literal",
                  FixedString<8>("fixed"),
                  secret(std::string("hunter2")),
                  Timestamp{42});
  FASTMM_LOG_TRACE("no args");
  FASTMM_LOG_ERROR("id={}", InstrumentId{3}.value);
  lg.flush();
  const std::string out = read_all(f);
  INFO(out);
  CHECK(out.find("int=-5 uint=7 dbl=1.50 bool=true char=x px=50000.5 qty=0.002 n=100.001 side=Sell "
                 "state=Live") != std::string::npos);
  CHECK(out.find("str=view lit=literal fs=fixed key=*** ts=42") != std::string::npos);
  CHECK(out.find("hunter2") == std::string::npos);
  CHECK(out.find("INFO ") != std::string::npos);
  CHECK(out.find("WARN ") != std::string::npos);
  CHECK(out.find("log_test.cpp:") != std::string::npos);
#if FASTMM_MIN_LOG_LEVEL <= 0
  CHECK(out.find("no args") != std::string::npos);  // TRACE compiled in
#else
  CHECK(out.find("no args") == std::string::npos);  // TRACE compiled out by the Release floor
#endif
  CHECK(out.find("id=3") != std::string::npos);
  lg.stop();
  std::fclose(f);
}

TEST_CASE("core.log: level filter, ordering within a thread, drops when the ring is full") {
  std::FILE* f = std::tmpfile();
  auto& lg = Logger::instance();
  lg.set_level(LogLevel::Warn);
  // sink not running: fill the thread ring beyond capacity -> drops counted, never blocks
  const std::uint64_t d0 = lg.dropped();
  for (std::size_t i = 0; i < kLogRingSize + 100; ++i) FASTMM_LOG_WARN("filler {}", i);
  CHECK(lg.dropped() - d0 >= 100);
  FASTMM_LOG_INFO("should be filtered");
  lg.start(f, LogLevel::Off);
  lg.flush();
  std::string out = read_all(f);
  CHECK(out.find("should be filtered") == std::string::npos);
  CHECK(count_lines(out) == kLogRingSize);
  // ordering: sequence numbers appear in increasing order
  std::size_t pos = 0;
  std::size_t last = 0;
  bool ordered = true;
  int seen = 0;
  while ((pos = out.find("filler ", pos)) != std::string::npos) {
    pos += 7;
    const std::size_t v = std::stoul(out.substr(pos, 10));
    if (seen > 0 && v <= last) ordered = false;
    last = v;
    ++seen;
  }
  CHECK(ordered);
  CHECK(seen == static_cast<int>(kLogRingSize));
  lg.stop();
  std::fclose(f);
  lg.set_level(LogLevel::Info);
}

TEST_CASE("core.log: flush and stop wake an idle sink instead of waiting out its backoff") {
  std::FILE* f = std::tmpfile();
  auto& lg = Logger::instance();
  lg.start(f, LogLevel::Off);
  // Idle long enough for the sink's timed waits to reach their 10 ms cap.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  std::vector<std::int64_t> us;
  for (int i = 0; i < 9; ++i) {
    FASTMM_LOG_WARN("idle flush {}", i);
    const auto t0 = std::chrono::steady_clock::now();
    lg.flush();
    us.push_back(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0)
            .count());
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }
  std::sort(us.begin(), us.end());
  CHECK(us[us.size() / 2] < 2'000);  // waiting out the wait: 5 ms on average
  CHECK(count_lines(read_all(f)) == 9);
  const auto t0 = std::chrono::steady_clock::now();
  lg.stop();
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(8));
  std::fclose(f);
}

TEST_CASE("core.log: multiple threads each get their own ring") {
  std::FILE* f = std::tmpfile();
  auto& lg = Logger::instance();
  lg.set_level(LogLevel::Info);
  lg.start(f, LogLevel::Off);
  std::vector<std::thread> ts;
  for (int t = 0; t < 4; ++t) {
    ts.emplace_back([t] {
      for (int i = 0; i < 500; ++i) FASTMM_LOG_INFO("thread {} msg {}", t, i);
    });
  }
  for (auto& t : ts) t.join();
  lg.flush();
  const std::string out = read_all(f);
  CHECK(count_lines(out) == 2000);
  lg.stop();
  std::fclose(f);
}

TEST_CASE("core.log: exited threads release their ring for reuse") {
  std::FILE* f = std::tmpfile();
  REQUIRE(f != nullptr);
  Logger& lg = Logger::instance();
  const LogLevel prev_level = lg.level();
  lg.set_level(LogLevel::Info);
  lg.start(f, LogLevel::Off);
  const std::uint64_t dropped_before = lg.dropped();
  // Three times the slot limit, one after another. Without reuse the limit is hit after
  // kLogMaxThreads threads and every later record is dropped.
  const int n = static_cast<int>(3 * kLogMaxThreads);
  for (int i = 0; i < n; ++i) {
    std::thread([i] { FASTMM_LOG_INFO("short-lived thread {}", i); }).join();
    lg.flush();  // a ring is reusable only after the sink has written its records
  }
  CHECK(lg.dropped() == dropped_before);
  CHECK(lg.thread_slots() < kLogMaxThreads);
  lg.stop();
  lg.set_level(prev_level);
  std::fclose(f);
}

TEST_CASE("core.log: under a LogClockScope a line carries the simulated time") {
  std::FILE* f = std::tmpfile();
  REQUIRE(f != nullptr);
  auto& lg = Logger::instance();
  lg.start(f, LogLevel::Off);
  const SimClock clock(Timestamp{1'700'000'000'123'000'000});
  {
    const LogClockScope scope(clock);
    FASTMM_LOG_WARN("simulated");
  }
  FASTMM_LOG_WARN("wall");
  lg.flush();
  const std::string out = read_all(f);
  INFO(out);
  const std::string sim = "2023-11-14T22:13:20.123000000Z";
  REQUIRE(out.find("simulated") != std::string::npos);
  REQUIRE(out.find("wall") != std::string::npos);
  CHECK(out.rfind(sim, out.find("simulated")) != std::string::npos);
  CHECK(out.find(sim, out.find("simulated")) == std::string::npos);  // not on the "wall" line
  lg.stop();
  std::fclose(f);
}

TEST_CASE("core.log: under a LogRunScope a line names its run") {
  static constexpr LogDescriptor desc{"x", "dir/file.cpp", 7, LogLevel::Warn};
  LogRecord r{};
  r.desc = &desc;
  std::string out;
  Logger::format_record(r, out);
  CHECK(out.find("] file.cpp:7 ") != std::string::npos);
  {
    const LogRunScope run(12);
    CHECK(detail::t_log_run == 13);
    {
      SimClock clock;
      const LogClockScope keep_clock;
      const LogRunScope keep;
      log_switch_run(clock, 3);
      CHECK(detail::t_log_run == 4);
    }
    CHECK(detail::t_log_run == 13);
  }
  CHECK(detail::t_log_run == 0);
  r.run = 13;
  out.clear();
  Logger::format_record(r, out);
  CHECK(out.find("] #12 file.cpp:7 ") != std::string::npos);
}

TEST_CASE("core.log: format_record handles malformed argument count gracefully") {
  static constexpr LogDescriptor desc{"a={} b={}", __FILE__, __LINE__, LogLevel::Info};
  LogRecord r{};
  r.desc = &desc;
  r.ts_ns = 1'700'000'000'000'000'000LL;
  r.nargs = 1;
  detail::ArgPacker pk{r.args, r.args + sizeof(r.args)};
  detail::pack_arg(pk, 1);
  r.used = static_cast<std::uint8_t>(pk.p - r.args);
  std::string out;
  Logger::format_record(r, out);
  CHECK(out.find("format error") != std::string::npos);
  CHECK(out.find("2023-11-14T22:13:20.000000000Z") != std::string::npos);
}

TEST_CASE("core.log: reopen after a rename puts what follows in a new file at the path") {
  namespace fs = std::filesystem;
  const fs::path dir =
      fs::temp_directory_path() / ("fastmm-log-reopen-" + std::to_string(::getpid()));
  fs::remove_all(dir);
  fs::create_directories(dir);
  const fs::path path = dir / "session.log";
  const fs::path rotated = dir / "session.log.1";
  auto slurp = [](const fs::path& p) {
    std::ifstream in(p);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  };
  auto& lg = Logger::instance();
  REQUIRE(lg.start_file(path.string(), LogLevel::Off));
  CHECK(lg.to_file());
  FASTMM_LOG_WARN("before rotation");
  fs::rename(path, rotated);
  CHECK(lg.reopen());
  FASTMM_LOG_WARN("after rotation");
  // The signal handler's path: only asks.
  fs::rename(path, dir / "session.log.2");
  lg.request_reopen();
  lg.flush();  // the sink acts on a reopen before the flush it reads with it
  FASTMM_LOG_WARN("after the second rotation");
  lg.flush();
  // A path that cannot be opened keeps the old file.
  fs::rename(path, dir / "session.log.3");
  fs::remove_all(dir / "session.log");
  fs::create_directory(dir / "session.log");
  CHECK_FALSE(lg.reopen());
  FASTMM_LOG_WARN("still in the third file");
  lg.stop();
  CHECK_FALSE(lg.to_file());
  CHECK(slurp(rotated).find("before rotation") != std::string::npos);
  CHECK(slurp(rotated).find("after rotation") == std::string::npos);
  CHECK(slurp(dir / "session.log.2").find("after rotation") != std::string::npos);
  CHECK(slurp(dir / "session.log.3").find("after the second rotation") != std::string::npos);
  CHECK(slurp(dir / "session.log.3").find("still in the third file") != std::string::npos);
  fs::remove_all(dir);
}

TEST_CASE("core.log: reopen without a file of its own is refused") {
  std::FILE* f = std::tmpfile();
  auto& lg = Logger::instance();
  lg.start(f, LogLevel::Off);
  CHECK_FALSE(lg.reopen());
  FASTMM_LOG_WARN("caller's stream");
  lg.stop();
  CHECK(read_all(f).find("caller's stream") != std::string::npos);
  std::fclose(f);
}
