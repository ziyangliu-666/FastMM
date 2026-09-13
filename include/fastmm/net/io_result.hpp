#pragma once
// Result type for the non-throwing hot I/O paths. Every read/write/handshake returns one of
// these instead of throwing: callers branch on the flags, not on exceptions.
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace fastmm::net {

enum class NetError : std::uint8_t {
  None = 0,
  Syscall,       // errno in IoResult::err
  Closed,        // peer closed (EOF) - also reported through IoResult::closed
  Tls,           // OpenSSL failure (see TlsStream::last_error())
  Protocol,      // malformed peer data (HTTP/WS)
  Timeout,       // reactor timer expired
  Overflow,      // fixed buffer too small (message larger than capacity)
  Resolve,       // DNS failure
  InvalidState,  // operation not valid in the current state
  Canceled,      // explicitly aborted by the user
};

constexpr std::string_view to_string(NetError e) noexcept {
  switch (e) {
    case NetError::None:
      return "none";
    case NetError::Syscall:
      return "syscall";
    case NetError::Closed:
      return "closed";
    case NetError::Tls:
      return "tls";
    case NetError::Protocol:
      return "protocol";
    case NetError::Timeout:
      return "timeout";
    case NetError::Overflow:
      return "overflow";
    case NetError::Resolve:
      return "resolve";
    case NetError::InvalidState:
      return "invalid_state";
    case NetError::Canceled:
      return "canceled";
  }
  return "?";
}

struct IoResult {
  std::size_t bytes = 0;    // bytes transferred (may be > 0 together with want_* flags)
  bool want_read = false;   // retry when the fd becomes readable
  bool want_write = false;  // retry when the fd becomes writable
  bool closed = false;      // orderly EOF from the peer
  int err = 0;              // errno for NetError::Syscall
  NetError error = NetError::None;

  // The operation may be retried later; nothing is wrong.
  bool would_block() const noexcept { return want_read || want_write; }
  bool ok() const noexcept { return error == NetError::None && !closed; }
  bool failed() const noexcept { return error != NetError::None; }

  static IoResult done(std::size_t n) noexcept { return IoResult{.bytes = n}; }
  static IoResult wants_read(std::size_t n = 0) noexcept {
    return IoResult{.bytes = n, .want_read = true};
  }
  static IoResult wants_write(std::size_t n = 0) noexcept {
    return IoResult{.bytes = n, .want_write = true};
  }
  static IoResult eof() noexcept { return IoResult{.closed = true, .error = NetError::Closed}; }
  static IoResult syscall(int e) noexcept { return IoResult{.err = e, .error = NetError::Syscall}; }
  static IoResult failure(NetError e) noexcept { return IoResult{.error = e}; }
};

}  // namespace fastmm::net
