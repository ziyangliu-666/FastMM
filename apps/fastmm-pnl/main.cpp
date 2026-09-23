// fastmm-pnl: what a deployment traded, read from the store rather than from a journal.
//
//   fastmm-pnl pnl --since yesterday          what each instrument made, by day
//   fastmm-pnl fills --session 1700000000     the fills of one session
//   fastmm-pnl sessions                       what has run
//
// The journal answers "what exactly happened, byte for byte" (fastmm-replay); this answers the
// daily questions. docs/how-to/operations/query-trading-records.md.
#include "fastmm/core/enums.hpp"
#include "fastmm/store/registry.hpp"
#include "fastmm/store/sqlite_store.hpp"
#include "fastmm/version.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

namespace {

using fastmm::store::QueryFilter;
using fastmm::store::Rows;

constexpr int kOk = 0;
constexpr int kUsage = 2;
constexpr int kNotFound = 3;

const char* kUsageText =
    "usage: fastmm-pnl <command> [options]\n"
    "\n"
    "commands:\n"
    "  sessions            one row per session: when it ran, what it made, how it ended\n"
    "  fills               one row per execution\n"
    "  orders              one row per order, in its last known state\n"
    "  pnl                 realised, fees and net by UTC day and instrument\n"
    "  positions           the last position snapshot of each session and instrument\n"
    "  recover             what the newest session left behind\n"
    "\n"
    "options:\n"
    "  --store <path>      store file (default runs/<engine>.db)\n"
    "  --backend <name>    storage backend (default sqlite)\n"
    "  --engine <name>     [engine] name to filter on\n"
    "  --session <id>      one session id\n"
    "  --instrument <sym>  one symbol\n"
    "  --since <day>       inclusive UTC day, YYYY-MM-DD, or today|yesterday\n"
    "  --until <day>       inclusive UTC day, YYYY-MM-DD, or today|yesterday\n"
    "  --day <day>         shorthand for --since <day> --until <day>\n"
    "  --limit <n>         at most n rows\n"
    "  --csv               comma-separated output instead of an aligned table\n"
    "  --version           print the version and exit\n"
    "  -h, --help          this text\n";

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
  char buf[16];
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

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fputs(kUsageText, stderr);
    return kUsage;
  }
  std::string command;
  std::string backend = "sqlite";
  std::string store_path;
  std::string engine_dir = "runs";
  bool csv = false;
  QueryFilter f;

  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    const auto value = [&](const char* what) -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "fastmm-pnl: %s needs a value\n", what);
        std::exit(kUsage);
      }
      return argv[++i];
    };
    if (a == "-h" || a == "--help") {
      std::fputs(kUsageText, stdout);
      return kOk;
    }
    if (a == "--version") {
      fmt::print("fastmm-pnl {}\n", FASTMM_VERSION_STRING);
      return kOk;
    }
    if (a == "--store") {
      store_path = value("--store");
    } else if (a == "--backend") {
      backend = value("--backend");
    } else if (a == "--engine") {
      f.engine = value("--engine");
    } else if (a == "--session") {
      f.session_id = std::strtoull(value("--session").c_str(), nullptr, 10);
    } else if (a == "--instrument") {
      f.instrument = value("--instrument");
    } else if (a == "--since") {
      f.from = resolve_day(value("--since"));
    } else if (a == "--until") {
      f.to = resolve_day(value("--until"));
    } else if (a == "--day") {
      f.from = resolve_day(value("--day"));
      f.to = f.from;
    } else if (a == "--limit") {
      f.limit = std::strtoul(value("--limit").c_str(), nullptr, 10);
    } else if (a == "--csv") {
      csv = true;
    } else if (!a.empty() && a[0] == '-') {
      return bad_usage("unknown option " + std::string(a));
    } else if (command.empty()) {
      command = a;
    } else {
      return bad_usage("unexpected argument " + std::string(a));
    }
  }
  if (command.empty()) return bad_usage("a command is required (see --help)");

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
