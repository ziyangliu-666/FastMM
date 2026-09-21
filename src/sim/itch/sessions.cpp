// fastmm-sim-itch: SoupBinTCP connections, the GLIMPSE snapshot and the OUCH gateway.
#include "sim_itch_impl.hpp"

#include "fastmm/codecs/itch/glimpse.hpp"
#include "fastmm/codecs/itch/itch_messages.hpp"
#include "fastmm/codecs/itch/nasdaq_fields.hpp"
#include "fastmm/codecs/ouch/ouch_common.hpp"
#include "fastmm/codecs/soupbin/soupbin.hpp"

#include <cstring>

namespace fastmm::sim::itch {

namespace ouch50 = codecs::ouch50;
namespace soupbin = codecs::soupbin;
namespace nasdaq = codecs::nasdaq;

namespace {

constexpr std::size_t kInitialBuffer = std::size_t{64} * 1024;
constexpr std::size_t kMaxInBuffer = 16U << 20;
constexpr std::size_t kMaxOutBuffer = 512U << 20;

// OUCH 5.0 "Order Reject Reasons".
constexpr std::uint16_t kRejectInvalidPrice = 0x001D;
constexpr std::uint16_t kRejectInvalidQuantity = 0x0013;
constexpr std::uint16_t kRejectInvalidSymbol = 0x0017;
constexpr std::uint16_t kRejectCrossed = 0x0012;
constexpr std::uint16_t kRejectOther = 0x000F;

soupbin::ServerConfig server_config(const std::string& user,
                                    const std::string& password,
                                    const std::string& session,
                                    std::size_t messages) {
  soupbin::ServerConfig c;
  c.username = std::string_view(user);
  c.password = std::string_view(password);
  c.session = std::string_view(session);
  c.history_messages = messages;
  c.history_bytes = messages * 48;
  return c;
}

std::uint32_t shares(Qty q) noexcept {
  std::uint32_t n = 0;
  return nasdaq::qty_to_shares(q, n) ? n : 0;
}

}  // namespace

// ---- TcpConn --------------------------------------------------------------------------------

TcpConn::TcpConn(Impl& impl, net::TcpSocket sock)
    : impl_(impl), sock_(std::move(sock)), in_(kInitialBuffer) {
  out_.reserve(kInitialBuffer);
}

bool TcpConn::send(std::span<const std::byte> bytes) noexcept {
  if (dead_) return false;
  if (out_off_ == out_.size() && !writing_ && !corked_) {
    out_.clear();
    out_off_ = 0;
    const net::IoResult r = sock_.write(bytes);
    if (r.failed() || r.closed) {
      close();
      return false;
    }
    bytes = bytes.subspan(r.bytes);
    if (bytes.empty()) return true;
  }
  if (out_.size() - out_off_ + bytes.size() > kMaxOutBuffer) {
    close();
    return false;
  }
  out_.insert(out_.end(), bytes.begin(), bytes.end());
  if (!writing_ && !corked_) {
    writing_ = true;
    impl_.reactor.modify(sock_.fd(), *this, net::IoEvent::ReadWrite);
  }
  return true;
}

void TcpConn::uncork() noexcept {
  corked_ = false;
  if (!dead_) flush();
}

void TcpConn::flush() noexcept {
  while (out_off_ < out_.size()) {
    const net::IoResult r =
        sock_.write(std::span<const std::byte>(out_.data() + out_off_, out_.size() - out_off_));
    if (r.failed() || r.closed) {
      close();
      return;
    }
    out_off_ += r.bytes;
    if (r.would_block()) {
      if (!writing_) {
        writing_ = true;
        impl_.reactor.modify(sock_.fd(), *this, net::IoEvent::ReadWrite);
      }
      return;
    }
  }
  out_.clear();
  out_off_ = 0;
  if (writing_) {
    writing_ = false;
    impl_.reactor.modify(sock_.fd(), *this, net::IoEvent::Read);
  }
}

void TcpConn::on_readable() {
  while (!dead_) {
    if (in_used_ == in_.size()) {
      if (in_.size() >= kMaxInBuffer) {
        close();
        return;
      }
      in_.resize(in_.size() * 2);
    }
    const net::IoResult r =
        sock_.read(std::span<std::byte>(in_.data() + in_used_, in_.size() - in_used_));
    if (r.bytes > 0) {
      rx_tsc_ = rdtscp().v;
      in_used_ += r.bytes;
      process();
    }
    if (r.closed || r.failed()) {
      close();
      return;
    }
    if (r.would_block()) return;
  }
}

void TcpConn::process() noexcept {
  const soupbin::SoupBinFramer framer;
  std::size_t used = 0;
  while (!dead_) {
    const codecs::FrameView f =
        framer.next(std::span<const std::byte>(in_.data() + used, in_used_ - used));
    if (!f.complete()) break;
    used += f.consumed;
    on_packet(f);
  }
  if (dead_) return;
  if (used != 0) {
    std::memmove(in_.data(), in_.data() + used, in_used_ - used);
    in_used_ -= used;
  }
}

void TcpConn::on_writable() {
  flush();
}

void TcpConn::on_error(int) {
  close();
}

void TcpConn::close() noexcept {
  if (dead_) return;
  dead_ = true;
  if (sock_.valid()) {
    impl_.reactor.remove(sock_.fd());
    sock_.close();
  }
}

// ---- GLIMPSE --------------------------------------------------------------------------------

GlimpseConn::GlimpseConn(Impl& impl, net::TcpSocket sock, std::int64_t now_ns)
    : TcpConn(impl, std::move(sock)),
      session_(*this,
               inbound_,
               server_config(impl.cfg.glimpse_username,
                             impl.cfg.glimpse_password,
                             impl.cfg.glimpse_session,
                             impl.cfg.glimpse_max_messages)) {
  session_.on_connect(now_ns);
}

void GlimpseConn::on_packet(const codecs::FrameView& f) noexcept {
  session_.on_frame(f);
  if (session_.state() == codecs::SessionState::Up && !sent_) {
    sent_ = true;
    ++impl_.stats.glimpse_logins;
    cork();
    send_snapshot();
    uncork();
  }
  if (session_.state() == codecs::SessionState::Down) close();
}

void GlimpseConn::on_timer(std::int64_t now_ns) noexcept {
  session_.on_timer(now_ns);
  if (session_.state() == codecs::SessionState::Down) close();
}

// Directory, trading state, every resting order best level first in queue order, End of
// Snapshot. Runs between two engine calls: the books equal the published stream up to
// published() and the snapshot continues at published() + 1.
void GlimpseConn::send_snapshot() noexcept {
  Impl& im = impl_;
  std::array<std::byte, 64> buf{};
  const codecs::itch::ItchEncoder& enc = im.enc;
  bool ok = true;
  auto put = [&](std::size_t n) {
    if (ok && (n == 0 || !session_.send_sequenced(std::span<const std::byte>(buf.data(), n))))
      ok = false;
  };
  for (const Impl::Symbol& s : im.symbols)
    put(enc.stock_directory(buf, s.locate, im.itch_ts(), s.cfg.symbol));
  for (const Impl::Symbol& s : im.symbols) {
    codecs::itch::StockTradingAction m{};
    if (!enc.header(m.hdr, 'H', s.locate, im.itch_ts())) continue;
    nasdaq::put_alpha(m.stock, sizeof m.stock, s.cfg.symbol);
    m.trading_state = 'T';
    m.reserved = ' ';
    nasdaq::put_alpha(m.reason, sizeof m.reason, "");
    put(codecs::itch::ItchEncoder::write(buf, m));
  }
  std::uint64_t orders = 0;
  for (const Impl::Symbol& s : im.symbols) {
    for (const Side side : {Side::Buy, Side::Sell}) {
      s.engine->for_each_resting(InstrumentId{0}, side, [&](const SimOrder& o) {
        put(enc.add_order(buf,
                          s.locate,
                          im.itch_ts(),
                          s.pub->ref_of(o.order_id),
                          o.side,
                          o.leaves(),
                          s.cfg.symbol,
                          o.price));
        ++orders;
      });
    }
  }
  put(codecs::itch::glimpse::write_end_of_snapshot(buf, im.tx.published() + 1));
  if (!ok) {
    ++im.stats.snapshot_overflows;
    close();
    return;
  }
  ++im.stats.snapshots;
  im.stats.snapshot_orders += orders;
}

// ---- OUCH -----------------------------------------------------------------------------------

void OuchInbound::on_unsequenced(std::span<const std::byte> msg) noexcept {
  conn->on_unsequenced(msg);
}

OuchConn::OuchConn(Impl& impl, net::TcpSocket sock, AccountId account, std::int64_t now_ns)
    : TcpConn(impl, std::move(sock)),
      inbound_{this},
      session_(*this,
               inbound_,
               server_config(impl.cfg.ouch_username,
                             impl.cfg.ouch_password,
                             impl.cfg.ouch_session,
                             impl.cfg.ouch_history_messages)),
      account_(account) {
  session_.on_connect(now_ns);
}

void OuchConn::on_packet(const codecs::FrameView& f) noexcept {
  const bool was_up = session_.state() == codecs::SessionState::Up;
  session_.on_frame(f);
  if (!was_up && session_.state() == codecs::SessionState::Up) ++impl_.stats.ouch_logins;
  if (session_.state() == codecs::SessionState::Down) close();
}

void OuchConn::on_timer(std::int64_t now_ns) noexcept {
  session_.on_timer(now_ns);
  if (session_.state() == codecs::SessionState::Down) close();
}

// Cancel on disconnect: the connection's orders leave the book (D on the feed). Runs from
// Impl::reap(), never inside an engine call.
void OuchConn::release() noexcept {
  closing_ = true;
  std::vector<std::pair<std::size_t, ClientOrderId>> open;
  open.reserve(orders_.size());
  for (const auto& [urn, o] : orders_) open.emplace_back(o.symbol, o.engine_id);
  const Timestamp now = impl_.sim_now();
  for (const auto& [sym, id] : open) {
    Impl::Symbol& s = impl_.symbols[sym];
    s.pub->cancel(*s.engine, account_, id, now);
  }
  orders_.clear();
  urn_by_engine_.clear();
  impl_.accounts[account_] = nullptr;
}

void OuchConn::out(std::size_t n) noexcept {
  if (closing_ || dead() || n == 0) return;
  static_cast<void>(session_.send_sequenced(std::span<const std::byte>(buf_.data(), n)));
}

std::uint32_t OuchConn::urn_of(ClientOrderId engine_id) const noexcept {
  const auto it = urn_by_engine_.find(engine_id.value);
  return it == urn_by_engine_.end() ? 0 : it->second;
}

void OuchConn::forget(std::uint32_t urn) noexcept {
  const auto it = orders_.find(urn);
  if (it == orders_.end()) return;
  urn_by_engine_.erase(it->second.engine_id.value);
  orders_.erase(it);
}

void OuchConn::reject(std::uint32_t urn, std::uint16_t reason, const char* cl_ord_id14) noexcept {
  ++impl_.stats.order_rejects;
  out(ouch50::host::rejected(buf_, impl_.itch_ts(), urn, reason, cl_ord_id14));
}

void OuchConn::on_unsequenced(std::span<const std::byte> msg) noexcept {
  if (msg.empty()) {
    ++impl_.stats.malformed;
    return;
  }
  switch (static_cast<char>(msg[0])) {
    case 'O':
      enter(msg);
      break;
    case 'U':
      replace(msg);
      break;
    case 'X':
      cancel(msg);
      break;
    default:
      ++impl_.stats.ignored;
      break;
  }
}

void OuchConn::enter(std::span<const std::byte> msg) noexcept {
  ouch50::host::EnterView v;
  if (!ouch50::host::parse_enter(msg, v)) {
    ++impl_.stats.malformed;
    if (msg.size() >= sizeof(ouch50::EnterOrder)) {
      const auto& m = codecs::ouch::view_as<ouch50::EnterOrder>(msg.data());
      reject(m.user_ref_num.get(), kRejectOther, m.cl_ord_id);
    }
    return;
  }
  // The token is timed first, before anything else of the order is looked at.
  if (v.seq_token != 0) impl_.record_wire_to_wire(v.seq_token, rx_tsc_);
  if (v.user_ref_num <= last_urn_) {  // not increasing: ignored as a retransmission
    ++impl_.stats.ignored;
    return;
  }
  last_urn_ = v.user_ref_num;
  const std::size_t sym = impl_.find_symbol(v.symbol);
  if (sym == std::string::npos) {
    reject(v.user_ref_num, kRejectInvalidSymbol, v.raw->cl_ord_id);
    return;
  }
  Impl::Symbol& s = impl_.symbols[sym];
  if (v.price.raw % s.cfg.tick.raw != 0) {
    reject(v.user_ref_num, kRejectInvalidPrice, v.raw->cl_ord_id);
    return;
  }
  if (v.qty.raw % s.cfg.lot.raw != 0) {
    reject(v.user_ref_num, kRejectInvalidQuantity, v.raw->cl_ord_id);
    return;
  }
  ++impl_.stats.orders;
  const ClientOrderId id{v.user_ref_num};
  orders_[v.user_ref_num] = Order{sym, id, *v.raw};
  urn_by_engine_[id.value] = v.user_ref_num;
  NewOrder o;
  o.account = account_;
  o.cl_ord_id = id;
  o.instrument = InstrumentId{0};
  o.side = v.side;
  o.type = v.post_only ? OrderType::PostOnly : OrderType::Limit;
  o.tif = v.tif;
  o.price = v.price;
  o.qty = v.qty;
  s.pub->submit(*s.engine, o, impl_.sim_now());
}

// OUCH 5.0 2.2: a replace of an order that is no longer live is ignored; when the replacement
// cannot be entered the original is gone and a Canceled message says so.
void OuchConn::replace(std::span<const std::byte> msg) noexcept {
  ouch50::host::ReplaceView r;
  if (!ouch50::host::parse_replace(msg, r)) {
    ++impl_.stats.malformed;
    return;
  }
  if (r.user_ref_num <= last_urn_) {
    ++impl_.stats.ignored;
    return;
  }
  last_urn_ = r.user_ref_num;
  const auto it = orders_.find(r.orig_user_ref_num);
  if (it == orders_.end()) {
    ++impl_.stats.ignored;
    return;
  }
  const Order orig = it->second;
  Impl::Symbol& s = impl_.symbols[orig.symbol];
  const SimOrder* old = s.engine->find(account_, orig.engine_id);
  if (old == nullptr) {
    ++impl_.stats.ignored;
    return;
  }
  const Qty old_leaves = old->leaves();
  ++impl_.stats.replaces;
  replacing_ = &r;
  replaced_ = false;
  s.pub->replace(*s.engine,
                 account_,
                 orig.engine_id,
                 ClientOrderId{r.user_ref_num},
                 r.price,
                 r.qty,
                 impl_.sim_now());
  replacing_ = nullptr;
  if (!replaced_ && s.engine->find(account_, orig.engine_id) == nullptr &&
      orders_.contains(r.orig_user_ref_num)) {
    out(ouch50::host::canceled(
        buf_, impl_.itch_ts(), r.orig_user_ref_num, shares(old_leaves), 'U'));
    forget(r.orig_user_ref_num);
  }
}

// Quantity 0 cancels the order; a smaller quantity reduces it in place (X on the feed).
void OuchConn::cancel(std::span<const std::byte> msg) noexcept {
  ouch50::host::CancelView c;
  if (!ouch50::host::parse_cancel(msg, c)) {
    ++impl_.stats.malformed;
    return;
  }
  const auto it = orders_.find(c.user_ref_num);
  if (it == orders_.end()) {  // "superfluous Cancel Order messages are silently ignored"
    ++impl_.stats.ignored;
    return;
  }
  Order& ord = it->second;
  Impl::Symbol& s = impl_.symbols[ord.symbol];
  const SimOrder* o = s.engine->find(account_, ord.engine_id);
  if (o == nullptr) {
    ++impl_.stats.ignored;
    return;
  }
  ++impl_.stats.cancels;
  if (c.quantity == 0) {
    s.pub->cancel(*s.engine, account_, ord.engine_id, impl_.sim_now());
    return;
  }
  Qty target{};
  if (!nasdaq::shares_to_qty(c.quantity, target) || target >= o->leaves()) {
    ++impl_.stats.ignored;
    return;
  }
  const Qty decrement = o->leaves() - target;
  const ClientOrderId old_id = ord.engine_id;
  const ClientOrderId new_id{(std::uint64_t{1} << 40) | next_synthetic_++};
  reducing_ = true;
  s.pub->replace(*s.engine, account_, old_id, new_id, o->price, target, impl_.sim_now());
  reducing_ = false;
  urn_by_engine_.erase(old_id.value);
  urn_by_engine_[new_id.value] = c.user_ref_num;
  ord.engine_id = new_id;
  out(ouch50::host::canceled(buf_, impl_.itch_ts(), c.user_ref_num, shares(decrement), 'U'));
}

// ---- engine effects on this account ---------------------------------------------------------

void OuchConn::order_ack(const SimOrder& o, std::uint64_t ref) noexcept {
  if (reducing_) return;
  if (replacing_ != nullptr) {
    const std::uint32_t orig_urn = replacing_->orig_user_ref_num;
    const auto it = orders_.find(orig_urn);
    if (it == orders_.end()) return;
    Order next = it->second;
    const ouch50::EnterOrder original = next.enter;
    forget(orig_urn);
    next.engine_id = o.cl_ord_id;
    orders_[replacing_->user_ref_num] = next;
    urn_by_engine_[o.cl_ord_id.value] = replacing_->user_ref_num;
    replaced_ = true;
    out(ouch50::host::replaced(
        buf_, impl_.itch_ts(), *replacing_->raw, original, shares(o.leaves()), ref, 'L'));
    return;
  }
  const auto it = orders_.find(urn_of(o.cl_ord_id));
  if (it == orders_.end()) return;
  out(ouch50::host::accepted(buf_, impl_.itch_ts(), it->second.enter, shares(o.qty), ref, 'L'));
}

void OuchConn::order_reject(const NewOrder& n, RejectReason why) noexcept {
  if (replacing_ != nullptr || reducing_) return;  // the replace path answers for itself
  const std::uint32_t urn = urn_of(n.cl_ord_id);
  const auto it = orders_.find(urn);
  if (it == orders_.end()) return;
  const std::uint16_t reason =
      why == RejectReason::PostOnlyWouldCross ? kRejectCrossed : kRejectOther;
  reject(urn, reason, it->second.enter.cl_ord_id);
  forget(urn);
}

void OuchConn::order_cancel(const SimOrder& o, CancelReason why) noexcept {
  if (why == CancelReason::Replaced) return;
  const std::uint32_t urn = urn_of(o.cl_ord_id);
  if (urn == 0) return;
  char reason = 'U';
  if (is_expiry(why)) reason = 'I';
  if (why == CancelReason::Stp) reason = 'Q';
  out(ouch50::host::canceled(buf_, impl_.itch_ts(), urn, shares(o.leaves()), reason));
  forget(urn);
}

void OuchConn::order_fill(
    const SimOrder& o, Price px, Qty qty, char liquidity, std::uint64_t match) noexcept {
  const std::uint32_t urn = urn_of(o.cl_ord_id);
  if (urn == 0) return;
  ++impl_.stats.executions;
  std::uint64_t p4 = 0;
  static_cast<void>(nasdaq::price_to_price4(px, p4));
  out(ouch50::host::executed(buf_, impl_.itch_ts(), urn, shares(qty), p4, liquidity, match));
  if (o.leaves().is_zero()) forget(urn);
}

}  // namespace fastmm::sim::itch
