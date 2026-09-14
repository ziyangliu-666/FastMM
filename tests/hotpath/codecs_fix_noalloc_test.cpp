// The FIX hot path allocates nothing after construction: framer, view, builder, decoder, encoder,
// session on_frame / on_timer including heartbeats, test requests, gap detection and resend from
// the MessageStore.
#include "alloc_counter.hpp"
#include "test_support.hpp"

#include "fastmm/codecs/fix/fix.hpp"
#include "fastmm/core/msg_ring.hpp"

#include <cstring>
#include <memory>

using namespace fastmm;
using namespace fastmm::codecs;
using namespace fastmm::codecs::fix;
using fastmm::test::NoAllocScope;

namespace {

struct Buffer {
  std::unique_ptr<char[]> data = std::make_unique<char[]>(1U << 22);
  std::size_t size = 0;
  static void send(void* self, std::span<const char> bytes) noexcept {
    auto* b = static_cast<Buffer*>(self);
    std::memcpy(b->data.get() + b->size, bytes.data(), bytes.size());
    b->size += bytes.size();
  }
};

std::int64_t g_now = 1'789'374'615'000'000'000;
std::int64_t clock_now(void*) noexcept {
  return g_now;
}

FixSessionConfig config(FixRole role) {
  FixSessionConfig c;
  c.role = role;
  c.sender_comp_id = role == FixRole::Initiator ? "CLIENT" : "VENUE";
  c.target_comp_id = role == FixRole::Initiator ? "VENUE" : "CLIENT";
  c.heartbeat_interval_s = 30;
  c.store_max_messages = 1U << 16;
  c.store_max_bytes = 32U << 20;
  return c;
}

struct Harness {
  FixSymbolTable symbols;
  Buffer to_acceptor;
  Buffer to_initiator;
  std::unique_ptr<FixSession> initiator;
  std::unique_ptr<FixSession> acceptor;
  FixFramer ini_framer;
  FixFramer acc_framer;
  std::unique_ptr<FixDecoder> decoder;
  std::unique_ptr<FixEncoder> encoder;
  MsgRing ring{1U << 22};
  venues::EventSink sink{&ring, venues::SinkPolicy::Drop, 10};
  char er[1024];
  std::uint64_t exec_seq = 0;
  std::uint64_t events = 0;

  Harness() {
    REQUIRE(symbols.add("BTCUSDT", InstrumentId{0}));
    initiator =
        std::make_unique<FixSession>(config(FixRole::Initiator), &Buffer::send, &to_acceptor);
    acceptor =
        std::make_unique<FixSession>(config(FixRole::Acceptor), &Buffer::send, &to_initiator);
    initiator->set_clock(&clock_now, nullptr);
    acceptor->set_clock(&clock_now, nullptr);
    decoder = std::make_unique<FixDecoder>(symbols, VenueId{0});
    encoder = std::make_unique<FixEncoder>(*initiator, symbols);
  }

  // Delivers a buffer in two chunks (exercising partial frames) and clears it.
  template <class OnApp>
  void deliver(Buffer& b, FixSession& s, FixFramer& f, OnApp&& on_app) {
    const std::size_t half = b.size / 2;
    std::size_t off = 0;
    for (const std::size_t limit : {half, b.size}) {
      for (;;) {
        const FrameView fr =
            f.next(std::as_bytes(std::span<const char>(b.data.get() + off, limit - off)));
        if (!fr.complete()) break;
        if (!s.on_frame(fr)) on_app(s.view());
        off += fr.consumed;
      }
    }
    REQUIRE(off == b.size);
    b.size = 0;
  }

