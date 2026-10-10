#pragma once
// Why a side of an instrument is or is not quoting (ctx.note_quote, the status file's quote
// table): what the strategy last asked for on the side against the
// orders working there, the first thing the engine finds in the way, and the strategy's own
// reason and budget for the side. The engine knows its own obstacles (quoting off, a pull, a kill,
// the feed-lag gate, a reject backoff, a wait for an order token, a refusal); why a model chose not
// to quote, or quoted less than its budget, only the strategy can say.
#include "fastmm/core/enums.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace fastmm {

inline constexpr std::size_t kQuoteNoteLen = 24;  // QuoteNote::reason, NUL-terminated
inline constexpr std::size_t kMaxStrategyMetrics = 32;
inline constexpr std::size_t kStrategyMetricNameLen = 24;
inline constexpr std::size_t kQuoteDiagAccounts = 8;  // a pool's accounts (EngineConfig::pools)

// The first obstacle between what the strategy asked for on a side and orders working there. The
// engine checks them in this order, except that a venue kill comes before the pull it causes.
enum class QuoteBlock : std::uint8_t {
  Quoting = 0,      // the working orders cover what the strategy asked for
  NotWanted = 1,    // the strategy asks for nothing on this side (its note may say why)
  QuotingOff = 2,   // the session does not quote: pulled, killed, reconciling, parameters stale
  Pulled = 3,       // this instrument or its venue is pulled (fastmm-ctl pull --instrument/--venue)
  VenueKilled = 4,  // its venue's kill switch
  FeedLag = 5,      // the venue's feed-lag gate holds ([risk] max_feed_lag_ms)
  Backoff = 6,      // a venue refusal put the side in its reject backoff
  Starved = 7,      // waiting for an order token ([risk] orders_per_sec)
  Refused = 8,      // the last order on the side was refused (`reason`) within the last 2 s
  Pending = 9,      // orders on their way: sent, not yet working, or a requote waiting its turn
};
[[nodiscard]] constexpr std::string_view to_string(QuoteBlock b) noexcept {
  switch (b) {
    case QuoteBlock::Quoting:
      return "quoting";
    case QuoteBlock::NotWanted:
      return "not_wanted";
    case QuoteBlock::QuotingOff:
      return "quoting_off";
    case QuoteBlock::Pulled:
      return "pulled";
    case QuoteBlock::VenueKilled:
      return "venue_killed";
    case QuoteBlock::FeedLag:
      return "feed_lag";
    case QuoteBlock::Backoff:
      return "backoff";
    case QuoteBlock::Starved:
      return "starved";
    case QuoteBlock::Refused:
      return "refused";
    case QuoteBlock::Pending:
      return "pending";
  }
  return "?";
}

// What the strategy says about a side (ctx.note_quote): a short reason ("inventory_cap",
// "model_off", "cooldown"; empty: none) and the budget it gave the side, in the settlement
// currency (raw 1e-8; 0: none given).
struct QuoteNote {
  char reason[kQuoteNoteLen] = {};
  std::int64_t budget_raw = 0;
  std::int64_t set_ns = 0;  // engine time of the note
  [[nodiscard]] std::string_view reason_view() const noexcept {
    std::size_t n = 0;
    while (n < kQuoteNoteLen && reason[n] != '\0') ++n;
    return {reason, n};
  }
};

// One side of an instrument as the engine publishes it (EngineLiveStats::quotes).
struct LiveQuoteSide {
  std::int64_t desired_px_raw = 0;   // level 0 of the strategy's last ladder (0: none)
  std::int64_t desired_qty_raw = 0;  // over every level
  std::int64_t working_px_raw = 0;   // best working quote price (0: none)
  std::int64_t working_qty_raw = 0;  // leaves of every open order on the side
  // Leaves of the working quotes by pool account (index in the pool, 0 the primary).
  std::int64_t account_qty_raw[kQuoteDiagAccounts] = {};
  std::int64_t block_since_ns = 0;  // engine time `block` began
  std::uint16_t working_orders = 0;
  std::uint8_t desired_levels = 0;
  QuoteBlock block = QuoteBlock::NotWanted;
  RejectReason reason = RejectReason::None;  // block Refused: the refusal; otherwise None
  std::uint8_t pad_[3] = {};
  QuoteNote note;
};

struct LiveQuoteInstrument {
  std::uint32_t instrument = 0;
  std::uint8_t venue = 0;
  std::uint8_t pad_[3] = {};
  std::int64_t position_raw = 0;
  std::int64_t avg_px_raw = 0;
  std::int64_t realized_raw = 0;
  std::int64_t unrealized_raw = 0;
  std::int64_t mid_raw = 0;  // 0: no two-sided book
  LiveQuoteSide sides[2];    // by Side: Buy, Sell
};

// A number the strategy publishes by name (ctx.metric): a budget split, a model state.
struct LiveMetric {
  char name[kStrategyMetricNameLen] = {};
  double value = 0.0;
};

}  // namespace fastmm
