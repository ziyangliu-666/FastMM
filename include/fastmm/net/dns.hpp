#pragma once
// DNS resolution. getaddrinfo() blocks, so the async variant runs it on a helper thread and
// delivers the result to the reactor thread through Reactor::post().
#include "fastmm/net/reactor.hpp"
#include "fastmm/net/tcp_socket.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
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

class AsyncResolver {
 public:
  using Callback = std::function<void(ResolveResult)>;

  explicit AsyncResolver(Reactor& reactor);
  ~AsyncResolver();  // stops the worker; results not yet posted are dropped
  AsyncResolver(const AsyncResolver&) = delete;
  AsyncResolver& operator=(const AsyncResolver&) = delete;

  // Thread-safe. `cb` runs on the reactor thread (via post) once the lookup completes.
  void resolve(std::string host, std::uint16_t port, Callback cb);

 private:
  struct Request {
    std::string host;
    std::uint16_t port;
    Callback cb;
  };
  void worker(const std::stop_token& st);

  Reactor& reactor_;
  std::mutex mutex_;
  std::condition_variable_any cv_;
  std::deque<Request> queue_;
  std::jthread thread_;
};

}  // namespace fastmm::net
