#include "fastmm/backtest/replay.hpp"

#include "fastmm/backtest/journal_source.hpp"
#include "fastmm/backtest/registrations.hpp"
#include "fastmm/core/journal.hpp"
#include "fastmm/sim/outbound_hash.hpp"
#include "fastmm/sim/sim_backend.hpp"
#include "fastmm/strategies/registry.hpp"

#include <cstring>
#include <stdexcept>
#include <string_view>

namespace fastmm::bt {

namespace {

void open_or_throw(JournalReader& reader, const std::string& path) {
  if (auto r = reader.open(path); !r) {
    throw std::runtime_error("cannot open journal " + path + ": " +
                             std::string(to_string(r.error())));
  }
}

std::string header_strategy(const JournalFileHeader& h) {
  return std::string(h.strategy, strnlen(h.strategy, sizeof h.strategy));
}

std::string decimal(const auto& v) {
  char buf[48];
  const std::size_t n = v.to_decimal(buf);
  return std::string(buf, n);
}

std::string cl_ord(ClientOrderId id) {
  return std::to_string(id.value) + " (epoch " + std::to_string(id.value >> 32) + ", seq " +
         std::to_string(id.value & 0xFFFF'FFFFULL) + ")";
}

}  // namespace

std::string describe_outbound(const EventHeader& h) {
  std::string s(to_string(h.type));
  s += " venue=" + std::to_string(h.venue.value) + " inst=" + std::to_string(h.instrument.value);
  switch (h.type) {
    case EventType::OutNewOrder: {
      const auto& m = msg_cast<OutNewOrderMsg>(&h);
      s += " cl_ord_id=" + cl_ord(m.cl_ord_id) + " " + std::string(to_string(m.side)) + " " +
           std::string(to_string(m.type)) + " " + std::string(to_string(m.tif)) +
           " px=" + decimal(m.price) + " qty=" + decimal(m.qty);
      if (m.reduce_only != 0) s += " reduce_only";
      break;
    }
    case EventType::OutCancel: {
      const auto& m = msg_cast<OutCancelMsg>(&h);
      s += " cl_ord_id=" + cl_ord(m.cl_ord_id) +
           " venue_order_id=" + std::string(std::string_view(m.venue_order_id));
      break;
    }
    case EventType::OutReplace: {
      const auto& m = msg_cast<OutReplaceMsg>(&h);
      s += " cl_ord_id=" + cl_ord(m.cl_ord_id) + " orig=" + cl_ord(m.orig_cl_ord_id) +
           " px=" + decimal(m.price) + " qty=" + decimal(m.qty);
      break;
    }
    default:
      break;
  }
  s += " recv_ts=" + std::to_string(h.recv_ts.ns);
  if ((h.flags & EventHeader::kDropped) != 0) s += " (dropped by the transport)";
  return s;
}

JournalInfo inspect_journal(const std::string& path) {
  JournalReader reader;
  open_or_throw(reader, path);
  JournalInfo info;
  const JournalFileHeader& h = reader.header();
  info.strategy = header_strategy(h);
  info.version = reader.version();
  info.rng_seed = h.rng_seed;
  info.config_hash = h.config_hash;
  info.start_ts = h.start_ts_ns;
  info.has_session = reader.has_session();
  if (info.has_session) {
    info.session_epoch = h.session_epoch;
    info.quoting_enabled = h.quoting_enabled != 0;
    info.replace_venues = h.replace_venues;
  }
  info.config_toml = std::string(reader.config_text());
  info.strategy_meta = std::string(reader.strategy_meta());
  reader.for_each([&](const EventHeader* e) {
    ++info.messages;
    if ((e->flags & EventHeader::kEngineTime) != 0) info.engine_time = true;
    if ((e->flags & EventHeader::kOutbound) != 0) {
      ++info.outbound_messages;
      if ((e->flags & EventHeader::kDropped) != 0) ++info.dropped_outbound;
    } else if (JournalSource::is_market_data(e->type)) {
      ++info.market_data_messages;
    }
  });
  return info;
}

BacktestConfig journal_config(const std::string& path) {
  JournalReader reader;
  open_or_throw(reader, path);
  const std::string_view text = reader.config_text();
  if (text.empty()) {
    throw std::runtime_error("journal " + path + " does not embed its configuration (format v" +
                             std::to_string(reader.version()) +
                             "); pass the configuration the session ran with");
  }
  Config::LoadOptions lo;
  lo.substitute_env = false;
  lo.allow_inline_secrets = true;  // secrets are never embedded; do not second-guess the text
  try {
    BacktestConfig cfg = BacktestConfig::from_config(Config::parse(text, lo, path + " (embedded)"));
    cfg.measure_wall_clock = false;
    cfg.output_dir.clear();
    cfg.journal_out.clear();
    return cfg;
  } catch (const std::exception& e) {
    throw std::runtime_error("journal " + path + ": embedded configuration: " + e.what());
  }
}

ReplayResult replay_journal(const std::string& path, const ReplayOptions& opt) {
  return replay_journal(path, journal_config(path), opt);
}

ReplayResult replay_journal(const std::string& path,
                            const BacktestConfig& cfg,
                            const ReplayOptions& opt) {
  JournalReader reader;
  open_or_throw(reader, path);

  ReplayResult res;
  res.strategy = opt.strategy.empty() ? header_strategy(reader.header()) : opt.strategy;
  if (res.strategy.empty()) throw std::invalid_argument("replay: no strategy given or recorded");

  // Expected stream first: load_outbound() walks the reader, JournalFeed then rewinds it.
  std::vector<sim::ReplayTransport::Bytes> expected = sim::ReplayTransport::load_outbound(reader);
  std::vector<std::uint64_t> dropped = sim::ReplayTransport::load_dropped(reader);
  sim::OutboundHasher recorded;
  {
    std::size_t d = 0;
    for (std::uint64_t i = 0; i < expected.size(); ++i) {
      if (d < dropped.size() && dropped[d] == i) {
        ++d;
        continue;  // never sent
      }
      recorded.add(*reinterpret_cast<const EventHeader*>(expected[i].data()));
    }
  }
  res.recorded_sha256 = recorded.hex();
  res.recorded_messages = recorded.count();

  InstrumentTable instruments;
  for (const Instrument& inst : reader.instruments()) {
    if (!instruments.add(inst)) throw std::runtime_error("replay: bad instrument table in journal");
  }
  if (instruments.size() == 0) instruments = cfg.instruments;

  const JournalFileHeader& header = reader.header();
  const bool session = opt.session_from_journal && reader.has_session();
  res.session_restored = session;
  const Timestamp start{header.start_ts_ns};
  sim::ReplayBackend backend(reader, start);
  backend.transport.set_supports_replace(cfg.transport.supports_replace);
  if (session) {
    // The recording's effective cancel-replace per venue (live: venue capability && config).
    for (std::uint8_t v = 0; v < kMaxVenues; ++v)
      backend.transport.set_supports_replace(VenueId{v}, ((header.replace_venues >> v) & 1U) != 0);
  }
  backend.transport.set_dropped(std::move(dropped));
  const std::size_t expected_count = expected.size();
  if (opt.verify) backend.transport.set_expected(std::move(expected));

  register_builtin_strategies();
  RunnerDeps deps;
  deps.engine = cfg.engine;
  if (session) {
    deps.engine.session_epoch = header.session_epoch;
    deps.engine.quoting_enabled = header.quoting_enabled != 0;
    deps.engine.rng_seed = header.rng_seed;
  }
  deps.instruments = &instruments;
  deps.params = cfg.params;
  deps.backend = &backend;
  const StrategyEntry* entry = StrategyRegistry::instance().find(res.strategy);
  if (entry == nullptr)
    throw std::invalid_argument("replay: unknown strategy '" + res.strategy + "'");
  backend.feed.set_param_updates(opt.param_updates);
  backend.feed.set_param_schema(*entry->schema);
  std::unique_ptr<IEngineRunner> runner =
      StrategyRegistry::instance().make(res.strategy, TransportKind::Replay, deps);
  if (runner == nullptr)
    throw std::invalid_argument("replay: strategy '" + res.strategy + "' has no replay factory");

  sim::ReplayDriver driver(backend.clock, backend.feed, backend.hooks);
  res.events = driver.run_all();
  driver.finish();

  res.outbound_sha256 = backend.transport.hash_hex();
  res.outbound_messages = backend.transport.count();
  res.first_mismatch = backend.transport.first_mismatch();
  if (opt.verify) {
    const std::uint64_t attempts = backend.transport.attempts();
    const auto& exp = backend.transport.expected();
    if (res.first_mismatch < 0 && attempts != expected_count) {
      // Every message matched, but one stream ended early.
      res.first_mismatch =
          static_cast<std::int64_t>(attempts < expected_count ? attempts : expected_count);
    }
    if (res.first_mismatch >= 0) {
      const auto i = static_cast<std::size_t>(res.first_mismatch);
      res.expected_message =
          i < exp.size() ? describe_outbound(*reinterpret_cast<const EventHeader*>(exp[i].data()))
                         : std::string("(none: the recording ends here)");
      const auto& act = backend.transport.mismatch_actual();
      res.actual_message =
          !act.empty() ? describe_outbound(*reinterpret_cast<const EventHeader*>(act.data()))
                       : std::string("(none: the replay ends here)");
    }
  }
  return res;
}

}  // namespace fastmm::bt
