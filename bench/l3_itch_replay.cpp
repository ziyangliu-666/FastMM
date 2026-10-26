// fastmm-bench-l3-itch: replays a TotalView-ITCH 5.0 file (2-byte big-endian length before each
// message, the layout of NASDAQ's sample files) into one L3Book per stock locate and reports the
// latency of every book message: percentiles, the worst messages, and the window recentres per
// book with their cost. --check replays again, untimed, against a std::map book per locate and
// compares the touch after every message and every level at the end.
//
//   fastmm-bench-l3-itch FILE [--window N] [--max-orders N] [--max-overflow N] [--top K]
//                        [--from HH:MM[:SS]] [--check] [--trace SYMBOL]
//
// --from applies the messages before that exchange time untimed, so the books are warm when
// timing starts (a session's open, say).
//
// --trace prints the touches and the window after every recentre of SYMBOL's book.
//
// --top K books the K locates with the most book messages (0: every locate that adds an order).
// Messages of other locates are not timed. The book uses ITCH's 0.0001 tick, so sub-penny
// orders are kept exactly.
#include "fastmm/core/book/l3_book.hpp"

#include <x86intrin.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace fastmm;

namespace {

constexpr std::int64_t kUnit = 10'000;  // ITCH price unit (0.0001) in Price raw units

std::uint64_t be(const unsigned char* p, int n) {
  std::uint64_t v = 0;
  for (int i = 0; i < n; ++i) v = (v << 8U) | p[i];
  return v;
}

struct Msg {
  std::uint64_t off;  // a day of ITCH is several GiB
  std::uint16_t len;
  std::uint16_t locate;
};

struct Book {
  std::unique_ptr<L3Book> l3;
  std::string symbol;
  std::uint64_t msgs = 0;
  std::uint64_t errors = 0;
  std::uint64_t recentre_cycles = 0;
  std::uint64_t recentre_max = 0;
  std::uint32_t recentre_msgs = 0;
  std::size_t peak_overflow = 0;  // most levels one side's overflow store held
};

// Reference book for --check.
struct RefOrder {
  std::uint16_t locate;
  Side side;
  std::int64_t px;
  std::int64_t qty;
};
struct RefBook {
  std::map<std::int64_t, std::int64_t> side[2];  // price -> qty
};

bool is_book_msg(char t) {
  return t == 'A' || t == 'F' || t == 'E' || t == 'C' || t == 'X' || t == 'D' || t == 'U';
}

L3Error apply(L3Book& b, const unsigned char* m) {
  const char t = static_cast<char>(m[0]);
  const unsigned char* p = m + 11;
  switch (t) {
    case 'A':
    case 'F':
      return b.add(be(p, 8),
                   p[8] == 'B' ? Side::Buy : Side::Sell,
                   Price::from_raw(static_cast<std::int64_t>(be(p + 21, 4)) * kUnit),
                   Qty::from_int(static_cast<std::int64_t>(be(p + 9, 4))));
    case 'E':
    case 'C':
      return b.execute(be(p, 8), Qty::from_int(static_cast<std::int64_t>(be(p + 8, 4))));
    case 'X':
      return b.cancel(be(p, 8), Qty::from_int(static_cast<std::int64_t>(be(p + 8, 4))));
    case 'D':
      return b.remove(be(p, 8));
    case 'U':
      return b.replace(be(p, 8),
                       be(p + 8, 8),
                       Price::from_raw(static_cast<std::int64_t>(be(p + 20, 4)) * kUnit),
                       Qty::from_int(static_cast<std::int64_t>(be(p + 16, 4))));
    default:
      return L3Error::None;
  }
}

void ref_reduce(std::unordered_map<std::uint64_t, RefOrder>& orders,
                std::vector<RefBook>& books,
                std::uint64_t ref,
                std::int64_t by) {
  auto it = orders.find(ref);
  if (it == orders.end()) return;
  RefOrder& o = it->second;
  if (by <= 0 || by > o.qty) by = o.qty;
  auto& lv = books[o.locate].side[static_cast<int>(o.side)];
  auto li = lv.find(o.px);
  li->second -= by;
  if (li->second == 0) lv.erase(li);
  o.qty -= by;
  if (o.qty == 0) orders.erase(it);
}

void ref_apply(std::unordered_map<std::uint64_t, RefOrder>& orders,
               std::vector<RefBook>& books,
               const unsigned char* m,
               std::uint16_t locate) {
  const char t = static_cast<char>(m[0]);
  const unsigned char* p = m + 11;
  auto add = [&](std::uint64_t ref, Side s, std::int64_t px, std::int64_t q) {
    if (q <= 0 || orders.count(ref) != 0) return;
    orders[ref] = RefOrder{locate, s, px, q};
    books[locate].side[static_cast<int>(s)][px] += q;
  };
  switch (t) {
    case 'A':
    case 'F':
      add(be(p, 8),
          p[8] == 'B' ? Side::Buy : Side::Sell,
          static_cast<std::int64_t>(be(p + 21, 4)),
          static_cast<std::int64_t>(be(p + 9, 4)));
      break;
    case 'E':
    case 'C':
    case 'X':
      ref_reduce(orders, books, be(p, 8), static_cast<std::int64_t>(be(p + 8, 4)));
      break;
    case 'D':
      ref_reduce(orders, books, be(p, 8), 0);
      break;
    case 'U': {
      auto it = orders.find(be(p, 8));
      if (it == orders.end()) break;
      const Side s = it->second.side;
      ref_reduce(orders, books, be(p, 8), 0);
      add(be(p + 8, 8),
          s,
          static_cast<std::int64_t>(be(p + 20, 4)),
          static_cast<std::int64_t>(be(p + 16, 4)));
      break;
    }
    default:
      break;
  }
}

double cycles_per_ns() {
  using clk = std::chrono::steady_clock;
  const auto t0 = clk::now();
  const std::uint64_t c0 = __rdtsc();
  while (clk::now() - t0 < std::chrono::milliseconds(200)) {
  }
  const std::uint64_t c1 = __rdtsc();
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - t0).count();
  return static_cast<double>(c1 - c0) / static_cast<double>(ns);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: %s FILE [--window N] [--max-orders N] [--max-overflow N] [--top K] "
                 "[--from HH:MM[:SS]] [--check] [--trace SYMBOL]\n",
                 argv[0]);
    return 2;
  }
  L3BookConfig cfg{.max_orders = std::size_t{1} << 16};
  std::size_t top = 100;
  bool check = false;
  std::string trace;
  std::uint64_t from_ns = 0;
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    auto num = [&] { return static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 0)); };
    if (a == "--window" && i + 1 < argc) {
      cfg.price_window_ticks = num();
    } else if (a == "--max-orders" && i + 1 < argc) {
      cfg.max_orders = num();
    } else if (a == "--max-overflow" && i + 1 < argc) {
      cfg.max_overflow_levels = num();
    } else if (a == "--top" && i + 1 < argc) {
      top = num();
    } else if (a == "--from" && i + 1 < argc) {
      unsigned h = 0;
      unsigned m = 0;
      unsigned sec = 0;
      if (std::sscanf(argv[++i], "%u:%u:%u", &h, &m, &sec) < 2) {
        std::fprintf(stderr, "--from takes HH:MM[:SS]\n");
        return 2;
      }
      from_ns = ((h * 60ULL + m) * 60ULL + sec) * 1'000'000'000ULL;
    } else if (a == "--trace" && i + 1 < argc) {
      trace = argv[++i];
    } else if (a == "--check") {
      check = true;
    } else {
      std::fprintf(stderr, "unknown argument %s\n", a.c_str());
      return 2;
    }
  }

  std::ifstream in(argv[1], std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "cannot open %s\n", argv[1]);
    return 1;
  }
  const std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
  std::vector<Msg> msgs;
  std::vector<std::string> symbol(65536);
  std::vector<std::uint64_t> per_locate(65536, 0);
  std::size_t total = 0;
  for (std::size_t o = 0; o + 2 <= data.size();) {
    const auto len = static_cast<std::uint16_t>(be(&data[o], 2));
    if (len < 11 || o + 2 + len > data.size()) break;
    const unsigned char* m = &data[o + 2];
    const auto locate = static_cast<std::uint16_t>(be(m + 1, 2));
    ++total;
    if (m[0] == 'R') symbol[locate].assign(reinterpret_cast<const char*>(m + 11), 8);
    if (is_book_msg(static_cast<char>(m[0]))) {
      msgs.push_back(Msg{o + 2, len, locate});
      ++per_locate[locate];
    }
    o += 2U + len;
  }

  std::vector<std::uint16_t> order;
  for (std::size_t l = 0; l < 65536; ++l)
    if (per_locate[l] != 0) order.push_back(static_cast<std::uint16_t>(l));
  std::sort(
      order.begin(), order.end(), [&](auto a, auto b) { return per_locate[a] > per_locate[b]; });
  if (top != 0 && order.size() > top) order.resize(top);
  std::vector<Book> books(65536);
  for (const std::uint16_t l : order) {
    books[l].l3 = std::make_unique<L3Book>(Price::from_raw(kUnit), cfg);
    books[l].symbol = symbol[l];
    while (!books[l].symbol.empty() && books[l].symbol.back() == ' ') books[l].symbol.pop_back();
  }

  // Timed replay.
  const double cpn = cycles_per_ns();
  std::vector<std::uint32_t> lat;
  lat.reserve(msgs.size());
  std::uint64_t booked = 0;
  std::uint64_t by_kind[8] = {};  // book errors by L3Error
  auto count_error = [&](Book& bk, L3Error e) {
    if (e == L3Error::None) return;
    ++bk.errors;
    ++by_kind[static_cast<std::size_t>(e) & 7U];
  };
  for (const Msg& msg : msgs) {
    Book& bk = books[msg.locate];
    if (!bk.l3) continue;
    const unsigned char* m = &data[msg.off];
    auto note_overflow = [&] {
      bk.peak_overflow = std::max({bk.peak_overflow,
                                   bk.l3->overflow_levels(Side::Buy),
                                   bk.l3->overflow_levels(Side::Sell)});
    };
    if (be(m + 5, 6) < from_ns) {  // ITCH timestamp: nanoseconds since midnight
      count_error(bk, apply(*bk.l3, m));
      note_overflow();
      continue;
    }
    const std::uint64_t r0 = bk.l3->recentre_count();
    _mm_lfence();
    const std::uint64_t c0 = __rdtsc();
    const L3Error e = apply(*bk.l3, m);
    _mm_lfence();
    const std::uint64_t c = __rdtsc() - c0;
    lat.push_back(static_cast<std::uint32_t>(std::min<std::uint64_t>(c, UINT32_MAX)));
    ++bk.msgs;
    ++booked;
    count_error(bk, e);
    note_overflow();
    if (bk.l3->recentre_count() != r0) {
      ++bk.recentre_msgs;
      bk.recentre_cycles += c;
      bk.recentre_max = std::max(bk.recentre_max, c);
      if (!trace.empty() && bk.symbol == trace)
        std::printf("recentre after %c: bid %lld ask %lld window_low %lld overflow %zu/%zu\n",
                    m[0],
                    static_cast<long long>(bk.l3->best_bid().price.raw / kUnit),
                    static_cast<long long>(bk.l3->best_ask().price.raw / kUnit),
                    static_cast<long long>(bk.l3->window_low().raw / kUnit),
                    bk.l3->overflow_levels(Side::Buy),
                    bk.l3->overflow_levels(Side::Sell));
    }
  }

  auto ns = [&](double c) { return c / cpn; };
  std::vector<std::uint32_t> sorted = lat;
  std::sort(sorted.begin(), sorted.end());
  auto pct = [&](double q) {
    if (sorted.empty()) return 0.0;
    return ns(sorted[std::min(sorted.size() - 1,
                              static_cast<std::size_t>(q * static_cast<double>(sorted.size())))]);
  };
  double sum = 0;
  for (const auto c : lat) sum += c;
  std::uint64_t recentres = 0;
  std::uint64_t failures = 0;
  std::uint64_t errors = 0;
  std::uint64_t resting = 0;
  std::uint64_t rc_cycles = 0;
  for (const std::uint16_t l : order) {
    recentres += books[l].l3->recentre_count();
    failures += books[l].l3->recentre_failures();
    errors += books[l].errors;
    resting += books[l].l3->order_count();
    rc_cycles += books[l].recentre_cycles;
  }
  std::printf("file messages %zu, book messages %zu, timed %llu over %zu books\n",
              total,
              msgs.size(),
              static_cast<unsigned long long>(booked),
              order.size());
  std::printf("window %zu ticks, max_orders %zu, max_overflow_levels %zu\n",
              cfg.price_window_ticks,
              cfg.max_orders,
              cfg.max_overflow_levels);
  std::printf("ns/msg mean %.1f  p50 %.1f  p90 %.1f  p99 %.1f  p99.9 %.1f  p99.99 %.1f  max %.1f\n",
              ns(sum / static_cast<double>(lat.size())),
              pct(0.5),
              pct(0.9),
              pct(0.99),
              pct(0.999),
              pct(0.9999),
              sorted.empty() ? 0.0 : ns(sorted.back()));
  std::printf("resting orders at the end %llu\n", static_cast<unsigned long long>(resting));
  std::printf("recentres %llu (failures %llu), %.1f%% of the time; book errors %llu\n",
              static_cast<unsigned long long>(recentres),
              static_cast<unsigned long long>(failures),
              100.0 * static_cast<double>(rc_cycles) / sum,
              static_cast<unsigned long long>(errors));
  if (errors != 0) {
    static constexpr const char* kKind[8] = {"",
                                             "duplicate ref",
                                             "unknown ref",
                                             "pool full",
                                             "out of window",
                                             "off tick",
                                             "bad qty",
                                             ""};
    std::printf("  errors by kind:");
    for (std::size_t k = 1; k < 7; ++k)
      if (by_kind[k] != 0)
        std::printf(" %s %llu", kKind[k], static_cast<unsigned long long>(by_kind[k]));
    std::printf("\n");
  }
  std::vector<std::uint16_t> by_rc = order;
  std::sort(by_rc.begin(), by_rc.end(), [&](auto a, auto b) {
    return books[a].l3->recentre_count() > books[b].l3->recentre_count();
  });
  for (std::size_t i = 0; i < std::min<std::size_t>(5, by_rc.size()); ++i) {
    const Book& b = books[by_rc[i]];
    if (b.recentre_msgs == 0) break;
    std::printf("  %-8s msgs %8llu  recentres %5llu  mean %.1f ns  max %.1f ns\n",
                b.symbol.c_str(),
                static_cast<unsigned long long>(b.msgs),
                static_cast<unsigned long long>(b.l3->recentre_count()),
                ns(static_cast<double>(b.recentre_cycles) / b.recentre_msgs),
                ns(static_cast<double>(b.recentre_max)));
  }

  // The books whose overflow store came closest to max_overflow_levels: the capacity to size.
  std::vector<std::uint16_t> by_ov = order;
  std::sort(by_ov.begin(), by_ov.end(), [&](auto a, auto b) {
    return books[a].peak_overflow > books[b].peak_overflow;
  });
  std::printf("peak overflow levels per side:");
  for (std::size_t i = 0; i < std::min<std::size_t>(5, by_ov.size()); ++i)
    std::printf(" %s %zu", books[by_ov[i]].symbol.c_str(), books[by_ov[i]].peak_overflow);
  std::printf("\n");

  if (!check) return errors == 0 ? 0 : 1;

  // Untimed differential replay against std::map books.
  std::vector<Book> fresh(65536);
  for (const std::uint16_t l : order)
    fresh[l].l3 = std::make_unique<L3Book>(Price::from_raw(kUnit), cfg);
  std::unordered_map<std::uint64_t, RefOrder> ref_orders;
  std::vector<RefBook> ref_books(65536);
  std::uint64_t mismatches = 0;
  auto touch = [](const RefBook& r, Side s, std::int64_t& px, std::int64_t& q) {
    const auto& m = r.side[static_cast<int>(s)];
    if (m.empty()) {
      px = 0;
      q = 0;
      return;
    }
    const auto it = s == Side::Buy ? std::prev(m.end()) : m.begin();
    px = it->first;
    q = it->second;
  };
  for (const Msg& msg : msgs) {
    if (!fresh[msg.locate].l3) continue;
    const unsigned char* m = &data[msg.off];
    L3Book& b = *fresh[msg.locate].l3;
    apply(b, m);
    ref_apply(ref_orders, ref_books, m, msg.locate);
    for (const Side s : {Side::Buy, Side::Sell}) {
      std::int64_t px = 0;
      std::int64_t q = 0;
      touch(ref_books[msg.locate], s, px, q);
      const Level l = s == Side::Buy ? b.best_bid() : b.best_ask();
      if (l.price.raw != px * kUnit || l.qty.raw != Qty::from_int(q).raw) {
        if (mismatches++ < 5)
          std::printf("touch mismatch %s side %d: book %lld ref %lld\n",
                      symbol[msg.locate].c_str(),
                      static_cast<int>(s),
                      static_cast<long long>(l.price.raw),
                      static_cast<long long>(px * kUnit));
      }
    }
  }
  std::size_t levels = 0;
  for (const std::uint16_t l : order) {
    for (const Side s : {Side::Buy, Side::Sell}) {
      const auto& ref = ref_books[l].side[static_cast<int>(s)];
      std::vector<Level> got;
      fresh[l].l3->for_each_level(s, [&](const Level& lv) { got.push_back(lv); });
      std::vector<Level> want;
      if (s == Side::Buy) {
        for (auto it = ref.rbegin(); it != ref.rend(); ++it)
          want.push_back(Level{Price::from_raw(it->first * kUnit), Qty::from_int(it->second)});
      } else {
        for (const auto& [p, q] : ref)
          want.push_back(Level{Price::from_raw(p * kUnit), Qty::from_int(q)});
      }
      levels += want.size();
      bool same = got.size() == want.size();
      for (std::size_t i = 0; same && i < got.size(); ++i)
        same = got[i].price == want[i].price && got[i].qty == want[i].qty;
      if (!same && mismatches++ < 10) std::printf("final levels differ: %s\n", symbol[l].c_str());
    }
  }
  std::printf("check: %llu mismatches, %zu levels, %zu resting orders\n",
              static_cast<unsigned long long>(mismatches),
              levels,
              ref_orders.size());
  return mismatches == 0 && errors == 0 ? 0 : 1;
}
