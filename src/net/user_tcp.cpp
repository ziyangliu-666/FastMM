// UserTcp (see user_tcp.hpp).
#include "fastmm/net/user_tcp.hpp"

#include "fastmm/core/time.hpp"

#include <sys/random.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>

namespace fastmm::net {

namespace {

constexpr std::uint8_t kFin = 0x01;
constexpr std::uint8_t kSyn = 0x02;
constexpr std::uint8_t kRst = 0x04;
constexpr std::uint8_t kPsh = 0x08;
constexpr std::uint8_t kAck = 0x10;

constexpr std::size_t kEth = 14;
constexpr std::size_t kIp = 20;
constexpr std::size_t kTcp = 20;
constexpr std::size_t kMinFrame = 60;
constexpr std::uint16_t kEtherIpv4 = 0x0800;
constexpr std::uint16_t kEtherArp = 0x0806;
constexpr std::uint16_t kMaxWindow = 0xFFFF;

[[nodiscard]] constexpr bool seq_lt(std::uint32_t a, std::uint32_t b) noexcept {
  return static_cast<std::int32_t>(a - b) < 0;
}
[[nodiscard]] constexpr bool seq_leq(std::uint32_t a, std::uint32_t b) noexcept {
  return static_cast<std::int32_t>(a - b) <= 0;
}
[[nodiscard]] constexpr bool seq_gt(std::uint32_t a, std::uint32_t b) noexcept {
  return static_cast<std::int32_t>(a - b) > 0;
}
[[nodiscard]] constexpr bool seq_geq(std::uint32_t a, std::uint32_t b) noexcept {
  return static_cast<std::int32_t>(a - b) >= 0;
}

[[nodiscard]] inline std::uint16_t get16(const std::byte* p) noexcept {
  return static_cast<std::uint16_t>((std::to_integer<unsigned>(p[0]) << 8U) |
                                    std::to_integer<unsigned>(p[1]));
}
[[nodiscard]] inline std::uint32_t get32(const std::byte* p) noexcept {
  return (std::to_integer<std::uint32_t>(p[0]) << 24U) |
         (std::to_integer<std::uint32_t>(p[1]) << 16U) |
         (std::to_integer<std::uint32_t>(p[2]) << 8U) | std::to_integer<std::uint32_t>(p[3]);
}
// Four bytes as they sit in memory: a network-byte-order address.
[[nodiscard]] inline std::uint32_t get_raw32(const std::byte* p) noexcept {
  std::uint32_t v = 0;
  std::memcpy(&v, p, 4);
  return v;
}
inline void put16(std::byte* p, std::uint16_t v) noexcept {
  p[0] = static_cast<std::byte>(v >> 8U);
  p[1] = static_cast<std::byte>(v & 0xFFU);
}
inline void put32(std::byte* p, std::uint32_t v) noexcept {
  p[0] = static_cast<std::byte>(v >> 24U);
  p[1] = static_cast<std::byte>((v >> 16U) & 0xFFU);
  p[2] = static_cast<std::byte>((v >> 8U) & 0xFFU);
  p[3] = static_cast<std::byte>(v & 0xFFU);
}
inline void put_raw32(std::byte* p, std::uint32_t v) noexcept {
  std::memcpy(p, &v, 4);
}

// One's-complement sum of big-endian 16-bit words, not folded.
[[nodiscard]] inline std::uint64_t csum_add(std::uint64_t sum,
                                            const std::byte* p,
                                            std::size_t n) noexcept {
  std::size_t i = 0;
  for (; i + 1 < n; i += 2) sum += get16(p + i);
  if (i < n) sum += std::to_integer<std::uint64_t>(p[i]) << 8U;
  return sum;
}
[[nodiscard]] inline std::uint16_t csum_fold(std::uint64_t sum) noexcept {
  while ((sum >> 16U) != 0) sum = (sum & 0xFFFFU) + (sum >> 16U);
  return static_cast<std::uint16_t>(sum);
}
// Pseudo-header sum for TCP over IPv4 (addresses as they sit in memory).
[[nodiscard]] inline std::uint64_t pseudo_sum(const std::byte* src4,
                                              const std::byte* dst4,
                                              std::size_t tcp_len) noexcept {
  std::uint64_t s = csum_add(0, src4, 4);
  s = csum_add(s, dst4, 4);
  return s + 6U + tcp_len;
}

std::uint32_t random32() noexcept {
  std::uint32_t v = 0;
  if (::getrandom(&v, sizeof v, GRND_NONBLOCK) != static_cast<ssize_t>(sizeof v)) {
    v = static_cast<std::uint32_t>(rdtscp().v * 0x9E37'79B9'7F4A'7C15ULL >> 32U);
  }
  return v;
}

}  // namespace

std::string_view to_string(TcpState s) noexcept {
  switch (s) {
    case TcpState::Closed:
      return "closed";
    case TcpState::Arp:
      return "arp";
    case TcpState::SynSent:
      return "syn_sent";
    case TcpState::Established:
      return "established";
    case TcpState::FinWait1:
      return "fin_wait_1";
    case TcpState::FinWait2:
      return "fin_wait_2";
    case TcpState::Closing:
      return "closing";
    case TcpState::TimeWait:
      return "time_wait";
    case TcpState::LastAck:
      return "last_ack";
  }
  return "?";
}

struct UserTcp::Segment {
  const std::byte* src_mac = nullptr;
  std::uint32_t src_ip = 0;  // network byte order
  std::uint16_t sport = 0;
  std::uint16_t dport = 0;
  std::uint32_t seq = 0;
  std::uint32_t ack = 0;
  std::uint8_t flags = 0;
  std::uint16_t wnd = 0;
  std::uint16_t mss = 0;  // MSS option, 0 when absent
  std::span<const std::byte> data;

