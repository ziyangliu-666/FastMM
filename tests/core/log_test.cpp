#include "fastmm/core/log.hpp"

#include "test_support.hpp"

#include <cstdio>
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
