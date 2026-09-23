#include "metrics_server.hpp"

#include "fastmm/core/status_prometheus.hpp"
#include "fastmm/core/status_segment.hpp"
#include "fastmm/core/time.hpp"

#include <fmt/format.h>

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstring>
#include <string_view>

namespace fastmm::top {

namespace {

constexpr int kPollMs = 200;          // how often serve() looks at `stop`
constexpr std::size_t kRequestMax = 4096;

std::string errno_text(const char* what) {
  return fmt::format("{}: {}", what, std::strerror(errno));
}

// "port" | "host:port" | "[v6addr]:port"
bool split_bind(const std::string& spec, std::string& host, std::string& port) {
  if (!spec.empty() && spec.front() == '[') {
    const std::size_t close = spec.find(']');
    if (close == std::string::npos || close + 2 >= spec.size() || spec[close + 1] != ':')
      return false;
    host = spec.substr(1, close - 1);
    port = spec.substr(close + 2);
  } else if (const std::size_t colon = spec.rfind(':'); colon != std::string::npos) {
    host = spec.substr(0, colon);
    port = spec.substr(colon + 1);
  } else {
    host = "127.0.0.1";
    port = spec;
  }
  if (host.empty() || port.empty()) return false;
  unsigned value = 0;
  const auto r = std::from_chars(port.data(), port.data() + port.size(), value);
  return r.ec == std::errc{} && r.ptr == port.data() + port.size() && value > 0 && value < 65536;
}

void send_all(int fd, std::string_view data) {
  while (!data.empty()) {
    const ssize_t n = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
    if (n <= 0) return;  // the scraper hung up; the next request starts clean
    data.remove_prefix(static_cast<std::size_t>(n));
  }
}

void respond(int fd, const char* status, const char* content_type, std::string_view body) {
  send_all(fd,
           fmt::format("HTTP/1.1 {}\r\nContent-Type: {}\r\nContent-Length: {}\r\n"
                       "Connection: close\r\n\r\n",
                       status,
                       content_type,
                       body.size()));
  send_all(fd, body);
}

// The first line's path, or an empty view when the request is not a GET this endpoint answers.
std::string_view request_path(std::string_view request) {
  if (request.substr(0, 4) != "GET ") return {};
  request.remove_prefix(4);
  const std::size_t end = request.find_first_of(" \r\n");
  std::string_view path = request.substr(0, end);
  const std::size_t query = path.find('?');
  return query == std::string_view::npos ? path : path.substr(0, query);
}

constexpr std::string_view kDown =
    "# HELP fastmm_up 1 while a status snapshot can be read\n"
    "# TYPE fastmm_up gauge\n"
    "fastmm_up 0\n";

constexpr std::string_view kIndex =
    "fastmm-top metrics exporter\n"
    "\n"
    "  /metrics   the running session in Prometheus text format\n";

}  // namespace

MetricsServer::~MetricsServer() {
  if (fd_ >= 0) ::close(fd_);
}

std::string MetricsServer::address() const {
  return host_.find(':') == std::string::npos ? fmt::format("{}:{}", host_, port_)
                                              : fmt::format("[{}]:{}", host_, port_);
}

bool MetricsServer::listen(const std::string& bind_spec, std::string* error) {
  std::string host;
  std::string port;
  if (!split_bind(bind_spec, host, port)) {
    if (error != nullptr) *error = "expected a port, host:port or [v6addr]:port";
    return false;
  }
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
  addrinfo* found = nullptr;
  const int rc = ::getaddrinfo(host.c_str(), port.c_str(), &hints, &found);
  if (rc != 0 || found == nullptr) {
    if (error != nullptr) *error = fmt::format("{}: {}", host, ::gai_strerror(rc));
    return false;
  }
  const int fd = ::socket(found->ai_family, found->ai_socktype | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    if (error != nullptr) *error = errno_text("socket");
    ::freeaddrinfo(found);
    return false;
  }
  const int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  const bool ok = ::bind(fd, found->ai_addr, found->ai_addrlen) == 0 && ::listen(fd, 16) == 0;
  ::freeaddrinfo(found);
  if (!ok) {
    if (error != nullptr) *error = errno_text("bind");
    ::close(fd);
    return false;
  }
  fd_ = fd;
  host_ = host;
  std::uint16_t value = 0;
  std::from_chars(port.data(), port.data() + port.size(), value);
  port_ = value;
  return true;
}

void MetricsServer::serve(const std::string& status_path, const std::atomic<int>& stop) {
  StatusReader reader;
  StatusSnapshot snapshot;
  std::string request(kRequestMax, '\0');
  while (stop.load() == 0) {
    pollfd p{fd_, POLLIN, 0};
    const int ready = ::poll(&p, 1, kPollMs);
    if (ready <= 0) continue;
    const int client = ::accept(fd_, nullptr, nullptr);
    if (client < 0) continue;
    // A client that connects and then says nothing must not hold the loop: give it two seconds.
    const timeval timeout{2, 0};
    ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    // One scrape per connection: read the head of the request, answer, close.
    const ssize_t n = ::recv(client, request.data(), request.size(), 0);
    const std::string_view path =
        n > 0 ? request_path(std::string_view(request.data(), static_cast<std::size_t>(n)))
              : std::string_view{};
    if (path == "/metrics") {
      std::string error;
      if (!reader.is_open()) static_cast<void>(reader.open(status_path, &error));
      if (reader.is_open() && reader.read(snapshot))
        respond(client, "200 OK", "text/plain; version=0.0.4; charset=utf-8",
                format_status_prometheus(snapshot, wall_now().ns));
      else
        respond(client, "200 OK", "text/plain; version=0.0.4; charset=utf-8", kDown);
    } else if (path == "/" || path.empty()) {
      respond(client, path.empty() ? "400 Bad Request" : "200 OK", "text/plain; charset=utf-8",
              kIndex);
    } else {
      respond(client, "404 Not Found", "text/plain; charset=utf-8", kIndex);
    }
    ::close(client);
  }
}

}  // namespace fastmm::top
