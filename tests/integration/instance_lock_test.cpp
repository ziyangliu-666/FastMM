// The instance lock (fastmm/live/instance_lock.hpp): one holder at a time, the holder's pid for
// whoever waits, and a lock that goes away with its process however that process ends.
#include "fastmm/live/instance_lock.hpp"

#include "test_support.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <filesystem>
#include <string>

using namespace fastmm::live;

namespace {

std::string lock_file(const char* stem) {
  const std::string path = (fastmm::test::tmp_dir() / "instance-lock" / stem).string();
  std::filesystem::remove(path);
  return path;
}

}  // namespace

TEST_CASE("integration.instance_lock: one holder, its pid, and release") {
  const std::string path = lock_file("one.lock");
  InstanceLock a;
  InstanceLock b;
  std::string error;
  REQUIRE(a.try_acquire(path, &error) == InstanceLock::Status::Acquired);  // creates the directory
  CHECK(a.held());
  CHECK(a.path() == path);
  CHECK(a.try_acquire(path) == InstanceLock::Status::Acquired);  // again: still ours
  CHECK(InstanceLock::holder_pid(path) == static_cast<std::uint32_t>(::getpid()));
  // Another open file description, even in the same process, is refused.
  CHECK(b.try_acquire(path) == InstanceLock::Status::Held);
  CHECK_FALSE(b.held());
  a.release();
  CHECK_FALSE(a.held());
  CHECK(InstanceLock::holder_pid(path) == 0);  // the pid goes with the lock
  CHECK(std::filesystem::exists(path));        // the file stays
  CHECK(b.try_acquire(path) == InstanceLock::Status::Acquired);
  CHECK(a.try_acquire(path) == InstanceLock::Status::Held);
  {
    InstanceLock scoped;
    CHECK(scoped.try_acquire(path) == InstanceLock::Status::Held);
  }
  b.release();
  {
    InstanceLock scoped;
    CHECK(scoped.try_acquire(path) == InstanceLock::Status::Acquired);
  }  // the destructor releases
  CHECK(a.try_acquire(path) == InstanceLock::Status::Acquired);
}

TEST_CASE("integration.instance_lock: a killed holder leaves no lock behind") {
  const std::string path = lock_file("killed.lock");
  int ready[2];
  REQUIRE(::pipe(ready) == 0);
  const pid_t child = ::fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    InstanceLock held;
    const char ok = held.try_acquire(path) == InstanceLock::Status::Acquired ? '1' : '0';
    static_cast<void>(!::write(ready[1], &ok, 1));
    for (;;) ::pause();  // SIGKILLed below: no destructor, no release
  }
  ::close(ready[1]);
  char ok = 0;
  REQUIRE(::read(ready[0], &ok, 1) == 1);
  ::close(ready[0]);
  REQUIRE(ok == '1');

  InstanceLock mine;
  CHECK(mine.try_acquire(path) == InstanceLock::Status::Held);
  CHECK(InstanceLock::holder_pid(path) == static_cast<std::uint32_t>(child));
  REQUIRE(::kill(child, SIGKILL) == 0);
  int status = 0;
  REQUIRE(::waitpid(child, &status, 0) == child);
  CHECK(WIFSIGNALED(status));
  CHECK(mine.try_acquire(path) == InstanceLock::Status::Acquired);
  CHECK(InstanceLock::holder_pid(path) == static_cast<std::uint32_t>(::getpid()));
}

TEST_CASE("integration.instance_lock: the default path and an unusable one") {
  CHECK(instance_lock_path("", "runs", "mm") == "runs/mm.lock");
  CHECK(instance_lock_path("/var/lock/mm.lock", "runs", "mm") == "/var/lock/mm.lock");
  InstanceLock l;
  std::string error;
  CHECK(l.try_acquire("/proc/fastmm-no-such-dir/x.lock", &error) == InstanceLock::Status::Error);
  CHECK(error.find("/proc/fastmm-no-such-dir/x.lock") != std::string::npos);
  CHECK_FALSE(l.held());
}
