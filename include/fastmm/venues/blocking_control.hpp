#pragma once
// BlockingControl: the synchronous requests a connector makes when its reactor thread cannot be
// relied on, i.e. the kill-switch cancel-all and the countdown stop at shutdown. One BlockingHttp
// from the connector's config, one request or one per target, and a bounded wait on a rate limit:
// the kill switch has no other remedy than these calls, so a 418/429 is waited out rather than
// taken as final. Bounded, because a real IP ban lasts minutes and the caller must not hang on it.
// Control path only: allocates freely, sleeps.
#include "fastmm/venues/blocking_http.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace fastmm::venues {

struct BlockingRequest {
  std::string method;
  std::string target;   // path plus query
  std::string headers;  // "Name: value\r\n" block
  std::string body;
};

struct BlockingRetry {
  int max_retries = 3;         // per request, after the first refusal
  std::int64_t wait_ms = 400;  // between attempts when the venue sends no Retry-After
  // All requests and waits of one BlockingControl together: a wait that would end past it is not
  // started. Requests already under way finish (each has its own timeout).
  std::int64_t deadline_ms = 10'000;
  // A refusal the venue reports inside a 2xx (Bybit retCode 10006, OKX 50011). HTTP 418 and 429
  // are always one.
  bool (*rate_limited)(const HttpReply&) = nullptr;
};

// The options every connector config spells the same way.
template <class Cfg>
[[nodiscard]] BlockingHttpOptions blocking_options(const Cfg& cfg) {
  BlockingHttpOptions o;
  o.ca_file = cfg.ca_file;
  o.insecure_tls = cfg.insecure_tls;
  o.timeout_ms = cfg.http_timeout_ms;
  return o;
}

class BlockingControl {
 public:
  // Fills in one attempt; called again for every retry, so a signed request is signed afresh.
  // False: the request cannot be built, and is not sent.
  using Build = std::function<bool(BlockingRequest&)>;

  // `venue` names the connector in the log. Never throws: a bad URL fails every request.
  BlockingControl(std::string_view venue,
                  const std::string& rest_url,
                  BlockingHttpOptions opts,
                  BlockingRetry retry = {});
  template <class Cfg>
  explicit BlockingControl(const Cfg& cfg, BlockingRetry retry = {})
      : BlockingControl(cfg.name, cfg.rest_url, blocking_options(cfg), retry) {}
  ~BlockingControl();
  BlockingControl(const BlockingControl&) = delete;
  BlockingControl& operator=(const BlockingControl&) = delete;

  // One request, retried while the venue refuses it for rate. `label` names it in the log.
  HttpReply send(std::string_view label, const Build& build);

  // One request per target. `accepted(reply, why)` says whether the venue did what was asked,
  // which includes "there was nothing to cancel"; `why` is logged when it did not. True when
  // every target was accepted; each outcome is logged.
  template <class Target, class Name, class BuildFor, class Accepted>
  bool per_target(std::string_view what,
                  std::span<const Target> targets,
                  const Name& name,
                  const BuildFor& build,
                  const Accepted& accepted) {
    bool all_ok = true;
    for (const Target& t : targets) {
      const std::string label = std::string(what) + " for " + std::string(name(t));
      const HttpReply reply = send(label, [&](BlockingRequest& q) { return build(t, q); });
      std::string why;
      const bool ok = accepted(reply, why);
      report(label, reply, ok, why);
      all_ok = all_ok && ok;
    }
    return all_ok;
  }

  // Logs the outcome of `label`: INFO when `ok`, ERROR with the status and `why` (or the transport
  // error, or the start of the body) when not.
  void report(std::string_view label, const HttpReply& reply, bool ok, std::string_view why) const;

  [[nodiscard]] static bool is_rate_limit_status(int status) noexcept {
    return status == 418 || status == 429;
  }

 private:
  [[nodiscard]] bool rate_limited(const HttpReply& r) const noexcept;

  std::string venue_;
  std::unique_ptr<BlockingHttp> http_;
  std::string setup_error_;
  BlockingRetry retry_;
  std::int64_t deadline_ns_ = 0;
};

}  // namespace fastmm::venues
