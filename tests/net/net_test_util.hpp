#pragma once
// Helpers shared by the net tests: reactor pumping with a deadline, byte/string views.
#include "test_support.hpp"

#include "fastmm/net/reactor.hpp"
#include "fastmm/net/tcp_socket.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

// FASTMM_BACKEND_TEST("title", fn) { ... } defines the body once as `static void
// fn(ReactorBackend backend)` and registers it twice, as "title [epoll]" and "title [io_uring]".
// The io_uring case passes with a message when the kernel cannot run io_uring (ENOSYS, EPERM,
// kernel.io_uring_disabled, ENOMEM, too old). The body constructs `Reactor r(backend)`.
#define FASTMM_BACKEND_TEST(title, fn)                                   \
  static void fn(::fastmm::net::ReactorBackend backend);                 \
  TEST_CASE(title " [epoll]") {                                          \
    fn(::fastmm::net::ReactorBackend::Epoll);                            \
  }                                                                      \
  TEST_CASE(title " [io_uring]") {                                       \
    if (!::fastmm::net::Reactor::io_uring_supported()) {                 \
      MESSAGE("io_uring is not available on this kernel, test skipped"); \
      return;                                                            \
    }                                                                    \
    fn(::fastmm::net::ReactorBackend::IoUring);                          \
  }                                                                      \
  static void fn([[maybe_unused]] ::fastmm::net::ReactorBackend backend)

namespace fastmm::net::test {

// Runs the reactor until `pred()` is true or `timeout_ms` elapsed. Returns pred().
template <class Pred>
bool run_until(Reactor& r, Pred pred, int timeout_ms = 3000) {
  const std::int64_t deadline =
      Reactor::now_ns() + static_cast<std::int64_t>(timeout_ms) * 1'000'000;
  while (!pred()) {
    if (Reactor::now_ns() >= deadline) return false;
    r.run_once(10);
  }
  return true;
}

inline std::string_view sv(std::span<const std::byte> s) {
  return {reinterpret_cast<const char*>(s.data()), s.size()};
}
inline std::span<const std::byte> bytes(std::string_view s) {
  return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}
inline std::span<std::byte> mutable_bytes(std::string& s) {
  return {reinterpret_cast<std::byte*>(s.data()), s.size()};
}

// Ephemeral loopback listener; the OS picks the port.
inline TcpSocket listen_ephemeral(std::uint16_t& port) {
  TcpSocket l = TcpSocket::listen(SockAddr::loopback_v4(0));
  REQUIRE(l.valid());
  auto addr = l.local_addr();
  REQUIRE(addr.has_value());
  port = addr->port();
  REQUIRE(port != 0);
  return l;
}

inline std::filesystem::path tls_fixture(const char* name) {
  return fastmm::test::fixtures_dir() / "tls" / name;
}

}  // namespace fastmm::net::test
