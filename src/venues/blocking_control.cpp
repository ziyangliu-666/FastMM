#include "fastmm/venues/blocking_control.hpp"

#include "fastmm/core/log.hpp"
#include "fastmm/net/reactor.hpp"
#include "fastmm/venues/decimal.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <thread>

namespace fastmm::venues {

namespace {

constexpr std::int64_t kNsPerMs = 1'000'000;

// Retry-After in seconds (RFC 9110 also allows an HTTP date; no venue here sends one). -1 when
// absent or not a number.
std::int64_t retry_after_ms(const HttpReply& r) noexcept {
  const std::string_view v = r.header("Retry-After");
  if (v.empty()) return -1;
  const auto s = parse_int64(v);
  return s && *s >= 0 ? *s * 1000 : -1;
}

}  // namespace

BlockingControl::BlockingControl(std::string_view venue,
                                 const std::string& rest_url,
                                 BlockingHttpOptions opts,
                                 BlockingRetry retry)
    : venue_(venue),
      retry_(retry),
      deadline_ns_(net::Reactor::now_ns() +
                   std::max<std::int64_t>(0, retry.deadline_ms) * kNsPerMs) {
  try {
    http_ = std::make_unique<BlockingHttp>(rest_url, std::move(opts));
  } catch (const std::exception& e) {
    setup_error_ = e.what();
  }
}

BlockingControl::~BlockingControl() = default;

bool BlockingControl::rate_limited(const HttpReply& r) const noexcept {
  if (is_rate_limit_status(r.status)) return true;
  return retry_.rate_limited != nullptr && r.error.empty() && retry_.rate_limited(r);
}

HttpReply BlockingControl::send(std::string_view label, const Build& build) {
  HttpReply reply;
  if (http_ == nullptr) {
    reply.error = setup_error_;
    return reply;
  }
  for (int attempt = 0;; ++attempt) {
    BlockingRequest q;
    if (!build(q)) {
      reply = HttpReply{};
      reply.error = "request not built";
      return reply;
    }
    try {
      reply = http_->request(q.method, q.target, q.headers, q.body);
    } catch (const std::exception& e) {
      reply = HttpReply{};
      reply.error = e.what();
      return reply;
    }
    if (!rate_limited(reply) || attempt >= retry_.max_retries) return reply;
    const std::int64_t asked = retry_after_ms(reply);
    const std::int64_t wait_ms = asked >= 0 ? asked : retry_.wait_ms;
    if (net::Reactor::now_ns() + wait_ms * kNsPerMs > deadline_ns_) {
      FASTMM_LOG_ERROR("{}: {} rate limited ({}); the venue asks for {} ms, past the deadline",
                       venue_,
                       label,
                       reply.status,
                       wait_ms);
      return reply;
    }
    FASTMM_LOG_WARN(
        "{}: {} rate limited ({}); retrying in {} ms", venue_, label, reply.status, wait_ms);
    std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
  }
}

void BlockingControl::report(std::string_view label,
                             const HttpReply& reply,
                             bool ok,
                             std::string_view why) const {
  if (ok) {
    FASTMM_LOG_INFO("{}: {} ok", venue_, label);
    return;
  }
  std::string_view detail = reply.error;
  if (detail.empty()) detail = why;
  if (detail.empty()) detail = std::string_view(reply.body).substr(0, 160);
  FASTMM_LOG_ERROR("{}: {} failed: status={} {}", venue_, label, reply.status, detail);
}

}  // namespace fastmm::venues
