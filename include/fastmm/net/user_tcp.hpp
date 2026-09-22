#pragma once
// UserTcp: a user-space TCP client for one connection to one known peer over raw Ethernet frames
// (order entry without the kernel TCP stack). The engine is a state machine over frames: the
// owner feeds received frames to on_frame(), drives on_timer(), and the engine writes frames
// through a FrameTx. No allocation after construction; single-threaded.
//
// What it implements (RFC 9293 client side):
//   - ARP for the next hop and ARP replies for its own address (the address must not be assigned
//     to a kernel interface, or the kernel answers the peer's segments with RSTs)
//   - active open with the MSS option; no window scaling, SACK or timestamps
//   - receive: segments ahead of rcv_nxt are kept in the receive buffer (up to 16 ranges) and
//     answered with a duplicate ACK; every segment with data or FIN is ACKed at once (no delayed
//     ACK)
//   - sending without Nagle: send() transmits at once unless corked; segments are cut at the
//     peer's MSS and limited by its window and the congestion window
//   - RTO per RFC 6298 (Karn's rule, exponential backoff), go-back-N from snd_una on timeout,
//     fast retransmit on three duplicate ACKs, Reno-style cwnd/ssthresh
//   - zero-window probes, RFC 5961 challenge ACKs for in-window RST and SYN, RST for segments of
//     unknown connections
//   - FIN in both directions (FIN_WAIT_1/2, CLOSING, TIME_WAIT, LAST_ACK); abort() sends RST
// Not implemented: SACK, IP fragments, IP options on send, VLAN tags, IPv6.
//
// Callbacks (UserTcpHandler) run from on_frame() / on_timer() / connect(); a handler may call
// send(), close() or abort() but not connect().
#include "fastmm/core/config_macros.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace fastmm::net {

using MacAddr = std::array<std::uint8_t, 6>;

// Frame output: send_frame() queues one Ethernet frame (false: dropped, e.g. the TX ring is
// full; TCP retransmits), flush() hands the queued frames to the device.
class FrameTx {
 public:
  virtual ~FrameTx() = default;
  virtual bool send_frame(std::span<const std::byte> frame) noexcept = 0;
  virtual void flush() noexcept = 0;
};

class UserTcpHandler {
 public:
  virtual ~UserTcpHandler() = default;
  virtual void on_tcp_connected() noexcept = 0;
  // Bytes received so far and not yet consumed; returns the bytes consumed.
  virtual std::size_t on_tcp_data(std::span<const std::byte> bytes) noexcept = 0;
  // The connection is gone: 0 = the peer closed it (FIN), otherwise ECONNREFUSED, ECONNRESET,
  // ETIMEDOUT (retransmissions or ARP exhausted) or ENOBUFS (receive buffer full).
  virtual void on_tcp_closed(int err) noexcept = 0;
};

struct UserTcpConfig {
  MacAddr local_mac{};
  std::uint32_t local_ip = 0;     // network byte order; not assigned to any kernel interface
  std::uint32_t remote_ip = 0;    // network byte order
  std::uint16_t remote_port = 0;  // host byte order
  std::uint32_t next_hop_ip = 0;  // ARP target; 0 = remote_ip (on-link peer)
  std::uint16_t local_port = 0;   // 0 = pick one per connect() (49152..65535)
  std::uint16_t mtu = 1500;
  std::size_t tx_buffer = std::size_t{1} << 20;  // bytes, power of two
  std::size_t rx_buffer = std::size_t{1} << 20;
  std::int64_t rto_initial_ns = 200'000'000;
  // Linux's default: at least the peer's delayed-ACK timer (up to 200 ms), or segments the peer
  // answers with nothing (heartbeats) are resent early.
  std::int64_t rto_min_ns = 200'000'000;
  std::int64_t rto_max_ns = 2'000'000'000;
  std::uint32_t max_retransmits = 12;  // consecutive RTO expiries before ETIMEDOUT
  std::uint32_t syn_retries = 6;
  std::int64_t arp_interval_ns = 100'000'000;
  std::uint32_t arp_retries = 20;
  std::int64_t time_wait_ns = 1'000'000'000;
  bool verify_checksums = true;  // IPv4 header and TCP checksums of received segments
};

enum class TcpState : std::uint8_t {
  Closed,
  Arp,  // resolving the next hop before the SYN
  SynSent,
  Established,
  FinWait1,
  FinWait2,
  Closing,
  TimeWait,
  LastAck,
};

[[nodiscard]] std::string_view to_string(TcpState s) noexcept;

