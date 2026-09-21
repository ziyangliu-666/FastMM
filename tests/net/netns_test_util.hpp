#pragma once
// Runs a doctest case inside a user + network namespace with a multicast-capable lo.
//
//   TEST_CASE("KernelDatagramSource: ...") {
//     if (!fastmm::net::test::in_multicast_netns()) return;
//     ...  // runs in the namespace
//   }
//
// In the test process in_multicast_netns() re-executes the binary for this test case only, with
// FASTMM_TEST_NETNS set, checks the child's exit status and returns false. In the child it enters
// the namespace (once per process), reports through a pipe that it ran, and returns true. When
// namespaces are unavailable (kernel setting, AppArmor, seccomp) the child exits with 77 and the
// parent passes with a message.
// Test case names must not contain ',' '*' or '?' (doctest filter syntax).
#include "netns.hpp"

#include <doctest/doctest.h>

#include <sys/wait.h>

#include <cstdlib>
#include <string>
#include <string_view>

namespace fastmm::net::test {

inline constexpr int kNetnsUnavailable = 77;

inline bool in_multicast_netns() {
  if (const char* env = std::getenv("FASTMM_TEST_NETNS"); env != nullptr) {
    static const std::string err = enter_multicast_netns();
    const int report_fd = std::atoi(env);
    if (!err.empty()) {
      if (report_fd > 2) [[maybe_unused]]
        const ssize_t w = ::write(report_fd, err.data(), err.size());
      std::fflush(stdout);
      std::_Exit(kNetnsUnavailable);
    }
    if (report_fd > 2) [[maybe_unused]]
      const ssize_t w = ::write(report_fd, "+", 1);  // the case ran
    return true;
  }

  const std::string name = doctest::getContextOptions()->currentTest->m_name;
  REQUIRE(name.find_first_of(",*?") == std::string::npos);
  int report[2];
  REQUIRE(::pipe2(report, O_CLOEXEC) == 0);
  std::fflush(stdout);
  std::fflush(stderr);
  const pid_t pid = ::fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    ::close(report[0]);
    ::fcntl(report[1], F_SETFD, 0);  // the child writes its skip reason here
    const std::string fd_text = std::to_string(report[1]);
    ::setenv("FASTMM_TEST_NETNS", fd_text.c_str(), 1);
    std::string filter = "--test-case=" + name;
    char self[] = "/proc/self/exe";
    char minimal[] = "--minimal";  // failures only
    char* argv[] = {self, filter.data(), minimal, nullptr};
    ::execv(self, argv);
    std::_Exit(127);
  }
  ::close(report[1]);
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  std::string reason;
  char buf[256];
  for (ssize_t n = 0; (n = ::read(report[0], buf, sizeof(buf))) > 0;) {
    reason.append(buf, static_cast<std::size_t>(n));
  }
  ::close(report[0]);
  if (WIFEXITED(status) && WEXITSTATUS(status) == kNetnsUnavailable) {
    MESSAGE("skipped: no user and network namespace (" << reason << ")");
    return false;
  }
  INFO("test case re-run in a network namespace: exit status " << status);
  CHECK((WIFEXITED(status) && WEXITSTATUS(status) == 0));
  CHECK_MESSAGE(reason.starts_with("+"), "the child did not run the test case");
  return false;
}

}  // namespace fastmm::net::test
