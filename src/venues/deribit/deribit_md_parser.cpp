#include "fastmm/venues/deribit/deribit_md_parser.hpp"

#include "fastmm/venues/deribit/deribit_json.hpp"

#include <simdjson.h>

#include <cstdlib>
#include <limits>
#include <utility>

namespace fastmm::venues::deribit {

namespace sj = simdjson;
namespace od = simdjson::ondemand;

static_assert(kJsonPadding == sj::SIMDJSON_PADDING, "RecvBuffer padding must match simdjson");

struct DeribitMdParser::Impl {
  od::parser parser;
  explicit Impl(std::size_t capacity) {
    if (parser.allocate(capacity) != sj::SUCCESS) std::abort();
  }
};

DeribitMdParser::DeribitMdParser(const SymbolTable& symbols,
                                 const InstrumentTable& instruments,
                                 VenueId venue,
                                 std::size_t capacity)
    : impl_(std::make_unique<Impl>(capacity)),
      symbols_(symbols),
      instruments_(instruments),
      venue_(venue) {}
DeribitMdParser::~DeribitMdParser() = default;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

[[nodiscard]] inline sj::padded_string_view padded(std::string_view s) noexcept {
  return sj::padded_string_view(s.data(), s.size(), s.size() + sj::SIMDJSON_PADDING);
}

enum class Num : std::uint8_t { Ok, Missing, Malformed };

// Fixed-point field from the raw number token; `null` and absent fields are Missing.
template <class F, class V>
Num fixed_of(V&& v, F& out) noexcept {
  std::string_view tok;
  if (std::forward<V>(v).raw_json_token().get(tok) != sj::SUCCESS) return Num::Missing;
  if (tok.starts_with("null")) return Num::Missing;
  const auto f = json_fixed<F>(tok);
  if (!f) return Num::Malformed;
  out = *f;
  return Num::Ok;
}

template <class V>
double double_or_nan(V&& v) noexcept {
  double d = 0.0;
  if (std::forward<V>(v).get_double().get(d) != sj::SUCCESS) return kNaN;
  return d;
}

// Reads [[action, price, amount], ...] into `levels` (at most `max` kept). Returns the number kept,
// -1 if malformed, -2 if more than `max` levels arrived; `*extra` counts the levels beyond `max`
// when `truncate` is set (snapshots keep the best `max` levels, which Deribit sends first).
int read_levels(od::value arr_val,
                Level* levels,
                std::uint32_t max,
                Qty contract_size,
                bool truncate,
                std::uint32_t* extra) noexcept {
  od::array arr;
  if (arr_val.get_array().get(arr) != sj::SUCCESS) return -1;
  std::uint32_t n = 0;
  for (auto lvl_res : arr) {
    od::array triple;
    if (lvl_res.get_array().get(triple) != sj::SUCCESS) return -1;
    if (n >= max) {
      if (!truncate) return -2;
      ++*extra;
      continue;
    }
    std::string_view action;
    Price px{};
    Qty amount{};
    int idx = 0;
    for (auto item : triple) {
      if (idx == 0) {
        if (item.get_string().get(action) != sj::SUCCESS) return -1;
      } else if (idx == 1) {
        if (fixed_of(item, px) != Num::Ok) return -1;
      } else if (idx == 2) {
        if (fixed_of(item, amount) != Num::Ok) return -1;
      }
      ++idx;
    }
    if (idx < 3) return -1;
    Qty qty{};
    if (action == "delete") {
      qty = Qty{};
    } else if (action == "new" || action == "change") {
      qty = amount_to_contracts(amount, contract_size);
    } else {
      return -1;
    }
    levels[n++] = Level{px, qty};
  }
  return static_cast<int>(n);
}

enum class Channel : std::uint8_t { Book, Ticker, Trades };

// "book.NAME.INTERVAL" -> NAME (instrument names contain no dots; grouped book channels such as
// book.NAME.none.10.100ms have more parts and are not decoded).
bool split_channel(std::string_view channel, Channel& kind, std::string_view& name) noexcept {
  std::string_view rest;
  if (channel.starts_with("book.")) {
    kind = Channel::Book;
    rest = channel.substr(5);
  } else if (channel.starts_with("ticker.")) {
    kind = Channel::Ticker;
    rest = channel.substr(7);
  } else if (channel.starts_with("trades.")) {
    kind = Channel::Trades;
    rest = channel.substr(7);
  } else {
    return false;
  }
  const std::size_t dot = rest.find('.');
  if (dot == std::string_view::npos || dot == 0) return false;
  if (rest.find('.', dot + 1) != std::string_view::npos) return false;
  name = rest.substr(0, dot);
  return true;
}

}  // namespace

