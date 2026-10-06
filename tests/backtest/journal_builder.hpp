#pragma once
// JournalBuilder: a journal written event by event, for the fill-check and journal-source tests.
#include "backtest_test_util.hpp"

#include "fastmm/core/journal.hpp"
#include "fastmm/core/msg_ring.hpp"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fastmm::bt::testutil {

// Writes a journal event by event, each stamped with the current `now` as its receive time.
class JournalBuilder {
 public:
  // live: the journal carries a TSC calibration, as a live session's does (its depth feed then
  // includes our own orders).
  explicit JournalBuilder(const std::string& path, bool live = false) : ring_(1U << 20) {
    Instrument i{};
    i.symbol = "BTCUSDT";
    i.tick = px("0.01");
    i.lot = qt("0.001");
    i.flags = Instrument::kEnabled;
    i.venue = VenueId{0};
    REQUIRE(table_.add(i));
    JournalSessionInfo info;
    info.session_id = 1;
    info.strategy = "fill_check_test";
    info.instruments = &table_;
    if (live) info.tsc.tsc0 = 1;
    fw_ = std::make_unique<JournalFileWriter>(ring_, path, info);
    REQUIRE(fw_->ok());
  }
  ~JournalBuilder() { close(); }
  JournalBuilder(const JournalBuilder&) = delete;
  JournalBuilder& operator=(const JournalBuilder&) = delete;

  std::int64_t now = 1'000'000'000;
  std::int64_t venue = 0;  // exch_ts of the next inbound events (0: none)

  void close() {
    if (!fw_) return;
    fw_->drain_once();
    fw_->stop();
    fw_.reset();
  }

  // levels: {price, qty}; bids then asks.
  void book(bool snapshot,
            std::vector<std::pair<const char*, const char*>> bids,
            std::vector<std::pair<const char*, const char*>> asks) {
    const auto nb = static_cast<std::uint32_t>(bids.size());
    const auto na = static_cast<std::uint32_t>(asks.size());
    alignas(64) std::byte buf[BookDeltaMsg::size_for(8, 8)]{};
    REQUIRE(nb <= 8);
    REQUIRE(na <= 8);
    auto& d = *reinterpret_cast<BookDeltaMsg*>(buf);
    init_header(d,
                snapshot ? EventType::BookSnapshot : EventType::BookDelta,
                InstrumentId{0},
                VenueId{0},
                BookDeltaMsg::size_for(nb, na));
    if (snapshot) d.hdr.flags |= EventHeader::kSnapshot;
    d.bid_count = nb;
    d.ask_count = na;
    std::size_t i = 0;
    for (const auto& [p, q] : bids) d.levels()[i++] = Level{px(p), qt(q)};
    for (const auto& [p, q] : asks) d.levels()[i++] = Level{px(p), qt(q)};
    put(d.hdr);
  }
  void trade(const char* p, const char* q, Side aggressor, std::uint64_t id = 0) {
    TradeMsg t{};
    init_header(t, EventType::Trade, InstrumentId{0});
    t.price = px(p);
    t.qty = qt(q);
    t.aggressor = aggressor;
    t.trade_id = id;
    t.hdr.venue_seq = id;
    put(t.hdr);
  }
  void ticker(const char* bid, const char* bid_qty, const char* ask, const char* ask_qty) {
    BookTickerMsg m{};
    init_header(m, EventType::BookTicker, InstrumentId{0});
    m.bid_px = px(bid);
    m.bid_qty = qt(bid_qty);
    m.ask_px = px(ask);
    m.ask_qty = qt(ask_qty);
    put(m.hdr);
  }
  void out_new(std::uint64_t id, Side side, const char* p, const char* q) {
    OutNewOrderMsg m{};
    init_header(m, EventType::OutNewOrder, InstrumentId{0});
    m.cl_ord_id = ClientOrderId{id};
    m.side = side;
    m.price = px(p);
    m.qty = qt(q);
    m.type = OrderType::PostOnly;
    m.tif = TimeInForce::Gtc;
    put(m.hdr, true);
  }
  void out_cancel(std::uint64_t id) {
    OutCancelMsg m{};
    init_header(m, EventType::OutCancel, InstrumentId{0});
    m.cl_ord_id = ClientOrderId{id};
    put(m.hdr, true);
  }
  void out_replace(std::uint64_t orig, std::uint64_t id, const char* p, const char* q) {
    OutReplaceMsg m{};
    init_header(m, EventType::OutReplace, InstrumentId{0});
    m.orig_cl_ord_id = ClientOrderId{orig};
    m.cl_ord_id = ClientOrderId{id};
    m.price = px(p);
    m.qty = qt(q);
    put(m.hdr, true);
  }
  void ack(std::uint64_t id) {
    OrderAckMsg m{};
    init_header(m, EventType::OrderAck, InstrumentId{0});
    m.cl_ord_id = ClientOrderId{id};
    put(m.hdr);
  }
  void cancel_ack(std::uint64_t id) {
    OrderCancelAckMsg m{};
    init_header(m, EventType::OrderCancelAck, InstrumentId{0});
    m.cl_ord_id = ClientOrderId{id};
    put(m.hdr);
  }
  void fill(std::uint64_t id,
            const char* p,
            const char* q,
            const char* leaves,
            const char* exec_id = "") {
    OrderFillMsg m{};
    init_header(m, EventType::OrderFill, InstrumentId{0});
    m.cl_ord_id = ClientOrderId{id};
    m.exec_id = ExecId(exec_id);
    m.price = px(p);
    m.qty = qt(q);
    m.leaves_qty = qt(leaves);
    m.liquidity = Liquidity::Maker;
    put(m.hdr);
  }

