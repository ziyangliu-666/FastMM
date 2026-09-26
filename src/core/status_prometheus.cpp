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
void gateway_metrics(Exposition& e, const StatusSnapshot& s);
void venue_metrics(Exposition& e, const StatusSnapshot& s);

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
  return e.take();
}

namespace {

void engine_metrics(Exposition& e, const StatusSnapshot& s) {
  e.gauge("fastmm_realized_pnl",
          "realized PnL, quote currency",
          static_cast<double>(s.realized_pnl_raw) * kRawToQuote);
  e.gauge("fastmm_unrealized_pnl",
          "unrealized PnL, quote currency",
          static_cast<double>(s.unrealized_pnl_raw) * kRawToQuote);
  e.gauge(
      "fastmm_fees", "fees paid, quote currency", static_cast<double>(s.fees_raw) * kRawToQuote);
  e.gauge("fastmm_pnl_carry",
          "net PnL of earlier sessions that max_loss is measured against as well, quote currency",
          static_cast<double>(s.pnl_carry_raw) * kRawToQuote);

  e.counter("fastmm_events_total", "events the engine consumed", s.events);
  e.counter("fastmm_book_updates_total", "book updates applied", s.book_updates);
  e.counter("fastmm_orders_sent_total", "new orders sent", s.orders_sent);
  e.counter("fastmm_cancels_sent_total", "cancels sent", s.cancels_sent);
  e.counter("fastmm_replaces_sent_total", "replaces sent", s.replaces_sent);
  e.counter("fastmm_fills_total", "executions received", s.fills);
  e.counter("fastmm_risk_rejects_total", "orders refused by the pre-trade checks", s.risk_rejects);
  e.counter("fastmm_venue_rejects_total", "orders refused by a venue", s.venue_rejects);
  e.gauge("fastmm_flatten_state",
          "FlattenState of the operator flatten: 0 off, 1 working, 2 flat, 3 timed out, 4 stopped",
          static_cast<double>(s.flatten_state));
  e.gauge("fastmm_flatten_instruments_left",
          "instruments in the flatten's scope that still hold a position",
          static_cast<double>(s.flatten_instruments_left));
  e.counter("fastmm_flatten_orders_total", "reduce-only orders a flatten sent", s.flatten_orders);
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

  e.family("fastmm_venue_tick_to_trade_quantile_seconds",
           "gauge",
           "socket read to order write, measured on the network thread");
  for (std::size_t i = 0; i < venues; ++i)
    latency_quantiles(
        e,
        "fastmm_venue_tick_to_trade_quantile_seconds",
        fmt::format("venue=\"{}\"", label(name_of(s.venues[i].name, sizeof s.venues[i].name))),
        s.venues[i].wire_tick_to_trade);

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
