#include "fastmm/core/messages.hpp"

#include "test_support.hpp"

#include <vector>

using namespace fastmm;

TEST_CASE("core.messages: header init and sizes") {
  TradeMsg t{};
  init_header(t, EventType::Trade, InstrumentId{3}, VenueId{1});
  CHECK(t.hdr.len == 128);
  CHECK(t.hdr.type == EventType::Trade);
  CHECK(t.hdr.version == kMessageVersion);
  CHECK(t.hdr.instrument == InstrumentId{3});
  CHECK(t.hdr.venue == VenueId{1});
  static_assert(sizeof(OrderFillMsg) == 256);
  static_assert(sizeof(ReconcileMsg) == 192);
  static_assert(sizeof(OutReplaceMsg) == 192);
  static_assert(kMaxMsgBytes % 64 == 0);
  const auto& back = msg_cast<TradeMsg>(&t.hdr);
  CHECK(&back == &t);
}

TEST_CASE("core.messages: BookDeltaMsg flexible levels") {
  const std::uint32_t len = BookDeltaMsg::size_for(3, 2);
  CHECK(len == ((96 + 5 * 16 + 63) / 64) * 64);
  CHECK(len % 64 == 0);
  std::vector<std::byte> buf(len);
  auto* m = reinterpret_cast<BookDeltaMsg*>(buf.data());
  init_header(*m, EventType::BookDelta, InstrumentId{0}, VenueId{0}, len);
  m->bid_count = 3;
  m->ask_count = 2;
  m->first_update_id = 10;
  m->last_update_id = 12;
  Level* lv = m->levels();
  for (std::uint32_t i = 0; i < 5; ++i)
    lv[i] = Level{Price::from_int(100 + static_cast<std::int64_t>(i)), Qty::from_int(1)};
  CHECK(m->bids().size() == 3);
  CHECK(m->asks().size() == 2);
  CHECK(m->bids()[2].price == Price::from_int(102));
  CHECK(m->asks()[0].price == Price::from_int(103));
  CHECK_FALSE(m->is_snapshot());
  m->hdr.flags |= EventHeader::kSnapshot;
  CHECK(m->is_snapshot());
  CHECK(BookDeltaMsg::size_for(0, 0) == 128);
}
