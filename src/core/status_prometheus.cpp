#include "fastmm/core/status_prometheus.hpp"

#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/latency.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <string_view>

namespace fastmm {

namespace {

constexpr double kRawToQuote = 1e-8;
constexpr double kNsToS = 1e-9;

std::string_view name_of(const char* s, std::size_t capacity) {
  const std::string_view v(s, capacity);
  const std::size_t n = v.find('\0');
  return n == std::string_view::npos ? v : v.substr(0, n);
}

// Prometheus label values escape a backslash, a double quote and a newline; nothing else.
std::string label(std::string_view v) {
  std::string out;
  out.reserve(v.size());
  for (char c : v) {
    if (c == '\\' || c == '"') {
      out += '\\';
      out += c;
    } else if (c == '\n') {
      out += "\\n";
    } else {
      out += c;
    }
  }
  return out;
}

class Exposition {
 public:
  // One HELP/TYPE pair per metric family, immediately before its samples.
  void family(std::string_view name, std::string_view type, std::string_view help) {
    fmt::format_to(
        std::back_inserter(out_), "# HELP {} {}\n# TYPE {} {}\n", name, help, name, type);
  }
  void gauge(std::string_view name, std::string_view help, double value) {
    family(name, "gauge", help);
    value_of(name, "", value);
  }
  void counter(std::string_view name, std::string_view help, std::uint64_t value) {
    family(name, "counter", help);
    fmt::format_to(std::back_inserter(out_), "{} {}\n", name, value);
  }
  void value_of(std::string_view name, std::string_view labels, double value) {
    if (labels.empty())
      fmt::format_to(std::back_inserter(out_), "{} {:.10g}\n", name, value);
    else
      fmt::format_to(std::back_inserter(out_), "{}{{{}}} {:.10g}\n", name, labels, value);
  }
  void value_of(std::string_view name, std::string_view labels, std::uint64_t value) {
    if (labels.empty())
      fmt::format_to(std::back_inserter(out_), "{} {}\n", name, value);
    else
      fmt::format_to(std::back_inserter(out_), "{}{{{}}} {}\n", name, labels, value);
  }
  [[nodiscard]] std::string take() { return std::move(out_); }

