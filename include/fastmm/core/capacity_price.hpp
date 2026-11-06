#pragma once
// The capacity price ([capacity]): every action that uses an order slot is a purchase.
//
// A new order, and a move of a resting one, takes a slot of its account's order window and of the
// day's orders. It is sent when what it is worth is at least what the slots cost at that moment:
//
//   send  iff  value >= price(account) = L * h(z_window) + L * (h(z_weight) - 1) + day prices
//
//   value      what the strategy says the order brings in (the level's or NewOrderRequest's
//              value), in the quote currency; else the [capacity] value default: "off" (worth
//              any price) or "learned" (notional * yield * margin_bps, the yield being the
//              filled notional per placed notional of the instrument, side and size class)
//   a move     buys the gain: the value at the new price less the resting order's (keep). A
//              move away from the touch cancels for free; its new order is bought as a new one
//   L          what an order of the venue's minimum notional is worth at the session's yield
//   h(z)       the online-knapsack threshold on the share z of the window used (Zhou,
//              Chakrabarty and Lukose 2008): 1 below z0 = 1 / (1 + ln R), (R e)^z / e above, R at a
//              full window; R = [risk] max_order_notional / the minimum notional
//   z_window   the account's orders in the current window less the first fills in it (a venue
//              that credits fills), over orders_10s
//   z_weight   the pool's requests in the current minute over weight_1m (0: not priced)
//   day prices the account's (orders_1d) and the strategy's (orders_per_day): the value at which
//              the demand of the rest of the UTC day fits what is left of the budget, shared out
//              by the demand of each hour of the day
//
// Cancels are never priced. A level whose order is not worth its price is priced out: its
// target stays in its slot, and the engine applies it again when the window rolls over or a fill
// gives a slot back. Every input is the engine's own: its sends, its fills, the configuration and
// the clock, so a replay prices every order as the session did.
//
// Hot path: z is a counter, h a table of 101 entries, the price a few multiply-adds. The yield
// estimates decay and the day prices are fitted once a minute (tick()). No allocation.
#include "fastmm/core/enums.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/order.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/transport.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace fastmm {

enum class CapacityMode : std::uint8_t {
  Off = 0,      // nothing is priced
  Observe = 1,  // prices are computed, counted and recorded; nothing is refused
  New = 2,      // new orders are refused below their price; moves are observed
  Enforce = 3,  // new orders and moves are refused below their price
};
[[nodiscard]] constexpr std::string_view to_string(CapacityMode m) noexcept {
  switch (m) {
    case CapacityMode::Off:
      return "off";
    case CapacityMode::Observe:
      return "observe";
    case CapacityMode::New:
      return "new";
    case CapacityMode::Enforce:
      return "enforce";
  }
  return "off";
}

// The value of a quote level whose strategy states none.
enum class CapacityValue : std::uint8_t {
  Off = 0,      // worth any price: the level is sent as without [capacity]
  Learned = 1,  // notional * yield(instrument, side, size class) * margin_bps
};

struct CapacityConfig {
  CapacityMode mode = CapacityMode::Off;
  CapacityValue value = CapacityValue::Off;
  // Each account's order window: its length and the orders it admits (Binance Spot: 10 s, 100).
  // 0 orders: the window is not priced.
  std::int64_t window_ms = 10'000;
  std::int64_t orders_10s = 0;
  // The venue gives a slot back on an order's first fill (Binance's unfilled order count).
  bool fill_credit = false;
  // Each account's orders per UTC day (Binance Spot: 200,000); 0: no day price per account.
  std::int64_t orders_1d = 0;
  // The strategy's own orders per UTC day over every account; 0: no budget.
  std::int64_t orders_per_day = 0;
  // Request weight per minute shared by the accounts of a pool (one IP); 0: not priced. A new
  // order, a replace and a cancel each count 1.
  std::int64_t weight_1m = 0;
  // value = "learned": what a filled unit of notional earns, bps (rebate plus edge).
  double margin_bps = 1.0;
  // A move of this many bps or more is worth a whole new order when the strategy states no keep
  // value: the resting order keeps value * (1 - move / full_move_bps).
  double full_move_bps = 10.0;
  // The threshold's range in notional, filled by the engine: the smallest minimum notional of the
  // instruments and [risk] max_order_notional (quote currency).
  double min_notional = 0.0;
  double max_notional = 0.0;
  // The accounts priced, bit per VenueId ([capacity] venues: those venues and their pools'
  // members); the others are neither priced nor counted.
  std::uint32_t accounts = ~0U;
  [[nodiscard]] bool on() const noexcept { return mode != CapacityMode::Off; }
};

