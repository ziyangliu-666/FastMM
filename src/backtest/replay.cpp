#include "fastmm/backtest/replay.hpp"

#include "fastmm/backtest/journal_source.hpp"
#include "fastmm/backtest/registrations.hpp"
#include "fastmm/core/journal.hpp"
#include "fastmm/sim/outbound_hash.hpp"
#include "fastmm/sim/sim_backend.hpp"
#include "fastmm/strategies/registry.hpp"

#include <cstring>
#include <stdexcept>

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

}  // namespace

JournalInfo inspect_journal(const std::string& path) {
  JournalReader reader;
  open_or_throw(reader, path);
  JournalInfo info;
  const JournalFileHeader& h = reader.header();
  info.strategy = header_strategy(h);
  info.rng_seed = h.rng_seed;
  info.config_hash = h.config_hash;
  info.start_ts = h.start_ts_ns;
  reader.for_each([&](const EventHeader* e) {
    ++info.messages;
    if ((e->flags & EventHeader::kOutbound) != 0) {
      ++info.outbound_messages;
    } else if (JournalSource::is_market_data(e->type)) {
      ++info.market_data_messages;
    }
  });
  return info;
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
  sim::OutboundHasher recorded;
  for (const auto& b : expected) recorded.add(*reinterpret_cast<const EventHeader*>(b.data()));
  res.recorded_sha256 = recorded.hex();
  res.recorded_messages = recorded.count();

  InstrumentTable instruments;
  for (const Instrument& inst : reader.instruments()) {
    if (!instruments.add(inst)) throw std::runtime_error("replay: bad instrument table in journal");
  }
  if (instruments.size() == 0) instruments = cfg.instruments;

  const Timestamp start{reader.header().start_ts_ns};
  sim::ReplayBackend backend(reader, start);
  backend.transport.set_supports_replace(cfg.transport.supports_replace);
  if (opt.verify) backend.transport.set_expected(std::move(expected));

  register_builtin_strategies();
  RunnerDeps deps;
  deps.engine = cfg.engine;
  deps.instruments = &instruments;
  deps.params = cfg.params;
  deps.backend = &backend;
  if (StrategyRegistry::instance().find(res.strategy) == nullptr)
    throw std::invalid_argument("replay: unknown strategy '" + res.strategy + "'");
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
  return res;
}

}  // namespace fastmm::bt
