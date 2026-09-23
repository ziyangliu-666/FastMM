// The control socket's protocol (fastmm/live/control_socket.hpp): what each command turns into on
// the engine's control ring, what it refuses, and one round trip over a real AF_UNIX socket.
#include "fastmm/live/control_socket.hpp"

#include "test_support.hpp"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::live;

namespace {

Price px(const char* s) {
  return Price::from_decimal(s).value();
}
Qty qt(const char* s) {
  return Qty::from_decimal(s).value();
}

InstrumentTable make_table() {
  InstrumentTable t;
  Instrument i{};
  i.symbol = "BTCUSDT";
  i.venue = VenueId{0};
  i.flags = Instrument::kEnabled;
  i.tick = px("0.01");
  i.lot = qt("0.001");
  REQUIRE(t.add(i));
  i.symbol = "ETHUSDT";
  i.venue = VenueId{1};
  REQUIRE(t.add(i));
  return t;
}

// A control plane whose engine is a vector.
struct Fake {
  InstrumentTable table = make_table();
  std::vector<std::vector<std::byte>> sent;
  std::vector<std::pair<ControlPlane::ParamValues, InstrumentId>> params;
  std::string param_error;
  bool ring_full = false;
  bool stopped = false;
  bool cleared = false;
  ControlPlane plane;

  Fake() {
    plane.instruments = &table;
    plane.limits.max_position = qt("1");
    plane.limits.orders_per_sec = 5;
    plane.submit = [this](const EventHeader& h) {
      if (ring_full) return false;
      const auto* b = reinterpret_cast<const std::byte*>(&h);
      sent.emplace_back(b, b + h.len);
      return true;
    };
    plane.params = [this](const ControlPlane::ParamValues& v, InstrumentId i) {
      if (!param_error.empty()) return param_error;
      params.emplace_back(v, i);
      return std::string();
    };
    plane.status = [] { return std::string("state      RUNNING\n"); };
    plane.request_stop = [this] { stopped = true; };
    plane.clear_kill = [this] {
      cleared = true;
      return true;
    };
    plane.venue = [this](std::string_view name, VenueId& out) {
      if (name == "binance") {
        out = VenueId{0};
        return true;
      }
      if (name == "bybit") {
        out = VenueId{1};
        return true;
      }
      return false;
    };
  }
  std::string run(std::string_view request) { return control_command(request, plane); }
  [[nodiscard]] const ControlMsg& last() const {
    REQUIRE_FALSE(sent.empty());
    return *reinterpret_cast<const ControlMsg*>(sent.back().data());
  }
};

}  // namespace

TEST_CASE("integration.control: pull and resume carry their scope to the control ring") {
  Fake f;
  CHECK(f.run("pull").starts_with("ok"));
  CHECK(f.last().command == ControlCommand::PullQuotes);
  CHECK_FALSE(f.last().hdr.instrument.valid());
  CHECK_FALSE(f.last().hdr.venue.valid());

  CHECK(f.run("pull --instrument ETHUSDT").starts_with("ok"));
  CHECK(f.last().hdr.instrument == InstrumentId{1});
  CHECK(f.run("resume --venue binance").starts_with("ok"));
  CHECK(f.last().command == ControlCommand::ResumeQuotes);
  CHECK(f.last().hdr.venue == VenueId{0});
  CHECK(f.run("resume --instrument binance:BTCUSDT").starts_with("ok"));
  CHECK(f.last().hdr.instrument == InstrumentId{0});

  CHECK(f.run("pull --instrument NOPE").starts_with("error"));
  CHECK(f.run("pull --venue nope").starts_with("error"));
  CHECK(f.run("pull --instrument").starts_with("error"));
  CHECK(f.run("pull --instrument BTCUSDT --venue binance").starts_with("error"));
  CHECK(f.run("pull --max-slippage-bps 5").starts_with("error"));
  CHECK(f.sent.size() == 4);  // nothing reached the ring
}

TEST_CASE("integration.control: flatten carries the slippage allowance") {
  Fake f;
  CHECK(f.run("flatten").starts_with("ok"));
  CHECK(f.last().command == ControlCommand::Flatten);
  CHECK(f.last().arg == 0);  // the engine takes [engine] flatten_slippage_bps
  CHECK(f.run("flatten --instrument BTCUSDT --max-slippage-bps 15").starts_with("ok"));
  CHECK(f.last().arg == 15);
  CHECK(f.last().hdr.instrument == InstrumentId{0});
  CHECK(f.run("flatten --max-slippage-bps 0").starts_with("error"));
  CHECK(f.run("flatten --max-slippage-bps x").starts_with("error"));
  CHECK(f.run("flatten --venue binance").starts_with("error"));  // no per-venue flatten
}