MdDecodeResult DeribitMdParser::decode(std::string_view json,
                                       Timestamp recv_ts,
                                       Cycles t0,
                                       std::span<std::byte> out) noexcept {
  ++stats_.frames;
  MdDecodeResult r;
  auto malformed = [&]() {
    ++stats_.malformed;
    r.status = ParseStatus::Malformed;
    r.count = 0;
    r.len = 0;
    return r;
  };
  if (out.size() < kDecoderScratchBytes) {
    ++stats_.overflow;
    r.status = ParseStatus::Overflow;
    return r;
  }
  od::document doc;
  od::object root;
  if (impl_->parser.iterate(padded(json)).get(doc) != sj::SUCCESS ||
      doc.get_object().get(root) != sj::SUCCESS)
    return malformed();

  std::string_view method;
  if (root["method"].get_string().get(method) == sj::SUCCESS) {
    if (method == "heartbeat") {
      // {"params":{"type":"test_request"},"method":"heartbeat","jsonrpc":"2.0"} (recorded)
      root.reset();
      od::object params;
      std::string_view type;
      const bool test = root["params"].get_object().get(params) == sj::SUCCESS &&
                        params["type"].get_string().get(type) == sj::SUCCESS &&
                        type == "test_request";
      ++stats_.heartbeats;
      r.frame = test ? FrameKind::TestRequest : FrameKind::Heartbeat;
      r.status = ParseStatus::Ignored;
      return r;
    }
    if (method != "subscription") {
      ++stats_.ignored;
      r.status = ParseStatus::Ignored;
      return r;
    }
    r.frame = FrameKind::Notification;
    root.reset();
    od::object params;
    if (root["params"].get_object().get(params) != sj::SUCCESS) return malformed();
    std::string_view channel;
    if (params["channel"].get_string().get(channel) != sj::SUCCESS) return malformed();
    Channel kind = Channel::Book;
    std::string_view name;
    if (!split_channel(channel, kind, name)) {
      ++stats_.ignored;
      r.status = ParseStatus::Ignored;
      return r;
    }
    const InstrumentId inst = symbols_.find(venue_, name);
    if (!inst.valid() || !instruments_.contains(inst)) {
      ++stats_.unknown_symbol;
      r.status = ParseStatus::UnknownSymbol;
      return r;
    }
    const Instrument& ins = instruments_.get(inst);
    const Qty csize = ins.contract_multiplier;

    switch (kind) {
      case Channel::Book: {
        od::object data;
        if (params["data"].get_object().get(data) != sj::SUCCESS) return malformed();
        std::int64_t ts = 0;
        std::string_view type;
        std::uint64_t change_id = 0;
        if (data["timestamp"].get_int64().get(ts) != sj::SUCCESS) return malformed();
        if (data["type"].get_string().get(type) != sj::SUCCESS) return malformed();
        if (data["change_id"].get_uint64().get(change_id) != sj::SUCCESS) return malformed();
        const bool snapshot = type == "snapshot";
        if (!snapshot && type != "change") return malformed();
        auto* m = reinterpret_cast<BookDeltaMsg*>(out.data());
        Level* levels = m->levels();
        std::uint32_t extra = 0;
        od::value bids;
        if (data["bids"].get(bids) != sj::SUCCESS) return malformed();
        const int nb = read_levels(bids, levels, kMaxBookLevelsPerMsg, csize, snapshot, &extra);
        if (nb == -1) return malformed();
        if (nb == -2) {
          ++stats_.overflow;
          r.status = ParseStatus::Overflow;
          return r;
        }
        od::value asks;
        if (data["asks"].get(asks) != sj::SUCCESS) return malformed();
        const int na =
            read_levels(asks, levels + nb, kMaxBookLevelsPerMsg, csize, snapshot, &extra);
        if (na == -1) return malformed();
        if (na == -2) {
          ++stats_.overflow;
          r.status = ParseStatus::Overflow;
          return r;
        }
        std::uint64_t prev = 0;
        if (data["prev_change_id"].get_uint64().get(prev) != sj::SUCCESS) {
          if (!snapshot) return malformed();  // "Every message except the first also contains"
          prev = 0;
        }
        if (extra > 0) ++stats_.truncated_snapshots;
        const auto bid_count = static_cast<std::uint32_t>(nb);
        const auto ask_count = static_cast<std::uint32_t>(na);
        const std::uint32_t len = BookDeltaMsg::size_for(bid_count, ask_count);
        init_header(
            *m, snapshot ? EventType::BookSnapshot : EventType::BookDelta, inst, venue_, len);
        if (snapshot) m->hdr.flags |= EventHeader::kSnapshot;
        m->bid_count = bid_count;
        m->ask_count = ask_count;
        m->first_update_id = change_id;
        m->last_update_id = change_id;
        m->prev_update_id = prev;
        m->hdr.venue_seq = change_id;
        m->hdr.exch_ts = Timestamp{ts * 1'000'000};
        m->hdr.recv_ts = recv_ts;
        m->hdr.t0_cycles = t0;
        if (snapshot) {
          ++stats_.book_snapshots;
        } else {
          ++stats_.book_changes;
        }
        r.status = ParseStatus::Ok;
        r.kind = snapshot ? MdKind::BookSnapshot : MdKind::BookDelta;
        r.len = len;
        r.count = 1;
        return r;
      }
      case Channel::Ticker: {
        od::object data;
        if (params["data"].get_object().get(data) != sj::SUCCESS) return malformed();
        std::int64_t ts = 0;
        if (data["timestamp"].get_int64().get(ts) != sj::SUCCESS) return malformed();
        const bool option = ins.asset_class == AssetClass::Option;
        double delta = kNaN;
        double gamma = kNaN;
        double vega = kNaN;
        double theta = kNaN;
        double rho = kNaN;
        bool has_greeks = false;
        {
          od::object greeks;
          if (data["greeks"].get_object().get(greeks) == sj::SUCCESS) {
            has_greeks = true;
            delta = double_or_nan(greeks["delta"]);
            gamma = double_or_nan(greeks["gamma"]);
            vega = double_or_nan(greeks["vega"]);
            theta = double_or_nan(greeks["theta"]);
            rho = double_or_nan(greeks["rho"]);
          }
        }
        Price index_px{};
        Price mark_px{};
        Price bid_px{};
        Price ask_px{};
        Price underlying_px{};
        Qty bid_amount{};
        Qty ask_amount{};
        if (fixed_of(data["index_price"], index_px) == Num::Malformed) return malformed();
        if (fixed_of(data["mark_price"], mark_px) == Num::Malformed) return malformed();
        const double rate = double_or_nan(data["interest_rate"]);
        if (fixed_of(data["best_ask_price"], ask_px) == Num::Malformed) return malformed();
        if (fixed_of(data["best_bid_price"], bid_px) == Num::Malformed) return malformed();
        const double mark_iv = double_or_nan(data["mark_iv"]);
        const double bid_iv = double_or_nan(data["bid_iv"]);
        const double ask_iv = double_or_nan(data["ask_iv"]);
        if (fixed_of(data["underlying_price"], underlying_px) == Num::Malformed) return malformed();
        if (fixed_of(data["best_ask_amount"], ask_amount) == Num::Malformed) return malformed();
        if (fixed_of(data["best_bid_amount"], bid_amount) == Num::Malformed) return malformed();

        auto* bt = reinterpret_cast<BookTickerMsg*>(out.data());
        init_header(*bt, EventType::BookTicker, inst, venue_);
        bt->bid_px = bid_px;
        bt->bid_qty = bid_px.is_zero() ? Qty{} : amount_to_contracts(bid_amount, csize);
        bt->ask_px = ask_px;
        bt->ask_qty = ask_px.is_zero() ? Qty{} : amount_to_contracts(ask_amount, csize);
        bt->hdr.exch_ts = Timestamp{ts * 1'000'000};
        bt->hdr.recv_ts = recv_ts;
        bt->hdr.t0_cycles = t0;
        ++stats_.book_tickers;
        std::uint32_t written = sizeof(BookTickerMsg);
        r.count = 1;
        if (option || has_greeks) {
          auto* ot = reinterpret_cast<OptionTickerMsg*>(out.data() + written);
          init_header(*ot, EventType::OptionTicker, inst, venue_);
          ot->mark_price = mark_px;
          ot->underlying_price = underlying_px;
          ot->index_price = index_px;
          ot->mark_iv = mark_iv / 100.0;  // percent on the wire (31.2 = 31.2 %)
          ot->bid_iv = bid_iv / 100.0;
          ot->ask_iv = ask_iv / 100.0;
          ot->delta = delta;
          ot->gamma = gamma;
          ot->vega = vega;
          ot->theta = theta;
          ot->rho = rho;
          ot->interest_rate = rate;
          ot->hdr.exch_ts = Timestamp{ts * 1'000'000};
          ot->hdr.recv_ts = recv_ts;
          ot->hdr.t0_cycles = t0;
          written += sizeof(OptionTickerMsg);
          ++r.count;
          ++stats_.option_tickers;
        }
        r.status = ParseStatus::Ok;
        r.kind = MdKind::BookTicker;
        r.len = written;
        return r;
      }
      case Channel::Trades: {
        od::array data;
        if (params["data"].get_array().get(data) != sj::SUCCESS) return malformed();
        std::uint32_t written = 0;
        std::uint32_t count = 0;
        for (auto item : data) {
          od::object t;
          if (item.get_object().get(t) != sj::SUCCESS) return malformed();
          std::int64_t ts = 0;
          Price px{};
          Qty amount{};
          std::string_view direction;
          std::uint64_t seq = 0;
          std::string_view id;
          // Recorded field order: timestamp, price, amount, direction, index_price,
          // instrument_name, trade_seq, mark_price, tick_direction, trade_id, contracts.
          if (t["timestamp"].get_int64().get(ts) != sj::SUCCESS) return malformed();
          if (fixed_of(t["price"], px) != Num::Ok) return malformed();
          if (fixed_of(t["amount"], amount) != Num::Ok) return malformed();
          if (t["direction"].get_string().get(direction) != sj::SUCCESS) return malformed();
          if (t["trade_seq"].get_uint64().get(seq) != sj::SUCCESS) seq = 0;
          if (t["trade_id"].get_string().get(id) != sj::SUCCESS) return malformed();
          if (written + sizeof(TradeMsg) > out.size()) {
            ++stats_.overflow;
            break;
          }
          auto* m = reinterpret_cast<TradeMsg*>(out.data() + written);
          init_header(*m, EventType::Trade, inst, venue_);
          m->price = px;
          m->qty = amount_to_contracts(amount, csize);
          m->trade_id = trade_id_of(id);
          m->aggressor = direction == "sell" ? Side::Sell : Side::Buy;
          m->hdr.venue_seq = seq;
          m->hdr.exch_ts = Timestamp{ts * 1'000'000};
          m->hdr.recv_ts = recv_ts;
          m->hdr.t0_cycles = t0;
          written += sizeof(TradeMsg);
          ++count;
          ++stats_.trades;
        }
        if (count == 0) {
          ++stats_.ignored;
          r.status = ParseStatus::Ignored;
          return r;
        }
        r.status = ParseStatus::Ok;
        r.kind = MdKind::Trade;
        r.len = written;
        r.count = count;
        return r;
      }
    }
    return malformed();
  }

  // JSON-RPC response: {"jsonrpc":"2.0","id":..,"result":..|"error":{..},"usIn":..}
  root.reset();
  {
    od::value idv;
    if (root["id"].get(idv) != sj::SUCCESS) {
      ++stats_.ignored;
      r.status = ParseStatus::Ignored;
      return r;
    }
    std::int64_t num = 0;
    std::string_view text;
    if (idv.get_int64().get(num) == sj::SUCCESS) {
      r.rpc.id = num;
    } else if (idv.get_string().get(text) == sj::SUCCESS) {
      r.rpc.id_text = text;
    }
  }
  r.frame = FrameKind::Response;
  ++stats_.responses;
  root.reset();
  od::object err;
  if (root["error"].get_object().get(err) == sj::SUCCESS) {
    r.rpc.is_error = true;
    std::int64_t code = 0;
    if (err["code"].get_int64().get(code) == sj::SUCCESS) r.rpc.error_code = code;
    std::string_view msg;
    if (err["message"].get_string().get(msg) == sj::SUCCESS) r.rpc.error_message = msg;
    od::object data;
    std::string_view reason;
    if (err["data"].get_object().get(data) == sj::SUCCESS &&
        data["reason"].get_string().get(reason) == sj::SUCCESS)
      r.rpc.error_reason = reason;
    r.status = ParseStatus::Error;
    return r;
  }
  root.reset();
  od::array arr;
  if (root["result"].get_array().get(arr) == sj::SUCCESS) {
    std::uint32_t n = 0;
    for (auto item : arr) {
      static_cast<void>(item);
      ++n;
    }
    r.result_items = n;
  }
  r.status = ParseStatus::Ignored;
  return r;
}

}  // namespace fastmm::venues::deribit
