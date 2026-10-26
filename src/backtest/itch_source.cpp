#include "fastmm/backtest/itch_source.hpp"

#include "fastmm/backtest/binance_source.hpp"
#include "fastmm/codecs/itch/itch_decoder.hpp"
#include "fastmm/codecs/itch/itch_l2_bridge.hpp"
#include "fastmm/codecs/itch/nasdaq_fields.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/msg_ring.hpp"
#include "fastmm/venues/event_sink.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <stdexcept>
#include <string>

namespace fastmm::bt {

namespace {

[[noreturn]] void fail(const std::string& what) {
  throw std::runtime_error("itch: " + what);
}

std::uint64_t be(const unsigned char* p, int n) noexcept {
  std::uint64_t v = 0;
  for (int i = 0; i < n; ++i) v = (v << 8U) | p[i];
  return v;
}

bool is_order_message(unsigned char t) noexcept {
  return t == 'A' || t == 'F' || t == 'E' || t == 'C' || t == 'X' || t == 'D' || t == 'U';
}

constexpr std::int64_t kNsPerDay = 86'400'000'000'000;

// The trading day of a NASDAQ file name (MMDDYYYY.NASDAQ_ITCH50), or empty.
std::string date_from_name(const std::string& path) {
  const std::size_t slash = path.find_last_of('/');
  const std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
  if (base.size() < 8) return {};
  for (std::size_t i = 0; i < 8; ++i)
    if (base[i] < '0' || base[i] > '9') return {};
  return base.substr(4, 4) + "-" + base.substr(0, 2) + "-" + base.substr(2, 2);
}

}  // namespace

Timestamp new_york_midnight(std::string_view date) {
  // Noon UTC falls on the same calendar day in New York.
  const std::int64_t noon = parse_utc_time("", date).ns + kNsPerDay / 2;
  return Timestamp{codecs::nasdaq::eastern_midnight(noon)};
}

std::int64_t parse_time_of_day(std::string_view s) {
  return parse_utc_time(s, "1970-01-01").ns;
}

struct ItchSource::State {
  State(const ItchSourceConfig& cfg, Timestamp midnight)
      : ring(std::size_t{1} << 24),
        sink(&ring, venues::SinkPolicy::Drop, 0),
        bridge(
            sink,
            codecs::itch::ItchL2BridgeConfig{.venue = cfg.venue,
                                             .depth = cfg.depth,
                                             .book = L3BookConfig{.max_orders = cfg.max_orders}}),
        orders(cfg.venue) {
    bridge.set_midnight(midnight);
    orders.set_midnight(midnight);
    for (const auto& [symbol, id] : cfg.symbols) {
      if (!bridge.add_instrument(symbol, id)) fail("cannot follow symbol '" + symbol + "'");
      if (!orders.add_symbol(symbol, id)) fail("cannot follow symbol '" + symbol + "'");
    }
  }
  MsgRing ring;
  venues::EventSink sink;
  codecs::itch::ItchL2Bridge bridge;
  codecs::itch::ItchDecoder orders;  // the order-by-order events the venue takes
  EventBuf out;
  std::vector<OrderAddL3Msg> resting;  // the queues at `start`, yielded before anything else
  std::size_t resting_at = 0;
  std::size_t off = 0;
  std::uint64_t seq = 0;
  std::int64_t group_ts = -1;  // ITCH timestamp of the messages since the last end_datagram()
  bool open = false;           // the books are being yielded
  bool done = false;
};

ItchSource::ItchSource(ItchSourceConfig cfg) : cfg_(std::move(cfg)) {
  if (cfg_.symbols.empty()) fail("no instrument of the configuration is on the source's venue");
  if (cfg_.depth < 1 || cfg_.depth > codecs::itch::ItchL2Bridge::kMaxDepth)
    fail("depth must be 1 to 256");
  if (cfg_.date.empty()) cfg_.date = date_from_name(cfg_.path);
  if (cfg_.date.empty()) fail("date=<YYYY-MM-DD> is required: the file name does not give it");
  midnight_ = new_york_midnight(cfg_.date);
  const int fd = ::open(cfg_.path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) fail("cannot open " + cfg_.path);
  struct stat sb {};
  if (::fstat(fd, &sb) != 0 || sb.st_size <= 0) {
    ::close(fd);
    fail(cfg_.path + " is empty");
  }
  size_ = static_cast<std::size_t>(sb.st_size);
  void* p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
  ::close(fd);
  if (p == MAP_FAILED) fail("cannot map " + cfg_.path);
  data_ = static_cast<const unsigned char*>(p);
  ::madvise(p, size_, MADV_SEQUENTIAL);
  // The first message's time, or `start`.
  std::int64_t first = cfg_.start_ns;
  if (first < 0 && size_ >= 13) first = static_cast<std::int64_t>(be(data_ + 2 + 5, 6));
  first_ts_ = Timestamp{midnight_.ns + (first < 0 ? 0 : first)};
  begin();
}

ItchSource::~ItchSource() {
  if (data_ != nullptr) ::munmap(const_cast<unsigned char*>(data_), size_);
}

void ItchSource::begin() {
  st_ = std::make_unique<State>(cfg_, midnight_);
}

void ItchSource::reset() {
  begin();
}

void ItchSource::open_books(std::int64_t ts) {
  State& s = *st_;
  const Timestamp at{midnight_.ns + ts};
  s.resting.clear();
  s.resting_at = 0;
  for (const auto& [symbol, id] : cfg_.symbols) {
    const L3Book* book = s.bridge.book(id);
    if (book == nullptr) continue;
    for (const Side side : {Side::Buy, Side::Sell}) {
      book->for_each_level(side, [&](const Level& l) {
        book->for_each_order_at(side, l.price, [&](const L3Order& o) {
          OrderAddL3Msg m{};
          init_header(m, EventType::OrderAddL3, id, cfg_.venue);
          m.hdr.exch_ts = at;
          m.hdr.recv_ts = at;
          m.order_ref = o.ref;
          m.price = o.price;
          m.qty = o.qty;
          m.side = o.side;
          s.resting.push_back(m);
        });
        return true;
      });
    }
  }
  s.bridge.set_stamp(codecs::itch::DatagramStamp{Cycles{}, at});
  if (!s.bridge.mark_all_complete()) fail("no room for the books' snapshots");
  s.open = true;
}

bool ItchSource::step() {
  State& s = *st_;
  if (s.off + 2 > size_) return false;
  const std::size_t len = be(data_ + s.off, 2);
  if (len < 11 || s.off + 2 + len > size_) return false;
  const unsigned char* m = data_ + s.off + 2;
  s.off += 2 + len;
  const auto ts = static_cast<std::int64_t>(be(m + 5, 6));
  if (cfg_.end_ns >= 0 && ts > cfg_.end_ns) return false;
  if (!s.open && ts >= cfg_.start_ns) {
    s.bridge.end_datagram();
    open_books(ts);
    s.group_ts = ts;
  }
  if (s.open && ts != s.group_ts) {
    s.bridge.end_datagram();
    s.group_ts = ts;
  }
  const std::span<const std::byte> msg{reinterpret_cast<const std::byte*>(m), len};
  const Timestamp at{midnight_.ns + ts};
  ++s.seq;
  // The venue's events come first: the bridge prints a trade during the message and its delta at
  // the end of the timestamp.
  if (m[0] == 'R' || (s.open && is_order_message(m[0]))) {
    s.orders.set_venue_seq(s.seq);
    static_cast<void>(s.orders.decode(codecs::FrameView{msg, len, 0}, at.ns, s.sink));
  }
  static_cast<void>(s.bridge.on_itch_message(s.seq, msg, {Cycles{}, at}));
  return true;
}

const EventHeader* ItchSource::next() {
  State& s = *st_;
  for (;;) {
    if (s.resting_at < s.resting.size()) return &s.resting[s.resting_at++].hdr;
    if (const std::byte* p = s.ring.try_peek(); p != nullptr) {
      const auto* h = reinterpret_cast<const EventHeader*>(p);
      std::memcpy(s.out.bytes, p, h->len);
      s.ring.release();
      return &s.out.hdr();
    }
    if (s.done) return nullptr;
    if (!step()) {
      if (!s.open) open_books(cfg_.end_ns >= 0 ? cfg_.end_ns : 0);
      s.bridge.end_datagram();
      s.done = true;
    }
  }
}

std::string ItchSource::note() const {
  const auto& b = st_->bridge.stats();
  const auto& d = st_->orders.stats();
  if (b.book_errors == 0 && b.overflow == 0 && d.overflow == 0) return {};
  return "itch: " + std::to_string(b.book_errors) + " book errors (unknown or duplicate order, " +
         "full book), " + std::to_string(b.overflow + d.overflow) + " events without room";
}

}  // namespace fastmm::bt
