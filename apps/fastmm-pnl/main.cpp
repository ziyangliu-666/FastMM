// fastmm-pnl: what a deployment traded, read from the store rather than from a journal.
//
//   fastmm-pnl pnl --since yesterday          what each instrument made, by day
//   fastmm-pnl fills --session 1700000000     the fills of one session
//   fastmm-pnl sessions                       what has run
//
// The journal answers "what exactly happened, byte for byte" (fastmm-replay); this answers the
// daily questions. docs/how-to/operations/query-trading-records.md.
#include "command_line.hpp"

#include "fastmm/core/enums.hpp"
#include "fastmm/store/registry.hpp"
#include "fastmm/store/sqlite_store.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using fastmm::store::QueryFilter;
using fastmm::store::Rows;

constexpr int kOk = 0;
constexpr int kUsage = 2;
constexpr int kNotFound = 3;

// "today" and "yesterday" resolve against the host's UTC clock.
std::string resolve_day(std::string_view text) {
  int back = 0;
  if (text == "today") {
    back = 0;
  } else if (text == "yesterday") {
    back = 1;
  } else {
    return std::string(text);
  }
  const std::time_t now = std::time(nullptr) - static_cast<std::time_t>(back) * 86400;
  std::tm tm{};
  gmtime_r(&now, &tm);
  char buf[32];  // room for any int year, so gcc can prove it never truncates
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
  return buf;
}

void print_csv(const Rows& r) {
  std::string line;
  const auto quote = [&](const std::string& v) {
    if (v.find_first_of(",\"\n") == std::string::npos) return v;
    std::string out = "\"";
    for (const char c : v) {
      if (c == '"') out += '"';
      out += c;
    }
    return out + "\"";
  };
  for (std::size_t i = 0; i < r.columns.size(); ++i) {
    if (i != 0) line += ',';
    line += quote(r.columns[i]);
  }
  fmt::print("{}\n", line);
  for (const auto& row : r.rows) {
    line.clear();
    for (std::size_t i = 0; i < row.size(); ++i) {
      if (i != 0) line += ',';
      line += quote(row[i]);
    }
    fmt::print("{}\n", line);
  }
}

void print_table(const Rows& r) {
  if (r.columns.empty()) return;
  std::vector<std::size_t> width(r.columns.size());
  for (std::size_t i = 0; i < r.columns.size(); ++i) width[i] = r.columns[i].size();
  for (const auto& row : r.rows) {
    for (std::size_t i = 0; i < row.size() && i < width.size(); ++i)
      width[i] = std::max(width[i], row[i].size());
  }
  std::string line;
  for (std::size_t i = 0; i < r.columns.size(); ++i) {
    if (i != 0) line += "  ";
    line += fmt::format("{:<{}}", r.columns[i], width[i]);
  }
  fmt::print("{}\n", line);
  for (const auto& row : r.rows) {
    line.clear();
    for (std::size_t i = 0; i < row.size() && i < width.size(); ++i) {
      if (i != 0) line += "  ";
      line += fmt::format("{:<{}}", row[i], width[i]);
    }
    fmt::print("{}\n", line);
  }
  fmt::print("{} row(s)\n", r.rows.size());
}

int bad_usage(const std::string& msg) {
  std::fprintf(stderr, "fastmm-pnl: %s\n", msg.c_str());
  return kUsage;
}

}  // namespace

