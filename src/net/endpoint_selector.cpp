#include "fastmm/net/endpoint_selector.hpp"

#include "fastmm/core/log.hpp"
#include "fastmm/core/thread_utils.hpp"

#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <span>
#include <utility>

namespace fastmm::net {

namespace {

std::int64_t mono_ns() noexcept {
  timespec ts{};
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

// Same address and port, whatever the padding of the storage holds.
bool same(const SockAddr& a, const SockAddr& b) noexcept {
  if (a.family() != b.family()) return false;
  if (a.family() == AF_INET) {
    const auto* x = reinterpret_cast<const sockaddr_in*>(&a.storage);
    const auto* y = reinterpret_cast<const sockaddr_in*>(&b.storage);
    return x->sin_addr.s_addr == y->sin_addr.s_addr && x->sin_port == y->sin_port;
  }
  if (a.family() == AF_INET6) {
    const auto* x = reinterpret_cast<const sockaddr_in6*>(&a.storage);
    const auto* y = reinterpret_cast<const sockaddr_in6*>(&b.storage);
    return std::memcmp(&x->sin6_addr, &y->sin6_addr, sizeof(in6_addr)) == 0 &&
           x->sin6_port == y->sin6_port;
  }
  return a.len == b.len && std::memcmp(&a.storage, &b.storage, a.len) == 0;
}

std::int64_t median(std::vector<std::int64_t>& v) {
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

std::int64_t us(std::int64_t ns) noexcept {
  return ns < 0 ? -1 : (ns + 500) / 1000;
}

// "ip:port" into `buf`, as a view (no allocation).
std::string_view text(const SockAddr& a, std::span<char> buf) noexcept {
  return {buf.data(), a.format(buf)};
}

}  // namespace

std::optional<std::int64_t> probe_connect_ns(const SockAddr& peer,
                                             const SourceAddress& source,
                                             std::uint32_t timeout_ms) {
  const int fd = ::socket(peer.family(), SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) return std::nullopt;
  struct Closer {
    int fd;
    ~Closer() {
      // A reset, not a FIN: the probe leaves nothing in TIME_WAIT on either side.
      const linger l{1, 0};
      ::setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof l);
      ::close(fd);
    }
  } closer{fd};
  if (const std::optional<SockAddr> local = source_bind_address(source, peer.family())) {
    if (::bind(fd, local->ptr(), local->len) != 0) return std::nullopt;
  }
  const std::int64_t t0 = mono_ns();
  if (::connect(fd, peer.ptr(), peer.len) == 0) return mono_ns() - t0;
  if (errno != EINPROGRESS) return std::nullopt;
  pollfd p{fd, POLLOUT, 0};
  const std::int64_t deadline = t0 + static_cast<std::int64_t>(timeout_ms) * 1'000'000;
  for (;;) {
    const std::int64_t left_ms = (deadline - mono_ns() + 999'999) / 1'000'000;
    if (left_ms <= 0) return std::nullopt;
    const int rc = ::poll(&p, 1, static_cast<int>(left_ms));
    if (rc > 0) break;
    if (rc < 0 && errno != EINTR) return std::nullopt;
  }
  const std::int64_t t1 = mono_ns();
  int err = 0;
  socklen_t len = sizeof err;
  if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) return std::nullopt;
  return t1 - t0;
}

// ------------------------------------------------------------------------------- EndpointHost

EndpointHost::EndpointHost(std::string host,
                           std::uint16_t port,
                           SourceAddress source,
                           EndpointSelectConfig cfg,
                           EndpointHooks hooks)
    : host_(std::move(host)),
      port_(port),
      source_(std::move(source)),
      cfg_(cfg),
      hooks_(std::move(hooks)) {
  if (!hooks_.resolve)
    hooks_.resolve = [](std::string_view h, std::uint16_t p) { return resolve_sync(h, p); };
  if (!hooks_.connect_ns) hooks_.connect_ns = probe_connect_ns;
  cfg_.samples = std::max<std::uint32_t>(cfg_.samples, 1);
}

std::int64_t EndpointHost::forget_after_ns() const noexcept {
  if (cfg_.forget_ms > 0) return static_cast<std::int64_t>(cfg_.forget_ms) * 1'000'000;
  const std::int64_t s = std::max<std::int64_t>(3LL * cfg_.probe_interval_s, 600);
  return s * 1'000'000'000;
}

ResolveResult EndpointHost::refresh(bool background) {
  const std::int64_t asked = mono_ns();
  const std::lock_guard serial(refresh_mutex_);
  if (!background) {
    // Another connection's lookup ran while this one waited: its answer is as fresh.
    const std::lock_guard lock(mutex_);
    if (last_refresh_ns_ >= asked && !addrs_.empty()) {
      ResolveResult out;
      out.addrs = ranked_locked();
      return out;
    }
  }
  ResolveResult r = hooks_.resolve(host_, port_);
  if (r.ok()) keep_reachable(source_, r.addrs);
  const std::int64_t now = mono_ns();
  const std::int64_t forget = forget_after_ns();
  std::vector<SockAddr> to_probe;
  {
    const std::lock_guard lock(mutex_);
    if (r.ok()) {
      for (const SockAddr& a : r.addrs) {
        auto it = std::find_if(addrs_.begin(), addrs_.end(), [&](const EndpointAddress& e) {
          return same(e.addr, a);
        });
        if (it == addrs_.end()) {
          EndpointAddress e;
          e.addr = a;
          e.seen_ns = now;
          addrs_.push_back(e);
        } else {
          it->seen_ns = now;
        }
      }
    }
    std::erase_if(
        addrs_, [&](const EndpointAddress& e) { return e.users == 0 && now - e.seen_ns > forget; });
    for (const EndpointAddress& e : addrs_)
      if (now - e.seen_ns <= forget) to_probe.push_back(e.addr);
  }

  // Rounds over the addresses, so a slow moment of this thread is shared among them rather than
  // landing on one. An address whose first connect fails is not tried again this probe.
  std::vector<std::vector<std::int64_t>> samples(to_probe.size());
  std::vector<bool> dead(to_probe.size(), false);
  for (std::uint32_t round = 0; round < cfg_.samples; ++round) {
    for (std::size_t i = 0; i < to_probe.size(); ++i) {
      if (dead[i]) continue;
      const std::optional<std::int64_t> t =
          hooks_.connect_ns(to_probe[i], source_, cfg_.probe_timeout_ms);
      if (t) {
        samples[i].push_back(*t);
      } else if (samples[i].empty()) {
        dead[i] = true;
      }
      if (background && cfg_.probe_gap_us > 0)
        std::this_thread::sleep_for(std::chrono::microseconds(cfg_.probe_gap_us));
    }
  }

  ResolveResult out;
  {
    const std::lock_guard lock(mutex_);
    for (std::size_t i = 0; i < to_probe.size(); ++i) {
      auto it = std::find_if(addrs_.begin(), addrs_.end(), [&](const EndpointAddress& e) {
        return same(e.addr, to_probe[i]);
      });
      if (it == addrs_.end()) continue;
      it->probed = true;
      it->samples = static_cast<std::uint32_t>(samples[i].size());
      if (samples[i].empty()) {
        it->rtt_ns = -1;
        it->min_ns = -1;
      } else {
        it->min_ns = *std::min_element(samples[i].begin(), samples[i].end());
        it->rtt_ns = median(samples[i]);
      }
    }
    sort_locked();
    last_refresh_ns_ = mono_ns();
    out.addrs = ranked_locked();
  }
  generation_.fetch_add(1, std::memory_order_acq_rel);
  if (out.addrs.empty()) out.gai_error = r.gai_error != 0 ? r.gai_error : EAI_NONAME;
  return out;
}

// Measured addresses by median connect time, then the rest in the order the lookups gave them.
void EndpointHost::sort_locked() {
  std::stable_sort(
      addrs_.begin(), addrs_.end(), [](const EndpointAddress& a, const EndpointAddress& b) {
        const bool ma = a.rtt_ns >= 0;
        const bool mb = b.rtt_ns >= 0;
        if (ma != mb) return ma;
        return ma && a.rtt_ns < b.rtt_ns;
      });
}

std::vector<SockAddr> EndpointHost::ranked_locked() const {
  const std::int64_t now = mono_ns();
  const std::int64_t forget = forget_after_ns();
  std::vector<SockAddr> good;
  std::vector<SockAddr> rest;
  for (const EndpointAddress& e : addrs_) {
    const bool usable =
        now - e.seen_ns <= forget && e.failed_until <= now && (e.rtt_ns >= 0 || !e.probed);
    (usable ? good : rest).push_back(e.addr);
  }
  good.insert(good.end(), rest.begin(), rest.end());
  return good;
}

const EndpointAddress* EndpointHost::best_locked(std::int64_t now) const noexcept {
  const std::int64_t forget = forget_after_ns();
  const EndpointAddress* best = nullptr;
  for (const EndpointAddress& e : addrs_) {
    if (e.rtt_ns < 0 || now - e.seen_ns > forget || e.failed_until > now) continue;
    if (best == nullptr || e.rtt_ns < best->rtt_ns) best = &e;
  }
  return best;
}

std::optional<EndpointChoice> EndpointHost::acquire(bool lease) {
  const std::lock_guard lock(mutex_);
  if (addrs_.empty()) return std::nullopt;
  const std::int64_t now = mono_ns();
  const std::int64_t forget = forget_after_ns();
  const auto present = [&](const EndpointAddress& e) { return now - e.seen_ns <= forget; };
  const auto usable = [&](const EndpointAddress& e) { return present(e) && e.failed_until <= now; };
  // The widest set that still has something: measured and usable; usable; present; anything.
  int level = 0;
  const auto in = [&](const EndpointAddress& e) {
    switch (level) {
      case 0:
        return usable(e) && e.rtt_ns >= 0;
      case 1:
        return usable(e) && !e.probed;
      case 2:
        return present(e);
      default:
        return true;
    }
  };
  for (; level < 4; ++level)
    if (std::any_of(addrs_.begin(), addrs_.end(), in)) break;

  EndpointAddress* pick = nullptr;
  const EndpointAddress* best = level == 0 ? best_locked(now) : nullptr;
  const std::int64_t spread = static_cast<std::int64_t>(cfg_.spread_us) * 1000;
  for (EndpointAddress& e : addrs_) {
    if (!in(e)) continue;
    if (best != nullptr && e.rtt_ns > best->rtt_ns + spread) continue;
    if (pick == nullptr) {
      pick = &e;
      continue;
    }
    if (level == 2) {
      // Every present address failed: the one that failed longest ago.
      if (e.failed_until < pick->failed_until) pick = &e;
      continue;
    }
    if (lease && e.users != pick->users) {
      if (e.users < pick->users) pick = &e;
      continue;
    }
    if (e.rtt_ns >= 0 && pick->rtt_ns >= 0 && e.rtt_ns < pick->rtt_ns) pick = &e;
  }
  if (lease) ++pick->users;
  EndpointChoice c;
  c.addr = pick->addr;
  c.rtt_ns = pick->rtt_ns;
  c.best_ns = best != nullptr ? best->rtt_ns : pick->rtt_ns;
  c.addresses = static_cast<std::uint32_t>(addrs_.size());
  c.users = pick->users;
  return c;
}

void EndpointHost::release(const SockAddr& addr) noexcept {
  const std::lock_guard lock(mutex_);
  for (EndpointAddress& e : addrs_)
    if (same(e.addr, addr) && e.users > 0) {
      --e.users;
      return;
    }
}

void EndpointHost::failed(const SockAddr& addr) noexcept {
  const std::lock_guard lock(mutex_);
  for (EndpointAddress& e : addrs_)
    if (same(e.addr, addr)) e.failed_until = mono_ns() + kFailedHoldNs;
}

bool EndpointHost::should_move(const SockAddr& addr) const noexcept {
  const std::lock_guard lock(mutex_);
  const std::int64_t now = mono_ns();
  const EndpointAddress* best = best_locked(now);
  if (best == nullptr || same(best->addr, addr)) return false;
  const auto it = std::find_if(
      addrs_.begin(), addrs_.end(), [&](const EndpointAddress& e) { return same(e.addr, addr); });
  if (it == addrs_.end() || now - it->seen_ns > forget_after_ns()) return true;
  if (cfg_.switch_us == 0 || it->rtt_ns < 0) return false;
  return it->rtt_ns - best->rtt_ns > static_cast<std::int64_t>(cfg_.switch_us) * 1000;
}

bool EndpointHost::probed() const noexcept {
  const std::lock_guard lock(mutex_);
  return std::any_of(
      addrs_.begin(), addrs_.end(), [](const EndpointAddress& e) { return e.probed; });
}

std::vector<EndpointAddress> EndpointHost::addresses() const {
  const std::lock_guard lock(mutex_);
  return addrs_;
}

std::vector<SockAddr> EndpointHost::ranked() const {
  const std::lock_guard lock(mutex_);
  return ranked_locked();
}

// --------------------------------------------------------------------------- EndpointSelector

EndpointSelector& EndpointSelector::instance() {
  static EndpointSelector selector;
  return selector;
}

EndpointSelector::~EndpointSelector() {
  thread_.request_stop();
  cv_.notify_all();
}

void EndpointSelector::configure(const EndpointSelectConfig& cfg, EndpointHooks hooks) {
  const std::lock_guard lock(mutex_);
  cfg_ = cfg;
  hooks_ = std::move(hooks);
  enabled_.store(cfg.enabled, std::memory_order_relaxed);
  hosts_.clear();  // hosts made from here on take the new settings
  cv_.notify_all();
}

EndpointSelectConfig EndpointSelector::config() const {
  const std::lock_guard lock(mutex_);
  return cfg_;
}

std::shared_ptr<EndpointHost> EndpointSelector::host(std::string_view host,
                                                     std::uint16_t port,
                                                     const SourceAddress& source) {
  if (host.empty() || SockAddr::from_ip(host, port)) return nullptr;
  std::string key(host);
  key.append("|").append(std::to_string(port)).append("|").append(source.ip);
  key.append("|").append(source.interface);
  const std::lock_guard lock(mutex_);
  std::weak_ptr<EndpointHost>& slot = hosts_[key];
  if (std::shared_ptr<EndpointHost> h = slot.lock()) return h;
  auto h = std::make_shared<EndpointHost>(std::string(host), port, source, cfg_, hooks_);
  slot = h;
  if (cfg_.probe_interval_s > 0 && !thread_.joinable())
    thread_ = std::jthread([this](const std::stop_token& st) { run(st); });
  return h;
}

std::vector<std::shared_ptr<EndpointHost>> EndpointSelector::live_hosts() {
  const std::lock_guard lock(mutex_);
  std::vector<std::shared_ptr<EndpointHost>> out;
  for (auto it = hosts_.begin(); it != hosts_.end();) {
    if (std::shared_ptr<EndpointHost> h = it->second.lock()) {
      out.push_back(std::move(h));
      ++it;
    } else {
      it = hosts_.erase(it);
    }
  }
  return out;
}

void EndpointSelector::probe_all() {
  for (const std::shared_ptr<EndpointHost>& h : live_hosts()) {
    const std::vector<SockAddr> before = h->ranked();
    static_cast<void>(h->refresh(/*background=*/true));
    const std::vector<EndpointAddress> after = h->addresses();
    if (after.empty() || after.front().rtt_ns < 0) continue;
    // A line when the fastest address changes, not every round.
    if (!before.empty() && same(before.front(), after.front().addr)) continue;
    std::array<char, 64> best{};
    FASTMM_LOG_INFO("endpoints {}:{}: {} addresses, fastest {} {} us, slowest reachable {} us",
                    h->host(),
                    h->port(),
                    after.size(),
                    text(after.front().addr, best),
                    us(after.front().rtt_ns),
                    us(std::max_element(after.begin(),
                                        after.end(),
                                        [](const EndpointAddress& a, const EndpointAddress& b) {
                                          return a.rtt_ns < b.rtt_ns;
                                        })
                           ->rtt_ns));
  }
}

void EndpointSelector::run(const std::stop_token& st) {
  set_thread_name("fm-endpoints");
  static_cast<void>(leave_pinned_cpus());
  for (;;) {
    std::uint32_t interval = 0;
    {
      std::unique_lock lock(mutex_);
      interval = cfg_.probe_interval_s;
      if (interval == 0) {
        cv_.wait(lock, st, [this] { return cfg_.probe_interval_s > 0; });
      } else {
        cv_.wait_for(lock, st, std::chrono::seconds(interval), [] { return false; });
      }
      if (st.stop_requested()) return;
      if (cfg_.probe_interval_s == 0) continue;
    }
    probe_all();
    if (st.stop_requested()) return;
  }
}

void log_endpoint_choice(std::string_view what, const EndpointHost& h, const EndpointChoice& c) {
  std::array<char, 64> addr{};
  FASTMM_LOG_INFO("{} {}:{} -> {} connect {} us, fastest {} us of {} addresses, {} here",
                  what,
                  h.host(),
                  h.port(),
                  text(c.addr, addr),
                  us(c.rtt_ns),
                  us(c.best_ns),
                  c.addresses,
                  c.users);
}

}  // namespace fastmm::net
