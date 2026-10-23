#include "fastmm/net/dns.hpp"

#include "fastmm/core/thread_utils.hpp"

#include <netdb.h>

#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>

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

namespace detail {

struct ResolveTarget {
  std::mutex mutex;
  Reactor* reactor = nullptr;  // null once the AsyncResolver is gone

  bool alive() {
    const std::lock_guard lock(mutex);
    return reactor != nullptr;
  }

  // Posts cb(result) unless the resolver is gone. The task asks again: the resolver may be
  // destroyed between the post and the task, both on the reactor thread.
  static void deliver(const std::shared_ptr<ResolveTarget>& t,
                      AsyncResolver::Callback cb,
                      ResolveResult result) {
    const std::lock_guard lock(t->mutex);
    if (t->reactor == nullptr) return;
    t->reactor->post([t, cb = std::move(cb), r = std::move(result)]() mutable {
      if (t->alive()) cb(std::move(r));
    });
  }
};

}  // namespace detail

namespace {

// The process's lookup helpers: started on first use, stopped and joined at exit (a helper inside
// getaddrinfo() finishes that lookup first).
class ResolverPool {
 public:
  static ResolverPool& instance() {
    static ResolverPool pool;
    return pool;
  }

  ResolverPool(const ResolverPool&) = delete;
  ResolverPool& operator=(const ResolverPool&) = delete;
  ~ResolverPool() {
    for (std::jthread& t : threads_) t.request_stop();
    cv_.notify_all();
  }

  void submit(std::shared_ptr<detail::ResolveTarget> target,
              std::string host,
              std::uint16_t port,
              AsyncResolver::Callback cb) {
    {
      const std::lock_guard lock(mutex_);
      queue_.push_back(Job{std::move(target), std::move(host), port, std::move(cb)});
      if (idle_ < queue_.size() && threads_.size() < kMaxResolverThreads)
        threads_.emplace_back([this](const std::stop_token& st) { work(st); });
    }
    cv_.notify_one();
  }

 private:
  ResolverPool() = default;

  struct Job {
    std::shared_ptr<detail::ResolveTarget> target;
    std::string host;
    std::uint16_t port = 0;
    AsyncResolver::Callback cb;
  };

  void work(const std::stop_token& st) {
    set_thread_name("fm-resolve");
    static_cast<void>(leave_pinned_cpus());
    for (;;) {
      Job job;
      {
        std::unique_lock lock(mutex_);
        ++idle_;
        const bool got = cv_.wait(lock, st, [this] { return !queue_.empty(); });
        --idle_;
        if (!got) return;  // stop requested
        job = std::move(queue_.front());
        queue_.pop_front();
      }
      // A resolver destroyed while its lookup waited needs no lookup.
      if (!job.target->alive()) continue;
      ResolveResult result = resolve_sync(job.host, job.port);
      if (st.stop_requested()) return;
      detail::ResolveTarget::deliver(job.target, std::move(job.cb), std::move(result));
    }
  }

  std::mutex mutex_;
  std::condition_variable_any cv_;
  std::deque<Job> queue_;
  std::size_t idle_ = 0;  // helpers waiting for a job
  std::vector<std::jthread> threads_;
};

}  // namespace

AsyncResolver::AsyncResolver(Reactor& reactor)
    : target_(std::make_shared<detail::ResolveTarget>()) {
  target_->reactor = &reactor;
}

AsyncResolver::~AsyncResolver() {
  const std::lock_guard lock(target_->mutex);
  target_->reactor = nullptr;
}

void AsyncResolver::resolve(std::string host, std::uint16_t port, Callback cb) {
  if (auto numeric = SockAddr::from_ip(host, port)) {
    ResolveResult r;
    r.addrs.push_back(*numeric);
    detail::ResolveTarget::deliver(target_, std::move(cb), std::move(r));
    return;
  }
  ResolverPool::instance().submit(target_, std::move(host), port, std::move(cb));
}

}  // namespace fastmm::net
