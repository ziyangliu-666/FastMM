// fastmm-bench-queue-truth: scores the backtest's queue models against a day of TotalView-ITCH,
// where the outcome of every displayed order is known.
//
//   fastmm-bench-queue-truth FILE [--top K] [--hidden]
//
// Every order that joins the touch (its price equals the best on its side when it is added),
// improves it, or joins the level behind it is followed until it leaves the book. Truth is what
// ITCH executed against it. Each model sees what a backtest would see and follows the order over
// the same life:
//
//   l2 c=...   the l2_queue rule (core/queue_model.hpp: queue_after_level_change on every level
//              change of others' quantity at its price, queue_after_trade on every printed
//              execution) for each queue_conservatism; the L2 view is exact, every message
//   l3 fifo    the l3_queue rule (l3_execution_reaches): the order's place in the arrival sequence,
//              cancels and executions of the orders in front of it
//
// For each kind of order: orders filled in truth and by each model (both, truth only, model only),
// filled quantity against truth, then the markouts of the fills each model produces against the
// true fills, at 1 s, 10 s and 60 s, in basis points of notional: what a backtest's fill model
// does to its PnL. --hidden also feeds P trades (executions against undisplayed orders) to the L2
// rule as prints; ITCH does not say which side they took, so this shows what a feed that prints
// them does to the model. --top K follows the K locates with the most added orders (default 100).
#include "fastmm/core/queue_model.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace fastmm;