  // Acceptor application logic: answers D with New + Fill, F with Canceled, G with Replaced, and
  // publishes a two-level incremental refresh.
  void venue_app(const FixView& v) {
    const char type = v.msg_type()[0];
    const std::string_view cl = type == 'F' ? v.get(tag::kOrigClOrdID) : v.get(tag::kClOrdID);
    const char exec =
        type == 'D' ? exec_type::kNew : (type == 'F' ? exec_type::kCanceled : exec_type::kReplaced);
    send_er(cl, exec, false);
    if (type == 'D') send_er(cl, exec_type::kTrade, true);
    FixBuilder b = acceptor->begin_app(std::span<char>(er), msg::kMarketDataIncrementalRefresh);
    b.field_uint(tag::kNoMDEntries, 2)
        .field_char(tag::kMDUpdateAction, kMdChange)
        .field_char(tag::kMDEntryType, kMdBid)
        .field(tag::kSymbol, "BTCUSDT")
        .field_decimal(tag::kMDEntryPx, Price::from_int(100))
        .field_decimal(tag::kMDEntrySize, Qty::from_int(1))
        .field_char(tag::kMDUpdateAction, kMdDelete)
        .field_char(tag::kMDEntryType, kMdOffer)
        .field_decimal(tag::kMDEntryPx, Price::from_int(101));
    const std::size_t n = b.finish();
    REQUIRE(acceptor->send_app(std::span<const char>(er, n)));
  }
  void send_er(std::string_view cl, char exec, bool fill) {
    FixBuilder b = acceptor->begin_app(std::span<char>(er), msg::kExecutionReport);
    b.field(tag::kOrderID, "1")
        .field(tag::kClOrdID, cl)
        .field_uint(tag::kExecID, ++exec_seq)
        .field_char(tag::kExecType, exec)
        .field_char(tag::kOrdStatus, '0')
        .field(tag::kSymbol, "BTCUSDT")
        .field_char(tag::kSide, kSideBuy)
        .field_decimal(tag::kLeavesQty, Qty::from_int(1))
        .field_decimal(tag::kCumQty, Qty::from_int(fill ? 1 : 0));
    if (fill) {
      b.field_decimal(tag::kLastQty, Qty::from_int(1))
          .field_decimal(tag::kLastPx, Price::from_int(100));
    }
    const std::size_t n = b.finish();
    REQUIRE(acceptor->send_app(std::span<const char>(er, n)));
  }
  void client_app(const FixView& v) {
    REQUIRE(decoder->decode_view(v, g_now, sink) == venues::ParseStatus::Ok);
    while (ring.try_peek() != nullptr) {
      ring.release();
      ++events;
    }
  }
  void pump() {
    for (int i = 0; i < 16 && (to_acceptor.size != 0 || to_initiator.size != 0); ++i) {
      deliver(to_acceptor, *acceptor, acc_framer, [&](const FixView& v) { venue_app(v); });
      deliver(to_initiator, *initiator, ini_framer, [&](const FixView& v) { client_app(v); });
    }
    REQUIRE(to_acceptor.size == 0);
    REQUIRE(to_initiator.size == 0);
  }

  // Not inlined: gcc's -Warray-bounds misfires when OrderCommand::from() is inlined for a
  // 128-byte message and it analyses the 192-byte OutReplaceMsg branch.
  FASTMM_NOINLINE static venues::OrderCommand command_of(const EventHeader& h) noexcept {
    return *venues::OrderCommand::from(h);
  }

  void round(std::uint32_t i) {
    OutNewOrderMsg n{};
    init_header(n, EventType::OutNewOrder, InstrumentId{0}, VenueId{0});
    n.cl_ord_id = make_cl_ord_id(1, 3 * i + 1);
    n.side = Side::Buy;
    n.type = OrderType::PostOnly;
    n.price = Price::from_int(100);
    n.qty = Qty::from_int(2);
    REQUIRE(encoder->send(*initiator, command_of(n.hdr)));
    pump();
    OutReplaceMsg r{};
    init_header(r, EventType::OutReplace, InstrumentId{0}, VenueId{0});
    r.orig_cl_ord_id = n.cl_ord_id;
    r.cl_ord_id = make_cl_ord_id(1, 3 * i + 2);
    r.price = Price::from_int(99);
    r.qty = Qty::from_int(1);
    REQUIRE(encoder->send(*initiator, command_of(r.hdr)));
    pump();
    OutCancelMsg c{};
    init_header(c, EventType::OutCancel, InstrumentId{0}, VenueId{0});
    c.cl_ord_id = r.cl_ord_id;
    REQUIRE(encoder->send(*initiator, command_of(c.hdr)));
    pump();

    // Lose one venue message and skip a sequence number: ResendRequest, resend + GapFill.
    send_er(encode_cl_ord_id(c.cl_ord_id).view(), exec_type::kNew, false);
    to_initiator.size = 0;
    acceptor->set_next_sender_seq(acceptor->next_sender_seq() + 1);
    send_er(encode_cl_ord_id(c.cl_ord_id).view(), exec_type::kNew, false);
    pump();
    REQUIRE(initiator->state() == SessionState::Up);

    // Heartbeats and a test request.
    g_now += 37'000'000'000;
    initiator->on_timer(g_now);
    acceptor->on_timer(g_now);
    pump();
  }
};

}  // namespace

TEST_CASE("codecs.fix.noalloc: session, framer, decoder and encoder hot path") {
  auto h = std::make_unique<Harness>();
  REQUIRE(h->initiator->logon(g_now));
  h->pump();
  REQUIRE(h->initiator->state() == SessionState::Up);
  REQUIRE(h->acceptor->state() == SessionState::Up);
  h->round(0);  // warm: pages of the store, ring and buffers
  const std::uint64_t events_before = h->events;
  const std::uint64_t resent_before = h->acceptor->stats().messages_resent;
  {
    NoAllocScope guard;
    for (std::uint32_t i = 1; i < 500; ++i) h->round(i);
    CHECK(guard.allocations_so_far() == 0);
  }
  CHECK(h->events > events_before);
  CHECK(h->acceptor->stats().messages_resent > resent_before);
  CHECK(h->acceptor->stats().gap_fills_sent > 0);
  CHECK(h->initiator->stats().test_requests_sent > 0);
  CHECK(h->initiator->stats().heartbeats_sent > 0);
  CHECK(h->decoder->stats().malformed == 0);
}