// What an action costs on one account now (ctx.capacity).
struct CapacityQuote {
  double price = 0.0;         // the whole price: what an action's value must reach
  double floor = 0.0;         // L, the value of a minimum-notional order
  double window = 0.0;        // the account window's part (L * h(z))
  double weight = 0.0;        // the pool's request weight part
  double day = 0.0;           // the account's and the strategy's day prices
  double window_share = 0.0;  // z of the account's window
  std::int64_t window_used = 0;
  std::int64_t window_orders = 0;  // orders_10s (0: the window is not priced)
  bool on = false;
};

// A minute of one account (CapacityRecord): what was priced and refused.
struct CapacityMinute {
  std::uint32_t decided = 0;  // actions priced
  std::uint32_t sent = 0;     // new orders and replaces sent
  std::uint32_t priced_out_new = 0;
  std::uint32_t priced_out_move = 0;
  std::uint32_t observed_new = 0;  // would have been priced out (observe, or moves under "new")
  std::uint32_t observed_move = 0;
  std::uint32_t peak_used = 0;  // the most orders in one window
  double price_min = 0.0;
  double price_max = 0.0;
  double price_sum = 0.0;
  void clear() noexcept { *this = CapacityMinute{}; }
};

// h(z): the knapsack threshold as a multiple of L, precomputed.
class CapacityThreshold {
 public:
  static constexpr int kSteps = 100;
  void configure(double ratio) noexcept {
    ratio_ = std::max(ratio, 1.0);
    const double ln = std::log(ratio_);
    z0_ = 1.0 / (1.0 + ln);
    for (int k = 0; k <= kSteps; ++k) {
      const double z = static_cast<double>(k) / kSteps;
      table_[static_cast<std::size_t>(k)] =
          z < z0_ ? 1.0 : std::max(1.0, std::exp((z * (ln + 1.0)) - 1.0));
    }
  }
  // The multiple of L at window share z (clamped to [0, 1]).
  [[nodiscard]] double at(double z) const noexcept {
    const double c = z <= 0.0 ? 0.0 : (z >= 1.0 ? 1.0 : z);
    return table_[static_cast<std::size_t>(c * kSteps)];
  }
  [[nodiscard]] double z0() const noexcept { return z0_; }
  [[nodiscard]] double ratio() const noexcept { return ratio_; }

 private:
  std::array<double, kSteps + 1> table_{};
  double ratio_ = 1.0;
  double z0_ = 1.0;
};

// The day price of one budget: the value at which the rest of the day's demand fits what is
// left. Demand is every action priced, by value as a multiple of L in quarter octaves.
class CapacityDayPrice {
 public:
  static constexpr int kBuckets = 64;
  static constexpr int kZero = 16;  // bucket of value L: [L, L * 2^(1/4))
  // The lower edge of bucket b as a multiple of L.
  [[nodiscard]] static double edge(int b) noexcept {
    return std::exp2(static_cast<double>(b - kZero) * 0.25);
  }
  // The bucket of a value v given as a multiple of L (v / L); infinite: the top bucket.
  [[nodiscard]] static int bucket(double rel) noexcept {
    if (!(rel > 0.0)) return 0;
    if (rel >= kTopEdge) return kBuckets - 1;
    int e = 0;
    const double m = std::frexp(rel, &e);  // rel = m * 2^e, m in [0.5, 1)
    // quarter octave within [0.5, 1): thresholds 2^(-1), 2^(-3/4), 2^(-1/2), 2^(-1/4)
    int q = 0;
    if (m >= 0.5946035575013605) q = 1;
    if (m >= 0.7071067811865476) q = 2;
    if (m >= 0.8408964152537145) q = 3;
    const int b = ((e - 1) * 4) + q + kZero;
    return std::clamp(b, 0, kBuckets - 1);
  }

  void note(double rel) noexcept { cur_[static_cast<std::size_t>(bucket(rel))] += 1.0; }
  void note_sent() noexcept { ++used_; }
  [[nodiscard]] double price() const noexcept { return lambda_; }
  [[nodiscard]] std::int64_t used() const noexcept { return used_; }

