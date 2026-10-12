#include "fastmm/venues/hyperliquid/hl_order_encoder.hpp"

#include "fastmm/venues/hyperliquid/hl_msgpack.hpp"
#include "fastmm/venues/json_writer.hpp"

#include <algorithm>

namespace fastmm::venues::hyperliquid {

namespace {

constexpr std::int64_t kPow10[] = {1,
                                   10,
                                   100,
                                   1'000,
                                   10'000,
                                   100'000,
                                   1'000'000,
                                   10'000'000,
                                   100'000'000,
                                   1'000'000'000,
                                   10'000'000'000,
                                   100'000'000'000,
                                   1'000'000'000'000,
                                   10'000'000'000'000,
                                   100'000'000'000'000,
                                   1'000'000'000'000'000,
                                   10'000'000'000'000'000,
                                   100'000'000'000'000'000};
constexpr int kScaleDigits = 8;  // Fixed: raw / 1e8
constexpr std::string_view kCloidMarker = "0x666d000000000000";
constexpr char kHex[] = "0123456789abcdef";

int digits_of(std::int64_t v) noexcept {
  int n = 1;
  while (n < 19 && v >= kPow10[n]) ++n;
  return n;
}

int hex_digit(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

void write_order_msgpack(MsgpackWriter& m, const OrderWire& o) noexcept {
  m.map(o.cloid.empty() ? 6 : 7);
  m.str("a");
  m.uint(o.asset);
  m.str("b");
  m.boolean(o.buy);
  m.str("p");
  m.str(o.px.view());
  m.str("s");
  m.str(o.sz.view());
  m.str("r");
  m.boolean(o.reduce_only);
  m.str("t");
  m.map(1);
  m.str("limit");
  m.map(1);
  m.str("tif");
  m.str(o.tif);
  if (!o.cloid.empty()) {
    m.str("c");
    m.str(o.cloid.view());
  }
}

void write_order_json(JsonWriter& j, const OrderWire& o) noexcept {
  j.begin_object();
  j.key("a").integer(o.asset);
  j.key("b").boolean(o.buy);
  j.key("p").string(o.px.view());
  j.key("s").string(o.sz.view());
  j.key("r").boolean(o.reduce_only);
  j.key("t").begin_object().key("limit").begin_object().key("tif").string(o.tif);
  j.end_object().end_object();
  if (!o.cloid.empty()) j.key("c").string(o.cloid.view());
  j.end_object();
}

}  // namespace

// ---- client order ids ---------------------------------------------------------------------------

Cloid cloid_of(ClientOrderId id) noexcept {
  Cloid c;
  c.append(kCloidMarker);
  char hex[16];
  std::uint64_t v = id.value;
  for (int i = 15; i >= 0; --i) {
    hex[i] = kHex[v & 0x0f];
    v >>= 4;
  }
  c.append(std::string_view(hex, sizeof hex));
  return c;
}

std::optional<ClientOrderId> cl_ord_id_of_cloid(std::string_view cloid) noexcept {
  if (cloid.size() != kCloidMarker.size() + 16) return std::nullopt;
  for (std::size_t i = 0; i < kCloidMarker.size(); ++i) {
    const char a = cloid[i];
    const char lower = a >= 'A' && a <= 'Z' ? static_cast<char>(a + 32) : a;
    if (lower != kCloidMarker[i]) return std::nullopt;
  }
  std::uint64_t v = 0;
  for (std::size_t i = kCloidMarker.size(); i < cloid.size(); ++i) {
    const int d = hex_digit(cloid[i]);
    if (d < 0) return std::nullopt;
    v = (v << 4) | static_cast<std::uint64_t>(d);
  }
  if (v == 0) return std::nullopt;
  return ClientOrderId{v};
}

// ---- prices and sizes ---------------------------------------------------------------------------

int price_decimals(Price px, int sz_decimals) noexcept {
  const int max_dec = std::clamp(6 - sz_decimals, 0, kScaleDigits);
  const std::int64_t raw = px.raw < 0 ? -px.raw : px.raw;
  const std::int64_t integer = raw / kPow10[kScaleDigits];
  int sig_dec = 0;
  if (integer >= 1) {
    sig_dec = std::max(0, 5 - digits_of(integer));
  } else {
    // 0.000123: the first significant digit is the 4th decimal, so 4 + 4 decimals.
    int first = 1;
    while (first < kScaleDigits && raw < kPow10[kScaleDigits - first]) ++first;
    sig_dec = first + 4;
  }
  return std::min(max_dec, sig_dec);
}

Price tick_at(Price px, int sz_decimals) noexcept {
  return Price::from_raw(kPow10[kScaleDigits - price_decimals(px, sz_decimals)]);
}

Price round_price(Price px, int sz_decimals, bool down) noexcept {
  if (px.raw <= 0) return Price{};
  const std::int64_t step = tick_at(px, sz_decimals).raw;
  const std::int64_t floor = px.raw / step * step;
  if (down || floor == px.raw) return Price::from_raw(floor);
  const Price up = Price::from_raw(floor + step);
  // Rounding up past a power of ten can leave a step the new magnitude does not allow
  // (99.999 -> 100.00 is fine; the grid only coarsens), so round again on the new grid.
  const std::int64_t step2 = tick_at(up, sz_decimals).raw;
  return Price::from_raw((up.raw + step2 - 1) / step2 * step2);
}

Qty round_size(Qty q, int sz_decimals) noexcept {
  const int dec = std::clamp(sz_decimals, 0, kScaleDigits);
  const std::int64_t step = kPow10[kScaleDigits - dec];
  return Qty::from_raw(q.raw / step * step);
}

WireDecimal wire_text(std::int64_t raw) noexcept {
  WireDecimal out;
  if (raw == 0) {
    out.append("0");
    return out;
  }
  if (raw < 0) out.append("-");
  const std::uint64_t mag =
      raw < 0 ? 0 - static_cast<std::uint64_t>(raw) : static_cast<std::uint64_t>(raw);
  const std::uint64_t integer = mag / 100'000'000ULL;
  std::uint64_t frac = mag % 100'000'000ULL;
  char buf[24];
  std::size_t n = 0;
  std::uint64_t v = integer;
  char rev[20];
  std::size_t k = 0;
  do {
    rev[k++] = static_cast<char>('0' + v % 10);
    v /= 10;
  } while (v != 0);
  while (k > 0) buf[n++] = rev[--k];
  if (frac != 0) {
    buf[n++] = '.';
    char f[8];
    for (int i = 7; i >= 0; --i) {
      f[i] = static_cast<char>('0' + frac % 10);
      frac /= 10;
    }
    int last = 7;
    while (last > 0 && f[last] == '0') --last;
    for (int i = 0; i <= last; ++i) buf[n++] = f[i];
  }
  out.append(std::string_view(buf, n));
  return out;
}

// ---- actions ------------------------------------------------------------------------------------

bool ActionBuilder::finish(bool ok, std::size_t mp, std::size_t js, ActionKind k) noexcept {
  if (!ok) {
    msgpack_len_ = 0;
    json_len_ = 0;
    return false;
  }
  msgpack_len_ = mp;
  json_len_ = js;
  kind_ = k;
  return true;
}

bool ActionBuilder::order(std::span<const OrderWire> orders) noexcept {
  MsgpackWriter m({msgpack_, kMsgpackBytes});
  JsonWriter j({json_, kJsonBytes});
  m.map(3);
  m.str("type");
  m.str("order");
  m.str("orders");
  m.array(static_cast<std::uint32_t>(orders.size()));
  for (const OrderWire& o : orders) write_order_msgpack(m, o);
  m.str("grouping");
  m.str("na");
  j.begin_object().key("type").string("order").key("orders").begin_array();
  for (const OrderWire& o : orders) write_order_json(j, o);
  j.end_array().key("grouping").string("na").end_object();
  return finish(m.ok() && j.ok() && !orders.empty(), m.size(), j.size(), ActionKind::Order);
}

bool ActionBuilder::cancel(std::span<const CancelWire> cancels) noexcept {
  MsgpackWriter m({msgpack_, kMsgpackBytes});
  JsonWriter j({json_, kJsonBytes});
  m.map(2);
  m.str("type");
  m.str("cancel");
  m.str("cancels");
  m.array(static_cast<std::uint32_t>(cancels.size()));
  j.begin_object().key("type").string("cancel").key("cancels").begin_array();
  for (const CancelWire& c : cancels) {
    m.map(2);
    m.str("a");
    m.uint(c.asset);
    m.str("o");
    m.uint(c.oid);
    j.begin_object();
    j.key("a").integer(c.asset);
    j.key("o").integer(static_cast<std::int64_t>(c.oid));
    j.end_object();
  }
  j.end_array().end_object();
  return finish(m.ok() && j.ok() && !cancels.empty(), m.size(), j.size(), ActionKind::Cancel);
}

bool ActionBuilder::cancel_by_cloid(std::span<const CancelCloidWire> cancels) noexcept {
  MsgpackWriter m({msgpack_, kMsgpackBytes});
  JsonWriter j({json_, kJsonBytes});
  m.map(2);
  m.str("type");
  m.str("cancelByCloid");
  m.str("cancels");
  m.array(static_cast<std::uint32_t>(cancels.size()));
  j.begin_object().key("type").string("cancelByCloid").key("cancels").begin_array();
  for (const CancelCloidWire& c : cancels) {
    m.map(2);
    m.str("asset");
    m.uint(c.asset);
    m.str("cloid");
    m.str(c.cloid.view());
    j.begin_object();
    j.key("asset").integer(c.asset);
    j.key("cloid").string(c.cloid.view());
    j.end_object();
  }
  j.end_array().end_object();
  return finish(
      m.ok() && j.ok() && !cancels.empty(), m.size(), j.size(), ActionKind::CancelByCloid);
}

bool ActionBuilder::batch_modify(std::span<const ModifyWire> modifies) noexcept {
  MsgpackWriter m({msgpack_, kMsgpackBytes});
  JsonWriter j({json_, kJsonBytes});
  m.map(2);
  m.str("type");
  m.str("batchModify");
  m.str("modifies");
  m.array(static_cast<std::uint32_t>(modifies.size()));
  j.begin_object().key("type").string("batchModify").key("modifies").begin_array();
  for (const ModifyWire& w : modifies) {
    m.map(2);
    m.str("oid");
    j.begin_object().key("oid");
    if (w.oid != 0) {
      m.uint(w.oid);
      j.integer(static_cast<std::int64_t>(w.oid));
    } else {
      m.str(w.by_cloid.view());
      j.string(w.by_cloid.view());
    }
    m.str("order");
    write_order_msgpack(m, w.order);
    j.key("order");
    write_order_json(j, w.order);
    j.end_object();
  }
  j.end_array().end_object();
  return finish(m.ok() && j.ok() && !modifies.empty(), m.size(), j.size(), ActionKind::BatchModify);
}

bool ActionBuilder::schedule_cancel(std::int64_t time_ms) noexcept {
  MsgpackWriter m({msgpack_, kMsgpackBytes});
  JsonWriter j({json_, kJsonBytes});
  m.map(time_ms > 0 ? 2 : 1);
  m.str("type");
  m.str("scheduleCancel");
  j.begin_object().key("type").string("scheduleCancel");
  if (time_ms > 0) {
    m.str("time");
    m.uint(static_cast<std::uint64_t>(time_ms));
    j.key("time").integer(time_ms);
  }
  j.end_object();
  return finish(m.ok() && j.ok(), m.size(), j.size(), ActionKind::ScheduleCancel);
}

// ---- payloads and frames ------------------------------------------------------------------------

std::size_t ActionSigner::write_payload(const ActionBuilder& a,
                                        std::uint64_t nonce,
                                        std::span<char> out) const noexcept {
  if (a.json().empty()) return 0;
  Signature sig;
  if (!signer_.sign_action(a.msgpack(), nonce, mainnet_, sig, vault_)) return 0;
  char r[kHexQuantityChars];
  char s[kHexQuantityChars];
  const std::size_t rn = hex_quantity(sig.r, r);
  const std::size_t sn = hex_quantity(sig.s, s);
  JsonWriter j(out);
  j.begin_object();
  j.key("action").raw_value(a.json());
  j.key("nonce").integer(static_cast<std::int64_t>(nonce));
  j.key("signature").begin_object();
  j.key("r").string(std::string_view(r, rn));
  j.key("s").string(std::string_view(s, sn));
  j.key("v").integer(sig.v);
  j.end_object();
  j.key("vaultAddress");
  if (vault_.empty()) {
    j.null();
  } else {
    j.string(vault_);
  }
  j.key("expiresAfter").null();
  j.end_object();
  return j.ok() ? j.size() : 0;
}

std::size_t ActionSigner::write_post(const ActionBuilder& a,
                                     std::uint64_t nonce,
                                     std::uint64_t post_id,
                                     std::span<char> out) const noexcept {
  JsonWriter head(out);
  head.begin_object()
      .key("method")
      .string("post")
      .key("id")
      .integer(static_cast<std::int64_t>(post_id))
      .key("request")
      .begin_object()
      .key("type")
      .string("action")
      .key("payload");
  if (!head.ok()) return 0;
  const std::size_t prefix = head.size();
  const std::size_t body = write_payload(a, nonce, out.subspan(prefix));
  if (body == 0 || prefix + body + 2 > out.size()) return 0;
  out[prefix + body] = '}';
  out[prefix + body + 1] = '}';
  return prefix + body + 2;
}

std::size_t write_info_post(std::string_view payload,
                            std::uint64_t post_id,
                            std::span<char> out) noexcept {
  JsonWriter j(out);
  j.begin_object()
      .key("method")
      .string("post")
      .key("id")
      .integer(static_cast<std::int64_t>(post_id))
      .key("request")
      .begin_object()
      .key("type")
      .string("info")
      .key("payload")
      .raw_value(payload)
      .end_object()
      .end_object();
  return j.ok() ? j.size() : 0;
}

std::size_t write_subscription(bool subscribe,
                               std::string_view type,
                               std::string_view field,
                               std::string_view value,
                               std::span<char> out) noexcept {
  JsonWriter j(out);
  j.begin_object()
      .key("method")
      .string(subscribe ? "subscribe" : "unsubscribe")
      .key("subscription")
      .begin_object()
      .key("type")
      .string(type)
      .key(field)
      .string(value)
      .end_object()
      .end_object();
  return j.ok() ? j.size() : 0;
}

}  // namespace fastmm::venues::hyperliquid
