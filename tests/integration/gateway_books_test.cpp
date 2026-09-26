// The books a strategy behind fastmm-gateway starts from: an attaching strategy, and one whose
// market-data ring overflowed, get snapshots of the gateway's own copy of each book, so the venue
// is not asked to resync and no other strategy's books pause. Strategy "a" trades BTCUSDT and
// "b" BTCUSDC on the simulator; each receives both books. Everything is a real child process.
#include "gateway_util.hpp"

#include "fastmm/core/book/l2_book.hpp"
#include "fastmm/core/journal.hpp"
#include "fastmm/core/status_segment.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace fastmm;
using namespace fastmm::integration;

#if defined(FASTMM_LIVE_EXE) && defined(FASTMM_GATEWAY_EXE)

namespace {

constexpr InstrumentId kUsdt{0};  // the gateway's table: a's
constexpr InstrumentId kUsdc{1};  // b's

GatewayProcess spawn_gateway_status(const SessionFiles& f, const std::string& status) {
  GatewayProcess g;
  g.socket = f.config + ".gw";
  g.log = f.config + ".gw.log";
  remove_all_of({g.socket, g.log, status});
  g.pid = spawn_process(
      FASTMM_GATEWAY_EXE,
      {"--config", f.config, "--socket", g.socket, "--log", g.log, "--status", status});
  return g;
}

// Samples the gateway's status file until stopped: whether its venue's books were ever not all
// synced (the connector resyncing them), and what each attachment dropped.
class BooksWatch {
 public:
  explicit BooksWatch(std::string path) : path_(std::move(path)) {
    thread_ = std::thread([this] { run(); });
  }
  BooksWatch(const BooksWatch&) = delete;
  BooksWatch& operator=(const BooksWatch&) = delete;
  ~BooksWatch() { stop(); }
  void stop() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
  }
  [[nodiscard]] int samples() const { return samples_.load(); }
  [[nodiscard]] int unsynced() const { return unsynced_.load(); }
  // md_dropped of the attachment of this pid, in the last sample.
  [[nodiscard]] std::uint64_t dropped(pid_t pid) const {
    const std::lock_guard<std::mutex> l(mu_);
    const auto it = dropped_.find(pid);
    return it == dropped_.end() ? 0 : it->second;
  }

