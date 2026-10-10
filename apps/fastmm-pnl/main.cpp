// fastmm-pnl: what a deployment traded, read from the store rather than from a journal.
//
//   fastmm-pnl pnl --since yesterday          what each instrument made, by day
//   fastmm-pnl fills --session 1700000000     the fills of one session
//   fastmm-pnl sessions                       what has run
//   fastmm-pnl audit --exchange trades.json   the stored fills against the venue's export
//   fastmm-pnl params --at "2026-10-09 14:00"  the strategy parameters in effect then
//
// The journal answers "what exactly happened, byte for byte" (fastmm-replay); this answers the
// daily questions. docs/how-to/operations/query-trading-records.md.
#include "command_line.hpp"

#include "fastmm/core/enums.hpp"
#include "fastmm/core/fill_audit.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/store/fill_audit_file.hpp"
#include "fastmm/store/registry.hpp"
#include "fastmm/store/sqlite_store.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
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
constexpr int kDuplicates = 4;  // `duplicates` found some
constexpr int kDiffers = 5;     // `audit` found executions that are not as they should be
// The store's fills are read this far past the audit's window: a fill the two sides stamp a little
// differently is compared rather than reported missing on one side and phantom on the other.
constexpr std::int64_t kAuditMarginMs = 60'000;

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

