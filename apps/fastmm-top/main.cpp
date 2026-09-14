// fastmm-top: terminal dashboard for a running fastmm-live session. Reads the status file the
// engine's control thread publishes (see fastmm/core/status_segment.hpp) and redraws it in place.
#include "fastmm/core/status_segment.hpp"
#include "fastmm/core/thread_utils.hpp"
#include "fastmm/core/time.hpp"

#include <unistd.h>

#include <atomic>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace {

std::atomic<int> g_signal{0};
void on_signal(int sig) {
  g_signal.store(sig);
}

void usage(std::FILE* out) {
  std::fputs(
      "usage: fastmm-top [--name <engine name> | --path <status file>] [options]\n"
      "\n"
      "  --name <engine>     read /dev/shm/fastmm-<engine>.status ([engine] name in the config)\n"
      "  --path <file>       read this status file (fastmm-live --status <file>)\n"
      "  --interval <ms>     refresh period, default 500\n"
      "  --once              print one frame and exit (exit code 3 if no status is available)\n"
      "  --no-color          plain output\n",
      out);
}

}  // namespace

int main(int argc, char** argv) {
  std::string path;
  int interval_ms = 500;
  bool once = false;
  bool color = ::isatty(STDOUT_FILENO) != 0;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    const auto value = [&](std::string& out) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "fastmm-top: %s needs a value\n", argv[i]);
        return false;
      }
      out = argv[++i];
      return true;
    };
    std::string v;
    if (a == "--help" || a == "-h") {
      usage(stdout);
      return 0;
    }
    if (a == "--name") {
      if (!value(v)) return 2;
      path = fastmm::default_status_path(v);
    } else if (a == "--path") {
      if (!value(path)) return 2;
    } else if (a == "--interval") {
      if (!value(v)) return 2;
      const auto r = std::from_chars(v.data(), v.data() + v.size(), interval_ms);
      if (r.ec != std::errc{} || interval_ms < 50) {
        std::fprintf(stderr, "fastmm-top: --interval needs milliseconds >= 50\n");
        return 2;
      }
    } else if (a == "--once") {
      once = true;
    } else if (a == "--no-color") {
      color = false;
    } else {
      std::fprintf(stderr, "fastmm-top: unknown argument '%s'\n\n", argv[i]);
      usage(stderr);
      return 2;
    }
  }
  if (path.empty()) {
    usage(stderr);
    return 2;
  }
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  fastmm::StatusReader reader;
  fastmm::StatusSnapshot snap;
  std::string error;
  bool first = true;
  while (g_signal.load() == 0) {
    if (!reader.is_open() && !reader.open(path, &error)) {
      if (once) {
        std::fprintf(stderr,
                     "fastmm-top: no status segment at %s (%s); is fastmm-live running?\n",
                     path.c_str(),
                     error.c_str());
        return 3;
      }
    }
    std::string frame;
    // A writer of another build: its layout differs, so say so instead of waiting silently.
    const std::uint32_t version = reader.is_open() ? reader.segment_version() : 0;
    if (reader.is_open() && reader.read(snap)) {
      frame = fastmm::format_status(snap, fastmm::wall_now().ns, color);
    } else if (version != 0 && version != fastmm::kStatusVersion) {
      error = fastmm::status_version_mismatch(version);
      if (once) {
        std::fprintf(stderr, "fastmm-top: %s: %s\n", path.c_str(), error.c_str());
        return 3;
      }
      frame.append("fastmm-top: ").append(path).append(": ").append(error).append("\n");
    } else if (once) {
      std::fprintf(stderr, "fastmm-top: no status segment at %s yet\n", path.c_str());
      return 3;
    } else if (!reader.is_open() && !error.empty()) {
      frame.append("fastmm-top: waiting for ")
          .append(path)
          .append(" (")
          .append(error)
          .append(") ...\n");
    } else {
      frame = "fastmm-top: waiting for " + path + " ...\n";
    }
    if (!once) std::fputs(first ? "\x1b[2J\x1b[H" : "\x1b[H\x1b[J", stdout);
    std::fputs(frame.c_str(), stdout);
    std::fflush(stdout);
    first = false;
    if (once) return 0;
    fastmm::sleep_for(fastmm::milliseconds(interval_ms));
  }
  return 0;
}
