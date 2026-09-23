// fastmm-ctl: the operator's client for a running fastmm-live session. It connects to the
// session's control socket (fastmm/live/control_socket.hpp), sends one command and prints the
// reply. Everything it does can also be done with `socat - UNIX-CONNECT:<socket>,socktype=5`.
#include "fastmm/config/config.hpp"
#include "fastmm/live/control_socket.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <string_view>

namespace {

void usage(std::FILE* out) {
  std::fputs(
      "usage: fastmm-ctl [--name <engine> | --path <socket> | --config <file.toml>] <command>\n"
      "\n"
      "  --name <engine>     talk to <dir>/<engine>.ctl ([engine] name in the config)\n"
      "  --dir <directory>   where --name looks, default runs ([engine] journal_dir)\n"
      "  --path <socket>     talk to this socket (fastmm-live --control <path>)\n"
      "  --config <file>     take the engine name and journal_dir from a configuration file\n"
      "  --timeout <ms>      how long to wait for the reply, default 2000\n"
      "  --help\n"
      "\n",
      out);
  std::fputs(std::string(fastmm::live::control_usage()).c_str(), out);
  std::fputs(
      "\n"
      "examples:\n"
      "  fastmm-ctl --name mm status\n"
      "  fastmm-ctl --name mm pull --instrument BTCUSDT\n"
      "  fastmm-ctl --name mm param half_spread_bps=8\n"
      "  fastmm-ctl --name mm limits max_position=0.5 orders_per_sec=10\n"
      "  fastmm-ctl --name mm flatten --max-slippage-bps 15\n"
      "\n"
      "Exit codes: 0 the session answered ok, 1 it answered error, 2 bad command line,\n"
      "3 no session answered (no socket, or it is not running).\n",
      out);
}

constexpr int kExitOk = 0;
constexpr int kExitError = 1;
constexpr int kExitUsage = 2;
constexpr int kExitUnreachable = 3;

}  // namespace

int main(int argc, char** argv) {
  std::string path;
  std::string name;
  std::string dir = "runs";
  std::string config;
  int timeout_ms = 2000;
  std::string command;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    const auto value = [&](std::string& out) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "fastmm-ctl: %s needs a value\n", argv[i]);
        return false;
      }
      out = argv[++i];
      return true;
    };
    if (command.empty()) {
      if (a == "--help" || a == "-h") {
        usage(stdout);
        return kExitOk;
      }
      if (a == "--path") {
        if (!value(path)) return kExitUsage;
        continue;
      }
      if (a == "--name") {
        if (!value(name)) return kExitUsage;
        continue;
      }
      if (a == "--dir") {
        if (!value(dir)) return kExitUsage;
        continue;
      }
      if (a == "--config") {
        if (!value(config)) return kExitUsage;
        continue;
      }
      if (a == "--timeout") {
        std::string v;
        if (!value(v)) return kExitUsage;
        const auto r = std::from_chars(v.data(), v.data() + v.size(), timeout_ms);
        if (r.ec != std::errc{} || timeout_ms <= 0) {
          std::fprintf(stderr, "fastmm-ctl: --timeout needs milliseconds > 0\n");
          return kExitUsage;
        }
        continue;
      }
      if (a.starts_with("--")) {
        std::fprintf(stderr, "fastmm-ctl: unknown option '%s'\n\n", argv[i]);
        usage(stderr);
        return kExitUsage;
      }
    }
    // Everything from the first non-option word on is the command, flags included.
    if (!command.empty()) command += ' ';
    command += a;
  }
  if (command.empty()) {
    usage(stderr);
    return kExitUsage;
  }
  if (!config.empty()) {
    try {
      fastmm::Config::LoadOptions lo;
      lo.substitute_env = false;
      const fastmm::Config cfg = fastmm::Config::load(config, lo);
      if (name.empty()) name = cfg.engine.name;
      dir = cfg.engine.journal_dir;
    } catch (const std::exception& e) {
      std::fprintf(stderr, "fastmm-ctl: %s\n", e.what());
      return kExitUsage;
    }
  }
  if (path.empty()) {
    if (name.empty()) {
      std::fprintf(stderr, "fastmm-ctl: one of --name, --path or --config is required\n\n");
      usage(stderr);
      return kExitUsage;
    }
    path = dir + "/" + name + ".ctl";
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() + 1 > sizeof addr.sun_path) {
    std::fprintf(stderr, "fastmm-ctl: socket path is too long: %s\n", path.c_str());
    return kExitUsage;
  }
  std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
  const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    std::fprintf(stderr, "fastmm-ctl: socket: %s\n", std::strerror(errno));
    return kExitUnreachable;
  }
  timeval tv{};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  static_cast<void>(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv));
  static_cast<void>(::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv));
  if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
    std::fprintf(stderr,
                 "fastmm-ctl: cannot reach %s: %s (is fastmm-live running with a control "
                 "socket?)\n",
                 path.c_str(),
                 std::strerror(errno));
    ::close(fd);
    return kExitUnreachable;
  }
  if (::send(fd, command.data(), command.size(), MSG_NOSIGNAL) < 0) {
    std::fprintf(stderr, "fastmm-ctl: send: %s\n", std::strerror(errno));
    ::close(fd);
    return kExitUnreachable;
  }
  char buf[64 * 1024];
  const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
  ::close(fd);
  if (n <= 0) {
    std::fprintf(stderr, "fastmm-ctl: no reply from %s within %d ms\n", path.c_str(), timeout_ms);
    return kExitUnreachable;
  }
  const std::string_view reply(buf, static_cast<std::size_t>(n));
  std::fwrite(reply.data(), 1, reply.size(), stdout);
  if (!reply.empty() && reply.back() != '\n') std::fputc('\n', stdout);
  return reply.starts_with("error") ? kExitError : kExitOk;
}
