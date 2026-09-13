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
