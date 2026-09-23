#pragma once
// The HTTP endpoint behind `fastmm-top --metrics`: serves the Prometheus text of the status file
// the engine publishes. It runs in fastmm-top, not in fastmm-live, so a scrape costs the trading
// process nothing; one request is served at a time, which is all a scrape needs.
#include <atomic>
#include <cstdint>
#include <string>

namespace fastmm::top {

// `bind_spec` is "port", "host:port" or "[v6addr]:port"; the default host is 127.0.0.1.
// Returns false and fills `error` when the address does not parse or the socket cannot listen.
struct MetricsServer {
  [[nodiscard]] bool listen(const std::string& bind_spec, std::string* error);
  // Serves until `stop` becomes non-zero, reading a fresh snapshot for every scrape; a scrape
  // that finds no session answers with fastmm_up 0.
  void serve(const std::string& status_path, const std::atomic<int>& stop);
  [[nodiscard]] std::string address() const;
  ~MetricsServer();

 private:
  int fd_ = -1;
  std::string host_;
  std::uint16_t port_ = 0;
};

}  // namespace fastmm::top
