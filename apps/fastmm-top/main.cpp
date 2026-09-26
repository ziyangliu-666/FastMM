// fastmm-top: terminal dashboard for a running fastmm-live session. Reads the status file the
// engine's control thread publishes (see fastmm/core/status_segment.hpp) and redraws it in place.
// With --metrics it serves the same snapshot as Prometheus metrics instead of drawing.
#include "command_line.hpp"
#include "metrics_server.hpp"

#include "fastmm/core/status_segment.hpp"
#include "fastmm/core/thread_utils.hpp"
#include "fastmm/core/time.hpp"

#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {

std::atomic<int> g_signal{0};
void on_signal(int sig) {
  g_signal.store(sig);
}

std::string other_build_message(const std::string& path, std::uint32_t version) {
  return "fastmm-top: " + path +
         " was written by a different FastMM build (status segment version " +
         std::to_string(version) + ", this fastmm-top reads version " +
         std::to_string(fastmm::kStatusVersion) +
         "); use fastmm-top from the same build as fastmm-live";
}

}  // namespace

static int run(int argc, char** argv) {
  std::string name;
  std::string gateway;
  std::string path;
  std::string metrics;
  int interval_ms = 500;
  bool once = false;
  bool json = false;
  bool no_color = false;

  CLI::App app("Terminal dashboard for a running fastmm-live session or fastmm-gateway.",
               "fastmm-top");
  fastmm::cli::setup(app);
  app.usage(
      "fastmm-top [--name <engine name> | --gateway <engine name> | --path <status file>] "
      "[OPTIONS]");
  app.add_option(
         "--name", name, "read /dev/shm/fastmm-<engine>.status ([engine] name in the config)")
      ->option_text("<engine>");
  app.add_option("--gateway",
                 gateway,
                 "read the fastmm-gateway's /dev/shm/fastmm-<engine>.gw.status ([engine] name in "
                 "its config)")
      ->option_text("<engine>");
  app.add_option("--path", path, "read this status file (fastmm-live or fastmm-gateway --status)")
      ->option_text("<file>");
  app.add_option("--interval", interval_ms, "refresh period, default 500")
      ->option_text("<ms>")
      ->check(CLI::Range(50, 3'600'000));
  app.add_flag("--once", once, "print one frame and exit (exit code 3 if no status is available)");
  app.add_flag("--json", json, "print the snapshot as one JSON object and exit (implies --once)");
  app.add_flag("--no-color", no_color, "plain output");
  app.add_option("--metrics",
                 metrics,
                 "serve the snapshot at /metrics in Prometheus text format until SIGINT, instead "
                 "of drawing (default host 127.0.0.1; off unless given). Scraping costs the "
                 "engine nothing: this process reads the status file, the engine never sees the "
                 "request.")
      ->option_text("<[host:]port>");
  if (const auto rc = fastmm::cli::parse(app, argc, argv)) return *rc;
  if (!name.empty() && !gateway.empty())
    return fastmm::cli::usage_error(app, "give --name or --gateway, not both");
  if (path.empty() && !name.empty()) path = fastmm::default_status_path(name);
  if (path.empty() && !gateway.empty()) path = fastmm::default_gateway_status_path(gateway);
  if (path.empty())
    return fastmm::cli::usage_error(app, "one of --name, --gateway or --path is required");
  once = once || json;
  const bool color = !no_color && ::isatty(STDOUT_FILENO) != 0;
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  if (!metrics.empty()) {
    if (once || json) {
      std::fprintf(stderr, "fastmm-top: --metrics serves until SIGINT; drop --once / --json\n");
      return 2;
    }
    fastmm::top::MetricsServer server;
    std::string error;
    if (!server.listen(metrics, &error)) {
      std::fprintf(stderr, "fastmm-top: --metrics %s: %s\n", metrics.c_str(), error.c_str());
      return 2;
    }
    std::printf(
        "fastmm-top: serving http://%s/metrics from %s\n", server.address().c_str(), path.c_str());
    std::fflush(stdout);
    server.serve(path, g_signal);
    return 0;
  }

  fastmm::StatusReader reader;
  fastmm::StatusSnapshot snap;
  std::string error;
  bool first = true;
  while (g_signal.load() == 0) {
    // A segment of another version is refused by open() (smaller layout) or fails read() (same
    // size or larger); either way segment_version() names it.
    if (!reader.is_open() && !reader.open(path, &error) && reader.segment_version() == 0) {
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
    const std::uint32_t version = reader.segment_version();
    if (reader.is_open() && reader.read(snap)) {
      frame = json ? fastmm::format_status_json(snap)
                   : fastmm::format_status(snap, fastmm::wall_now().ns, color);
    } else if (version != 0 && version != fastmm::kStatusVersion) {
      const std::string message = other_build_message(path, version);
      if (once) {
        std::fprintf(stderr, "%s\n", message.c_str());
        return 3;
      }
      frame.append(message).append("\n");
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

int main(int argc, char** argv) {
  return fastmm::cli::guarded_main("fastmm-top", [&] { return run(argc, argv); });
}
