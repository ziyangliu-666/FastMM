#pragma once
// DNS resolution. getaddrinfo() blocks, so the async variant runs it on a helper thread and
// delivers the result to the reactor thread through Reactor::post().
//
// The helpers are a pool shared by the whole process, at most kMaxResolverThreads, started when a
// lookup finds every one of them busy. They run where the main thread may run, on the default
// scheduler (leave_pinned_cpus): the connections that ask live on pinned, possibly SCHED_FIFO
// network threads, and a thread started from one inherits its core and its priority.
#include "fastmm/net/reactor.hpp"
#include "fastmm/net/tcp_socket.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm::net {

struct ResolveResult {
  std::vector<SockAddr> addrs;     // IPv4 first (v4 is what the venues publish), then IPv6
  int gai_error = 0;               // getaddrinfo() code; 0 on success
  std::string error_text() const;  // gai_strerror
  bool ok() const noexcept { return gai_error == 0 && !addrs.empty(); }
};

// Blocking resolve (startup path). Numeric hosts resolve without a lookup.
ResolveResult resolve_sync(std::string_view host, std::uint16_t port);

inline constexpr std::size_t kMaxResolverThreads = 4;

namespace detail {
struct ResolveTarget;  // the reactor results go to, until the AsyncResolver is destroyed
}

class AsyncResolver {
 public:
  using Callback = std::function<void(ResolveResult)>;

  explicit AsyncResolver(Reactor& reactor);
  // Results not yet delivered are dropped, posted ones included. Destroy it before the reactor.
  ~AsyncResolver();
  AsyncResolver(const AsyncResolver&) = delete;
  AsyncResolver& operator=(const AsyncResolver&) = delete;

  // Thread-safe. `cb` runs on the reactor thread (via post) once the lookup completes. A numeric
  // host is answered through the same post, without a helper.
  void resolve(std::string host, std::uint16_t port, Callback cb);

 private:
  std::shared_ptr<detail::ResolveTarget> target_;
};

}  // namespace fastmm::net