struct UserTcpStats {
  std::uint64_t frames_in = 0;    // frames given to on_frame()
  std::uint64_t segments_in = 0;  // TCP segments of this connection
  std::uint64_t segments_out = 0;
  std::uint64_t bytes_in = 0;     // payload delivered in order
  std::uint64_t bytes_out = 0;    // payload sent for the first time
  std::uint64_t retransmits = 0;  // segments sent again (RTO and fast retransmit)
  std::uint64_t rto_expiries = 0;
  std::uint64_t fast_retransmits = 0;
  std::uint64_t dup_acks = 0;
  std::uint64_t out_of_order = 0;      // segments ahead of rcv_nxt, dropped
  std::uint64_t bad_frames = 0;        // truncated, bad lengths or checksums
  std::uint64_t unknown_segments = 0;  // for another connection (answered with RST)
  std::uint64_t rst_in = 0;
  std::uint64_t rst_out = 0;
  std::uint64_t challenge_acks = 0;
  std::uint64_t zero_window_probes = 0;
  std::uint64_t tx_drops = 0;  // FrameTx::send_frame refused a frame
  std::uint64_t arp_requests = 0;
  std::uint64_t arp_replies = 0;
  std::int64_t srtt_ns = 0;
  std::int64_t rto_ns = 0;
  std::uint32_t cwnd = 0;
  std::uint32_t snd_wnd = 0;
};

class UserTcp {
 public:
  UserTcp(FrameTx& tx, UserTcpHandler& handler, const UserTcpConfig& cfg);
  ~UserTcp();
  UserTcp(const UserTcp&) = delete;
  UserTcp& operator=(const UserTcp&) = delete;
  UserTcp(UserTcp&&) = delete;
  UserTcp& operator=(UserTcp&&) = delete;

  // Starts a new connection (abandoning any previous one without a segment): ARP for the next
  // hop, then the SYN. `isn` 0 = random. False when the config is unusable.
  bool connect(std::int64_t now_ns, std::uint32_t isn = 0) noexcept;
  // Appends to the send buffer and, unless corked, transmits what the windows allow. False when
  // the connection cannot send (not established, closing) or the buffer is full (the
  // connection is then aborted: on_tcp_closed(ENOBUFS)).
  bool send(std::span<const std::byte> bytes) noexcept;
  void cork() noexcept { corked_ = true; }
  // Transmits what send() queued while corked; false when the connection is not usable.
  bool uncork() noexcept;
  // Graceful close: FIN after the queued data. No callback for it.
  void close() noexcept;
  // RST and forget the connection. No callback.
  void abort() noexcept;

  // One received Ethernet frame. `csum_unverified`: the device says the checksum was not
  // computed (a local sender with checksum offload, e.g. TP_STATUS_CSUMNOTREADY); skip it.
  // Frames produced in response are queued; call flush() after a batch.
  void on_frame(std::span<const std::byte> frame,
                std::int64_t now_ns,
                bool csum_unverified = false) noexcept;
  // Retransmission, persist, ARP and TIME_WAIT timers. Flushes.
  void on_timer(std::int64_t now_ns) noexcept;
  void flush() noexcept { tx_.flush(); }
  // Earliest time on_timer() has something to do; INT64_MAX when idle.
  [[nodiscard]] std::int64_t next_timer_ns() const noexcept;

  [[nodiscard]] TcpState state() const noexcept { return state_; }
  [[nodiscard]] bool established() const noexcept { return state_ == TcpState::Established; }
  [[nodiscard]] const UserTcpStats& stats() const noexcept { return stats_; }
  [[nodiscard]] std::uint16_t local_port() const noexcept { return lport_; }
  [[nodiscard]] std::uint32_t mss() const noexcept { return mss_; }
  // Bytes queued and not yet acknowledged.
  [[nodiscard]] std::size_t unacked() const noexcept { return tx_len_; }
  [[nodiscard]] bool peer_mac_known() const noexcept { return mac_known_; }
  [[nodiscard]] const MacAddr& peer_mac() const noexcept { return peer_mac_; }