namespace {

std::uint64_t be(const unsigned char* p, int n) {
  std::uint64_t v = 0;
  for (int i = 0; i < n; ++i) v = (v << 8U) | p[i];
  return v;
}

constexpr int kL2 = 5;
constexpr std::int64_t kConservatism[kL2] = {0, 2500, 5000, 7500, 10000};
constexpr int kL3 = kL2;         // model index of the l3 rule
constexpr int kTruth = kL2 + 1;  // index of the true fills
constexpr int kModels = kL2 + 2;
constexpr int kKinds = 3;
const char* const kKindName[kKinds] = {"join the touch", "improve the touch", "one level behind"};

Price px_of(std::int64_t itch_price) {
  return Price::from_raw(itch_price * 10'000);
}

struct Order {
  std::uint16_t loc;
  Side side;
  std::int64_t px;  // ITCH units (0.0001)
  std::int64_t qty;
  std::uint64_t prio;
  std::int32_t followed;  // index into `followed`, or -1
};

struct Followed {
  std::uint64_t ref;
  std::uint64_t prio;
  std::int64_t px;
  std::int64_t qty0;
  std::int64_t left;  // true remaining
  std::int64_t filled_true = 0;
  Qty ahead[kL2 + 1];
  Qty filled[kL2 + 1];
  std::uint16_t loc;
  Side side;
  std::uint8_t kind;
  bool alive = true;
};

struct Fill {
  std::uint64_t ts;
  std::int64_t px;
  std::int64_t qty;
  std::uint16_t loc;
  Side side;
  std::uint8_t model;
  std::uint8_t kind;
};

struct Tally {
  std::uint64_t orders = 0;
  std::uint64_t filled_true = 0;
  double qty_true = 0;
  std::uint64_t both[kL2 + 1] = {};
  std::uint64_t truth_only[kL2 + 1] = {};
  std::uint64_t model_only[kL2 + 1] = {};
  double qty_model[kL2 + 1] = {};
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s FILE [--top K] [--hidden]\n", argv[0]);
    return 2;
  }
  std::size_t top = 100;
  bool hidden = false;
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--top" && i + 1 < argc) {
      top = std::strtoull(argv[++i], nullptr, 0);
    } else if (a == "--hidden") {
      hidden = true;
    } else {
      std::fprintf(stderr, "unknown argument %s\n", a.c_str());
      return 2;
    }
  }
  const int fd = open(argv[1], O_RDONLY);
  struct stat sb {};
  if (fd < 0 || fstat(fd, &sb) != 0 || sb.st_size <= 0) {
    std::fprintf(stderr, "cannot read %s\n", argv[1]);
    return 1;
  }
  const auto size = static_cast<std::size_t>(sb.st_size);
  void* map = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (map == MAP_FAILED) {
    std::fprintf(stderr, "cannot map %s\n", argv[1]);
    return 1;
  }
  madvise(map, size, MADV_SEQUENTIAL);
  const auto* data = static_cast<const unsigned char*>(map);

  // The locates with the most added orders.
  std::vector<std::uint64_t> adds(65536, 0);
  for (std::size_t o = 0; o + 2 <= size;) {
    const std::size_t len = be(data + o, 2);
    if (len < 11 || o + 2 + len > size) break;
    const unsigned char* m = data + o + 2;
    if (m[0] == 'A' || m[0] == 'F') ++adds[be(m + 1, 2)];
    o += 2 + len;
  }
  std::vector<std::uint16_t> locs;
  for (std::size_t l = 0; l < adds.size(); ++l)
    if (adds[l] != 0) locs.push_back(static_cast<std::uint16_t>(l));
  std::sort(locs.begin(), locs.end(), [&](auto a, auto b) { return adds[a] > adds[b]; });
  if (top != 0 && locs.size() > top) locs.resize(top);
  std::vector<std::uint8_t> on(65536, 0);
  for (const std::uint16_t l : locs) on[l] = 1;

  std::unordered_map<std::uint64_t, Order> orders;
  orders.reserve(std::size_t{1} << 24);
  std::vector<std::map<std::int64_t, std::int64_t>> levels(65536 * 2);  // price -> displayed
  std::vector<std::vector<std::int32_t>> alive(65536 * 2);  // followed orders per (locate, side)
  std::vector<std::vector<std::pair<std::uint64_t, std::int64_t>>> mids(65536);
  std::vector<Followed> followed;
  std::vector<Fill> fills;
  Tally tally[kKinds];
  std::uint64_t prio = 0;

  const auto key = [](std::uint16_t loc, Side s) { return loc * 2U + static_cast<unsigned>(s); };
  const auto shown = [&](std::uint16_t loc, Side s, std::int64_t px) {
    const auto& lv = levels[key(loc, s)];
    const auto it = lv.find(px);
    return it == lv.end() ? std::int64_t{0} : it->second;
  };
  const auto best = [&](std::uint16_t loc, Side s, int rank) -> std::int64_t {
    const auto& lv = levels[key(loc, s)];
    if (static_cast<int>(lv.size()) <= rank) return -1;
    if (s == Side::Buy) return std::next(lv.rbegin(), rank)->first;
    return std::next(lv.begin(), rank)->first;
  };
  const auto level_add = [&](std::uint16_t loc, Side s, std::int64_t px, std::int64_t q) {
    auto& lv = levels[key(loc, s)];
    const std::int64_t v = (lv[px] += q);
    if (v <= 0) lv.erase(px);
  };
  const auto finish = [&](std::int32_t i) {
    Followed& f = followed[static_cast<std::size_t>(i)];
    f.alive = false;
    auto& v = alive[key(f.loc, f.side)];
    v.erase(std::find(v.begin(), v.end(), i));
    Tally& t = tally[f.kind];
    ++t.orders;
    const bool truth = f.filled_true > 0;
    t.filled_true += truth ? 1 : 0;
    t.qty_true += static_cast<double>(f.filled_true);
    for (int m = 0; m <= kL3; ++m) {
      const bool model = f.filled[m].is_positive();
      t.qty_model[m] += f.filled[m].to_double();
      if (truth && model) {
        ++t.both[m];
      } else if (truth) {
        ++t.truth_only[m];
      } else if (model) {
        ++t.model_only[m];
      }
    }
  };
  const auto model_fill = [&](Followed& f, int m, Qty q, std::uint64_t ts) {
    if (!q.is_positive()) return;
    f.filled[m] += q;
    fills.push_back(
        Fill{ts, f.px, q.raw / kFixedScale, f.loc, f.side, static_cast<std::uint8_t>(m), f.kind});
  };

  // Another order at (loc, side, px) lost `q` to a cancel.
  const auto on_cancel = [&](const Order& o, std::uint64_t ref, std::int64_t q) {
    const std::int64_t before = shown(o.loc, o.side, o.px);
    for (const std::int32_t i : alive[key(o.loc, o.side)]) {
      Followed& f = followed[static_cast<std::size_t>(i)];
      if (f.ref == ref || f.px != o.px) continue;
      // The feed shows the followed order itself; the model takes it out of the level.
      const Qty was = Qty::from_int(before - f.left);
      const Qty now = Qty::from_int(before - q - f.left);
      for (int m = 0; m < kL2; ++m)
        f.ahead[m] = queue_after_level_change(f.ahead[m], was, now, kConservatism[m]);
      if (o.prio < f.prio)
        f.ahead[kL3] = f.ahead[kL3] > Qty::from_int(q) ? f.ahead[kL3] - Qty::from_int(q) : Qty{};
    }
  };
  // An execution of `q` of order `o` (ref) printed at exec_px (printable or not).
  const auto on_execute = [&](const Order& o,
                              std::uint64_t ref,
                              std::int64_t q,
                              std::int64_t exec_px,
                              bool printable,
                              std::uint64_t ts) {
    const Side aggressor = opposite(o.side);
    for (const std::int32_t i : alive[key(o.loc, o.side)]) {
      Followed& f = followed[static_cast<std::size_t>(i)];
      if (f.ref == ref) continue;  // its own execution: below
      const Qty qq = Qty::from_int(q);
      if (!printable && f.px == o.px) {
        // No print: to an L2 feed the level just shrank.
        const std::int64_t before = shown(o.loc, o.side, o.px);
        const Qty was = Qty::from_int(before - f.left);
        const Qty now = Qty::from_int(before - q - f.left);
        for (int m = 0; m < kL2; ++m)
          f.ahead[m] = queue_after_level_change(f.ahead[m], was, now, kConservatism[m]);
      }
      if (printable) {
        for (int m = 0; m < kL2; ++m) {
          model_fill(f,
                     m,
                     queue_after_trade(f.ahead[m],
                                       f.side,
                                       px_of(f.px),
                                       Qty::from_int(f.qty0) - f.filled[m],
                                       px_of(exec_px),
                                       qq,
                                       aggressor),
                     ts);
        }
      }
      switch (
          l3_execution_reaches(f.side, px_of(f.px), f.prio, px_of(o.px), o.prio, px_of(exec_px))) {
        case L3Reach::Fills:
          f.ahead[kL3] = Qty{};
          model_fill(f, kL3, min(qq, Qty::from_int(f.qty0) - f.filled[kL3]), ts);
          break;
        case L3Reach::Ahead:
          f.ahead[kL3] = f.ahead[kL3] > qq ? f.ahead[kL3] - qq : Qty{};
          break;
        case L3Reach::None:
          break;
      }
    }
    if (o.followed < 0) return;
    // The followed order's own execution: truth, a print for the L2 rule, and the L3 rule's fill
    // (nothing can be ahead of an order that executes).
    Followed& f = followed[static_cast<std::size_t>(o.followed)];
    f.filled_true += q;
    fills.push_back(Fill{ts, f.px, q, f.loc, f.side, kTruth, f.kind});
    const Qty qq = Qty::from_int(q);
    if (printable) {
      for (int m = 0; m < kL2; ++m) {
        model_fill(f,
                   m,
                   queue_after_trade(f.ahead[m],
                                     f.side,
                                     px_of(f.px),
                                     Qty::from_int(f.qty0) - f.filled[m],
                                     px_of(exec_px),
                                     qq,
                                     aggressor),
                   ts);
      }
    }
    f.ahead[kL3] = Qty{};
    model_fill(f, kL3, min(qq, Qty::from_int(f.qty0) - f.filled[kL3]), ts);
  };
  const auto add = [&](std::uint16_t loc,
                       std::uint64_t ref,
                       Side s,
                       std::int64_t px,
                       std::int64_t q,
                       std::uint64_t ts) {
    std::int32_t idx = -1;
    const std::int64_t b = best(loc, s, 0);
    const std::int64_t opp = best(loc, opposite(s), 0);
    // The continuous session only: the crosses execute out of queue order.
    const bool continuous = ts > 34'205'000'000'000ULL && ts < 57'595'000'000'000ULL;
    int kind = -1;
    if (continuous && b >= 0 && opp >= 0) {
      if (px == b) {
        kind = 0;
      } else if (better(s, px_of(px), px_of(b)) && better(s, px_of(px), px_of(opp)) == false &&
                 px != opp) {
        kind = 1;
      } else if (px == best(loc, s, 1)) {
        kind = 2;
      }
    }
    ++prio;
    if (kind >= 0) {
      Followed f{};
      f.ref = ref;
      f.prio = prio;
      f.px = px;
      f.qty0 = f.left = q;
      f.loc = loc;
      f.side = s;
      f.kind = static_cast<std::uint8_t>(kind);
      for (Qty& a : f.ahead) a = Qty::from_int(shown(loc, s, px));
      idx = static_cast<std::int32_t>(followed.size());
      followed.push_back(f);
      alive[key(loc, s)].push_back(idx);
    }
    orders[ref] = Order{loc, s, px, q, prio, idx};
    level_add(loc, s, px, q);
  };
  // `q` (0: all) of `ref` cancelled.
  const auto cancel = [&](std::uint64_t ref, std::int64_t q) {
    const auto it = orders.find(ref);
    if (it == orders.end()) return;
    Order& o = it->second;
    if (q <= 0 || q > o.qty) q = o.qty;
    if (o.followed >= 0) {  // a cancel of its own ends a followed order's life
      finish(o.followed);
      o.followed = -1;
    }
    on_cancel(o, ref, q);
    level_add(o.loc, o.side, o.px, -q);
    o.qty -= q;
    if (o.qty == 0) orders.erase(it);
  };
  const auto execute = [&](std::uint64_t ref,
                           std::int64_t q,
                           std::int64_t exec_px,
                           bool printable,
                           std::uint64_t ts) {
    const auto it = orders.find(ref);
    if (it == orders.end()) return;
    Order& o = it->second;
    q = std::min(q, o.qty);
    on_execute(o, ref, q, exec_px < 0 ? o.px : exec_px, printable, ts);
    if (o.followed >= 0) followed[static_cast<std::size_t>(o.followed)].left -= q;
    level_add(o.loc, o.side, o.px, -q);
    o.qty -= q;
    if (o.qty == 0) {
      if (o.followed >= 0) finish(o.followed);
      orders.erase(it);
    }
  };

  for (std::size_t off = 0; off + 2 <= size;) {
    const std::size_t len = be(data + off, 2);
    if (len < 11 || off + 2 + len > size) break;
    const unsigned char* m = data + off + 2;
    off += 2 + len;
    const auto loc = static_cast<std::uint16_t>(be(m + 1, 2));
    if (on[loc] == 0) continue;
    const std::uint64_t ts = be(m + 5, 6);
    const unsigned char* p = m + 11;
    const auto i64 = [&](int at, int n) { return static_cast<std::int64_t>(be(p + at, n)); };
    switch (m[0]) {
      case 'A':
      case 'F':
        add(loc, be(p, 8), p[8] == 'B' ? Side::Buy : Side::Sell, i64(21, 4), i64(9, 4), ts);
        break;
      case 'E':
        execute(be(p, 8), i64(8, 4), -1, true, ts);
        break;
      case 'C':
        execute(be(p, 8), i64(8, 4), i64(21, 4), p[20] == 'Y', ts);
        break;
      case 'X':
        cancel(be(p, 8), i64(8, 4));
        break;
      case 'D':
        cancel(be(p, 8), 0);
        break;
      case 'U': {
        const auto it = orders.find(be(p, 8));
        if (it == orders.end()) break;
        const Side s = it->second.side;
        cancel(be(p, 8), 0);
        add(loc, be(p + 8, 8), s, i64(20, 4), i64(16, 4), ts);
        break;
      }
      case 'P':
        if (hidden) {
          // The resting side is unknown (Nasdaq sends 'B' for every P): a feed's print of it
          // reaches whichever side the model takes it to be.
          const Qty q = Qty::from_int(i64(9, 4));
          for (const Side s : {Side::Buy, Side::Sell}) {
            for (const std::int32_t i : alive[key(loc, s)]) {
              Followed& f = followed[static_cast<std::size_t>(i)];
              for (int mm = 0; mm < kL2; ++mm) {
                model_fill(f,
                           mm,
                           queue_after_trade(f.ahead[mm],
                                             f.side,
                                             px_of(f.px),
                                             Qty::from_int(f.qty0) - f.filled[mm],
                                             px_of(i64(21, 4)),
                                             q,
                                             Side::Buy),
                           ts);
              }
            }
          }
        }
        break;
      default:
        break;
    }
    const std::int64_t bb = best(loc, Side::Buy, 0);
    const std::int64_t ba = best(loc, Side::Sell, 0);
    if (bb >= 0 && ba > bb) {
      auto& v = mids[loc];
      const std::int64_t mid = bb + ba;  // twice the mid, ITCH units
      if (v.empty() || v.back().second != mid) v.emplace_back(ts, mid);
    }
  }
  for (std::size_t i = 0; i < followed.size(); ++i)
    if (followed[i].alive) finish(static_cast<std::int32_t>(i));

  std::printf("%zu locates, %zu orders followed\n", locs.size(), followed.size());
  for (int k = 0; k < kKinds; ++k) {
    const Tally& t = tally[k];
    std::printf("\n%s: %llu orders, %llu filled (%.3f), %.0f shares\n",
                kKindName[k],
                static_cast<unsigned long long>(t.orders),
                static_cast<unsigned long long>(t.filled_true),
                t.orders ? static_cast<double>(t.filled_true) / static_cast<double>(t.orders) : 0.0,
                t.qty_true);
    std::printf(
        "  model          both  truth only  model only    hit   false   error   qty/truth\n");
    for (int m = 0; m <= kL3; ++m) {
      const double hit =
          t.filled_true ? static_cast<double>(t.both[m]) / static_cast<double>(t.filled_true) : 0;
      const double unfilled = static_cast<double>(t.orders - t.filled_true);
      const double fls = unfilled > 0 ? static_cast<double>(t.model_only[m]) / unfilled : 0;
      const double either = static_cast<double>(t.both[m] + t.truth_only[m] + t.model_only[m]);
      const double err =
          either > 0 ? static_cast<double>(t.truth_only[m] + t.model_only[m]) / either : 0;
      char name[24];
      if (m < kL2) {
        std::snprintf(name, sizeof name, "l2 c=%.2f", static_cast<double>(kConservatism[m]) / 1e4);
      } else {
        std::snprintf(name, sizeof name, "l3 fifo");
      }
      std::printf("  %-10s %9llu %11llu %11llu  %6.3f  %6.3f  %6.3f  %10.4f\n",
                  name,
                  static_cast<unsigned long long>(t.both[m]),
                  static_cast<unsigned long long>(t.truth_only[m]),
                  static_cast<unsigned long long>(t.model_only[m]),
                  hit,
                  fls,
                  err,
                  t.qty_true > 0 ? t.qty_model[m] / t.qty_true : 0.0);
    }
  }

  // Markouts of each model's fills against the true ones.
  const std::uint64_t horizons[3] = {1'000'000'000ULL, 10'000'000'000ULL, 60'000'000'000ULL};
  const auto mid_at = [&](std::uint16_t loc, std::uint64_t ts) -> double {
    const auto& v = mids[loc];
    const auto it =
        std::upper_bound(v.begin(), v.end(), std::make_pair(ts, std::int64_t{INT64_MAX}));
    return it == v.begin() ? -1.0 : static_cast<double>(std::prev(it)->second) / 2.0;
  };
  double notional[kModels][kKinds] = {};
  double capture[kModels][kKinds] = {};
  double markout[kModels][kKinds][3] = {};
  std::uint64_t count[kModels][kKinds] = {};
  for (const Fill& f : fills) {
    const double m0 = mid_at(f.loc, f.ts);
    if (m0 < 0) continue;
    const double sign = f.side == Side::Buy ? 1.0 : -1.0;
    const auto q = static_cast<double>(f.qty);
    const auto px = static_cast<double>(f.px);
    notional[f.model][f.kind] += q * px;
    capture[f.model][f.kind] += sign * (m0 - px) * q;
    ++count[f.model][f.kind];
    for (int h = 0; h < 3; ++h)
      markout[f.model][f.kind][h] += sign * (mid_at(f.loc, f.ts + horizons[h]) - px) * q;
  }
  for (int k = 0; k < kKinds; ++k) {
    std::printf("\nfills of orders that %s: bps of notional\n",
                k == 0   ? "join the touch"
                : k == 1 ? "improve the touch"
                         : "join one level behind");
    std::printf("  model          fills   notional USD   capture   mo 1s  mo 10s  mo 60s\n");
    for (int m = 0; m < kModels; ++m) {
      char name[24];
      if (m < kL2) {
        std::snprintf(name, sizeof name, "l2 c=%.2f", static_cast<double>(kConservatism[m]) / 1e4);
      } else {
        std::snprintf(name, sizeof name, m == kL3 ? "l3 fifo" : "truth");
      }
      const double n = notional[m][k];
      const auto bps = [&](double v) { return n > 0 ? v / n * 1e4 : 0.0; };
      std::printf("  %-10s %9llu %14.0f   %7.3f %7.3f %7.3f %7.3f\n",
                  name,
                  static_cast<unsigned long long>(count[m][k]),
                  n / 1e4,
                  bps(capture[m][k]),
                  bps(markout[m][k][0]),
                  bps(markout[m][k][1]),
                  bps(markout[m][k][2]));
    }
  }
  munmap(map, size);
  return 0;
}