  // A position as the store's restore or a venue reports it.
  void position(const char* qty,
                const char* avg_px,
                std::uint8_t venue_id = 0,
                std::uint32_t instrument = 0) {
    ReconcileMsg m{};
    init_header(m, EventType::Reconcile, InstrumentId{instrument}, VenueId{venue_id});
    m.kind = ReconcileMsg::Kind::Position;
    m.position_qty = qt(qty);
    m.avg_px = px(avg_px);
    put(m.hdr);
  }
  // A parameter update of one field (its schema index and raw value), for every instrument.
  void param_update(std::uint16_t field, std::int64_t raw) {
    ParamUpdateMsg m{};
    init_header(m, EventType::ParamUpdate, ParamUpdateMsg::kAllInstruments);
    m.count = 1;
    m.publish_seq = 1;
    m.field[0] = field;
    m.value[0] = raw;
    put(m.hdr);
  }
  // One part of a StrategyStateMsg state, split into records of at most `chunk` bytes.
  void strategy_state(StrategyStateMsg::Kind kind,
                      std::uint64_t id,
                      std::uint8_t part,
                      const std::string& bytes,
                      std::uint8_t flags = 0,
                      std::uint32_t chunk = StrategyStateMsg::kChunkBytes) {
    StrategyStateMsg m{};
    init_header(m, EventType::StrategyState);
    m.hdr.flags |= EventHeader::kSynthetic;
    m.hdr.recv_ts = Timestamp{now};
    m.id = id;
    m.total = static_cast<std::uint32_t>(bytes.size());
    m.kind = kind;
    m.part = part;
    m.flags = flags;
    std::size_t off = 0;
    do {
      const std::size_t n = std::min<std::size_t>(bytes.size() - off, chunk);
      m.offset = static_cast<std::uint32_t>(off);
      m.bytes = static_cast<std::uint32_t>(n);
      m.hdr.len = StrategyStateMsg::size_for(m.bytes);
      REQUIRE(w_.record_with(m.hdr, reinterpret_cast<const std::byte*>(bytes.data()) + off, n));
      off += n;
    } while (off < bytes.size());
  }

  // A reconciliation snapshot listing `open` as the venue's open orders.
  void reconcile(std::vector<std::uint64_t> open) {
    ReconcileMsg m{};
    init_header(m, EventType::Reconcile, InstrumentId{0});
    m.kind = ReconcileMsg::Kind::Begin;
    put(m.hdr);
    for (const std::uint64_t id : open) {
      m.kind = ReconcileMsg::Kind::OpenOrder;
      m.cl_ord_id = ClientOrderId{id};
      put(m.hdr);
    }
    m.kind = ReconcileMsg::Kind::End;
    m.cl_ord_id = ClientOrderId{};
    put(m.hdr);
  }

 private:
  void put(EventHeader& h, bool outbound = false) {
    h.recv_ts = Timestamp{now};
    if (!outbound && venue != 0) h.exch_ts = Timestamp{venue};
    REQUIRE((outbound ? w_.record_outbound(h) : w_.record(h)));
  }

  InstrumentTable table_;
  MsgRing ring_;
  std::unique_ptr<JournalFileWriter> fw_;
  JournalWriter w_{&ring_};
};

inline std::string tmp_path(const char* name) {
  const auto p = fastmm::test::tmp_dir() / name;
  std::filesystem::remove(p);
  return p.string();
}

}  // namespace fastmm::bt::testutil
