#include "fastmm/net/dns.hpp"

#include <netdb.h>

#include <cstring>

namespace fastmm::net {

std::string ResolveResult::error_text() const {
  return gai_error == 0 ? std::string{} : std::string(::gai_strerror(gai_error));
}

ResolveResult resolve_sync(std::string_view host, std::uint16_t port) {
  ResolveResult result;
  if (auto numeric = SockAddr::from_ip(host, port)) {
    result.addrs.push_back(*numeric);
    return result;
  }
  const std::string host_z(host);
  const std::string port_z = std::to_string(port);
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_ADDRCONFIG | AI_NUMERICSERV;
  addrinfo* list = nullptr;
  result.gai_error = ::getaddrinfo(host_z.c_str(), port_z.c_str(), &hints, &list);
  if (result.gai_error != 0) return result;
  std::vector<SockAddr> v6;
  for (addrinfo* ai = list; ai != nullptr; ai = ai->ai_next) {
    if (ai->ai_addrlen > sizeof(sockaddr_storage)) continue;
    SockAddr a;
    std::memcpy(&a.storage, ai->ai_addr, ai->ai_addrlen);
    a.len = ai->ai_addrlen;
    (ai->ai_family == AF_INET ? result.addrs : v6).push_back(a);
  }
  result.addrs.insert(result.addrs.end(), v6.begin(), v6.end());
  ::freeaddrinfo(list);
  return result;
}

AsyncResolver::AsyncResolver(Reactor& reactor)
    : reactor_(reactor), thread_([this](const std::stop_token& st) { worker(st); }) {}

AsyncResolver::~AsyncResolver() {
  thread_.request_stop();
  cv_.notify_all();
  // jthread joins in its destructor.
}

void AsyncResolver::resolve(std::string host, std::uint16_t port, Callback cb) {
  {
    std::lock_guard lock(mutex_);
    queue_.push_back(Request{std::move(host), port, std::move(cb)});
  }
  cv_.notify_one();
}

void AsyncResolver::worker(const std::stop_token& st) {
  for (;;) {
    Request req;
    {
      std::unique_lock lock(mutex_);
      if (!cv_.wait(lock, st, [this] { return !queue_.empty(); })) return;  // stop requested
      req = std::move(queue_.front());
      queue_.pop_front();
    }
    ResolveResult result = resolve_sync(req.host, req.port);
    if (st.stop_requested()) return;
    reactor_.post([cb = std::move(req.cb), r = std::move(result)]() mutable { cb(std::move(r)); });
  }
}

}  // namespace fastmm::net
