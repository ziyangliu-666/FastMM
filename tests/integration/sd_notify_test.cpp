// The systemd notification protocol (live/sd_notify.hpp): the datagrams SystemdNotifier sends to
// $NOTIFY_SOCKET, the watchdog interval it takes from $WATCHDOG_USEC, and a fastmm-live child
// under it saying READY=1, WATCHDOG=1 and STOPPING=1, as a Type=notify unit sees it.
#include "fastmm/live/sd_notify.hpp"

#include "process_util.hpp"

#include "fastmm/live/session.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::integration;

namespace {

// The service manager's end: a datagram socket at `path` ('@' first: the abstract namespace).
struct NotifySocket {
  int fd = -1;
  std::string path;

  explicit NotifySocket(std::string p) : path(std::move(p)) {
    if (path[0] == '/') std::filesystem::remove(path);
    fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    REQUIRE(fd >= 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    REQUIRE(path.size() < sizeof addr.sun_path);
    std::memcpy(addr.sun_path, path.data(), path.size());
    if (path[0] == '@') addr.sun_path[0] = '\0';
    const auto len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size());
    REQUIRE(::bind(fd, reinterpret_cast<const sockaddr*>(&addr), len) == 0);
    timeval tv{0, 200'000};
    REQUIRE(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == 0);
  }
  ~NotifySocket() {
    ::close(fd);
    if (path[0] == '/') std::filesystem::remove(path);
  }
  NotifySocket(const NotifySocket&) = delete;
  NotifySocket& operator=(const NotifySocket&) = delete;

  // The next datagram, empty when none arrives within 200 ms.
  std::string next() const {
    char buf[512];
    const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
    return n > 0 ? std::string(buf, static_cast<std::size_t>(n)) : std::string();
  }
};

// Sets the variables for the scope of a case and removes them after.
struct Env {
  std::vector<std::string> names;
  void set(const char* name, const std::string& value) {
    REQUIRE(::setenv(name, value.c_str(), 1) == 0);
    names.emplace_back(name);
  }
  ~Env() {
    for (const std::string& n : names) ::unsetenv(n.c_str());
  }
};

}  // namespace

TEST_CASE("live.sd_notify: nothing is sent outside systemd") {
  ::unsetenv("NOTIFY_SOCKET");
  live::SystemdNotifier n;
  CHECK_FALSE(n.active());
  CHECK_FALSE(n.notify("READY=1"));
  n.keep_alive(0);  // no socket, no watchdog: nothing happens
  CHECK(n.watchdog_ns() == 0);
}

TEST_CASE("live.sd_notify: states reach the socket and the watchdog is pinged at half its period") {
  for (const std::string& path : {(fastmm::test::tmp_dir() / "sd_notify.sock").string(),
                                  "@fastmm-sd-notify-" + std::to_string(::getpid())}) {
    INFO(path);
    NotifySocket sock(path);
    Env env;
    env.set("NOTIFY_SOCKET", path);
    env.set("WATCHDOG_USEC", "100000");
    live::SystemdNotifier n;
    REQUIRE(n.active());
    CHECK(n.watchdog_ns() == 100'000'000);
    CHECK(n.notify("READY=1\nSTATUS=trading"));
    CHECK(sock.next() == "READY=1\nSTATUS=trading");
    n.keep_alive(1'000'000'000);
    CHECK(sock.next() == "WATCHDOG=1");
    n.keep_alive(1'000'000'000 + 49'000'000);  // within half the period: nothing
    n.keep_alive(1'000'000'000 + 50'000'000);
    CHECK(sock.next() == "WATCHDOG=1");
    CHECK(sock.next().empty());
  }
}

TEST_CASE("live.sd_notify: a watchdog meant for another process is not this one's") {
  const std::string path = (fastmm::test::tmp_dir() / "sd_notify_pid.sock").string();
  NotifySocket sock(path);
  Env env;
  env.set("NOTIFY_SOCKET", path);
  env.set("WATCHDOG_USEC", "100000");
  env.set("WATCHDOG_PID", std::to_string(::getpid() + 1));
  live::SystemdNotifier n;
  CHECK(n.active());
  CHECK(n.watchdog_ns() == 0);
  n.keep_alive(1'000'000'000);
  CHECK(sock.next().empty());
}

#ifdef FASTMM_LIVE_EXE
TEST_CASE("live.sd_notify: fastmm-live says READY=1, keeps the watchdog fed and says STOPPING=1") {
  ServerFixture fx;
  const SessionFiles f = write_config(fx, "sd-notify", "exit", "1000");
  remove_all_of({f.epoch, f.kill, f.journal_dir});
  const std::string path = (fastmm::test::tmp_dir() / "sd_notify_live.sock").string();
  NotifySocket sock(path);
  pid_t pid = 0;
  {
    Env env;  // the child's environment only
    env.set("NOTIFY_SOCKET", path);
    env.set("WATCHDOG_USEC", "200000");
    pid = spawn_live(f, 3);
  }
  std::vector<std::string> got;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (std::chrono::steady_clock::now() < deadline) {
    std::string m = sock.next();
    if (m.empty()) continue;
    got.push_back(m);
    if (m.starts_with("STOPPING=1")) break;
  }
  CHECK(reap(pid) == live::kExitOk);
  REQUIRE_MESSAGE(!got.empty(), fastmm::test::read_file(f.config + ".log"));
  CHECK(got.front() == "READY=1\nSTATUS=trading");
  CHECK(got.back() == "STOPPING=1");
  std::size_t pings = 0;
  for (const std::string& m : got) pings += m == "WATCHDOG=1" ? 1U : 0U;
  // 3 s of trading at a ping every 100 ms.
  CHECK(pings >= 20);
}
#endif