  // Once a minute: fold the minute's demand in, roll the day, and fit the price. `budget` <= 0:
  // no price. `kind` 0 for a weekday, 1 for a weekend (their hours differ).
  void tick(std::int64_t now_ns, std::int64_t budget, double floor, int kind) noexcept {
    constexpr std::int64_t kDayNs = 86'400'000'000'000;
    constexpr std::int64_t kHourNs = 3'600'000'000'000;
    double minute = 0.0;
    for (std::size_t b = 0; b < kBuckets; ++b) {
      hist_[b] = (hist_[b] * kKeep) + cur_[b];
      minute += cur_[b];
      cur_[b] = 0.0;
    }
    const std::int64_t day = now_ns / kDayNs;
    if (day != day_) {
      day_ = day;
      used_ = 0;
    }
    const int h = static_cast<int>((now_ns % kDayNs) / kHourNs);
    if (h != hour_ || kind != kind_) {
      if (hour_ >= 0) {
        double& d = dem_[static_cast<std::size_t>(kind_)][static_cast<std::size_t>(hour_)];
        bool& s = seen_[static_cast<std::size_t>(kind_)][static_cast<std::size_t>(hour_)];
        d = s ? (0.8 * d) + (0.2 * hour_count_) : hour_count_;
        s = true;
      }
      hour_ = h;
      kind_ = kind;
      hour_count_ = 0.0;
    }
    hour_count_ += minute;
    lambda_ = 0.0;
    if (budget <= 0) return;
    // The demand of an hour: its profile, else the recent rate (decisions per hour).
    double recent = 0.0;
    for (const double x : hist_) recent += x;
    const double recent_per_hour = recent * (1.0 - kKeep) * 60.0;
    const auto dw = [&](int k) {
      const auto ki = static_cast<std::size_t>(kind);
      const auto kk = static_cast<std::size_t>(k);
      return seen_[ki][kk] ? std::max(dem_[ki][kk], 1.0) : std::max(recent_per_hour, 1.0);
    };
    const double hour_left =
        1.0 - (static_cast<double>(now_ns % kHourNs) / static_cast<double>(kHourNs));
    double rest = dw(h) * hour_left;
    for (int k = h + 1; k < 24; ++k) rest += dw(k);
    const double remaining = static_cast<double>(std::max<std::int64_t>(budget - used_, 0));
    if (remaining >= rest || recent <= 0.0) return;
    // The fraction of the demand that fits; the most valuable part of the recent demand gets it.
    const double keep = remaining / rest * recent;
    double cum = 0.0;
    for (int b = kBuckets - 1; b >= 0; --b) {
      cum += hist_[static_cast<std::size_t>(b)];
      if (cum > keep) {
        lambda_ = floor * edge(std::min(b + 1, kBuckets - 1));
        return;
      }
    }
  }

 private:
  static constexpr double kKeep = 0.9;  // a minute's weight in the demand: ten minutes' memory
  static constexpr double kTopEdge = 4096.0;  // 2^((kBuckets - 1 - kZero) / 4) rounded up
  std::array<double, kBuckets> cur_{};
  std::array<double, kBuckets> hist_{};
  double dem_[2][24] = {};
  bool seen_[2][24] = {};
  double hour_count_ = 0.0;
  int hour_ = -1;
  int kind_ = 0;
  std::int64_t day_ = -1;
  std::int64_t used_ = 0;
  double lambda_ = 0.0;
};

// The prices, the windows and the learned yields of one engine.
class CapacityPrice {
 public:
  static constexpr int kClasses = 6;  // by notional over the minimum: <2, <6, <12, <22, <60, more
  static constexpr double kPriorYield = 0.02;    // filled per placed notional before any is learned
  static constexpr double kShrinkOrders = 20.0;  // a cell's estimate leans on its class's for this
  static constexpr std::int64_t kTickNs = 60'000'000'000;
  static constexpr std::int64_t kWeightWindowNs = 60'000'000'000;
  static constexpr double kYieldHalfLifeS = 6.0 * 3600.0;