// "YYYY-MM-DD" (UTC) as Unix ms at its start; false when it is not a day.
bool day_start_ms(const std::string& day, std::int64_t& out) {
  std::tm tm{};
  if (day.size() != 10 ||
      std::sscanf(day.c_str(), "%4d-%2d-%2d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) != 3)
    return false;
  tm.tm_year -= 1900;
  tm.tm_mon -= 1;
  out = timegm(&tm) * 1000;
  return true;
}

// "YYYY-MM-DD", "YYYY-MM-DD HH:MM" or "YYYY-MM-DD HH:MM:SS" (or with a T, and a trailing Z), UTC,
// as ns since the epoch; false when it does not parse.
bool parse_utc(std::string text, std::int64_t& out) {
  if (!text.empty() && (text.back() == 'Z' || text.back() == 'z')) text.pop_back();
  for (char& c : text) {
    if (c == 'T') c = ' ';
  }
  std::tm tm{};
  int hh = 0;
  int mm = 0;
  int ss = 0;
  char tail = 0;
  const int n = std::sscanf(text.c_str(),
                            "%4d-%2d-%2d %2d:%2d:%2d%c",
                            &tm.tm_year,
                            &tm.tm_mon,
                            &tm.tm_mday,
                            &hh,
                            &mm,
                            &ss,
                            &tail);
  if (n != 3 && n != 5 && n != 6) return false;
  if (tm.tm_mon < 1 || tm.tm_mon > 12 || tm.tm_mday < 1 || tm.tm_mday > 31 || hh < 0 || hh > 23 ||
      mm < 0 || mm > 59 || ss < 0 || ss > 60)
    return false;
  tm.tm_year -= 1900;
  tm.tm_mon -= 1;
  tm.tm_hour = hh;
  tm.tm_min = mm;
  tm.tm_sec = ss;
  const std::int64_t secs = timegm(&tm);
  out = secs * 1'000'000'000LL;
  return true;
}

std::string dec(std::int64_t raw) {
  char buf[fastmm::kMaxDecimalChars];
  return {buf, fastmm::Qty::from_raw(raw).to_decimal(buf)};
}

// One row per execution that is not as it should be.
Rows audit_rows(const fastmm::FillAuditReport& r) {
  using fastmm::AuditFill;
  Rows out;
  out.columns = {"kind",
                 "time_ms",
                 "symbol",
                 "exec_id",
                 "side",
                 "venue_qty",
                 "venue_price",
                 "venue_fee",
                 "booked_qty",
                 "booked_price",
                 "booked_fee",
                 "order_id",
                 "fields",
                 "session_id"};
  const auto fee = [](const AuditFill* x) {
    if (x == nullptr || !x->has_fee) return std::string();
    return x->fee_asset.empty() ? dec(x->fee_raw) : dec(x->fee_raw) + " " + x->fee_asset;
  };
  const auto add = [&](std::string_view kind,
                       const AuditFill* v,
                       const AuditFill* b,
                       const std::string& fields) {
    const AuditFill& f = v != nullptr ? *v : *b;
    std::string order = f.order_id;
    if (order.empty() && b != nullptr) order = b->order_id;
    out.rows.push_back({std::string(kind),
                        std::to_string(f.time_ms),
                        f.symbol,
                        f.exec_id,
                        std::string(to_string(f.side)),
                        v != nullptr ? dec(v->qty_raw) : std::string(),
                        v != nullptr ? dec(v->price_raw) : std::string(),
                        fee(v),
                        b != nullptr ? dec(b->qty_raw) : std::string(),
                        b != nullptr ? dec(b->price_raw) : std::string(),
                        fee(b),
                        order,
                        fields,
                        b != nullptr ? std::to_string(b->session_id) : std::string()});
  };
  for (const AuditFill& f : r.missing) add("missing", &f, nullptr, "");
  for (const AuditFill& f : r.phantom) add("phantom", nullptr, &f, "");
  for (const fastmm::AuditMismatch& m : r.mismatched)
    add("differs", &m.venue, &m.booked, fastmm::audit_fields_text(m.fields));
  for (const AuditFill& f : r.duplicates) add("twice", nullptr, &f, "");
  return out;
}

// fastmm-pnl audit: the store's fills of `venue` against the executions in `file`, over
// [from_ms, to_ms] (0: what the file spans). Prints what differs and returns the exit code.
int audit_command(fastmm::store::Reader& reader,
                  const QueryFilter& f,
                  const std::string& file,
                  const std::string& venue,
                  std::int64_t from_ms,
                  std::int64_t to_ms,
                  bool csv);

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
  std::string exchange_file;
  std::string venue;
  std::int64_t from_ms = 0;
  std::int64_t to_ms = 0;
  std::string at;
  std::uint64_t against = 0;
  bool summary = false;
  QueryFilter f;

  CLI::App app("What a deployment traded, read from the store.", "fastmm-pnl");
  fastmm::cli::setup(app);
  // The options may come before or after the command; `fallthrough` sends them here.
  const std::pair<const char*, const char*> commands[] = {
      {"sessions", "one row per session: when it ran, what it made, how it ended"},
      {"fills", "one row per execution"},
      {"orders", "one row per order, in its last known state"},
      {"pnl", "realised (and the funding in it), fees and net by UTC day and instrument"},
      {"funding", "one row per perpetual funding payment"},
      {"positions", "the last position snapshot of each session and instrument"},
      {"duplicates", "executions and funding payments stored more than once (booked twice)"},
      {"recover", "what the newest session left behind"},
      {"audit", "the stored fills against a file of the venue's executions (--exchange)"},
      {"params", "the strategy parameters in effect --at a time (default: now), and their source"},
      {"param-changes", "every parameter update: the starting set, then each one applied"},
      {"param-diff",
       "what a session started with against what --against (default: the one "
       "before) ended with"},
      {"config", "the effective configuration a session ran with (TOML, secrets omitted)"},
      {"rejects",
       "refused orders: who refused them (source), and the account's budget then; "
       "--summary sums them"},
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
  app.add_option("--exchange",
                 exchange_file,
                 "audit: the venue's executions, a JSON array (Binance myTrades / userTrades) or "
                 "CSV")
      ->option_text("<file>");
  app.add_option("--venue", venue, "audit: the [venues.<name>] the file is from")
      ->option_text("<name>");
  app.add_option("--from-ms", from_ms, "audit: window start, Unix ms (default: the file's first)")
      ->option_text("<ms>");
  app.add_option("--to-ms", to_ms, "audit: window end, Unix ms (default: the file's last)")
      ->option_text("<ms>");
  app.add_option("--at", at, "params, config: a UTC time, YYYY-MM-DD[ HH:MM[:SS]] (default: now)")
      ->option_text("<time>");
  app.add_option("--against", against, "param-diff: the earlier session to compare with")
      ->option_text("<id>");
  app.add_flag("--summary", summary, "rejects: one row per account, instrument, side and source");
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

  // Neither --store nor --engine: runs/fastmm.db, the store of an [engine] without a name. When it
  // is not there, name the stores that are.
  if (store_path.empty() && f.engine.empty() && backend == "sqlite") {
    const std::filesystem::path fallback = std::filesystem::path(engine_dir) / "fastmm.db";
    std::error_code ec;
    if (!std::filesystem::exists(fallback, ec)) {
      std::vector<std::string> found;
      for (const auto& e : std::filesystem::directory_iterator(engine_dir, ec)) {
        if (e.path().extension() == ".db") found.push_back(e.path().stem().string());
      }
      std::sort(found.begin(), found.end());
      std::string msg =
          "no store at " + fallback.string() +
          "; name one with --engine <[engine] name> (runs/<name>.db) or --store <path>";
      for (std::size_t i = 0; i < found.size(); ++i)
        msg += (i == 0 ? "; " + engine_dir + "/ has " : ", ") + found[i] + ".db";
      return bad_usage(msg);
    }
  }

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
    fmt::print("  pnl       realized {} (funding {}) unrealized {} fees {} net {}\n",
               s.realized,
               s.funding,
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
    for (const auto& d : s.duplicates)
      fmt::print("  twice     {} {} {} {} {} {} stored {} times (sessions {})\n",
                 d.funding ? "funding" : "execution",
                 d.id,
                 d.venue,
                 d.symbol,
                 d.side,
                 d.qty,
                 d.copies,
                 d.sessions);
    if (s.duplicate_count != 0)
      fmt::print("  twice     {} stored more than once: positions and PnL count them twice\n",
                 s.duplicate_count);
    if (s.open_orders.empty()) {
      fmt::print("  orders    none open at the last record\n");
    } else {
      for (const std::string& o : s.open_orders) fmt::print("  open      {}\n", o);
    }
    return kOk;
  }

  if (command == "audit")
    return audit_command(*reader, f, exchange_file, venue, from_ms, to_ms, csv);

  fastmm::store::ParamQuery pq;
  pq.engine = f.engine;
  pq.session_id = f.session_id;
  pq.instrument = f.instrument;
  if (!at.empty() && !parse_utc(at, pq.at_ns))
    return bad_usage("--at: not a UTC time (YYYY-MM-DD[ HH:MM[:SS]]): " + at);
  if (command == "config") {
    auto toml = reader->session_config(pq);
    if (!toml) return bad_usage(toml.error());
    fmt::print("{}", *toml);
    if (!toml->empty() && toml->back() != '\n') fmt::print("\n");
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
  } else if (command == "funding") {
    rows = reader->funding(f);
  } else if (command == "positions") {
    rows = reader->positions(f);
  } else if (command == "duplicates") {
    rows = reader->duplicates(f);
  } else if (command == "params") {
    rows = reader->params(pq);
  } else if (command == "rejects") {
    rows = summary ? reader->reject_summary(f) : reader->rejects(f);
  } else if (command == "param-changes") {
    rows = reader->param_changes(f);
  } else if (command == "param-diff") {
    if (f.session_id == 0) return bad_usage("param-diff needs --session <id>");
    rows = reader->param_diff(against, f.session_id);
  } else {
    return bad_usage("unknown command '" + command + "' (see --help)");
  }
  if (!rows) return bad_usage(rows.error());
  if (csv) {
    print_csv(*rows);
  } else {
    print_table(*rows);
  }
  // Every other answer is computed from rows that may hold an execution twice: say so. The rows
  // are left as they are.
  if (command == "duplicates") return rows->empty() ? kOk : kDuplicates;
  if (auto dup = reader->duplicates(f); dup && !dup->empty())
    std::fprintf(stderr,
                 "fastmm-pnl: warning: %zu execution(s) are stored more than once (a restart "
                 "booked them twice); positions, fees and PnL count them twice. List them with "
                 "`fastmm-pnl duplicates`\n",
                 dup->rows.size());
  return kOk;
}

namespace {

int audit_command(fastmm::store::Reader& reader,
                  const QueryFilter& f,
                  const std::string& file,
                  const std::string& venue,
                  std::int64_t from_ms,
                  std::int64_t to_ms,
                  bool csv) {
  if (file.empty()) return bad_usage("audit needs --exchange <file>");
  auto loaded = fastmm::store::load_venue_fills(file);
  if (!loaded) return bad_usage(loaded.error());
  const std::string want = f.instrument.empty() ? "" : fastmm::normalize_symbol(f.instrument);
  const auto wanted = [&](const fastmm::AuditFill& a) {
    return want.empty() || fastmm::normalize_symbol(a.symbol) == want;
  };
  std::vector<fastmm::AuditFill> exchange;
  for (fastmm::AuditFill& a : *loaded) {
    if (!venue.empty() && !a.venue.empty() && a.venue != venue) continue;
    if (wanted(a)) exchange.push_back(std::move(a));
  }
  // The window: --from-ms / --to-ms, else --since / --until (whole UTC days), else the file's span.
  std::int64_t day_ms = 0;
  if (from_ms == 0 && !f.from.empty()) {
    if (!day_start_ms(f.from, day_ms)) return bad_usage("--since: not a day: " + f.from);
    from_ms = day_ms;
  }
  if (to_ms == 0 && !f.to.empty()) {
    if (!day_start_ms(f.to, day_ms)) return bad_usage("--until: not a day: " + f.to);
    to_ms = day_ms + 86'400'000 - 1;
  }
  if (exchange.empty() && (from_ms == 0 || to_ms == 0))
    return bad_usage("audit: " + file +
                     " holds no execution; name the window (--from-ms, --to-ms)");
  if (from_ms == 0 || to_ms == 0) {
    std::int64_t first = exchange.front().time_ms;
    std::int64_t last = first;
    for (const fastmm::AuditFill& a : exchange) {
      first = std::min(first, a.time_ms);
      last = std::max(last, a.time_ms);
    }
    if (from_ms == 0) from_ms = first;
    if (to_ms == 0) to_ms = last;
  }
  fastmm::store::BookedFillQuery q;
  q.engine = f.engine;
  q.venue = venue;
  q.from_ms = from_ms - kAuditMarginMs;
  q.to_ms = to_ms + kAuditMarginMs;
  auto booked = reader.booked_fills(q);
  if (!booked) return bad_usage(booked.error());
  std::vector<fastmm::AuditFill> stored;
  for (fastmm::AuditFill& b : *booked) {
    if (wanted(b)) stored.push_back(std::move(b));
  }
  const fastmm::FillAuditReport r = fastmm::audit_fills(exchange, stored, from_ms, to_ms);
  const Rows out = audit_rows(r);
  if (csv) {
    print_csv(out);
  } else {
    print_table(out);
  }
  std::fprintf(stderr,
               "fastmm-pnl: audit %lld to %lld ms: venue %zu, store %zu; %zu agree, %zu missing, "
               "%zu phantom, %zu differ, %zu stored twice\n",
               static_cast<long long>(from_ms),
               static_cast<long long>(to_ms),
               r.venue_rows,
               r.booked_rows,
               r.matched,
               r.missing.size(),
               r.phantom.size(),
               r.mismatched.size(),
               r.duplicates.size());
  return r.clean() ? kOk : kDiffers;
}

}  // namespace

int main(int argc, char** argv) {
  return fastmm::cli::guarded_main("fastmm-pnl", [&] { return run(argc, argv); });
}