static int run(int argc, char** argv) {
  std::string backend = "sqlite";
  std::string store_path;
  std::string engine_dir = "runs";
  std::string since;
  std::string until;
  std::string day;
  bool csv = false;
  QueryFilter f;

  CLI::App app("What a deployment traded, read from the store.", "fastmm-pnl");
  fastmm::cli::setup(app);
  // The options may come before or after the command; `fallthrough` sends them here.
  const std::pair<const char*, const char*> commands[] = {
      {"sessions", "one row per session: when it ran, what it made, how it ended"},
      {"fills", "one row per execution"},
      {"orders", "one row per order, in its last known state"},
      {"pnl", "realised, fees and net by UTC day and instrument"},
      {"positions", "the last position snapshot of each session and instrument"},
      {"recover", "what the newest session left behind"},
  };
  for (const auto& [name, description] : commands)
    app.add_subcommand(name, description)->fallthrough();
  app.require_subcommand(0, 1);
  app.add_option("--store", store_path, "store file (default runs/<engine>.db)")
      ->option_text("<path>");
  app.add_option("--backend", backend, "storage backend (default sqlite)")->option_text("<name>");
  app.add_option("--engine", f.engine, "[engine] name to filter on")->option_text("<name>");
  app.add_option("--session", f.session_id, "one session id")->option_text("<id>");
  app.add_option("--instrument", f.instrument, "one symbol")->option_text("<sym>");
  app.add_option("--since", since, "inclusive UTC day, YYYY-MM-DD, or today|yesterday")
      ->option_text("<day>");
  app.add_option("--until", until, "inclusive UTC day, YYYY-MM-DD, or today|yesterday")
      ->option_text("<day>");
  app.add_option("--day", day, "shorthand for --since <day> --until <day>")->option_text("<day>");
  app.add_option("--limit", f.limit, "at most n rows")->option_text("<n>");
  app.add_flag("--csv", csv, "comma-separated output instead of an aligned table");
  if (const auto rc = fastmm::cli::parse(app, argc, argv)) return *rc;
  if (app.get_subcommands().empty()) return fastmm::cli::usage_error(app, "a command is required");
  const std::string command = app.get_subcommands().front()->get_name();
  f.from = resolve_day(!since.empty() ? since : day);
  f.to = resolve_day(!until.empty() ? until : day);

  fastmm::store::register_builtin_backends();
  auto& registry = fastmm::store::StoreRegistry::instance();
  auto reader = registry.make_reader(backend);
  if (reader == nullptr)
    return bad_usage("no queryable backend '" + backend + "' (have " + registry.names() + ")");

  fastmm::store::BackendOptions opts;
  opts.read_only = true;
  opts.default_dir = engine_dir;
  opts.engine_name = f.engine.empty() ? "fastmm" : f.engine;
  fastmm::GenericSection section;
  if (!store_path.empty()) section.values["path"] = store_path;
  opts.config = &section;
  if (auto r = reader->open(opts); !r) return bad_usage(r.error());

  if (command == "recover") {
    auto r = reader->recovery(f);
    if (!r) return bad_usage(r.error());
    if (!r->found) {
      fmt::print("no session recorded{}\n",
                 f.engine.empty() ? "" : fmt::format(" for engine '{}'", f.engine));
      return kNotFound;
    }
    const auto& s = *r;
    fmt::print("session {} ({})\n", s.session_id, s.strategy);
    fmt::print("  started   {}\n", s.started_utc);
    fmt::print("  stopped   {}\n",
               s.stopped_utc.empty() ? "never recorded: the process did not shut down cleanly"
                                     : s.stopped_utc);
    fmt::print("  exit      {} (kill {}{})\n",
               s.exit_code,
               s.kill_reason.empty() ? "None" : s.kill_reason,
               s.kill_latched ? ", latched" : "");
    fmt::print("  pnl       realized {} unrealized {} fees {} net {}\n",
               s.realized,
               s.unrealized,
               s.fees,
               s.net);
    fmt::print(
        "  fills     {}{}\n",
        s.fills,
        s.records_dropped == 0 ? "" : fmt::format(" ({} record(s) dropped)", s.records_dropped));
    if (!s.journal_complete) fmt::print("  journal   incomplete: the writer did not close it\n");
    for (const std::string& j : s.journals) fmt::print("  journal   {}\n", j);
    for (const std::string& p : s.positions) fmt::print("  position  {}\n", p);
    if (s.open_orders.empty()) {
      fmt::print("  orders    none open at the last record\n");
    } else {
      for (const std::string& o : s.open_orders) fmt::print("  open      {}\n", o);
    }
    return kOk;
  }

  fastmm::Result<Rows, std::string> rows = fastmm::fail(std::string("no command"));
  if (command == "sessions") {
    rows = reader->sessions(f);
  } else if (command == "fills") {
    rows = reader->fills(f);
  } else if (command == "orders") {
    rows = reader->orders(f);
  } else if (command == "pnl") {
    rows = reader->pnl(f);
  } else if (command == "positions") {
    rows = reader->positions(f);
  } else {
    return bad_usage("unknown command '" + command + "' (see --help)");
  }
  if (!rows) return bad_usage(rows.error());
  if (csv) {
    print_csv(*rows);
  } else {
    print_table(*rows);
  }
  return kOk;
}

int main(int argc, char** argv) {
  return fastmm::cli::guarded_main("fastmm-pnl", [&] { return run(argc, argv); });
}