  void configure(const CapacityConfig& cfg) noexcept {
    cfg_ = cfg;
    if (!(cfg_.min_notional > 0.0))
      cfg_.min_notional = cfg_.max_notional > 0.0 ? cfg_.max_notional / 100.0 : 1.0;
    if (!(cfg_.max_notional > cfg_.min_notional)) cfg_.max_notional = cfg_.min_notional * 100.0;
    window_ns_ = std::max<std::int64_t>(cfg_.window_ms, 1) * 1'000'000;
    threshold_.configure(cfg_.max_notional / cfg_.min_notional);
    update_floor();
  }
  [[nodiscard]] const CapacityConfig& config() const noexcept { return cfg_; }
  [[nodiscard]] bool on() const noexcept { return cfg_.on(); }
  [[nodiscard]] CapacityMode mode() const noexcept { return cfg_.mode; }
  [[nodiscard]] const CapacityThreshold& threshold() const noexcept { return threshold_; }
  [[nodiscard]] double floor() const noexcept { return floor_; }
  // Whether orders on `account` are priced ([capacity] venues).
  [[nodiscard]] bool prices(VenueId account) const noexcept {
    return account.value < 32 && ((cfg_.accounts >> account.value) & 1U) != 0;
  }

  // Whether a refusal below the price is enforced for a new order or for a move.
  [[nodiscard]] bool enforces(bool move) const noexcept {
    return cfg_.mode == CapacityMode::Enforce || (cfg_.mode == CapacityMode::New && !move);
  }

  // ---- inputs: the engine's own sends and fills -------------------------------------------------

  // A new order or a replace went to `account` (of the pool whose primary is `primary`).
  void on_send(VenueId account,
               VenueId primary,
               InstrumentId id,
               Side side,
               double notional,
               std::int64_t now_ns) noexcept {
    if (!prices(account)) return;
    if (account.value < kMaxVenues) {
      Window& w = window(account, now_ns);
      ++w.sent;
      AccountState& a = acct_[account.value];
      a.minute.peak_used = std::max(a.minute.peak_used, static_cast<std::uint32_t>(w.used()));
      ++a.minute.sent;
      a.day.note_sent();
    }
    all_.note_sent();
    on_request(primary, now_ns);
    if (id.value < kMaxInstruments && notional > 0.0) {
      Yield& c = cell(id, side, notional);
      c.placed += notional;
      c.n += 1.0;
      Yield& k = class_[static_cast<std::size_t>(class_of(notional))];
      k.placed += notional;
      k.n += 1.0;
      total_.placed += notional;
      total_.n += 1.0;
    }
  }
  // A cancel (or any other request) of the pool: weight only.
  void on_request(VenueId primary, std::int64_t now_ns) noexcept {
    if (cfg_.weight_1m <= 0 || primary.value >= kMaxVenues) return;
    Window& w = weight_[primary.value];
    const std::int64_t wid = now_ns / kWeightWindowNs;
    if (w.id != wid) w = Window{wid, 0, 0};
    ++w.sent;
  }
  // A fill of an order placed with `order_notional`; `first`: the order's first fill.
  void on_fill(VenueId account,
               InstrumentId id,
               Side side,
               double order_notional,
               double fill_notional,
               bool first,
               std::int64_t now_ns) noexcept {
    if (!prices(account)) return;
    if (first && cfg_.fill_credit && account.value < kMaxVenues) {
      Window& w = window(account, now_ns);
      if (w.credit < w.sent) {
        ++w.credit;
        credited_ = true;
      }
    }
    if (id.value < kMaxInstruments && order_notional > 0.0 && fill_notional > 0.0) {
      cell(id, side, order_notional).filled += fill_notional;
      class_[static_cast<std::size_t>(class_of(order_notional))].filled += fill_notional;
      total_.filled += fill_notional;
    }
  }

  // ---- the price ----------------------------------------------------------------------------