TEST_CASE("integration.control: limits sends the whole limit set, edited") {
  Fake f;
  const std::string reply = f.run("limits max_position=2.5 orders_per_sec=10 stp=false");
  CHECK(reply.starts_with("ok"));
  REQUIRE(f.sent.back().size() == sizeof(ControlLimitsMsg));
  const auto& m = *reinterpret_cast<const ControlLimitsMsg*>(f.sent.back().data());
  CHECK(m.command == ControlCommand::SetLimits);
  CHECK(m.limits.max_position == qt("2.5"));
  CHECK(m.limits.orders_per_sec == 10);
  CHECK_FALSE(m.limits.stp);
  // The keys it does not name keep what the session runs with, and the copy follows.
  CHECK(f.plane.limits.max_position == qt("2.5"));
  CHECK(f.run("limits price_collar_bps=50").starts_with("ok"));
  const auto& m2 = *reinterpret_cast<const ControlLimitsMsg*>(f.sent.back().data());
  CHECK(m2.limits.max_position == qt("2.5"));
  CHECK(m2.limits.price_collar_bps == 50);

  CHECK(f.run("limits nope=1").starts_with("error"));
  CHECK(f.run("limits max_position=abc").starts_with("error"));
  CHECK(f.run("limits max_position").starts_with("error"));
  CHECK(f.run("limits").starts_with("error"));
  CHECK(f.sent.size() == 2);
}

TEST_CASE("integration.control: param is validated on the control thread, never on the ring") {
  Fake f;
  CHECK(f.run("param half_spread_bps=8 quote_qty=0.01").starts_with("ok"));
  REQUIRE(f.params.size() == 1);
  CHECK(f.params[0].first.size() == 2);
  CHECK(f.params[0].first[0].first == "half_spread_bps");
  CHECK(f.params[0].first[0].second == "8");
  CHECK_FALSE(f.params[0].second.valid());
  CHECK(f.run("param half_spread_bps=8 --instrument ETHUSDT").starts_with("ok"));
  CHECK(f.params[1].second == InstrumentId{1});
  CHECK(f.sent.empty());  // a parameter update is not a control message

  f.param_error = "unknown parameter 'nope'";
  const std::string reply = f.run("param nope=1");
  CHECK(reply.starts_with("error"));
  CHECK(reply.find("unknown parameter") != std::string::npos);
  CHECK(f.run("param").starts_with("error"));
}

TEST_CASE("integration.control: kill, unkill, stop, status and the refusals") {
  Fake f;
  CHECK(f.run("kill").starts_with("ok"));
  CHECK(f.last().command == ControlCommand::TripKill);
  CHECK(f.run("unkill").starts_with("ok"));
  CHECK(f.cleared);
  CHECK(f.run("stop").starts_with("ok"));
  CHECK(f.stopped);
  CHECK(f.run("status") == "state      RUNNING\n");
  CHECK(f.run("help").find("flatten") != std::string::npos);
  CHECK(f.run("kill --instrument BTCUSDT").starts_with("error"));
  CHECK(f.run("").starts_with("error"));
  CHECK(f.run("dance").starts_with("error"));

  f.ring_full = true;
  const std::string reply = f.run("pull");
  CHECK(reply.starts_with("error"));
  CHECK(reply.find("full") != std::string::npos);
}

TEST_CASE("integration.control: the socket answers one command per connection, mode 0600") {
  Fake f;
  // Short: sockaddr_un holds 107 bytes, and the build directory alone can be longer.
  const std::string path = (std::filesystem::temp_directory_path() /
                            ("fastmm-ctl-" + std::to_string(::getpid()) + ".ctl"))
                               .string();
  std::filesystem::remove(path);
  ControlSocket socket;
  std::string error;
  REQUIRE_MESSAGE(socket.open(path, &error), error);
  struct stat st {};
  REQUIRE(::stat(path.c_str(), &st) == 0);
  CHECK((st.st_mode & 0777) == 0600);

  const auto ask = [&](const std::string& request) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    REQUIRE(fd >= 0);
    REQUIRE(::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) == 0);
    REQUIRE(::send(fd, request.data(), request.size(), MSG_NOSIGNAL) ==
            static_cast<ssize_t>(request.size()));
    socket.poll(f.plane);  // the control thread's 50 ms turn
    char buf[4096];
    const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
    ::close(fd);
    return n > 0 ? std::string(buf, static_cast<std::size_t>(n)) : std::string();
  };
  CHECK(ask("status") == "state      RUNNING\n");
  CHECK(ask("pull --instrument BTCUSDT").starts_with("ok"));
  CHECK(f.last().hdr.instrument == InstrumentId{0});
  CHECK(ask("nonsense").starts_with("error"));
  CHECK(socket.requests() == 3);

  // A client that connects and says nothing is dropped, not answered.
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
  const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  REQUIRE(::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) == 0);
  socket.poll(f.plane);
  CHECK(socket.requests() == 3);
  ::close(fd);

  socket.close();
  CHECK_FALSE(std::filesystem::exists(path));
  // A socket left behind by a session that crashed is replaced; a plain file is not touched.
  REQUIRE(socket.open(path, &error));
  socket.close();
  {
    const std::string file = path + ".notasocket";
    std::filesystem::remove(file);
    REQUIRE(std::ofstream(file).put('x'));
    CHECK_FALSE(socket.open(file, &error));
    CHECK(error.find("not a socket") != std::string::npos);
    CHECK(std::filesystem::exists(file));
    std::filesystem::remove(file);
  }
}
