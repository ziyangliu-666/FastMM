#pragma once
// In-process scripted venue endpoints for the connector tests: one HttpServer (REST routes +
// WebSocket upgrades) on an ephemeral loopback port, running its own Reactor on a background
// thread so blocking control calls (BlockingHttp reference data, cancel_all) can reach it
// while the connector under test runs on the test thread's reactor.
//
// Thread rules: routes and WsSessionHandler callbacks run on the server thread; the test
// thread talks to sessions only via post() and reads recorded data under the mutex.
#include "venue_test_util.hpp"

#include "fastmm/net/http_server.hpp"
#include "fastmm/net/reactor.hpp"
#include "fastmm/net/ws_server.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fastmm::venues::test {

class FakeVenueServer final : public net::WsSessionHandler {
 public:
  using TextFn = std::function<void(net::WsSession&, std::string_view)>;
  using OpenFn = std::function<void(net::WsSession&)>;

  FakeVenueServer()
      : server_(reactor_, [](net::TcpSocket&& s) { return net::PlainStream(std::move(s)); }) {
    server_.set_ws_handler(this);
  }
  ~FakeVenueServer() override { stop(); }

  // Register routes / handlers before start().
  void route(const std::string& method, const std::string& path, net::HttpRouteHandler h) {
    server_.route(method, path, std::move(h));
  }
  void on_ws_open(const std::string& path, OpenFn fn) { open_fns_[path] = std::move(fn); }
  void on_ws_text(const std::string& path, TextFn fn) { text_fns_[path] = std::move(fn); }

  void start() {
    REQUIRE(server_.listen(net::SockAddr::loopback_v4(0)));
    port_ = server_.port();
    thread_ = std::thread([this] { reactor_.run(stop_); });
  }
  void stop() {
    if (!thread_.joinable()) return;
    stop_.store(true);
    reactor_.wake();
    thread_.join();
  }
  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] std::string http_base() const {
    return "http://127.0.0.1:" + std::to_string(port_);
  }
  [[nodiscard]] std::string ws_base() const { return "ws://127.0.0.1:" + std::to_string(port_); }

  // Runs `fn` on the server thread and waits for it.
  void run_on_server(std::function<void()> fn) {
    std::promise<void> done;
    auto fut = done.get_future();
    reactor_.post([&] {
      fn();
      done.set_value();
    });
    fut.wait();
  }
  // Sends a text frame to every open session on `path` (server thread).
  void send_to(const std::string& path, std::string text) {
    run_on_server([this, path, text = std::move(text)] {
      for (net::WsSession* s : sessions_[path]) s->send_text(text);
    });
  }
  // Sends a binary frame to every open session on `path` (server thread).
  void send_binary_to(const std::string& path, std::vector<std::byte> data) {
    run_on_server([this, path, data = std::move(data)] {
      for (net::WsSession* s : sessions_[path]) s->send_binary(data);
    });
  }
  void close_sessions(const std::string& path) {
    run_on_server([this, path] {
      for (net::WsSession* s : sessions_[path]) s->close_abrupt();
    });
  }

  // Recorded frames per path (thread-safe copies).
  [[nodiscard]] std::vector<std::string> frames(const std::string& path) {
    std::lock_guard<std::mutex> lk(mu_);
    return received_[path];
  }
  [[nodiscard]] std::size_t open_count(const std::string& path) {
    std::lock_guard<std::mutex> lk(mu_);
    return opened_[path];
  }
  void record(const std::string& key, std::string value) {
    std::lock_guard<std::mutex> lk(mu_);
    received_[key].push_back(std::move(value));
  }

  // ---- WsSessionHandler (server thread) --------------------------------------------------
  void on_open(net::WsSession& s) override {
    const std::string path(s.path());
    sessions_[path].push_back(&s);
    {
      std::lock_guard<std::mutex> lk(mu_);
      ++opened_[path];
    }
    if (auto it = open_fns_.find(path); it != open_fns_.end()) it->second(s);
  }
  void on_text(net::WsSession& s, std::string_view t) override {
    const std::string path(s.path());
    record(path, std::string(t));
    if (auto it = text_fns_.find(path); it != text_fns_.end()) it->second(s, t);
  }
  void on_close(net::WsSession& s, std::uint16_t, std::string_view) override { forget(s); }
  void on_error(net::WsSession& s, net::NetError, std::string_view) override { forget(s); }

 private:
  void forget(net::WsSession& s) {
    for (auto& [path, v] : sessions_) std::erase(v, &s);
  }

  net::Reactor reactor_;
  net::HttpServer<net::PlainStream> server_;
  std::atomic<bool> stop_{false};
  std::thread thread_;
  std::uint16_t port_ = 0;
  std::map<std::string, OpenFn> open_fns_;
  std::map<std::string, TextFn> text_fns_;
  std::map<std::string, std::vector<net::WsSession*>> sessions_;
  std::mutex mu_;
  std::map<std::string, std::vector<std::string>> received_;
  std::map<std::string, std::size_t> opened_;
};

