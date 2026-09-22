// fastmm-sim-itch: engine effects -> ITCH -> MoldUDP64 -> multicast (see sim_itch_server.hpp).
#include "sim_itch_impl.hpp"

#include "fastmm/codecs/itch/itch_messages.hpp"

#include <arpa/inet.h>

#include <cerrno>
#include <cstring>

namespace fastmm::sim::itch {

namespace moldudp = codecs::moldudp;

// ---- StampRing ------------------------------------------------------------------------------

std::uint64_t StampRing::find(std::uint64_t seq) const noexcept {
  const std::size_t cap = e_.size();
  std::size_t lo = n_ > cap ? n_ - cap : 0;
  std::size_t hi = n_;  // search [lo, hi) for the last entry with first <= seq
  if (lo == hi || seq < e_[lo & mask_].first) return 0;
  while (hi - lo > 1) {
    const std::size_t mid = lo + (hi - lo) / 2;
    if (e_[mid & mask_].first <= seq) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  const Entry& e = e_[lo & mask_];
  return seq < e.first + e.count ? e.tsc : 0;
}

// ---- ItchOutput / ItchPublisher::Client ----------------------------------------------------

void Impl::publish(std::span<const std::byte> msg) noexcept {
  if (ended) {
    ++stats.publish_failures;
    return;
  }
  if (tx.next_unsent() > tx.published()) first_unsent_ns = net::Reactor::now_ns();
  if (tx.publish(msg) == 0) {
    ++stats.publish_failures;
    return;
  }
  ++stats.messages;
}

void Impl::emit(std::size_t n) noexcept {
  if (n == 0) {
    ++stats.publish_failures;
    return;
  }
  publish(std::span<const std::byte>(enc_buf.data(), n));
}

void Impl::on_ack(const SimOrder& o, std::uint64_t ref) noexcept {
  if (OuchConn* c = accounts[o.account]) c->order_ack(o, ref);
}
void Impl::on_reject(const NewOrder& n, RejectReason why) noexcept {
  if (OuchConn* c = accounts[n.account]) c->order_reject(n, why);
}
void Impl::on_cancel(const SimOrder& o, CancelReason why) noexcept {
  if (OuchConn* c = accounts[o.account]) c->order_cancel(o, why);
}
void Impl::on_fill(
    const SimOrder& o, Price px, Qty qty, char liquidity, std::uint64_t match) noexcept {
  if (OuchConn* c = accounts[o.account]) c->order_fill(o, px, qty, liquidity, match);
}

// ---- feed -----------------------------------------------------------------------------------

bool Impl::open_feed() {
  mcast = net::UdpSocket::open();
  if (!mcast.valid()) {
    error = std::string("multicast socket: ") + std::strerror(mcast.last_error());
    return false;
  }
  if (!cfg.source.empty()) {
    const auto addr = net::SockAddr::from_ip(cfg.source, 0);
    if (!addr || mcast.bind(*addr) != 0) {
      error = "cannot bind the multicast socket to " + cfg.source;
      return false;
    }
  }
  net::McastInterface ifc;
  if (const int rc = net::McastInterface::resolve(cfg.interface, ifc); rc != 0) {
    error = "unknown multicast interface '" + cfg.interface + "': " + std::strerror(-rc);
    return false;
  }
  if (const int rc = mcast.set_multicast_if(ifc); rc != 0) {
    error = "IP_MULTICAST_IF " + cfg.interface + ": " + std::strerror(-rc);
    return false;
  }
  if (mcast.set_multicast_ttl(cfg.ttl) != 0 || mcast.set_multicast_loop(cfg.loop) != 0) {
    error = "IP_MULTICAST_TTL / IP_MULTICAST_LOOP failed";
    return false;
  }
  if (cfg.sndbuf_bytes > 0) static_cast<void>(mcast.set_sndbuf(cfg.sndbuf_bytes));
  const SimItchLine* src[2] = {&cfg.line_a, &cfg.line_b};
  for (std::size_t i = 0; i < 2; ++i) {
    LineOut& l = lines[i];
    l.on = !src[i]->group.empty();
    if (!l.on) continue;
    std::uint32_t group = 0;
    if (!net::parse_ipv4(src[i]->group, group)) {
      error = "bad multicast group '" + src[i]->group + "'";
      return false;
    }
    l.addr.sin_family = AF_INET;
    l.addr.sin_addr.s_addr = group;
    l.addr.sin_port = htons(src[i]->port);
    l.rng.reseed(cfg.drop_seed + i);
    l.drop_rate = src[i]->drop_rate;
  }
  const std::size_t burst = cfg.burst;
  pkt_buf.assign(burst * cfg.max_datagram, std::byte{0});
  pkt_len.assign(burst, 0);
  pkt_first.assign(burst, 0);
  pkt_count.assign(burst, 0);
  pkt_tsc.assign(burst, 0);
  pkt_index.assign(burst, 0);
  msgs.assign(burst, mmsghdr{});
  iovs.assign(burst, iovec{});
  tokens = static_cast<double>(burst);
  return true;
}

void Impl::flush(std::int64_t now_ns, bool force) {
  const std::uint16_t per_packet = cfg.max_messages == 0
                                       ? moldudp::kMaxMessagesPerPacket
                                       : static_cast<std::uint16_t>(cfg.max_messages);
  while (tx.next_unsent() <= tx.published()) {
    if (!force && cfg.flush_us != 0 &&
        now_ns - first_unsent_ns < static_cast<std::int64_t>(cfg.flush_us) * 1000)
      return;
    std::size_t allowed = cfg.burst;
    if (cfg.packet_rate > 0.0 && !force) {
      tokens += static_cast<double>(now_ns - last_refill_ns) * cfg.packet_rate * 1e-9;
      last_refill_ns = now_ns;
      if (tokens > static_cast<double>(cfg.burst)) tokens = static_cast<double>(cfg.burst);
      if (tokens < 1.0) return;
      allowed = static_cast<std::size_t>(tokens);
      if (allowed > cfg.burst) allowed = cfg.burst;
    }
    std::size_t k = 0;
    while (k < allowed) {
      const std::span<std::byte> buf(pkt_buf.data() + k * cfg.max_datagram, cfg.max_datagram);
      const std::size_t n = tx.next_packet(buf, per_packet);
      if (n == 0) break;
      pkt_len[k] = n;
      pkt_first[k] = codecs::nasdaq::load_be64(buf.data() + 10);
      pkt_count[k] = codecs::nasdaq::load_be16(buf.data() + 18);
      ++k;
    }
    if (k == 0) return;
    if (cfg.packet_rate > 0.0 && !force) tokens -= static_cast<double>(k);
    send_packets(k, now_ns);
    first_unsent_ns = now_ns;
  }
}

void Impl::send_packets(std::size_t k, std::int64_t now_ns) {
  stats.packets += k;
  for (std::size_t i = 0; i < k; ++i) pkt_tsc[i] = 0;
  std::uint64_t fallback = 0;
  for (std::size_t line = 0; line < 2; ++line) {
    LineOut& l = lines[line];
    if (!l.on) continue;
    std::size_t m = 0;
    for (std::size_t i = 0; i < k; ++i) {
      if (l.drop_rate > 0.0 && l.rng.uniform01() < l.drop_rate) {
        ++stats.datagrams_dropped[line];
        continue;
      }
      iovs[m].iov_base = pkt_buf.data() + i * cfg.max_datagram;
      iovs[m].iov_len = pkt_len[i];
      msgs[m] = mmsghdr{};
      msgs[m].msg_hdr.msg_name = &l.addr;
      msgs[m].msg_hdr.msg_namelen = sizeof(l.addr);
      msgs[m].msg_hdr.msg_iov = &iovs[m];
      msgs[m].msg_hdr.msg_iovlen = 1;
      pkt_index[m] = i;
      ++m;
    }
    const std::uint64_t tsc = rdtscp().v;
    if (fallback == 0) fallback = tsc;
    std::size_t done = 0;
    int spins = 0;
    while (done < m) {
      const int rc = mcast.send_batch(std::span<mmsghdr>(msgs.data() + done, m - done));
      if (rc > 0) {
        for (std::size_t j = done; j < done + static_cast<std::size_t>(rc); ++j) {
          if (pkt_tsc[pkt_index[j]] == 0) pkt_tsc[pkt_index[j]] = tsc;
        }
        done += static_cast<std::size_t>(rc);
        stats.datagrams_sent[line] += static_cast<std::uint64_t>(rc);
        continue;
      }
      if ((rc == 0 || rc == -EAGAIN || rc == -ENOBUFS) && ++spins < 1000) continue;
      stats.send_errors += m - done;
      break;
    }
  }
  for (std::size_t i = 0; i < k; ++i)
    stamps.push(pkt_first[i], pkt_count[i], pkt_tsc[i] != 0 ? pkt_tsc[i] : fallback);
  last_send_ns = now_ns;
}

void Impl::send_control(std::span<const std::byte> datagram) {
  for (LineOut& l : lines) {
    if (!l.on) continue;
    net::SockAddr to;
    std::memcpy(&to.storage, &l.addr, sizeof(l.addr));
    to.len = sizeof(l.addr);
    if (!mcast.send_to(datagram, to).ok()) ++stats.send_errors;
  }
}

void Impl::end_session() {
  if (ended || !opened) return;
  flush(net::Reactor::now_ns(), true);
  std::array<std::byte, moldudp::kHeaderLength> buf{};
  const std::size_t n = tx.end_of_session(buf);
  send_control({buf.data(), n});
  ended = true;
}

void Impl::record_wire_to_wire(std::uint64_t seq, std::uint64_t rx_tsc) noexcept {
  ++stats.w2w_tokens;
  const std::uint64_t tx_tsc = stamps.find(seq);
  if (tx_tsc == 0 || rx_tsc < tx_tsc) {
    ++stats.w2w_misses;
    return;
  }
  w2w.record(static_cast<std::uint64_t>(clock.cycles_to_ns(rx_tsc - tx_tsc)));
}

// Opening spin: System Event O, Stock Directory per symbol, S and Q, Trading Action T per symbol,
// then the seeded books.
void Impl::start_day() {
  started = true;
  emit(enc.system_event(enc_buf, itch_ts(), 'O'));
  for (const Symbol& s : symbols)
    emit(enc.stock_directory(enc_buf, s.locate, itch_ts(), s.cfg.symbol));
  emit(enc.system_event(enc_buf, itch_ts(), 'S'));
  emit(enc.system_event(enc_buf, itch_ts(), 'Q'));
  for (const Symbol& s : symbols) {
    codecs::itch::StockTradingAction m{};
    if (!enc.header(m.hdr, 'H', s.locate, itch_ts())) continue;
    codecs::nasdaq::put_alpha(m.stock, sizeof m.stock, s.cfg.symbol);
    m.trading_state = 'T';
    m.reserved = ' ';
    codecs::nasdaq::put_alpha(m.reason, sizeof m.reason, "");
    emit(codecs::itch::ItchEncoder::write(enc_buf, m));
  }
  const Timestamp now = sim_now();
  for (Symbol& s : symbols) {
    if (cfg.generator_enabled && cfg.seed_levels > 0) {
      s.gen->seed_book(*s.engine, cfg.seed_levels, now);
      s.pub->after_call(*s.engine);
    }
  }
}

void Impl::run_market(std::int64_t now_ns) {
  if (!generator_on || !cfg.generator_enabled || ended) return;
  const auto elapsed = static_cast<double>(now_ns - steady_start_ns) * cfg.speed;
  const Timestamp sim = Timestamp{1'000'000'000 + static_cast<std::int64_t>(elapsed)};
  constexpr int kMaxSteps = 4096;  // per symbol and poll, so I/O is served under any rate
  for (Symbol& s : symbols) {
    for (int i = 0; i < kMaxSteps && s.gen->next_ts() <= sim; ++i) {
      s.gen->step(*s.engine);
      s.pub->after_call(*s.engine);
      ++stats.generator_actions;
    }
  }
}

}  // namespace fastmm::sim::itch