 private:
  struct Segment;
  void on_arp(std::span<const std::byte> f) noexcept;
  void on_tcp(const Segment& s) noexcept;
  void on_syn_sent(const Segment& s) noexcept;
  void process_ack(const Segment& s) noexcept;
  void accept_data(const Segment& s) noexcept;
  void deliver() noexcept;
  void add_ooo(std::uint32_t start, std::uint32_t end) noexcept;
  void send_arp(bool reply, const MacAddr& dst_mac, std::uint32_t dst_ip) noexcept;
  void send_syn() noexcept;
  void push() noexcept;
  void send_segment(std::uint32_t seq, std::uint32_t len, std::uint8_t flags) noexcept;
  void send_ack() noexcept;
  void send_rst_for(const Segment& s) noexcept;
  void send_raw(std::uint32_t seq,
                std::uint32_t ack,
                std::uint8_t flags,
                std::uint16_t src_port,
                std::uint16_t dst_port,
                std::uint16_t window) noexcept;
  void retransmit_head() noexcept;
  void arm_rto() noexcept;
  void on_rtt_sample(std::int64_t rtt) noexcept;
  void teardown(int err) noexcept;  // state Closed + on_tcp_closed(err)
  [[nodiscard]] std::uint16_t window() const noexcept;
  [[nodiscard]] std::uint32_t flight() const noexcept { return snd_nxt_ - snd_una_; }
  std::size_t build(std::byte* out,
                    std::uint32_t seq,
                    std::uint32_t ack,
                    std::uint8_t flags,
                    std::uint16_t src_port,
                    std::uint16_t dst_port,
                    std::uint16_t window,
                    std::span<const std::byte> options,
                    std::uint32_t data_seq,
                    std::uint32_t data_len) noexcept;

  FrameTx& tx_;
  UserTcpHandler& h_;
  UserTcpConfig cfg_;
  UserTcpStats stats_{};
  TcpState state_ = TcpState::Closed;
  std::uint64_t gen_ = 0;  // bumped by connect/teardown/abort: detects re-entrant handler calls

  // link layer
  MacAddr peer_mac_{};
  bool mac_known_ = false;
  std::uint32_t arp_tries_ = 0;
  std::int64_t arp_due_ns_ = 0;
  std::uint16_t ip_id_ = 0;

  // TCP
  std::uint16_t lport_ = 0;
  std::uint32_t iss_ = 0;
  std::uint32_t snd_una_ = 0;
  std::uint32_t snd_nxt_ = 0;
  std::uint32_t snd_max_ = 0;  // highest sequence sent + 1
  std::uint32_t snd_wnd_ = 0;
  std::uint32_t snd_wl1_ = 0;
  std::uint32_t snd_wl2_ = 0;
  std::uint32_t irs_ = 0;
  std::uint32_t rcv_nxt_ = 0;
  std::uint32_t mss_ = 536;
  std::uint32_t cwnd_ = 0;
  std::uint32_t ssthresh_ = 0xFFFF'FFFFU;
  std::uint32_t dup_acks_ = 0;
  std::uint32_t retries_ = 0;
  std::uint16_t last_adv_wnd_ = 0;
  bool fin_queued_ = false;  // close() called: FIN goes after the data
  bool fin_sent_ = false;
  std::uint32_t fin_seq_ = 0;
  bool corked_ = false;
  bool ack_pending_ = false;
  bool user_closed_ = false;        // close(): no more callbacks
  bool peer_fin_ = false;           // the peer's FIN was accepted
  bool peer_fin_reported_ = false;  // on_tcp_closed(0) was called for it

  // timers (0 = off)
  std::int64_t now_ns_ = 0;
  std::int64_t rto_due_ns_ = 0;
  std::int64_t persist_due_ns_ = 0;
  std::int64_t persist_backoff_ns_ = 0;
  std::int64_t time_wait_due_ns_ = 0;
  std::int64_t srtt_ = 0;
  std::int64_t rttvar_ = 0;
  std::int64_t rto_ = 0;
  bool timing_ = false;  // an RTT sample is in progress
  std::uint32_t timed_seq_ = 0;
  std::int64_t timed_at_ = 0;

  // buffers
  std::unique_ptr<std::byte[]> txb_;  // ring; the byte of snd_una_ is at tx_head_
  std::size_t tx_mask_ = 0;
  std::size_t tx_head_ = 0;
  std::size_t tx_len_ = 0;  // bytes from snd_una_ (in flight and unsent)
  std::unique_ptr<std::byte[]> rxb_;
  std::size_t rx_cap_ = 0;
  std::size_t rx_len_ = 0;
  struct OooRange {
    std::uint32_t start;
    std::uint32_t end;
  };
  std::array<OooRange, 16> ooo_{};  // received beyond rcv_nxt_, sorted, disjoint
  std::size_t ooo_n_ = 0;
  bool delivering_ = false;
  alignas(64) std::array<std::byte, 2048> frame_{};
};

}  // namespace fastmm::net