// Pumps `reactor` until pred() or the timeout; returns pred().
template <class Pred>
bool pump_until(net::Reactor& reactor, Pred pred, int timeout_ms = 5000) {
  const std::int64_t deadline =
      net::Reactor::now_ns() + static_cast<std::int64_t>(timeout_ms) * 1'000'000;
  while (!pred()) {
    if (net::Reactor::now_ns() >= deadline) return false;
    reactor.run_once(5);
  }
  return true;
}

// Extracts "key":"value" (string) from a JSON text; empty if absent. Test helper only.
inline std::string json_str(std::string_view json, std::string_view key) {
  const std::string needle = "\"" + std::string(key) + "\":\"";
  const std::size_t p = json.find(needle);
  if (p == std::string_view::npos) return {};
  const std::size_t start = p + needle.size();
  const std::size_t end = json.find('"', start);
  return std::string(json.substr(start, end - start));
}

// Extracts "key":123 (unquoted integer) as text; empty if absent. Test helper only.
inline std::string json_int(std::string_view json, std::string_view key) {
  const std::string needle = "\"" + std::string(key) + "\":";
  const std::size_t p = json.find(needle);
  if (p == std::string_view::npos) return {};
  std::size_t end = p + needle.size();
  while (end < json.size() && ((json[end] >= '0' && json[end] <= '9') || json[end] == '-')) ++end;
  return std::string(json.substr(p + needle.size(), end - p - needle.size()));
}

// Copies every message out of a ring sink and returns those of `type`.
struct Collected {
  std::vector<std::vector<std::byte>> all;
  void take(RecordingSink& rs) {
    for (auto& m : rs.drain()) all.push_back(std::move(m));
  }
  [[nodiscard]] std::size_t count(EventType t) const {
    std::size_t n = 0;
    for (const auto& m : all) n += RecordingSink::type_of(m) == t ? 1U : 0U;
    return n;
  }
  template <class M, class Pred>
  [[nodiscard]] const M* first_if(EventType t, Pred&& pred) const {
    for (const auto& m : all) {
      if (RecordingSink::type_of(m) == t && pred(RecordingSink::as<M>(m)))
        return &RecordingSink::as<M>(m);
    }
    return nullptr;
  }
  template <class M, class Pred>
  [[nodiscard]] const M* last_if(EventType t, Pred&& pred) const {
    const M* out = nullptr;
    for (const auto& m : all) {
      if (RecordingSink::type_of(m) == t && pred(RecordingSink::as<M>(m)))
        out = &RecordingSink::as<M>(m);
    }
    return out;
  }
  template <class M>
  [[nodiscard]] const M* last(EventType t) const {
    const M* out = nullptr;
    for (const auto& m : all) {
      if (RecordingSink::type_of(m) == t) out = &RecordingSink::as<M>(m);
    }
    return out;
  }
};

}  // namespace fastmm::venues::test
