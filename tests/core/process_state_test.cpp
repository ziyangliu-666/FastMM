// The resource state of a process from /proc: this test's own, a named thread among its threads.
#include "fastmm/core/process_state.hpp"

#include "test_support.hpp"

#include "fastmm/core/thread_utils.hpp"
#include "fastmm/core/time.hpp"

#include <unistd.h>

#include <atomic>
#include <string>
#include <thread>

using namespace fastmm;

TEST_CASE("core.process_state: this process, its threads and their counters") {
  std::atomic<bool> named{false};
  std::atomic<bool> stop{false};
  std::thread t([&] {
    set_thread_name("fm-probe");
    named = true;
    while (!stop) std::this_thread::yield();
  });
  while (!named) std::this_thread::yield();
  const ProcessState s = read_process_state(::getpid());
  stop = true;
  t.join();
  REQUIRE(s.ok);
  CHECK(s.resident_bytes > 0);
  CHECK(s.online_cpus > 0);
  bool probe = false;
  for (const ThreadState& x : s.threads) {
    CHECK(x.cpu >= 0);
    if (x.name == "fm-probe") probe = x.state != '?';
  }
  CHECK(probe);
  const std::string text = format_process_state(s, &s);  // same sample: no rate
  CHECK(text.find("fm-probe") != std::string::npos);
  CHECK(process_state_json(s).starts_with(R"("process": {"ok": true, "pid": )"));
  CHECK(process_state_prometheus(s).find("fastmm_thread_involuntary_switches_total{thread=\"") !=
        std::string::npos);
}

TEST_CASE("core.process_state: another process, or the pid of one that started elsewhere") {
  const ProcessState gone = read_process_state(0);
  CHECK_FALSE(gone.ok);
  CHECK(process_state_json(gone) == R"("process": {"ok": false, "error": "no process 0 here"})");
  CHECK(process_state_prometheus(gone).empty());
  // This process, but the status file says the session started an hour ago: a reused pid.
  const ProcessState other = read_process_state(::getpid(), wall_now().ns - 3'600'000'000'000);
  CHECK_FALSE(other.ok);
  CHECK(other.error.find("not the session's process") != std::string::npos);
  CHECK(read_process_state(::getpid(), wall_now().ns).ok);
}