  [[nodiscard]] bool has(std::uint8_t f) const noexcept { return (flags & f) != 0; }
  [[nodiscard]] std::uint32_t len() const noexcept {
    return static_cast<std::uint32_t>(data.size()) + (has(kSyn) ? 1U : 0U) + (has(kFin) ? 1U : 0U);
  }
};

UserTcp::UserTcp(FrameTx& tx, UserTcpHandler& handler, const UserTcpConfig& cfg)
    : tx_(tx), h_(handler), cfg_(cfg) {
  std::size_t cap = 4096;
  while (cap < cfg_.tx_buffer) cap <<= 1U;
  txb_.reset(new std::byte[cap]);
  tx_mask_ = cap - 1;
  rx_cap_ = std::max<std::size_t>(cfg_.rx_buffer, 4096);
  rxb_.reset(new std::byte[rx_cap_]);
  if (cfg_.next_hop_ip == 0) cfg_.next_hop_ip = cfg_.remote_ip;
  if (cfg_.mtu < 576) cfg_.mtu = 576;
  cfg_.mtu = std::min<std::uint16_t>(cfg_.mtu, 2048 - kEth);
  rto_ = cfg_.rto_initial_ns;
}

UserTcp::~UserTcp() = default;

// ---- frame building --------------------------------------------------------------------------

std::size_t UserTcp::build(std::byte* out,
                           std::uint32_t seq,
                           std::uint32_t ack,
                           std::uint8_t flags,
                           std::uint16_t src_port,
                           std::uint16_t dst_port,
                           std::uint16_t wnd,
                           std::span<const std::byte> options,
                           std::uint32_t data_seq,
                           std::uint32_t data_len) noexcept {
  const std::size_t thl = kTcp + options.size();
  const std::size_t total = kIp + thl + data_len;
  // Ethernet
  std::memcpy(out, peer_mac_.data(), 6);
  std::memcpy(out + 6, cfg_.local_mac.data(), 6);
  put16(out + 12, kEtherIpv4);
  // IPv4
  std::byte* ip = out + kEth;
  ip[0] = std::byte{0x45};
  ip[1] = std::byte{0};
  put16(ip + 2, static_cast<std::uint16_t>(total));
  put16(ip + 4, ip_id_++);
  put16(ip + 6, 0x4000);  // DF
  ip[8] = std::byte{64};
  ip[9] = std::byte{6};
  put16(ip + 10, 0);
  put_raw32(ip + 12, cfg_.local_ip);
  put_raw32(ip + 16, cfg_.remote_ip);
  put16(ip + 10, static_cast<std::uint16_t>(~csum_fold(csum_add(0, ip, kIp))));
  // TCP
  std::byte* t = ip + kIp;
  put16(t, src_port);
  put16(t + 2, dst_port);
  put32(t + 4, seq);
  put32(t + 8, ack);
  t[12] = static_cast<std::byte>((thl / 4) << 4U);
  t[13] = static_cast<std::byte>(flags);
  put16(t + 14, wnd);
  put16(t + 16, 0);
  put16(t + 18, 0);
  if (!options.empty()) std::memcpy(t + kTcp, options.data(), options.size());
  if (data_len != 0) {
    const std::size_t at = (tx_head_ + (data_seq - snd_una_)) & tx_mask_;
    const std::size_t first = std::min<std::size_t>(data_len, tx_mask_ + 1 - at);
    std::memcpy(t + thl, txb_.get() + at, first);
    if (first < data_len) std::memcpy(t + thl + first, txb_.get(), data_len - first);
  }
  const std::size_t tcp_len = thl + data_len;
  const std::uint64_t sum = csum_add(pseudo_sum(ip + 12, ip + 16, tcp_len), t, tcp_len);
  put16(t + 16, static_cast<std::uint16_t>(~csum_fold(sum)));
  std::size_t n = kEth + total;
  if (n < kMinFrame) {
    std::memset(out + n, 0, kMinFrame - n);
    n = kMinFrame;
  }
  return n;
}

void UserTcp::send_raw(std::uint32_t seq,
                       std::uint32_t ack,
                       std::uint8_t flags,
                       std::uint16_t src_port,
                       std::uint16_t dst_port,
                       std::uint16_t wnd) noexcept {
  const std::size_t n = build(frame_.data(), seq, ack, flags, src_port, dst_port, wnd, {}, 0, 0);
  ++stats_.segments_out;
  if (!tx_.send_frame(std::span<const std::byte>(frame_.data(), n))) ++stats_.tx_drops;
}

void UserTcp::send_segment(std::uint32_t seq, std::uint32_t len, std::uint8_t flags) noexcept {
  const std::uint16_t wnd = window();
  const std::size_t n =
      build(frame_.data(), seq, rcv_nxt_, flags, lport_, cfg_.remote_port, wnd, {}, seq, len);
  last_adv_wnd_ = wnd;
  ack_pending_ = false;
  ++stats_.segments_out;
  if (!tx_.send_frame(std::span<const std::byte>(frame_.data(), n))) ++stats_.tx_drops;
}

void UserTcp::send_ack() noexcept {
  send_segment(snd_nxt_, 0, kAck);
}

void UserTcp::send_syn() noexcept {
  state_ = TcpState::SynSent;
  const auto mss = static_cast<std::uint16_t>(cfg_.mtu - kIp - kTcp);
  const std::byte opt[4] = {std::byte{2},
                            std::byte{4},
                            static_cast<std::byte>(mss >> 8U),
                            static_cast<std::byte>(mss & 0xFFU)};
  const std::uint16_t wnd = window();
  const std::size_t n =
      build(frame_.data(), iss_, 0, kSyn, lport_, cfg_.remote_port, wnd, opt, 0, 0);
  last_adv_wnd_ = wnd;
  ++stats_.segments_out;
  if (!tx_.send_frame(std::span<const std::byte>(frame_.data(), n))) ++stats_.tx_drops;
  snd_nxt_ = iss_ + 1;
  snd_max_ = iss_ + 1;
  timed_at_ = now_ns_;  // RTT sample from the SYN-ACK unless the SYN was retransmitted
  arm_rto();
}

void UserTcp::send_arp(bool reply, const MacAddr& dst_mac, std::uint32_t dst_ip) noexcept {
  std::byte* f = frame_.data();
  static constexpr MacAddr kBroadcast{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  std::memcpy(f, reply ? dst_mac.data() : kBroadcast.data(), 6);
  std::memcpy(f + 6, cfg_.local_mac.data(), 6);
  put16(f + 12, kEtherArp);
  std::byte* a = f + kEth;
  put16(a, 1);  // Ethernet
  put16(a + 2, kEtherIpv4);
  a[4] = std::byte{6};
  a[5] = std::byte{4};
  put16(a + 6, reply ? 2 : 1);
  std::memcpy(a + 8, cfg_.local_mac.data(), 6);
  put_raw32(a + 14, cfg_.local_ip);
  if (reply) {
    std::memcpy(a + 18, dst_mac.data(), 6);
  } else {
    std::memset(a + 18, 0, 6);
  }
  put_raw32(a + 24, dst_ip);
  std::memset(f + kEth + 28, 0, kMinFrame - kEth - 28);
  if (reply) {
    ++stats_.arp_replies;
  } else {
    ++stats_.arp_requests;
  }
  if (!tx_.send_frame(std::span<const std::byte>(f, kMinFrame))) ++stats_.tx_drops;
}

// ---- API ---------------------------------------------------------------------------------------

bool UserTcp::connect(std::int64_t now_ns, std::uint32_t isn) noexcept {
  if (cfg_.local_ip == 0 || cfg_.remote_ip == 0 || cfg_.remote_port == 0) return false;
  ++gen_;
  now_ns_ = now_ns;
  lport_ = cfg_.local_port != 0 ? cfg_.local_port
                                : static_cast<std::uint16_t>(49152U + (random32() % 16384U));
  iss_ = isn != 0 ? isn : random32();
  snd_una_ = iss_;
  snd_nxt_ = iss_;
  snd_max_ = iss_;
  snd_wnd_ = 0;
  snd_wl1_ = 0;
  snd_wl2_ = 0;
  irs_ = 0;
  rcv_nxt_ = 0;
  mss_ = 536;
  cwnd_ = 0;
  ssthresh_ = 0xFFFF'FFFFU;
  dup_acks_ = 0;
  retries_ = 0;
  fin_queued_ = false;
  fin_sent_ = false;
  fin_seq_ = 0;
  corked_ = false;
  ack_pending_ = false;
  user_closed_ = false;
  peer_fin_ = false;
  peer_fin_reported_ = false;
  rto_due_ns_ = 0;
  persist_due_ns_ = 0;
  persist_backoff_ns_ = 0;
  time_wait_due_ns_ = 0;
  srtt_ = 0;
  rttvar_ = 0;
  rto_ = cfg_.rto_initial_ns;
  timing_ = false;
  tx_head_ = 0;
  tx_len_ = 0;
  rx_len_ = 0;
  ooo_n_ = 0;
  // Every connection resolves the next hop again (it may have moved since the last one).
  mac_known_ = false;
  state_ = TcpState::Arp;
  arp_tries_ = 1;
  arp_due_ns_ = now_ns + cfg_.arp_interval_ns;
  send_arp(false, peer_mac_, cfg_.next_hop_ip);
  tx_.flush();
  return true;
}

bool UserTcp::send(std::span<const std::byte> bytes) noexcept {
  if (state_ != TcpState::Established || fin_queued_) return false;
  if (bytes.size() > tx_mask_ + 1 - tx_len_) {
    abort();
    h_.on_tcp_closed(ENOBUFS);
    return false;
  }
  const std::size_t at = (tx_head_ + tx_len_) & tx_mask_;
  const std::size_t first = std::min(bytes.size(), tx_mask_ + 1 - at);
  std::memcpy(txb_.get() + at, bytes.data(), first);
  if (first < bytes.size()) std::memcpy(txb_.get(), bytes.data() + first, bytes.size() - first);
  tx_len_ += bytes.size();
  if (!corked_) {
    push();
    tx_.flush();
  }
  return true;
}

bool UserTcp::uncork() noexcept {
  corked_ = false;
  if (state_ != TcpState::Established) return false;
  push();
  tx_.flush();
  return true;
}

void UserTcp::close() noexcept {
  switch (state_) {
    case TcpState::Arp:
    case TcpState::SynSent:
      ++gen_;
      state_ = TcpState::Closed;
      rto_due_ns_ = 0;
      return;
    case TcpState::Established:
      user_closed_ = true;
      fin_queued_ = true;
      state_ = TcpState::FinWait1;
      corked_ = false;
      push();
      tx_.flush();
      return;
    default:
      user_closed_ = true;
      return;
  }
}

void UserTcp::abort() noexcept {
  if (state_ != TcpState::Closed && state_ != TcpState::Arp && state_ != TcpState::SynSent &&
      state_ != TcpState::TimeWait) {
    send_raw(snd_nxt_, 0, kRst, lport_, cfg_.remote_port, 0);
    ++stats_.rst_out;
    tx_.flush();
  }
  ++gen_;
  state_ = TcpState::Closed;
  rto_due_ns_ = 0;
  persist_due_ns_ = 0;
  time_wait_due_ns_ = 0;
}

void UserTcp::teardown(int err) noexcept {
  const bool notify = !user_closed_ && !peer_fin_reported_;
  ++gen_;
  state_ = TcpState::Closed;
  rto_due_ns_ = 0;
  persist_due_ns_ = 0;
  time_wait_due_ns_ = 0;
  if (notify) h_.on_tcp_closed(err);
}

std::uint16_t UserTcp::window() const noexcept {
  const std::size_t free = rx_cap_ - rx_len_;
  return static_cast<std::uint16_t>(std::min<std::size_t>(free, kMaxWindow));
}

void UserTcp::arm_rto() noexcept {
  rto_due_ns_ = now_ns_ + rto_;
}

void UserTcp::on_rtt_sample(std::int64_t rtt) noexcept {
  if (rtt <= 0) rtt = 1;
  if (srtt_ == 0) {
    srtt_ = rtt;
    rttvar_ = rtt / 2;
  } else {
    const std::int64_t err = srtt_ > rtt ? srtt_ - rtt : rtt - srtt_;
    rttvar_ = (3 * rttvar_ + err) / 4;
    srtt_ = (7 * srtt_ + rtt) / 8;
  }
  // As Linux: the minimum is added to the smoothed RTT, so a peer that delays its ACKs by up to
  // rto_min (heartbeats it does not answer) does not draw retransmissions.
  rto_ = std::min(srtt_ + std::max(4 * rttvar_, cfg_.rto_min_ns), cfg_.rto_max_ns);
  stats_.srtt_ns = srtt_;
}

// Sends what the peer's window and the congestion window allow, from snd_nxt_ on; then the FIN
// once every data byte has been sent.
void UserTcp::push() noexcept {
  if (state_ == TcpState::Closed || state_ == TcpState::Arp || state_ == TcpState::SynSent ||
      state_ == TcpState::TimeWait)
    return;
  const std::uint32_t data_end = snd_una_ + static_cast<std::uint32_t>(tx_len_);
  bool sent = false;
  for (;;) {
    if (seq_lt(snd_nxt_, data_end)) {
      const std::uint32_t unsent = data_end - snd_nxt_;
      const std::uint32_t edge = snd_una_ + snd_wnd_;
      const std::uint32_t usable = seq_gt(edge, snd_nxt_) ? edge - snd_nxt_ : 0;
      const std::uint32_t fl = flight();
      const std::uint32_t cwnd_room = cwnd_ > fl ? cwnd_ - fl : 0;
      const std::uint32_t n = std::min({unsent, mss_, usable, cwnd_room});
      if (n == 0) {
        if (usable == 0 && fl == 0 && persist_due_ns_ == 0) {
          persist_backoff_ns_ = rto_;
          persist_due_ns_ = now_ns_ + persist_backoff_ns_;
        }
        break;
      }
      const bool last = n == unsent;
      send_segment(snd_nxt_, n, last ? static_cast<std::uint8_t>(kAck | kPsh) : kAck);
      sent = true;
      const std::uint32_t end = snd_nxt_ + n;
      if (seq_lt(snd_nxt_, snd_max_)) {
        ++stats_.retransmits;
      } else if (!timing_) {
        timing_ = true;
        timed_seq_ = end;
        timed_at_ = now_ns_;
      }
      if (seq_gt(end, snd_max_)) {
        stats_.bytes_out += end - snd_max_;
        snd_max_ = end;
      }
      snd_nxt_ = end;
      if (rto_due_ns_ == 0) arm_rto();
      continue;
    }
    if (fin_queued_ && snd_nxt_ == (fin_sent_ ? fin_seq_ : data_end)) {
      if (!fin_sent_) {
        fin_sent_ = true;
        fin_seq_ = data_end;
      } else {
        ++stats_.retransmits;
      }
      send_segment(snd_nxt_, 0, static_cast<std::uint8_t>(kFin | kAck));
      sent = true;
      snd_nxt_ = fin_seq_ + 1;
      if (seq_gt(snd_nxt_, snd_max_)) snd_max_ = snd_nxt_;
      if (rto_due_ns_ == 0) arm_rto();
    }
    break;
  }
  if (sent) persist_due_ns_ = 0;
}

// ---- receive -----------------------------------------------------------------------------------

void UserTcp::on_frame(std::span<const std::byte> f,
                       std::int64_t now_ns,
                       bool csum_unverified) noexcept {
  now_ns_ = now_ns;
  ++stats_.frames_in;
  if (f.size() < kEth) {
    ++stats_.bad_frames;
    return;
  }
  const std::uint16_t type = get16(f.data() + 12);
  if (type == kEtherArp) {
    on_arp(f);
    return;
  }
  if (type != kEtherIpv4) return;
  if (f.size() < kEth + kIp) {
    ++stats_.bad_frames;
    return;
  }
  const std::byte* ip = f.data() + kEth;
  const unsigned ver_ihl = std::to_integer<unsigned>(ip[0]);
  const std::size_t ihl = (ver_ihl & 0x0FU) * 4U;
  const std::size_t total = get16(ip + 2);
  if ((ver_ihl >> 4U) != 4 || ihl < kIp || total < ihl + kTcp || kEth + total > f.size()) {
    ++stats_.bad_frames;
    return;
  }
  if (std::to_integer<unsigned>(ip[9]) != 6) return;
  if (get_raw32(ip + 16) != cfg_.local_ip) return;
  if ((get16(ip + 6) & 0x3FFFU) != 0) {  // fragment
    ++stats_.bad_frames;
    return;
  }
  const std::byte* t = ip + ihl;
  const std::size_t tcp_len = total - ihl;
  const std::size_t thl = (std::to_integer<unsigned>(t[12]) >> 4U) * 4U;
  if (thl < kTcp || thl > tcp_len) {
    ++stats_.bad_frames;
    return;
  }
  if (cfg_.verify_checksums) {
    const bool ip_ok = csum_fold(csum_add(0, ip, ihl)) == 0xFFFF;
    const bool tcp_ok =
        csum_unverified ||
        csum_fold(csum_add(pseudo_sum(ip + 12, ip + 16, tcp_len), t, tcp_len)) == 0xFFFF;
    if (!ip_ok || !tcp_ok) {
      ++stats_.bad_frames;
      return;
    }
  }
  Segment s;
  s.src_mac = f.data() + 6;
  s.src_ip = get_raw32(ip + 12);
  s.sport = get16(t);
  s.dport = get16(t + 2);
  s.seq = get32(t + 4);
  s.ack = get32(t + 8);
  s.flags = static_cast<std::uint8_t>(std::to_integer<unsigned>(t[13]) & 0x3FU);
  s.wnd = get16(t + 14);
  if (s.has(kSyn)) {
    for (std::size_t i = kTcp; i < thl;) {
      const unsigned kind = std::to_integer<unsigned>(t[i]);
      if (kind == 0) break;
      if (kind == 1) {
        ++i;
        continue;
      }
      if (i + 1 >= thl) break;
      const std::size_t len = std::to_integer<std::size_t>(t[i + 1]);
      if (len < 2 || i + len > thl) break;
      if (kind == 2 && len == 4) s.mss = get16(t + i + 2);
      i += len;
    }
  }
  s.data = std::span<const std::byte>(t + thl, tcp_len - thl);
  const bool ours = state_ != TcpState::Closed && state_ != TcpState::Arp &&
                    s.src_ip == cfg_.remote_ip && s.sport == cfg_.remote_port && s.dport == lport_;
  if (!ours) {
    ++stats_.unknown_segments;
    send_rst_for(s);
    return;
  }
  ++stats_.segments_in;
  on_tcp(s);
}

void UserTcp::on_arp(std::span<const std::byte> f) noexcept {
  if (f.size() < kEth + 28) return;
  const std::byte* a = f.data() + kEth;
  if (get16(a) != 1 || get16(a + 2) != kEtherIpv4 || std::to_integer<unsigned>(a[4]) != 6 ||
      std::to_integer<unsigned>(a[5]) != 4)
    return;
  const std::uint16_t op = get16(a + 6);
  MacAddr sha{};
  std::memcpy(sha.data(), a + 8, 6);
  const std::uint32_t spa = get_raw32(a + 14);
  const std::uint32_t tpa = get_raw32(a + 24);
  if (spa == cfg_.next_hop_ip && spa != 0) {
    const bool first = !mac_known_;
    peer_mac_ = sha;
    mac_known_ = true;
    if (first && state_ == TcpState::Arp) send_syn();
  }
  if (op == 1 && tpa == cfg_.local_ip) send_arp(true, sha, spa);
}

void UserTcp::send_rst_for(const Segment& s) noexcept {
  // Frames are built for the peer's address and MAC: other senders get no answer.
  if (s.has(kRst) || !mac_known_ || s.src_ip != cfg_.remote_ip) return;
  if (s.has(kAck)) {
    send_raw(s.ack, 0, kRst, s.dport, s.sport, 0);
  } else {
    send_raw(0, s.seq + s.len(), static_cast<std::uint8_t>(kRst | kAck), s.dport, s.sport, 0);
  }
  ++stats_.rst_out;
}

void UserTcp::on_syn_sent(const Segment& s) noexcept {
  if (s.has(kAck) && (seq_leq(s.ack, iss_) || seq_gt(s.ack, snd_max_))) {
    if (!s.has(kRst)) {
      send_raw(s.ack, 0, kRst, lport_, cfg_.remote_port, 0);
      ++stats_.rst_out;
    }
    return;
  }
  if (s.has(kRst)) {
    if (s.has(kAck)) {
      ++stats_.rst_in;
      teardown(ECONNREFUSED);
    }
    return;
  }
  if (!s.has(kSyn) || !s.has(kAck)) return;  // simultaneous open is not supported
  irs_ = s.seq;
  rcv_nxt_ = s.seq + 1;
  snd_una_ = s.ack;
  if (seq_lt(snd_nxt_, snd_una_)) snd_nxt_ = snd_una_;
  const auto own = static_cast<std::uint32_t>(cfg_.mtu - kIp - kTcp);
  mss_ = std::min<std::uint32_t>(s.mss != 0 ? s.mss : 536U, own);
  if (mss_ < 64) mss_ = 64;
  cwnd_ = 10 * mss_;
  snd_wnd_ = s.wnd;  // never scaled: no window scale option was offered
  snd_wl1_ = s.seq;
  snd_wl2_ = s.ack;
  if (retries_ == 0) on_rtt_sample(now_ns_ - timed_at_);
  timing_ = false;
  retries_ = 0;
  rto_due_ns_ = 0;
  state_ = TcpState::Established;
  send_ack();
  stats_.cwnd = cwnd_;
  stats_.snd_wnd = snd_wnd_;
  stats_.rto_ns = rto_;
  const std::uint64_t gen = gen_;
  h_.on_tcp_connected();
  if (gen != gen_) return;
  // Data carried by the SYN-ACK is not accepted (rcv_nxt stays at irs + 1): the peer resends it.
  if (tx_len_ != 0) push();
}

void UserTcp::on_tcp(const Segment& s) noexcept {
  if (state_ == TcpState::SynSent) {
    on_syn_sent(s);
    return;
  }
  // Sequence acceptability (RFC 9293 3.10.7.4).
  const std::uint32_t wnd = window();
  const std::uint32_t len = s.len();
  bool ok = false;
  if (len == 0) {
    ok = wnd == 0 ? s.seq == rcv_nxt_ : seq_geq(s.seq, rcv_nxt_) && seq_lt(s.seq, rcv_nxt_ + wnd);
  } else if (wnd != 0) {
    const std::uint32_t last = s.seq + len - 1;
    ok = (seq_geq(s.seq, rcv_nxt_) && seq_lt(s.seq, rcv_nxt_ + wnd)) ||
         (seq_geq(last, rcv_nxt_) && seq_lt(last, rcv_nxt_ + wnd));
  }
  if (!ok) {
    if (!s.has(kRst)) send_ack();
    return;
  }
  if (s.has(kRst)) {
    if (s.seq == rcv_nxt_) {
      ++stats_.rst_in;
      teardown(ECONNRESET);
    } else {
      ++stats_.challenge_acks;
      send_ack();
    }
    return;
  }
  if (s.has(kSyn)) {
    ++stats_.challenge_acks;
    send_ack();
    return;
  }
  if (!s.has(kAck)) return;
  if (seq_gt(s.ack, snd_max_)) {
    send_ack();
    return;
  }
  process_ack(s);
  if (state_ == TcpState::Closed) return;
  const std::uint64_t gen = gen_;
  if (state_ == TcpState::Established || state_ == TcpState::FinWait1 ||
      state_ == TcpState::FinWait2) {
    accept_data(s);
  }
  if (gen != gen_ || state_ == TcpState::Closed) return;
  push();
  if (ack_pending_) send_ack();
  if (rx_len_ != 0 && !delivering_) deliver();
  if (gen != gen_) return;
  if (peer_fin_ && !peer_fin_reported_) {
    peer_fin_reported_ = true;
    if (!user_closed_) h_.on_tcp_closed(0);
  }
}

void UserTcp::process_ack(const Segment& s) noexcept {
  if (seq_gt(s.ack, snd_una_)) {
    const std::uint32_t acked = s.ack - snd_una_;
    const bool fin_acked = fin_sent_ && seq_gt(s.ack, fin_seq_);
    const std::uint32_t data_acked = acked - (fin_acked ? 1U : 0U);
    tx_head_ = (tx_head_ + data_acked) & tx_mask_;
    tx_len_ -= std::min<std::size_t>(data_acked, tx_len_);
    snd_una_ = s.ack;
    if (seq_lt(snd_nxt_, snd_una_)) snd_nxt_ = snd_una_;
    if (timing_ && seq_geq(s.ack, timed_seq_)) {
      timing_ = false;
      on_rtt_sample(now_ns_ - timed_at_);
    }
    retries_ = 0;
    dup_acks_ = 0;
    if (cwnd_ < ssthresh_) {
      cwnd_ += std::min(acked, mss_);
    } else {
      cwnd_ += std::max<std::uint32_t>(1, mss_ * mss_ / std::max<std::uint32_t>(cwnd_, 1));
    }
    cwnd_ = std::min<std::uint32_t>(cwnd_, static_cast<std::uint32_t>(tx_mask_ + 1));
    if (snd_una_ == snd_max_) {
      rto_due_ns_ = 0;
    } else {
      arm_rto();
    }
    if (fin_acked) {
      switch (state_) {
        case TcpState::FinWait1:
          state_ = TcpState::FinWait2;
          break;
        case TcpState::Closing:
          state_ = TcpState::TimeWait;
          time_wait_due_ns_ = now_ns_ + cfg_.time_wait_ns;
          rto_due_ns_ = 0;
          break;
        case TcpState::LastAck:
          teardown(0);  // silent: EOF was reported when the peer's FIN arrived
          return;
        default:
          break;
      }
    }
  } else if (s.ack == snd_una_ && s.data.empty() && !s.has(kFin) && s.wnd == snd_wnd_ &&
             snd_max_ != snd_una_) {
    ++stats_.dup_acks;
    if (++dup_acks_ == 3) {
      ++stats_.fast_retransmits;
      ++stats_.retransmits;
      ssthresh_ = std::max(flight() / 2, 2 * mss_);
      cwnd_ = ssthresh_;
      timing_ = false;
      const auto n = static_cast<std::uint32_t>(std::min<std::size_t>(tx_len_, mss_));
      std::uint8_t flags = kAck;
      if (fin_sent_ && snd_una_ + n == fin_seq_) flags |= kFin;
      if (n != 0 || (flags & kFin) != 0) send_segment(snd_una_, n, flags);
      arm_rto();
    }
  }
  if (seq_geq(s.ack, snd_una_) &&
      (seq_lt(snd_wl1_, s.seq) || (snd_wl1_ == s.seq && seq_leq(snd_wl2_, s.ack)))) {
    snd_wnd_ = s.wnd;
    snd_wl1_ = s.seq;
    snd_wl2_ = s.ack;
    if (snd_wnd_ != 0) persist_due_ns_ = 0;
  }
  stats_.cwnd = cwnd_;
  stats_.snd_wnd = snd_wnd_;
  stats_.rto_ns = rto_;
}

void UserTcp::accept_data(const Segment& s) noexcept {
  std::uint32_t seq = s.seq;
  std::span<const std::byte> data = s.data;
  bool fin = s.has(kFin);
  if (seq_lt(seq, rcv_nxt_)) {
    const std::uint32_t skip = rcv_nxt_ - seq;
    if (skip > data.size()) {  // nothing new (an old FIN at most)
      ack_pending_ = true;
      return;
    }
    data = data.subspan(skip);
    seq += skip;
  }
  const std::size_t room = rx_cap_ - rx_len_;
  if (seq != rcv_nxt_) {
    // Ahead of rcv_nxt: stored where it belongs (the buffer maps rcv_nxt_ to rx_len_), its range
    // remembered; a duplicate ACK tells the peer about the hole. A FIN here is dropped.
    ++stats_.out_of_order;
    ack_pending_ = true;
    const std::size_t off = seq - rcv_nxt_;
    if (off >= room || data.empty()) return;
    const std::size_t n = std::min(data.size(), room - off);
    add_ooo(seq, seq + static_cast<std::uint32_t>(n));
    std::memcpy(rxb_.get() + rx_len_ + off, data.data(), n);
    return;
  }
  const std::size_t take = std::min(room, data.size());
  if (take != 0) {
    std::memcpy(rxb_.get() + rx_len_, data.data(), take);
    rx_len_ += take;
    rcv_nxt_ += static_cast<std::uint32_t>(take);
    stats_.bytes_in += take;
  }
  if (take != data.size()) fin = false;  // beyond the window: the peer resends the rest
  if (!data.empty() || fin) ack_pending_ = true;
  // Stored ranges the new bytes reach are in order now.
  while (ooo_n_ != 0 && seq_leq(ooo_[0].start, rcv_nxt_)) {
    if (seq_gt(ooo_[0].end, rcv_nxt_)) {
      const std::uint32_t more = ooo_[0].end - rcv_nxt_;
      rx_len_ += more;
      rcv_nxt_ += more;
      stats_.bytes_in += more;
      fin = false;  // a FIN is only taken at the end of the in-order data
    }
    std::copy(ooo_.begin() + 1, ooo_.begin() + ooo_n_, ooo_.begin());
    --ooo_n_;
  }
  if (!fin) return;
  rcv_nxt_ += 1;
  switch (state_) {
    case TcpState::Established:
      // The peer closed: FIN back after whatever is queued, then wait for its ACK.
      peer_fin_ = true;
      fin_queued_ = true;
      state_ = TcpState::LastAck;
      break;
    case TcpState::FinWait1:
      // Our FIN is not acknowledged yet (process_ack would have moved to FIN_WAIT_2).
      peer_fin_ = true;
      state_ = TcpState::Closing;
      break;
    case TcpState::FinWait2:
      peer_fin_ = true;
      state_ = TcpState::TimeWait;
      time_wait_due_ns_ = now_ns_ + cfg_.time_wait_ns;
      rto_due_ns_ = 0;
      break;
    default:
      break;
  }
}

// Inserts [start, end) into the sorted, disjoint out-of-order ranges, merging what touches it.
// With every slot taken the range is not remembered (its bytes arrive again).
void UserTcp::add_ooo(std::uint32_t start, std::uint32_t end) noexcept {
  std::size_t i = 0;
  while (i < ooo_n_ && seq_lt(ooo_[i].end, start)) ++i;
  std::size_t j = i;
  while (j < ooo_n_ && seq_leq(ooo_[j].start, end)) {
    if (seq_lt(ooo_[j].start, start)) start = ooo_[j].start;
    if (seq_gt(ooo_[j].end, end)) end = ooo_[j].end;
    ++j;
  }
  if (j == i) {  // no overlap: insert at i
    if (ooo_n_ == ooo_.size()) return;
    std::copy_backward(ooo_.begin() + i, ooo_.begin() + ooo_n_, ooo_.begin() + ooo_n_ + 1);
    ++ooo_n_;
  } else {  // ranges i..j-1 become one
    std::copy(ooo_.begin() + j, ooo_.begin() + ooo_n_, ooo_.begin() + i + 1);
    ooo_n_ -= j - i - 1;
  }
  ooo_[i] = OooRange{start, end};
}

void UserTcp::deliver() noexcept {
  if (user_closed_) {  // closed by the owner: no more callbacks, the bytes are dropped
    rx_len_ = 0;
    ooo_n_ = 0;
    return;
  }
  delivering_ = true;
  const std::uint64_t gen = gen_;
  const std::uint16_t before = window();
  const std::size_t used = h_.on_tcp_data(std::span<const std::byte>(rxb_.get(), rx_len_));
  delivering_ = false;
  if (gen != gen_) return;
  if (used > 0) {
    const std::size_t n = std::min(used, rx_len_);
    // Out-of-order bytes past rx_len_ move with the rest.
    const std::size_t extent = ooo_n_ == 0 ? rx_len_ : rx_len_ + (ooo_[ooo_n_ - 1].end - rcv_nxt_);
    std::memmove(rxb_.get(), rxb_.get() + n, extent - n);
    rx_len_ -= n;
  }
  if (rx_len_ == rx_cap_) {
    abort();
    h_.on_tcp_closed(ENOBUFS);
    return;
  }
  // Window update when the advertised window was small and the application made room.
  if (before < mss_ && window() >= 2 * mss_ && state_ != TcpState::Closed) send_ack();
}

// ---- timers ------------------------------------------------------------------------------------

std::int64_t UserTcp::next_timer_ns() const noexcept {
  std::int64_t t = INT64_MAX;
  if (state_ == TcpState::Arp) t = std::min(t, arp_due_ns_);
  if (rto_due_ns_ != 0) t = std::min(t, rto_due_ns_);
  if (persist_due_ns_ != 0) t = std::min(t, persist_due_ns_);
  if (time_wait_due_ns_ != 0) t = std::min(t, time_wait_due_ns_);
  return t;
}

void UserTcp::on_timer(std::int64_t now_ns) noexcept {
  now_ns_ = now_ns;
  if (state_ == TcpState::Arp && now_ns >= arp_due_ns_) {
    if (arp_tries_ >= cfg_.arp_retries) {
      teardown(ETIMEDOUT);
      return;
    }
    ++arp_tries_;
    arp_due_ns_ = now_ns + cfg_.arp_interval_ns;
    send_arp(false, peer_mac_, cfg_.next_hop_ip);
  }
  if (rto_due_ns_ != 0 && now_ns >= rto_due_ns_) {
    ++stats_.rto_expiries;
    rto_due_ns_ = 0;
    timing_ = false;
    rto_ = std::min(rto_ * 2, cfg_.rto_max_ns);
    stats_.rto_ns = rto_;
    if (state_ == TcpState::SynSent) {
      if (retries_ >= cfg_.syn_retries) {
        teardown(ETIMEDOUT);
        return;
      }
      ++retries_;
      ++stats_.retransmits;
      send_syn();
    } else if (retries_ >= cfg_.max_retransmits) {
      const bool notify = !user_closed_ && !peer_fin_reported_;
      abort();
      if (notify) h_.on_tcp_closed(ETIMEDOUT);
      return;
    } else {
      ++retries_;
      ssthresh_ = std::max(flight() / 2, 2 * mss_);
      cwnd_ = mss_;
      dup_acks_ = 0;
      snd_nxt_ = snd_una_;  // go back N: resend from the first unacknowledged byte
      push();
      if (rto_due_ns_ == 0 && snd_una_ != snd_max_) arm_rto();
      stats_.cwnd = cwnd_;
    }
  }
  if (persist_due_ns_ != 0 && now_ns >= persist_due_ns_) {
    // Zero-window probe: an old sequence number makes the peer answer with its window.
    ++stats_.zero_window_probes;
    send_raw(snd_una_ - 1, rcv_nxt_, kAck, lport_, cfg_.remote_port, window());
    persist_backoff_ns_ = std::min(persist_backoff_ns_ * 2, cfg_.rto_max_ns);
    persist_due_ns_ = now_ns + persist_backoff_ns_;
  }
  if (time_wait_due_ns_ != 0 && now_ns >= time_wait_due_ns_) {
    time_wait_due_ns_ = 0;
    ++gen_;
    state_ = TcpState::Closed;
  }
  tx_.flush();
}

}  // namespace fastmm::net