 private:
  void run() {
    StatusReader r;
    std::string err;
    while (!stop_.load()) {
      StatusSnapshot s;
      if (!r.is_open()) static_cast<void>(r.open(path_, &err));
      if (r.is_open() && r.read(s) && s.venue_count == 1 && s.venues[0].books_total > 0) {
        ++samples_;
        if (s.venues[0].books_synced < s.venues[0].books_total) ++unsynced_;
        const std::lock_guard<std::mutex> l(mu_);
        for (std::uint32_t k = 0; k < s.gateway.attachment_count; ++k) {
          const StatusAttachment& a = s.gateway.attachments[k];
          dropped_[static_cast<pid_t>(a.pid)] = a.md_dropped;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  std::string path_;
  std::atomic<bool> stop_{false};
  std::atomic<int> samples_{0};
  std::atomic<int> unsynced_{0};
  mutable std::mutex mu_;
  std::map<pid_t, std::uint64_t> dropped_;
  std::thread thread_;
};

// Every book of the gateway's venue synced in its status (its attaches' resyncs over, before).
void wait_books_synced(const std::string& path, const GatewayProcess& g) {
  REQUIRE_MESSAGE(wait_until(
                      [&] {
                        StatusReader r;
                        std::string err;
                        StatusSnapshot s;
                        return r.open(path, &err) && r.read(s) && s.venue_count == 1 &&
                               s.venues[0].books_total > 0 &&
                               s.venues[0].books_synced == s.venues[0].books_total;
                      },
                      10000),
                  "the gateway's books never synced: " << fastmm::test::read_file(g.log));
}

// One journaled event, 8-byte aligned.
using Event = std::vector<std::uint64_t>;
const EventHeader& hdr(const Event& e) {
  return *reinterpret_cast<const EventHeader*>(e.data());
}
const BookDeltaMsg& book_msg(const Event& e) {
  return *reinterpret_cast<const BookDeltaMsg*>(e.data());
}
bool is_book(const Event& e) {
  return hdr(e).type == EventType::BookDelta || hdr(e).type == EventType::BookSnapshot;
}

// The book events and the market-data channel's non-live states one engine consumed, in order.
std::vector<Event> read_md(const std::string& path) {
  std::vector<Event> out;
  JournalReader r;
  REQUIRE_MESSAGE(r.open(path).has_value(), "cannot open " << path);
  r.for_each([&](const EventHeader* h) {
    if ((h->flags & EventHeader::kOutbound) != 0) return;
    const bool book = h->type == EventType::BookDelta || h->type == EventType::BookSnapshot;
    const bool down = h->type == EventType::ConnectionState &&
                      msg_cast<ConnectionStateMsg>(h).channel == 0 &&
                      msg_cast<ConnectionStateMsg>(h).state != ConnState::Live;
    if (!book && !down) return;
    Event e((h->len + 7) / 8);
    std::memcpy(e.data(), h, h->len);
    out.push_back(std::move(e));
  });
  return out;
}

std::vector<std::string> journals(const SessionFiles& f) {
  std::vector<std::string> out;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(f.journal_dir, ec)) {
    if (e.path().extension() == ".fmj") out.push_back(e.path().string());
  }
  std::sort(out.begin(), out.end());
  return out;
}

using Book = L2Book<256>;  // Engine::Book

bool same(const Book& x, const Book& y) {
  if (x.seq() != y.seq() || x.has_snapshot() != y.has_snapshot()) return false;
  for (const Side s : {Side::Buy, Side::Sell}) {
    const auto& a = x.raw(s);
    const auto& b = y.raw(s);
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
      if (a[i].price != b[i].price || a[i].qty != b[i].qty) return false;
    }
  }
  return true;
}

// b's book of `inst` from its first snapshot at or after event `from`: the gateway's (synthetic),
// the same as a's book at that update id, then the same deltas as a's and the same book after
// each. Returns the deltas compared; `why` says where it stopped short.
std::size_t same_books(const std::vector<Event>& a,
                       const std::vector<Event>& b,
                       InstrumentId inst,
                       std::size_t from,
                       std::string& why) {
  std::size_t j = from;
  while (j < b.size() &&
         (hdr(b[j]).type != EventType::BookSnapshot || hdr(b[j]).instrument != inst))
    ++j;
  if (j == b.size()) {
    why = "no snapshot";
    return 0;
  }
  const BookDeltaMsg& snap = book_msg(b[j]);
  if ((snap.hdr.flags & EventHeader::kSynthetic) == 0) {
    why = "its snapshot is not the gateway's";
    return 0;
  }
  Book bb;
  bb.apply_delta(snap);
  Book ab;
  std::size_t i = 0;
  bool found = false;
  for (; i < a.size() && !found; ++i) {
    if (!is_book(a[i])) {
      ab.clear();  // as the engine does
      continue;
    }
    if (hdr(a[i]).instrument != inst) continue;
    ab.apply_delta(book_msg(a[i]));
    found = ab.has_snapshot() && ab.seq() == snap.last_update_id;
  }
  if (!found) {
    why = "a never had update id " + std::to_string(snap.last_update_id);
    return 0;
  }
  if (!same(ab, bb)) {
    why = "the snapshot is not a's book at update id " + std::to_string(snap.last_update_id);
    return 0;
  }
  std::size_t n = 0;
  for (++j; j < b.size(); ++j) {
    if (!is_book(b[j])) {
      why = "b's books were cleared after its snapshot";
      return n;
    }
    if (hdr(b[j]).instrument != inst) continue;
    while (i < a.size() && is_book(a[i]) && hdr(a[i]).instrument != inst) ++i;
    if (i == a.size()) return n;  // a stopped first
    if (!is_book(a[i])) {
      why = "a's books were cleared";
      return n;
    }
    const BookDeltaMsg& da = book_msg(a[i++]);
    const BookDeltaMsg& db = book_msg(b[j]);
    if (da.last_update_id != db.last_update_id || da.is_snapshot() != db.is_snapshot()) {
      why = "b got update id " + std::to_string(db.last_update_id) + " where a got " +
            std::to_string(da.last_update_id);
      return n;
    }
    ab.apply_delta(da);
    bb.apply_delta(db);
    if (!same(ab, bb)) {
      why = "the books differ after update id " + std::to_string(db.last_update_id);
      return n;
    }
    ++n;
  }
  return n;
}

// a's side of it: one snapshot per book (its own attach), no non-live state, and its BTCUSDT book
// never went a second without an update.
void check_a_never_paused(const std::vector<Event>& a) {
  std::map<std::uint32_t, int> snapshots;
  std::int64_t last_ns = 0;
  std::int64_t max_gap_ns = 0;
  int states = 0;
  for (const Event& e : a) {
    if (!is_book(e)) {
      ++states;
      continue;
    }
    if (hdr(e).type == EventType::BookSnapshot) ++snapshots[hdr(e).instrument.value];
    if (hdr(e).instrument != kUsdt) continue;
    const std::int64_t t = hdr(e).recv_ts.ns;
    if (last_ns != 0) max_gap_ns = std::max(max_gap_ns, t - last_ns);
    last_ns = t;
  }
  CHECK(states == 0);
  CHECK(snapshots[kUsdt.value] == 1);
  CHECK(snapshots[kUsdc.value] == 1);
  MESSAGE("a's longest time without a BTCUSDT book update: " << max_gap_ns / 1'000'000 << " ms");
  CHECK(max_gap_ns < 1'000'000'000);
}

}  // namespace

TEST_CASE(
    "gateway books: a strategy that attaches starts from the gateway's books, the venue is not "
    "asked for a snapshot and the other strategy's books never pause") {
  ServerFixture fx(two_markets());
  const Configs c = write_configs(fx, "gw-books");
  const std::string status = tmp_path("gw-books.gw.status");
  const GatewayProcess g = spawn_gateway_status(c.gw, status);
  wait_gateway_up(fx, g);

  const pid_t a = spawn_strategy(c.a, g);
  const std::uint16_t ea = wait_resting(fx, c.a, {});
  wait_books_synced(status, g);
  BooksWatch watch(status);
  REQUIRE(wait_until([&] { return watch.samples() > 0; }, 5000));
  const std::uint64_t snapshots = fx.server.stats().depth_snapshots;

  // Two attaches of b within two seconds: each was a venue resync, and the second one's snapshot
  // request waited out the connector's 2 s interval with every book frozen.
  const pid_t b1 = spawn_strategy(c.b, g);
  const std::uint16_t eb1 = wait_resting(fx, c.b, {ea});
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  stop_strategy(b1);
  const pid_t b2 = spawn_strategy(c.b, g);
  const std::uint16_t eb2 = wait_resting(fx, c.b, {ea, eb1});

  // The simulator's BTCUSDT book while b2 runs, to find in b2's journal.
  struct Top {
    std::uint64_t id;
    Level bid;
    Level ask;
  };
  std::vector<Top> tops;
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < until) {
    const sim::server::SimServerStats s = fx.server.stats();
    tops.push_back(Top{s.last_update_id, s.best_bid, s.best_ask});
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  watch.stop();
  CHECK(fx.server.stats().depth_snapshots == snapshots);
  INFO("gateway status samples " << watch.samples());
  CHECK(watch.unsynced() == 0);

  stop_strategy(b2);
  stop_strategy(a);
  stop_gateway(g);

  const std::vector<std::string> ja = journals(c.a);
  REQUIRE(ja.size() == 1);
  const std::vector<Event> ma = read_md(ja[0]);
  check_a_never_paused(ma);

  const std::vector<std::string> jb = journals(c.b);
  REQUIRE(jb.size() == 2);
  std::string b2_journal;
  for (const std::string& path : jb) {
    if (read_journal_epochs(path).epoch == eb2) b2_journal = path;
    INFO(path);
    const std::vector<Event> mb = read_md(path);
    for (const InstrumentId inst : {kUsdt, kUsdc}) {
      INFO("instrument " << inst.value);
      std::string why;
      const std::size_t n = same_books(ma, mb, inst, 0, why);
      MESSAGE(path << " instrument " << inst.value << ": " << n << " deltas as a's " << why);
      CHECK(why.empty());
      CHECK(n >= 3);
    }
  }

  // b2's BTCUSDT book is the simulator's at the update ids sampled.
  REQUIRE(!b2_journal.empty());
  const std::vector<Event> mb2 = read_md(b2_journal);
  std::map<std::uint64_t, std::pair<Level, Level>> b2_tops;
  Book bb;
  for (const Event& e : mb2) {
    if (!is_book(e)) {
      bb.clear();
      continue;
    }
    if (hdr(e).instrument != kUsdt) continue;
    bb.apply_delta(book_msg(e));
    if (bb.is_valid()) b2_tops[bb.seq()] = {bb.best_bid(), bb.best_ask()};
  }
  std::size_t matched = 0;
  for (const Top& t : tops) {
    const auto it = b2_tops.find(t.id);
    if (it == b2_tops.end()) continue;
    ++matched;
    INFO("update id " << t.id);
    CHECK(it->second.first.price == t.bid.price);
    CHECK(it->second.first.qty == t.bid.qty);
    CHECK(it->second.second.price == t.ask.price);
    CHECK(it->second.second.qty == t.ask.qty);
  }
  MESSAGE(matched << " of " << tops.size() << " simulator samples found in b2's journal");
  CHECK(matched > 0);
}

TEST_CASE(
    "gateway books: a strategy whose market-data ring overflowed gets the gateway's books back, "
    "and the other strategy's books never pause") {
  ServerFixture fx(two_markets());
  // The smallest md ring (64 KiB): a stopped strategy overflows it in seconds.
  const Configs c = write_configs(fx, "gw-books-lag");
  rewrite(c.gw.config, [](std::string& t) {
    replace_first(t, "md_ring_bytes = 4194304", "md_ring_bytes = 65536");
  });
  const std::string status = tmp_path("gw-books-lag.gw.status");
  const GatewayProcess g = spawn_gateway_status(c.gw, status);
  wait_gateway_up(fx, g);

  const pid_t a = spawn_strategy(c.a, g);
  const std::uint16_t ea = wait_resting(fx, c.a, {});
  const pid_t b = spawn_strategy(c.b, g);
  const std::uint16_t eb = wait_resting(fx, c.b, {ea});
  wait_books_synced(status, g);
  BooksWatch watch(status);
  REQUIRE(wait_until([&] { return watch.samples() > 0; }, 5000));
  const std::uint64_t snapshots = fx.server.stats().depth_snapshots;

  // b stops reading: its ring fills and drops. Then it reads again.
  REQUIRE(::kill(b, SIGSTOP) == 0);
  const bool dropped = wait_until([&] { return watch.dropped(b) > 0; }, 60000);
  REQUIRE(::kill(b, SIGCONT) == 0);
  REQUIRE_MESSAGE(dropped, "b's ring never dropped: " << fastmm::test::read_file(g.log));
  MESSAGE("b's ring dropped " << watch.dropped(b) << " events");
  // It trades again: its quotes are at the venue once more after its books came back.
  CHECK(wait_until([&] { return open_of(fx, eb) > 0; }, 20000));
  std::this_thread::sleep_for(std::chrono::seconds(3));
  watch.stop();
  CHECK(fx.server.stats().depth_snapshots == snapshots);
  INFO("gateway status samples " << watch.samples());
  CHECK(watch.unsynced() == 0);

  stop_strategy(b);
  stop_strategy(a);
  stop_gateway(g);

  const std::vector<std::string> ja = journals(c.a);
  REQUIRE(ja.size() == 1);
  const std::vector<Event> ma = read_md(ja[0]);
  check_a_never_paused(ma);

  // b: its attach's snapshots, the drop, the gateway's Resyncing, the gateway's snapshots again,
  // then a's deltas.
  const std::vector<std::string> jb = journals(c.b);
  REQUIRE(jb.size() == 1);
  const std::vector<Event> mb = read_md(jb[0]);
  std::size_t resync = mb.size();
  int states = 0;
  for (std::size_t k = 0; k < mb.size(); ++k) {
    if (is_book(mb[k])) continue;
    ++states;
    CHECK(msg_cast<ConnectionStateMsg>(&hdr(mb[k])).state == ConnState::Resyncing);
    if (resync == mb.size()) resync = k;
  }
  CHECK(states == 1);
  REQUIRE(resync < mb.size());
  for (const InstrumentId inst : {kUsdt, kUsdc}) {
    INFO("instrument " << inst.value);
    std::string why;
    const std::size_t before = same_books(ma, mb, inst, 0, why);
    MESSAGE("b before its ring dropped, instrument " << inst.value << ": " << before
                                                     << " deltas as a's; then " << why);
    why.clear();
    const std::size_t after = same_books(ma, mb, inst, resync + 1, why);
    MESSAGE("b after, instrument " << inst.value << ": " << after << " deltas as a's " << why);
    CHECK(why.empty());
    CHECK(after >= 5);
  }
}

#endif  // FASTMM_LIVE_EXE && FASTMM_GATEWAY_EXE
