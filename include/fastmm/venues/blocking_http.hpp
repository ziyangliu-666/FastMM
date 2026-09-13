#pragma once
// BlockingHttp: one synchronous HTTPS request on a private Reactor + HttpClient. Used only
// on control paths that must not depend on the venue's reactor thread being alive:
// reference data at startup, server-time probes, and the kill-switch cancel_all (6.7:
// "independent REST connection"). Allocates freely; never touched by the hot path.
#include "fastmm/net/http_message.hpp"
#include "fastmm/net/tls_stream.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fastmm::venues {

struct HttpReply {
  int status = 0;
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string error;  // non-empty on transport failure (status == 0)

  [[nodiscard]] bool ok() const noexcept { return error.empty() && status >= 200 && status < 300; }
  [[nodiscard]] std::string_view header(std::string_view name) const noexcept {
    for (const auto& [k, v] : headers) {
      if (net::iequals(k, name)) return v;
    }
    return {};
  }
};

struct BlockingHttpOptions {
  std::string ca_file;
  bool insecure_tls = false;
  std::uint32_t timeout_ms = 5000;
  std::uint32_t connect_timeout_ms = 5000;
};

class BlockingHttp {
 public:
  explicit BlockingHttp(std::string base_url, BlockingHttpOptions opts = {});

  // `target` is the path plus query ("/api/v3/depth?symbol=BTCUSDT"); `extra_headers` is a
  // pre-formatted "Name: value\r\n" block.
  HttpReply request(std::string_view method,
                    std::string_view target,
                    std::string_view extra_headers = {},
                    std::string_view body = {});
  HttpReply get(std::string_view target, std::string_view extra_headers = {}) {
    return request("GET", target, extra_headers);
  }

  [[nodiscard]] const std::string& base_url() const noexcept { return base_url_; }
  [[nodiscard]] std::string_view host() const noexcept { return host_; }

 private:
  std::string base_url_;
  std::string host_;
  std::uint16_t port_ = 0;
  bool tls_ = false;
  std::string base_path_;  // path prefix from the base URL ("" or "/api")
  BlockingHttpOptions opts_;
};

}  // namespace fastmm::venues