 private:
  std::string out_;
};

// The three quantiles of one latency as samples of `name`, under `labels` plus quantile="".
void latency_quantiles(Exposition& e,
                       std::string_view name,
                       const std::string& labels,
                       const StatusLatency& l) {
  for (const auto& [q, ns] : {std::pair<const char*, std::uint64_t>{"0.5", l.p50_ns},
                              {"0.99", l.p99_ns},
                              {"0.999", l.p999_ns}})
    e.value_of(name,
               labels.empty() ? fmt::format("quantile=\"{}\"", q)
                              : fmt::format("{},quantile=\"{}\"", labels, q),
               static_cast<double>(ns) * kNsToS);
}

void engine_metrics(Exposition& e, const StatusSnapshot& s);
void underlying_metrics(Exposition& e,
                        std::string_view prefix,
                        const StatusUnderlying (&u)[kStatusMaxUnderlyings]);
void gateway_metrics(Exposition& e, const StatusSnapshot& s);
void venue_metrics(Exposition& e, const StatusSnapshot& s);
void balance_metrics(Exposition& e, const StatusSnapshot& s);
void perp_metrics(Exposition& e, const StatusSnapshot& s);
void quote_metrics(Exposition& e, const StatusSnapshot& s);

}  // namespace

std::string format_status_prometheus(const StatusSnapshot& s, std::int64_t now_ns) {
  Exposition e;
  const std::string engine(label(name_of(s.engine_name, sizeof s.engine_name)));
  const std::string strategy(label(name_of(s.strategy, sizeof s.strategy)));

  e.gauge("fastmm_up", "1 while a status snapshot can be read", 1.0);
  e.family("fastmm_info", "gauge", "constant 1, labelled with what is running");
  e.value_of("fastmm_info",
             s.kind == StatusKind::Gateway
                 ? fmt::format("gateway=\"{}\",pid=\"{}\"", engine, s.pid)
                 : fmt::format("engine=\"{}\",strategy=\"{}\",pid=\"{}\",session_id=\"{}\"",
                               engine,
                               strategy,
                               s.pid,
                               s.session_id),
             1.0);
  e.gauge("fastmm_state",
          "session state: 0 starting, 1 running, 2 stopping, 3 stopped",
          static_cast<double>(static_cast<std::uint8_t>(s.state)));
  e.gauge("fastmm_dry_run", "1 when the session places no orders (--dry-run)", s.dry_run ? 1 : 0);
  e.gauge("fastmm_status_age_seconds",
          "age of the snapshot; the control thread publishes every 250 ms",
          static_cast<double>(now_ns - s.updated_ns) * kNsToS);
  e.gauge("fastmm_uptime_seconds",
          "wall-clock seconds since the session started",
          static_cast<double>(s.updated_ns - s.started_ns) * kNsToS);
  e.gauge("fastmm_kill_active", "1 while the global kill switch is engaged", s.kill_flags & 1u);
  e.gauge("fastmm_kill_latched",
          "1 when a max_loss trip is latched and the next start would refuse to trade",
          s.kill_latched ? 1 : 0);
  e.gauge("fastmm_kill_reason",
          "KillReason of the global kill switch, 0 while it is not set",
          static_cast<double>(s.kill_reason));
  if (s.kind == StatusKind::Gateway) {
    gateway_metrics(e, s);
  } else {
    engine_metrics(e, s);
  }
  venue_metrics(e, s);
  balance_metrics(e, s);
  perp_metrics(e, s);
  return e.take();
}

namespace {

// [risk.underlying] / [gateway.underlying]: <prefix>_underlying_net (absent while an inverse
// contract with a position has no mark) and <prefix>_underlying_max_net, base units. Nothing when
// no underlying is configured.
void underlying_metrics(Exposition& e,
                        std::string_view prefix,
                        const StatusUnderlying (&u)[kStatusMaxUnderlyings]) {
  if (u[0].name[0] == '\0') return;
  const std::string net = fmt::format("{}_underlying_net", prefix);
  const std::string max = fmt::format("{}_underlying_max_net", prefix);
  const auto labels = [](const StatusUnderlying& x) {
    return fmt::format("underlying=\"{}\"", label(name_of(x.name, sizeof x.name)));
  };
  e.family(net, "gauge", "net position in the underlying over every instrument, base units");
  for (const StatusUnderlying& x : u) {
    if (x.name[0] != '\0' && x.known != 0)
      e.value_of(net, labels(x), static_cast<double>(x.net_raw) * kRawToQuote);
  }
  e.family(max, "gauge", "max_net of the underlying applied now, 0 when off; base units");
  for (const StatusUnderlying& x : u) {
    if (x.name[0] != '\0')
      e.value_of(max, labels(x), static_cast<double>(x.max_net_raw) * kRawToQuote);
  }
}

// The quote table, per instrument and side: fastmm_quote_obstacle (the QuoteBlock code: 0 quoting,
// 1 not wanted, 2 quoting off, 3 pulled, 4 venue killed, 5 feed lag, 6 backoff, 7 starved,
// 8 refused, 9 pending) and its age, the asked and working quantities and the strategy's budget;
// fastmm_position per instrument; fastmm_strategy_metric{name} for each ctx.metric.
void quote_metrics(Exposition& e, const StatusSnapshot& s) {
  const std::uint32_t n = std::min<std::uint32_t>(s.quote_count, kStatusMaxQuotes);
  if (n != 0) {
    const auto labels = [&](std::uint32_t k, std::size_t sd) {
      return fmt::format("symbol=\"{}\",side=\"{}\"",
                         label(name_of(s.quotes[k].symbol, sizeof s.quotes[k].symbol)),
                         sd == 0 ? "buy" : "sell");
    };
    struct Field {
      const char* name;
      const char* help;
      double (*value)(const LiveQuoteSide&, std::int64_t);
    };
    static constexpr Field fields[] = {
        {"fastmm_quote_obstacle",
         "first obstacle to the side's quotes: 0 quoting, 1 not wanted, 2 quoting off, 3 pulled, "
         "4 venue killed, 5 feed lag, 6 backoff, 7 starved, 8 refused, 9 pending",
         [](const LiveQuoteSide& q, std::int64_t) { return static_cast<double>(q.block); }},
        {"fastmm_quote_obstacle_seconds",
         "how long the side's obstacle has held, seconds",
         [](const LiveQuoteSide& q, std::int64_t now) {
           return q.block_since_ns == 0 ? 0.0
                                        : static_cast<double>(now - q.block_since_ns) * kNsToS;
         }},
        {"fastmm_quote_asked_qty",
         "quantity the strategy asks for on the side over every level, base units",
         [](const LiveQuoteSide& q, std::int64_t) {
           return static_cast<double>(q.desired_qty_raw) * kRawToQuote;
         }},
        {"fastmm_quote_working_qty",
         "leaves of the side's open orders, base units",
         [](const LiveQuoteSide& q, std::int64_t) {
           return static_cast<double>(q.working_qty_raw) * kRawToQuote;
         }},
        {"fastmm_quote_budget",
         "budget the strategy gave the side (ctx.note_quote), quote currency; 0 when none",
         [](const LiveQuoteSide& q, std::int64_t) {
           return static_cast<double>(q.note.budget_raw) * kRawToQuote;
         }},
    };
    for (const Field& f : fields) {
      e.family(f.name, "gauge", f.help);
      for (std::uint32_t k = 0; k < n; ++k)
        for (std::size_t sd = 0; sd < 2; ++sd)
          e.value_of(f.name, labels(k, sd), f.value(s.quotes[k].q.sides[sd], s.updated_ns));
    }
    e.family("fastmm_position", "gauge", "position in the instrument, base units");
    for (std::uint32_t k = 0; k < n; ++k)
      e.value_of("fastmm_position",
                 fmt::format("symbol=\"{}\"",
                             label(name_of(s.quotes[k].symbol, sizeof s.quotes[k].symbol))),
                 static_cast<double>(s.quotes[k].q.position_raw) * kRawToQuote);
  }
  const std::uint32_t m = std::min<std::uint32_t>(s.metric_count, kMaxStrategyMetrics);
  if (m == 0) return;
  e.family("fastmm_strategy_metric", "gauge", "a number the strategy publishes (ctx.metric)");
  for (std::uint32_t k = 0; k < m; ++k)
    e.value_of(
        "fastmm_strategy_metric",
        fmt::format("name=\"{}\"", label(name_of(s.metrics[k].name, sizeof s.metrics[k].name))),
        s.metrics[k].value);
}

// fastmm_balance_{free,locked,total,equity,maintenance}{venue,asset,account}: the balance table's
// reported rows, in the asset's units; account="1" marks a venue's account-wide margin. Nothing
// before a venue reports.
void balance_metrics(Exposition& e, const StatusSnapshot& s) {
  const std::size_t n = std::min<std::size_t>(s.balance_count, kStatusMaxBalances);
  bool any = false;
  for (std::size_t i = 0; i < n; ++i) any = any || s.balances[i].known != 0;
  if (!any) return;
  const auto labels = [&](const StatusBalance& b) {
    const std::string_view venue = b.venue < kStatusMaxVenues
                                       ? name_of(s.venues[b.venue].name, sizeof s.venues[0].name)
                                       : std::string_view("?");
    return fmt::format("venue=\"{}\",asset=\"{}\",account=\"{}\"",
                       label(venue),
                       label(name_of(b.asset, sizeof b.asset)),
                       b.account != 0 ? 1 : 0);
  };
  struct Field {
    const char* name;
    const char* help;
    std::int64_t StatusBalance::*raw;
  };
  static constexpr Field kFields[] = {
      {"fastmm_balance_free",
       "spendable now (derivatives: available margin): the venue's report less this process's "
       "orders and fills since, asset units",
       &StatusBalance::free_raw},
      {"fastmm_balance_locked",
       "held by open orders (derivatives: initial margin in use), asset units",
       &StatusBalance::locked_raw},
      {"fastmm_balance_total",
       "free + locked (derivatives: wallet balance), asset units",
       &StatusBalance::total_raw},
      {"fastmm_balance_equity",
       "the venue's equity (wallet + unrealized PnL), asset units",
       &StatusBalance::equity_raw},
      {"fastmm_balance_maintenance",
       "the venue's maintenance margin, asset units",
       &StatusBalance::maintenance_raw},
  };
  for (const Field& f : kFields) {
    e.family(f.name, "gauge", f.help);
    for (std::size_t i = 0; i < n; ++i) {
      const StatusBalance& b = s.balances[i];
      if (b.known != 0) e.value_of(f.name, labels(b), static_cast<double>(b.*f.raw) * kRawToQuote);
    }
  }
}

// fastmm_perp_{mark,index,funding_rate,funding_interval_seconds,next_funding_seconds,
// open_interest,mark_age_seconds,funding_age_seconds,valued_at_mark}{venue,symbol}: the perp table,
// one series per derivative that has reported. Nothing before one does.
void perp_metrics(Exposition& e, const StatusSnapshot& s) {
  const std::size_t n = std::min<std::size_t>(s.perp_count, kStatusMaxPerps);
  if (n == 0) return;
  const auto labels = [&](const StatusPerp& p) {
    const std::string_view venue = p.venue < kStatusMaxVenues
                                       ? name_of(s.venues[p.venue].name, sizeof s.venues[0].name)
                                       : std::string_view("?");
    return fmt::format(
        "venue=\"{}\",symbol=\"{}\"", label(venue), label(name_of(p.symbol, sizeof p.symbol)));
  };
  struct Field {
    const char* name;
    const char* help;
    double (*value)(const StatusPerp&);
  };
  static constexpr Field kFields[] = {
      {"fastmm_perp_mark",
       "the venue's mark price",
       [](const StatusPerp& p) { return static_cast<double>(p.mark_raw) * kRawToQuote; }},
      {"fastmm_perp_index",
       "the venue's index price",
       [](const StatusPerp& p) { return static_cast<double>(p.index_raw) * kRawToQuote; }},
      {"fastmm_perp_funding_rate",
       "the funding rate the venue will apply next, per funding interval (positive: longs pay)",
       [](const StatusPerp& p) { return p.funding_rate; }},
      {"fastmm_perp_funding_interval_seconds",
       "the funding interval",
       [](const StatusPerp& p) { return static_cast<double>(p.funding_interval_ns) / 1e9; }},
      {"fastmm_perp_next_funding_seconds",
       "venue time of the next funding, Unix seconds; 0 when continuous",
       [](const StatusPerp& p) { return static_cast<double>(p.next_funding_ns) / 1e9; }},
      {"fastmm_perp_open_interest",
       "open interest, contracts",
       [](const StatusPerp& p) { return static_cast<double>(p.open_interest_raw) * kRawToQuote; }},
      {"fastmm_perp_mark_age_seconds",
       "time since the mark arrived; -1 never",
       [](const StatusPerp& p) {
         return p.mark_age_ns < 0 ? -1.0 : static_cast<double>(p.mark_age_ns) / 1e9;
       }},
      {"fastmm_perp_funding_age_seconds",
       "time since the funding rate arrived; -1 never",
       [](const StatusPerp& p) {
         return p.funding_age_ns < 0 ? -1.0 : static_cast<double>(p.funding_age_ns) / 1e9;
       }},
      {"fastmm_perp_valued_at_mark",
       "1 while the position is valued at the venue's mark ([accounting] mark = venue)",
       [](const StatusPerp& p) { return p.valued_at_mark != 0 ? 1.0 : 0.0; }},
  };
  for (const Field& f : kFields) {
    e.family(f.name, "gauge", f.help);
    for (std::size_t i = 0; i < n; ++i) e.value_of(f.name, labels(s.perps[i]), f.value(s.perps[i]));
  }
}

void engine_metrics(Exposition& e, const StatusSnapshot& s) {
  e.gauge("fastmm_realized_pnl",
          "realized PnL, quote currency",
          static_cast<double>(s.realized_pnl_raw) * kRawToQuote);
  e.gauge("fastmm_unrealized_pnl",
          "unrealized PnL, quote currency",
          static_cast<double>(s.unrealized_pnl_raw) * kRawToQuote);
  e.gauge(
      "fastmm_fees", "fees paid, quote currency", static_cast<double>(s.fees_raw) * kRawToQuote);
  e.gauge("fastmm_max_loss",
          "[risk] max_loss as the engine applies it now, 0 when off; quote currency",
          static_cast<double>(s.max_loss_raw) * kRawToQuote);
  e.gauge("fastmm_pnl_carry",
          "net PnL of earlier sessions that max_loss is measured against as well, quote currency",
          static_cast<double>(s.pnl_carry_raw) * kRawToQuote);
  underlying_metrics(e, "fastmm", s.underlyings);
  quote_metrics(e, s);

  e.counter("fastmm_events_total", "events the engine consumed", s.events);
  e.counter("fastmm_book_updates_total", "book updates applied", s.book_updates);
  e.counter("fastmm_orders_sent_total", "new orders sent", s.orders_sent);
  e.counter("fastmm_cancels_sent_total", "cancels sent", s.cancels_sent);
  e.counter("fastmm_replaces_sent_total", "replaces sent", s.replaces_sent);
  e.counter("fastmm_fills_total", "executions received", s.fills);
  e.counter("fastmm_risk_rejects_total", "orders refused by the pre-trade checks", s.risk_rejects);
  e.counter("fastmm_venue_rejects_total", "orders refused by a venue", s.venue_rejects);
  e.counter(
      "fastmm_quotes_withheld_balance_total",
      "quote orders held back after their old order ended: the balance no longer covered them",
      s.balance_withheld);
  if (s.risk_tokens >= 0)
    e.gauge("fastmm_risk_tokens",
            "[risk] orders_per_sec bucket: whole tokens left, shared by every account",
            static_cast<double>(s.risk_tokens));
  e.counter("fastmm_param_updates_total",
            "strategy parameter updates the engine has applied",
            s.param_updates);
  e.gauge("fastmm_param_control_pending",
          "parameter updates the control socket sent that the engine has not applied yet",
          s.param_control_published > s.param_control_applied
              ? static_cast<double>(s.param_control_published - s.param_control_applied)
              : 0.0);
  e.gauge("fastmm_flatten_state",
          "FlattenState of the operator flatten: 0 off, 1 working, 2 flat, 3 timed out, 4 stopped",
          static_cast<double>(s.flatten_state));
  e.gauge("fastmm_flatten_instruments_left",
          "instruments in the flatten's scope that still hold a position",
          static_cast<double>(s.flatten_instruments_left));
  e.counter("fastmm_flatten_orders_total", "reduce-only orders a flatten sent", s.flatten_orders);
  if (s.treasury.pools != 0) {
    const StatusTreasury& t = s.treasury;
    e.gauge("fastmm_treasury_in_flight",
            "pool treasury transfers sent and not yet resolved",
            static_cast<double>(t.in_flight));
    e.counter("fastmm_treasury_sent_total", "pool treasury transfers sent", t.sent);
    e.counter("fastmm_treasury_done_total", "pool treasury transfers done", t.done);
    e.counter("fastmm_treasury_failed_total", "pool treasury transfers failed", t.failed);
    e.counter("fastmm_treasury_timed_out_total",
              "pool treasury transfers unresolved past timeout_s",
              t.timed_out);
    e.counter("fastmm_treasury_limited_total",
              "pool treasury plans held back by min_interval_s, max_per_hour or cooldown_s",
              t.limited);
    e.counter("fastmm_treasury_dry_run_plans_total",
              "pool treasury transfers planned and not sent (dry_run)",
              t.dry_run_plans);
    e.counter("fastmm_treasury_errors_total",
              "pool treasury requests without an answer and ledger writes that failed",
              t.errors);
  }
  e.counter("fastmm_kills_total", "global kill switch trips", s.kills);
  e.counter("fastmm_venue_kills_total", "per-venue kill switch trips", s.venue_kills);

  e.family("fastmm_rejects_by_reason_total",
           "counter",
           "rejects by reason; only the most frequent reasons fit in the snapshot");
  for (const auto& [kind, entries] :
       {std::pair<const char*, const StatusRejectCount*>{"risk", s.risk_reject_reasons},
        {"venue", s.venue_reject_reasons}}) {
    for (std::size_t i = 0; i < kStatusMaxRejectReasons; ++i) {
      if (entries[i].count == 0) continue;
      e.value_of("fastmm_rejects_by_reason_total",
                 fmt::format("kind=\"{}\",reason=\"{}\"",
                             kind,
                             to_string(static_cast<RejectReason>(entries[i].reason))),
                 entries[i].count);
    }
  }

  e.family("fastmm_latency_quantile_seconds",
           "gauge",
           "engine latency intervals, from the T0..T5 stamps of the last publishing window");
  for (std::size_t i = 0; i < static_cast<std::size_t>(LatencyInterval::Count); ++i)
    latency_quantiles(e,
                      "fastmm_latency_quantile_seconds",
                      fmt::format("interval=\"{}\"", to_string(static_cast<LatencyInterval>(i))),
                      s.latency[i]);
  e.family("fastmm_latency_samples_total", "counter", "latency samples in the window");
  for (std::size_t i = 0; i < static_cast<std::size_t>(LatencyInterval::Count); ++i)
    e.value_of("fastmm_latency_samples_total",
               fmt::format("interval=\"{}\"", to_string(static_cast<LatencyInterval>(i))),
               s.latency[i].count);
}

void venue_metrics(Exposition& e, const StatusSnapshot& s) {
  const std::size_t venues = std::min<std::size_t>(s.venue_count, kStatusMaxVenues);
  if (venues == 0) return;

  auto per_venue =
      [&](std::string_view name, std::string_view type, std::string_view help, auto&& value) {
        e.family(name, type, help);
        for (std::size_t i = 0; i < venues; ++i)
          e.value_of(name,
                     fmt::format("venue=\"{}\"",
                                 label(name_of(s.venues[i].name, sizeof s.venues[i].name))),
                     value(s.venues[i]));
      };

  e.family("fastmm_venue_channel_state",
           "gauge",
           "venue channel state: 0 down, 1 connecting, 2 live, 3 stale");
  for (std::size_t i = 0; i < venues; ++i) {
    const StatusVenue& v = s.venues[i];
    const std::string venue = label(name_of(v.name, sizeof v.name));
    for (const auto& [channel, state] :
         {std::pair<const char*, std::uint8_t>{"md", v.md}, {"user", v.user}, {"order", v.order}})
      e.value_of("fastmm_venue_channel_state",
                 fmt::format("venue=\"{}\",channel=\"{}\"", venue, channel),
                 static_cast<double>(state));
  }
  per_venue("fastmm_venue_killed",
            "gauge",
            "1 while this venue's kill switch is engaged",
            [](const StatusVenue& v) { return static_cast<double>(v.killed ? 1 : 0); });
  per_venue("fastmm_venue_books_synced", "gauge", "books in sync", [](const StatusVenue& v) {
    return static_cast<double>(v.books_synced);
  });
  per_venue("fastmm_venue_books_total", "gauge", "books subscribed", [](const StatusVenue& v) {
    return static_cast<double>(v.books_total);
  });
  per_venue("fastmm_venue_clock_offset_seconds",
            "gauge",
            "venue clock minus the local clock",
            [](const StatusVenue& v) { return static_cast<double>(v.clock_offset_ms) * 1e-3; });
  per_venue("fastmm_venue_md_messages_total",
            "counter",
            "market-data messages",
            [](const StatusVenue& v) { return v.md_messages; });
  per_venue(
      "fastmm_venue_resyncs_total", "counter", "book resynchronisations", [](const StatusVenue& v) {
        return v.resyncs;
      });
  per_venue("fastmm_venue_orders_sent_total",
            "counter",
            "new orders sent to this venue",
            [](const StatusVenue& v) { return v.orders_sent; });
  per_venue("fastmm_venue_cancels_sent_total",
            "counter",
            "cancels sent to this venue",
            [](const StatusVenue& v) { return v.cancels_sent; });
  per_venue("fastmm_venue_replaces_sent_total",
            "counter",
            "replaces sent to this venue",
            [](const StatusVenue& v) { return v.replaces_sent; });
  per_venue("fastmm_venue_order_events_total",
            "counter",
            "order events received",
            [](const StatusVenue& v) { return v.order_events; });
  per_venue("fastmm_venue_reconnects_total",
            "counter",
            "connection re-establishments",
            [](const StatusVenue& v) { return v.reconnects; });
  per_venue("fastmm_venue_rest_errors_total", "counter", "REST errors", [](const StatusVenue& v) {
    return v.rest_errors;
  });
  per_venue("fastmm_venue_rate_limit_cooldowns_total",
            "counter",
            "rate-limit cooldowns",
            [](const StatusVenue& v) { return v.rate_limit_cooldowns; });
  per_venue("fastmm_venue_orders_10s_used",
            "gauge",
            "orders counted in the venue's 10 s window (a request count, not open orders)",
            [](const StatusVenue& v) { return static_cast<std::uint64_t>(v.orders_10s.used); });
  per_venue("fastmm_venue_orders_10s_admits",
            "gauge",
            "orders the connector admits in the 10 s window (its cap below the venue's limit)",
            [](const StatusVenue& v) { return static_cast<std::uint64_t>(v.orders_10s.admits); });
  per_venue("fastmm_venue_orders_1d_used",
            "gauge",
            "orders counted in the venue's 1 d window",
            [](const StatusVenue& v) { return static_cast<std::uint64_t>(v.orders_1d.used); });
  per_venue("fastmm_venue_orders_1d_admits",
            "gauge",
            "orders the connector admits in the 1 d window",
            [](const StatusVenue& v) { return static_cast<std::uint64_t>(v.orders_1d.admits); });
  per_venue("fastmm_venue_budget_paused",
            "gauge",
            "1 while the venue asked for a pause (429, -1003) or REST is stopped (418)",
            [](const StatusVenue& v) { return static_cast<std::uint64_t>(v.budget_paused); });
  per_venue("fastmm_venue_refused_orders_10s_total",
            "counter",
            "orders the connector's limiter refused on the 10 s window, never sent",
            [](const StatusVenue& v) { return v.refused_orders_10s; });
  per_venue("fastmm_venue_refused_orders_1m_total",
            "counter",
            "orders the connector's limiter refused on the 1 m window, never sent",
            [](const StatusVenue& v) { return v.refused_orders_1m; });
  per_venue("fastmm_venue_refused_orders_1d_total",
            "counter",
            "orders the connector's limiter refused on the 1 d window, never sent",
            [](const StatusVenue& v) { return v.refused_orders_1d; });
  per_venue("fastmm_venue_refused_weight_total",
            "counter",
            "orders the connector's limiter refused on the request weight, never sent",
            [](const StatusVenue& v) { return v.refused_weight; });
  per_venue("fastmm_venue_refused_paused_total",
            "counter",
            "orders the connector refused while paused, never sent",
            [](const StatusVenue& v) { return v.refused_paused; });
  per_venue("fastmm_venue_fill_audits_total",
            "counter",
            "fill audits that compared the venue's executions with the store's",
            [](const StatusVenue& v) { return v.fill_audits; });
  per_venue("fastmm_venue_fill_audit_failures_total",
            "counter",
            "fill audits that could not read the venue or the store",
            [](const StatusVenue& v) { return v.fill_audit_failures; });
  per_venue("fastmm_venue_fill_audit_missing_total",
            "counter",
            "executions the venue reports and the engine never booked",
            [](const StatusVenue& v) { return v.fill_audit_missing; });
  per_venue("fastmm_venue_fill_audit_phantom_total",
            "counter",
            "executions the engine booked and the venue does not report",
            [](const StatusVenue& v) { return v.fill_audit_phantom; });
  per_venue("fastmm_venue_fill_audit_mismatched_total",
            "counter",
            "executions whose quantity, price, fee, side or order differ",
            [](const StatusVenue& v) { return v.fill_audit_mismatched; });
  per_venue("fastmm_venue_fill_audit_duplicates_total",
            "counter",
            "executions the engine stored more than once",
            [](const StatusVenue& v) { return v.fill_audit_duplicates; });

  e.family("fastmm_venue_tick_to_trade_quantile_seconds",
           "gauge",
           "socket read to order write, measured on the network thread");
  for (std::size_t i = 0; i < venues; ++i)
    latency_quantiles(
        e,
        "fastmm_venue_tick_to_trade_quantile_seconds",
        fmt::format("venue=\"{}\"", label(name_of(s.venues[i].name, sizeof s.venues[i].name))),
        s.venues[i].wire_tick_to_trade);
  e.family("fastmm_venue_tick_to_trade_recent_quantile_seconds",
           "gauge",
           "socket read to order write over the last minute (window=\"1m\") and hour (\"1h\")");
  for (std::size_t i = 0; i < venues; ++i) {
    const std::string venue =
        fmt::format("venue=\"{}\"", label(name_of(s.venues[i].name, sizeof s.venues[i].name)));
    latency_quantiles(e,
                      "fastmm_venue_tick_to_trade_recent_quantile_seconds",
                      venue + ",window=\"1m\"",
                      s.venues[i].wire_tick_to_trade_1m);
    latency_quantiles(e,
                      "fastmm_venue_tick_to_trade_recent_quantile_seconds",
                      venue + ",window=\"1h\"",
                      s.venues[i].wire_tick_to_trade_1h);
  }

  const bool multicast = std::any_of(
      s.venues, s.venues + venues, [](const StatusVenue& v) { return v.feed.state != 0; });
  if (!multicast) return;

  auto per_feed =
      [&](std::string_view name, std::string_view type, std::string_view help, auto&& value) {
        e.family(name, type, help);
        for (std::size_t i = 0; i < venues; ++i) {
          if (s.venues[i].feed.state == 0) continue;
          e.value_of(name,
                     fmt::format("venue=\"{}\"",
                                 label(name_of(s.venues[i].name, sizeof s.venues[i].name))),
                     value(s.venues[i].feed));
        }
      };
  per_feed("fastmm_feed_state",
           "gauge",
           "multicast feed state: 1 down, 2 snapshot, 3 live, 4 lost",
           [](const StatusFeed& f) { return static_cast<double>(f.state); });
  per_feed("fastmm_feed_packets_total",
           "counter",
           "MoldUDP64 packets accepted",
           [](const StatusFeed& f) { return f.packets; });
  per_feed("fastmm_feed_gaps_total", "counter", "sequence gaps declared", [](const StatusFeed& f) {
    return f.gaps;
  });
  per_feed("fastmm_feed_recovered_total",
           "counter",
           "messages delivered from re-requests",
           [](const StatusFeed& f) { return f.recovered; });
  per_feed(
      "fastmm_feed_unrecovered_total", "counter", "sequences given up on", [](const StatusFeed& f) {
        return f.unrecovered;
      });
  per_feed("fastmm_feed_malformed_total",
           "counter",
           "datagrams that were not MoldUDP64 packets",
           [](const StatusFeed& f) { return f.malformed; });
  per_feed("fastmm_feed_book_errors_total",
           "counter",
           "L3 book inconsistencies",
           [](const StatusFeed& f) { return f.book_errors; });
  e.family("fastmm_feed_line_duplicates_total",
           "counter",
           "copies that arrived after the first copy, per line");
  for (std::size_t i = 0; i < venues; ++i) {
    const StatusFeed& f = s.venues[i].feed;
    if (f.state == 0) continue;
    const std::string venue = label(name_of(s.venues[i].name, sizeof s.venues[i].name));
    for (std::size_t line = 0; line < 2; ++line)
      e.value_of("fastmm_feed_line_duplicates_total",
                 fmt::format("venue=\"{}\",line=\"{}\"", venue, line == 0 ? "a" : "b"),
                 f.line_duplicates[line]);
  }
}

double quote(std::int64_t raw) {
  return static_cast<double>(raw) * kRawToQuote;
}

std::string position_labels(const StatusSnapshot& s, const StatusPosition& p) {
  const std::uint8_t v = p.venue < kStatusMaxVenues ? p.venue : 0;
  return fmt::format("venue=\"{}\",instrument=\"{}\"",
                     label(name_of(s.venues[v].name, sizeof s.venues[v].name)),
                     label(name_of(p.symbol, sizeof p.symbol)));
}

// fastmm-gateway: the account over every strategy, its positions, the attachments and the
// gateway's own routing counters.
void gateway_metrics(Exposition& e, const StatusSnapshot& s) {
  const StatusGateway& g = s.gateway;
  e.gauge("fastmm_account_net_pnl",
          "the account's net PnL over every strategy (carried + realized + unrealized - fees), "
          "quote currency",
          quote(g.net_pnl_raw));
  e.gauge("fastmm_account_realized_pnl", "the account's realized PnL", quote(s.realized_pnl_raw));
  e.gauge(
      "fastmm_account_unrealized_pnl", "the account's unrealized PnL", quote(s.unrealized_pnl_raw));
  e.gauge("fastmm_account_fees", "the account's fees", quote(s.fees_raw));
  e.gauge("fastmm_account_pnl_carry",
          "net PnL of earlier gateway runs that [gateway] max_loss is measured against as well",
          quote(s.pnl_carry_raw));
  e.gauge("fastmm_account_gross_exposure",
          "the account's gross exposure at the marks, quote currency",
          quote(g.gross_raw));
  e.gauge("fastmm_account_net_exposure",
          "the account's net exposure at the marks, quote currency",
          quote(g.net_raw));
  e.gauge("fastmm_account_max_loss", "[gateway] max_loss, 0 when off", quote(g.max_loss_raw));
  e.gauge("fastmm_account_max_gross_notional",
          "[gateway] max_gross_notional, 0 when off",
          quote(g.max_gross_raw));
  e.gauge("fastmm_account_max_net_notional",
          "[gateway] max_net_notional, 0 when off",
          quote(g.max_net_raw));
  underlying_metrics(e, "fastmm_account", g.underlyings);

  e.family("fastmm_account_position", "gauge", "the account's position, base units");
  const std::size_t np = std::min<std::size_t>(g.position_count, kStatusMaxPositions);
  for (std::size_t i = 0; i < np; ++i) {
    const StatusPosition& p = g.positions[i];
    const std::uint8_t v = p.venue < kStatusMaxVenues ? p.venue : 0;
    e.value_of("fastmm_account_position",
               fmt::format("venue=\"{}\",instrument=\"{}\"",
                           label(name_of(s.venues[v].name, sizeof s.venues[v].name)),
                           label(name_of(p.symbol, sizeof p.symbol))),
               static_cast<double>(p.qty_raw) * kRawToQuote);
  }
  e.family("fastmm_account_unattributed",
           "gauge",
           "a shared instrument: what the account holds that no strategy's position holds, base "
           "units");
  for (std::size_t i = 0; i < np; ++i) {
    const StatusPosition& p = g.positions[i];
    if (p.shared == 0) continue;
    e.value_of("fastmm_account_unattributed",
               position_labels(s, p),
               static_cast<double>(p.unattributed_raw) * kRawToQuote);
  }
  e.family("fastmm_account_unexplained",
           "gauge",
           "a shared instrument: the account's position less the strategies' and the unattributed "
           "part, base units; 0 when they agree");
  for (std::size_t i = 0; i < np; ++i) {
    const StatusPosition& p = g.positions[i];
    if (p.shared == 0) continue;
    e.value_of("fastmm_account_unexplained",
               position_labels(s, p),
               static_cast<double>(p.unexplained_raw) * kRawToQuote);
  }
  e.family("fastmm_gateway_instrument_traders", "gauge", "attachments trading the instrument");
  for (std::size_t i = 0; i < np; ++i) {
    const StatusPosition& p = g.positions[i];
    e.value_of(
        "fastmm_gateway_instrument_traders", position_labels(s, p), static_cast<double>(p.traders));
  }
  e.family("fastmm_gateway_instrument_owner",
           "gauge",
           "session epoch of the attachment that trades the instrument; absent when none does");
  for (std::size_t i = 0; i < np; ++i) {
    const StatusPosition& p = g.positions[i];
    if (p.owner_epoch == 0) continue;
    const std::uint8_t v = p.venue < kStatusMaxVenues ? p.venue : 0;
    e.value_of("fastmm_gateway_instrument_owner",
               fmt::format("venue=\"{}\",instrument=\"{}\"",
                           label(name_of(s.venues[v].name, sizeof s.venues[v].name)),
                           label(name_of(p.symbol, sizeof p.symbol))),
               static_cast<double>(p.owner_epoch));
  }

  const std::size_t na = std::min<std::size_t>(g.attachment_count, kStatusMaxAttachments);
  e.gauge("fastmm_gateway_attachments", "strategies attached", static_cast<double>(na));
  const auto who = [&](const StatusAttachment& a) {
    return fmt::format(
        "epoch=\"{}\",engine=\"{}\"", a.epoch, label(name_of(a.engine, sizeof a.engine)));
  };
  e.family("fastmm_gateway_attachment_info", "gauge", "constant 1 per attachment");
  for (std::size_t i = 0; i < na; ++i) {
    const StatusAttachment& a = g.attachments[i];
    e.value_of("fastmm_gateway_attachment_info",
               fmt::format("{},pid=\"{}\",attachment=\"{}\"", who(a), a.pid, a.id),
               1.0);
  }
  e.family("fastmm_gateway_attachment_uptime_seconds", "gauge", "seconds since it attached");
  for (std::size_t i = 0; i < na; ++i) {
    const StatusAttachment& a = g.attachments[i];
    e.value_of("fastmm_gateway_attachment_uptime_seconds",
               who(a),
               static_cast<double>(s.updated_ns - a.attached_ns) * kNsToS);
  }
  e.family("fastmm_gateway_attachment_md_dropped_total",
           "counter",
           "market-data events dropped because its ring was full");
  for (std::size_t i = 0; i < na; ++i)
    e.value_of("fastmm_gateway_attachment_md_dropped_total",
               who(g.attachments[i]),
               g.attachments[i].md_dropped);
  e.family("fastmm_gateway_attachment_refused_total",
           "counter",
           "its orders the gateway refused, by reason");
  for (std::size_t i = 0; i < na; ++i) {
    for (std::size_t k = 0; k < kStatusGatewayRefusals; ++k)
      e.value_of("fastmm_gateway_attachment_refused_total",
                 fmt::format("{},reason=\"{}\"",
                             who(g.attachments[i]),
                             to_string(kStatusGatewayRefusalReasons[k])),
                 g.attachments[i].refused[k]);
  }

  const std::size_t nv = std::min<std::size_t>(s.venue_count, kStatusMaxVenues);
  const auto venue = [&](std::size_t i) {
    return fmt::format("venue=\"{}\"", label(name_of(s.venues[i].name, sizeof s.venues[i].name)));
  };
  e.family("fastmm_gateway_refused_total", "counter", "orders the gateway refused, by reason");
  for (std::size_t i = 0; i < nv; ++i) {
    for (std::size_t k = 0; k < kStatusGatewayRefusals; ++k)
      e.value_of(
          "fastmm_gateway_refused_total",
          fmt::format("{},reason=\"{}\"", venue(i), to_string(kStatusGatewayRefusalReasons[k])),
          g.venues[i].refused[k]);
  }
  const auto per_venue = [&](std::string_view name, std::string_view help, auto&& value) {
    e.family(name, "counter", help);
    for (std::size_t i = 0; i < nv; ++i) e.value_of(name, venue(i), value(g.venues[i]));
  };
  per_venue("fastmm_gateway_md_discarded_total",
            "market-data events discarded with nothing attached",
            [](const StatusGatewayVenue& v) { return v.md_discarded; });
  per_venue("fastmm_gateway_order_discarded_total",
            "order events that arrived with nothing attached",
            [](const StatusGatewayVenue& v) { return v.order_discarded; });
  per_venue("fastmm_gateway_unrouted_total",
            "order events no attachment was there to take",
            [](const StatusGatewayVenue& v) { return v.unrouted; });
  per_venue("fastmm_gateway_cancels_total",
            "cancels the gateway sent itself (detached and dead sessions' orders, account kill)",
            [](const StatusGatewayVenue& v) { return v.gateway_cancels; });
  per_venue("fastmm_gateway_untracked_total",
            "orders the gateway's order table had no room for",
            [](const StatusGatewayVenue& v) { return v.untracked; });
  per_venue("fastmm_gateway_stale_replays_total",
            "replayed fills older than their owner's history, not routed",
            [](const StatusGatewayVenue& v) { return v.stale_replays; });
  per_venue("fastmm_gateway_account_skipped_total",
            "replayed fills the account's seed position holds already",
            [](const StatusGatewayVenue& v) { return v.account_skipped; });
  per_venue("fastmm_gateway_account_books_lost_total",
            "times the account's books started over (their ring was full)",
            [](const StatusGatewayVenue& v) { return v.account_md_lost; });
}

}  // namespace

}  // namespace fastmm
