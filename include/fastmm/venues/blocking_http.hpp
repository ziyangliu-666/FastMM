#pragma once
// BlockingHttp: synchronous HTTP(S) requests on a private Reactor + HttpClient. Used only on
// control paths that must not depend on the venue's reactor thread being alive: reference data at
// startup, server-time probes, and the kill-switch cancel_all (6.7: "independent REST
// connection"). Allocates freely; never touched by the hot path.
//
// One object keeps its connection between requests (HTTP/1.1 keep-alive) and the addresses it
// looked up: the kill switch cancels symbol after symbol, and a connection per request paid a
// lookup, a CA bundle load, the TCP and TLS handshakes and the request each time. The TLS context
// is the process's (net::TlsContext::shared_client).
// A connection idle for more than kReuseIdleMs is not reused, nor one the server has closed. A
// request whose reused connection turns out to be gone before any answer is sent again on a new
// one, unless it is a POST (it may have been carried out). Not thread-safe: one thread at a time.
#include "fastmm/net/http_message.hpp"
#include "fastmm/net/source_address.hpp"
#include "fastmm/net/tcp_socket.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
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
  // Largest response body. Binance USD-M's exchangeInfo has no symbol filter and was 1.1 MB on
  // production in 2026-09 (1 MiB is the streaming client's default).
  std::size_t recv_capacity = std::size_t{16} * 1024 * 1024;
  net::SourceAddress source;  // where the connection leaves this host from; empty: anywhere
};

class BlockingHttp {
 public:
  // A connection idle this long is closed rather than reused: well inside the keep-alive timeout
  // of the venues' front ends, so the server does not close it as the request goes out.
  static constexpr std::int64_t kReuseIdleMs = 10'000;

  explicit BlockingHttp(std::string base_url, BlockingHttpOptions opts = {});
  ~BlockingHttp();
  BlockingHttp(const BlockingHttp&) = delete;
  BlockingHttp& operator=(const BlockingHttp&) = delete;

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
  // Connections opened so far.
  [[nodiscard]] std::uint64_t connects() const noexcept { return connects_; }

 private:
  struct Session;  // the reactor, the TLS context and the open connection

  // One attempt on the open connection, connecting first when there is none. `reused`: the
  // connection was there before the attempt; `lost`: it closed or failed under the request.
  HttpReply attempt(std::string_view method,
                    std::string_view target,
                    std::string_view extra_headers,
                    std::string_view body,
                    bool& reused,
                    bool& lost);
  std::string connect();  // opens the connection; the error, empty on success

  std::string base_url_;
  std::string host_;
  std::uint16_t port_ = 0;
  bool tls_ = false;
  std::string base_path_;  // path prefix from the base URL ("" or "/api")
  BlockingHttpOptions opts_;
  std::vector<net::SockAddr> addrs_;  // looked up on first use, again after a failed connect
  std::unique_ptr<Session> session_;
  std::uint64_t connects_ = 0;
};

}  // namespace fastmm::venues