  [[nodiscard]] CapacityQuote quote(VenueId account,
                                    VenueId primary,
                                    std::int64_t now_ns) const noexcept {
    CapacityQuote q;
    q.on = on() && prices(account);
    q.floor = floor_;
    q.window_orders = cfg_.orders_10s;
    if (!q.on) return q;
    q.window = floor_;
    if (cfg_.orders_10s > 0 && account.value < kMaxVenues) {
      const Window& w = acct_[account.value].window;
      q.window_used = w.id == now_ns / window_ns_ ? w.used() : 0;
      q.window_share = static_cast<double>(q.window_used) / static_cast<double>(cfg_.orders_10s);
      q.window = floor_ * threshold_.at(q.window_share);
    }
    if (cfg_.weight_1m > 0 && primary.value < kMaxVenues) {
      const Window& w = weight_[primary.value];
      const std::int64_t used = w.id == now_ns / kWeightWindowNs ? w.used() : 0;
      q.weight =
          floor_ *
          (threshold_.at(static_cast<double>(used) / static_cast<double>(cfg_.weight_1m)) - 1.0);
    }
    q.day = all_.price() + (account.value < kMaxVenues ? acct_[account.value].day.price() : 0.0);
    q.price = q.window + q.weight + q.day;
    return q;
  }
  [[nodiscard]] double price(VenueId account, VenueId primary, std::int64_t now_ns) const noexcept {
    return quote(account, primary, now_ns).price;
  }

  // The learned value of a quote of `notional` on (id, side), quote currency.
  [[nodiscard]] double learned_value(InstrumentId id, Side side, double notional) const noexcept {
    if (!(notional > 0.0) || id.value >= kMaxInstruments) return 0.0;
    const int c = class_of(notional);
    const double global = shrink(total_, kPriorYield);
    const double cls = shrink(class_[static_cast<std::size_t>(c)], global);
    const double y =
        shrink(cells_[id.value][static_cast<std::size_t>(side)][static_cast<std::size_t>(c)], cls);
    return notional * y * cfg_.margin_bps * 1e-4;
  }

  // An action of `value` (a move: its gain) was priced at `price` on `account`; returns whether it
  // passes. Records it in the account's minute and in the day's demand. A refusal of instrument
  // `id`'s side that was already refused in the same window is the same demand asked again (a
  // strategy requoting a priced-out level): refused, and recorded once.
  bool decide(VenueId account,
              double value,
              double price,
              bool move,
              bool enforced,
              InstrumentId id = InstrumentId{},
              Side side = Side::Buy,
              std::int64_t now_ns = 0) noexcept {
    if (value != value) value = std::numeric_limits<double>::infinity();  // unpriced
    const bool pass = !(value < price);
    if (!pass && enforced && id.value < kMaxInstruments) {
      std::int64_t& last = refused_[id.value][static_cast<std::size_t>(side)];
      const std::int64_t w = (now_ns / window_ns_) + 1;
      if (last == w) return false;
      last = w;
    }
    const double rel = floor_ > 0.0 ? value / floor_ : std::numeric_limits<double>::infinity();
    all_.note(rel);
    if (account.value >= kMaxVenues) return pass;
    AccountState& a = acct_[account.value];
    a.day.note(rel);
    CapacityMinute& m = a.minute;
    if (m.decided == 0) {
      m.price_min = price;
      m.price_max = price;
    } else {
      m.price_min = std::min(m.price_min, price);
      m.price_max = std::max(m.price_max, price);
    }
    ++m.decided;
    m.price_sum += price;
    if (!pass) {
      if (enforced) {
        ++(move ? m.priced_out_move : m.priced_out_new);
        ++(move ? totals_.priced_out_move : totals_.priced_out_new);
      } else {
        ++(move ? m.observed_move : m.observed_new);
        ++(move ? totals_.observed_move : totals_.observed_new);
      }
    }
    ++totals_.decided;
    return pass;
  }
  [[nodiscard]] const CapacityMinute& totals() const noexcept { return totals_; }

  // ---- the clock ----------------------------------------------------------------------------

  // A priced-out level is applied again once a window has rolled over or a fill gave a slot back.
  [[nodiscard]] bool recheck_due(std::int64_t now_ns) const noexcept {
    return credited_ || now_ns >= recheck_ns_;
  }
  void rechecked(std::int64_t now_ns) noexcept {
    credited_ = false;
    recheck_ns_ = ((now_ns / window_ns_) + 1) * window_ns_;
  }
  [[nodiscard]] bool tick_due(std::int64_t now_ns) const noexcept { return now_ns >= tick_ns_; }
  // Once a minute: decay the yields, refit the floor and the day prices, and hand each account's
  // minute to `emit(VenueId, const CapacityMinute&)` before clearing it.
  template <class Emit>
  void tick(std::int64_t now_ns, Emit&& emit) noexcept {
    const bool first = tick_ns_ == 0;
    tick_ns_ = ((now_ns / kTickNs) + 1) * kTickNs;
    if (first) {
      last_decay_ns_ = now_ns;
      return;
    }
    const double dt = static_cast<double>(now_ns - last_decay_ns_) * 1e-9;
    last_decay_ns_ = now_ns;
    if (dt > 0.0) {
      const double k = std::exp2(-dt / kYieldHalfLifeS);
      decay(total_, k);
      for (Yield& y : class_) decay(y, k);
      for (auto& inst : cells_)
        for (auto& side : inst)
          for (Yield& y : side) decay(y, k);
    }
    update_floor();
    const int kind = weekend(now_ns) ? 1 : 0;
    all_.tick(now_ns, cfg_.orders_per_day, floor_, kind);
    for (std::size_t v = 0; v < kMaxVenues; ++v) {
      AccountState& a = acct_[v];
      a.day.tick(now_ns, cfg_.orders_1d, floor_, kind);
      if (a.minute.decided != 0 || a.minute.sent != 0) {
        emit(VenueId{static_cast<std::uint8_t>(v)}, a.minute);
        a.minute.clear();
      }
    }
  }
  [[nodiscard]] double day_price(VenueId account) const noexcept {
    return all_.price() + (account.value < kMaxVenues ? acct_[account.value].day.price() : 0.0);
  }

 private:
  struct Window {
    std::int64_t id = -1;
    std::int64_t sent = 0;
    std::int64_t credit = 0;
    [[nodiscard]] std::int64_t used() const noexcept {
      return std::max<std::int64_t>(sent - credit, 0);
    }
  };
  struct Yield {
    double placed = 0.0;
    double filled = 0.0;
    double n = 0.0;
  };
  struct AccountState {
    Window window;
    CapacityDayPrice day;
    CapacityMinute minute;
  };

  Window& window(VenueId account, std::int64_t now_ns) noexcept {
    Window& w = acct_[account.value].window;
    const std::int64_t wid = now_ns / window_ns_;
    if (w.id != wid) w = Window{wid, 0, 0};
    return w;
  }
  [[nodiscard]] int class_of(double notional) const noexcept {
    const double r = notional / cfg_.min_notional;
    if (r < 2.0) return 0;
    if (r < 6.0) return 1;
    if (r < 12.0) return 2;
    if (r < 22.0) return 3;
    if (r < 60.0) return 4;
    return 5;
  }
  Yield& cell(InstrumentId id, Side side, double notional) noexcept {
    return cells_[id.value][static_cast<std::size_t>(side)]
                 [static_cast<std::size_t>(class_of(notional))];
  }
  // A yield estimate leaning on `prior` with the weight of kShrinkOrders orders of its own size.
  [[nodiscard]] static double shrink(const Yield& y, double prior) noexcept {
    if (!(y.n > 0.0) || !(y.placed > 0.0)) return prior;
    const double w = kShrinkOrders * (y.placed / y.n);
    return (y.filled + (prior * w)) / (y.placed + w);
  }
  static void decay(Yield& y, double k) noexcept {
    y.placed *= k;
    y.filled *= k;
    y.n *= k;
  }
  void update_floor() noexcept {
    floor_ = cfg_.min_notional * shrink(total_, kPriorYield) * cfg_.margin_bps * 1e-4;
  }
  [[nodiscard]] static bool weekend(std::int64_t ns) noexcept {
    const auto wd = (ns / 86'400'000'000'000 + 4) % 7;  // 1970-01-01 was a Thursday
    return wd == 0 || wd == 6;
  }

  CapacityConfig cfg_{};
  CapacityThreshold threshold_{};
  std::int64_t window_ns_ = 10'000'000'000;
  double floor_ = 0.0;
  std::array<AccountState, kMaxVenues> acct_{};
  std::array<Window, kMaxVenues> weight_{};  // by pool primary
  CapacityDayPrice all_{};
  Yield cells_[kMaxInstruments][2][kClasses] = {};
  std::array<Yield, kClasses> class_{};
  Yield total_{};
  CapacityMinute totals_{};
  std::int64_t refused_[kMaxInstruments][2] = {};  // 1 + the window of the last refusal
  std::int64_t recheck_ns_ = 0;
  bool credited_ = false;
  std::int64_t tick_ns_ = 0;
  std::int64_t last_decay_ns_ = 0;
};

}  // namespace fastmm
