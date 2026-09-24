#pragma once
// NasdaqItchVenue (kind = "nasdaq_itch", ADR-0015 section 5): Nasdaq TotalView-ITCH 5.0 over
// MoldUDP64 multicast, GLIMPSE 5.0 for the initial book, and optionally OUCH 5.0 order entry to
// fastmm-sim-itch.
//
// Market data, all on the venue's network thread:
//
//   Kernel | Xdp | DpdkDatagramSource             lines A and B      (rx_backend)
//     -> moldudp::Receiver<RxHandler, ItchRxMeta> A/B arbitration, reorder, gap timeout,
//                                                 re-requests to `rerequest` (line 2)
//     -> RecoveryBuffer                           while waiting for a snapshot
//     -> codecs::itch::ItchL2Bridge               one L3Book per instrument -> BookDelta / Trade
//
// Every datagram gets an ItchRxMeta: T0 (rdtscp of the receive batch), recv_ts (kernel receive
// time, the NIC time with hw_clock = "phc_synced", the T0 wall clock without either) and a
// datagram number. Messages drained later from the reorder buffer keep their datagram's meta; the
// bridge's end_datagram() runs whenever the datagram number changes and after every packet, so
// each datagram yields at most one BookDeltaMsg per instrument with that datagram's T0. The
// kernel-to-T0 hop (t0_wall_ns - sw_ts_ns) is recorded per datagram.
//
// Startup and recovery: multicast is joined at connect() and messages are buffered (at most
// recovery_buffer_packets datagrams, allocated at connect) while a GLIMPSE snapshot is taken over
// SoupBinTCP. The snapshot builds the directory and the L3 books; the buffered messages from End
// of Snapshot's sequence number on are applied, the books are marked complete (BookSnapshot per
// instrument, ConnectionState Live) and later messages are applied as they arrive. Where the
// buffer does not reach back to that sequence number the receiver is reset to it and the hole is
// re-requested (without a re-request server the snapshot is taken again). The same procedure runs
// after an unrecoverable gap and after any L3 inconsistency (a bridge book error: an unknown or
// duplicate order reference cannot be repaired from later messages). The buffer overflowing while
// a snapshot is taken restarts it; overflowing twice in a row stops the feed (FeedState::Lost, the
// venue's kill switch with KillReason::FeedLost). A failed GLIMPSE connection is retried after
// one second.
//
// Without glimpse_url the receiver starts at sequence 1 and the books are complete from the first
// message: the venue must start before the directory spin. An unrecoverable gap then stops the
// feed.
//
// Polling: in [engine] spin_mode = "busy" the sources are polled from poll() on every loop
// iteration and are not registered with the reactor; otherwise the reactor waits on their
// descriptors. poll() also runs the receiver's gap timer and the session timers every 0.5 ms.
//
// Order entry: order_entry = "none" rejects every order (RejectReason::VenueReject). "sim_ouch"
// sends OUCH 5.0 over SoupBinTCP to fastmm-sim-itch: the decoder turns Accepted, Replaced,
// Canceled, Executed and Rejected into order events. A new order's ClOrdID is the sequence token
// of the first ITCH message of the receive batch that triggered it (ouch50::put_seq_token): the
// engine copies only t0_cycles into outbound messages, so the venue keeps a table from each
// batch's t0_cycles to that sequence number. Orders triggered by anything else (timers, fills,
// snapshots) carry the regular ClOrdID. The simulator cancels an account's orders when its
// connection closes, so a lost OUCH connection is reported as a reconciliation with no open
// orders; cancel_all() shuts the connection down from the calling thread for the same effect.
//
// The OUCH connection runs on a kernel TCP socket (TcpLink, TCP_NODELAY). Kernel bypass for it is
// OpenOnload or NVIDIA XLIO through LD_PRELOAD (docs/how-to/operations/low-latency-tcp.md).
//
// Market data may be unicast: a line's "group" is then a local address the simulator sends to (no
// IGMP). With dpdk_exception_port (a net_tap vdev) the kernel keeps an interface on the DPDK port
// for everything the venue does not take itself (ARP, GLIMPSE, re-requests, IGMP, kernel TCP).
#include "fastmm/codecs/itch/glimpse.hpp"
#include "fastmm/codecs/itch/itch_l2_bridge.hpp"
#include "fastmm/codecs/moldudp/moldudp64.hpp"
#include "fastmm/codecs/ouch/ouch50.hpp"
#include "fastmm/codecs/soupbin/soupbin_session.hpp"
#include "fastmm/config/config.hpp"
#include "fastmm/core/latency.hpp"
#include "fastmm/core/seqlock.hpp"
#include "fastmm/net/dpdk_datagram_source.hpp"
#include "fastmm/net/kernel_datagram_source.hpp"
#include "fastmm/net/udp_socket.hpp"
#include "fastmm/net/xdp_datagram_source.hpp"
#include "fastmm/venues/nasdaq/recovery_buffer.hpp"
#include "fastmm/venues/nasdaq/tcp_link.hpp"
#include "fastmm/venues/order_commands.hpp"
#include "fastmm/venues/venue.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm::venues::nasdaq {

enum class RxBackend : std::uint8_t { Kernel = 0, AfXdp = 1, Dpdk = 2 };
enum class OrderEntry : std::uint8_t { None = 0, SimOuch = 1 };
enum class HwClock : std::uint8_t { None = 0, PhcSynced = 1 };

struct ItchLine {
  std::string interface;  // name or IPv4 address; "" = routing table (kernel backend)
  std::string group;      // "" disables the line (B only)
  std::uint16_t port = 0;
  std::string source;  // "" = any-source join
};

struct NasdaqItchVenueConfig {
  std::string name = "nasdaq_itch";
  RxBackend rx_backend = RxBackend::Kernel;
  std::array<ItchLine, 2> lines;
  std::vector<std::uint32_t> queues;  // af_xdp RX queues on every interface (empty: all)
  net::XdpMode xdp_mode = net::XdpMode::Auto;
  int rcvbuf_bytes = 0;             // kernel: SO_RCVBUF, 0 = system default
  std::uint32_t batch = 32;         // datagrams per recvmmsg (kernel) / RX descriptors per poll
  std::string dpdk_eal_args;        // dpdk: space-separated rte_eal_init arguments
  std::string dpdk_port;            // dpdk: ethdev name; "" = the first port
  std::string dpdk_exception_port;  // dpdk: net_tap vdev for the kernel's traffic; "" = none
  std::string dpdk_exception_ip;    // dpdk: "a.b.c.d/len" for its interface
  std::int64_t dpdk_exception_interval_ns = 20'000;
  bool busy_poll = false;   // [engine] spin_mode = "busy"
  std::string rerequest;    // "ip:port" of the MoldUDP64 re-request server; "" = none
  std::string glimpse_url;  // "ip:port" of GLIMPSE 5.0; "" = start at sequence 1
  std::string glimpse_username = "glimps";
  std::string glimpse_password = "glimpse";
  std::uint32_t reorder_packets = 256;
  std::int64_t gap_timeout_ns = 2'000'000;
  std::uint32_t max_request_attempts = 4;
  std::int64_t request_timeout_ns = 250'000'000;
  std::uint32_t recovery_buffer_packets = 65536;
  std::uint32_t depth = 20;
  std::size_t price_window_ticks = std::size_t{1} << 16;  // per side, units of 0.0001
  std::size_t max_orders = std::size_t{1} << 18;          // per instrument
  bool hw_timestamps = false;  // SIOCSHWTSTAMP on the line interfaces (kernel backend)
  HwClock hw_clock = HwClock::None;
  OrderEntry order_entry = OrderEntry::None;
  std::string ouch_url;  // "ip:port" (sim_ouch)
  std::string ouch_username = "fmouch";
  std::string ouch_password = "ouch";
  bool dry_run = false;  // no order entry
};

class NasdaqItchVenue final : public Venue {
 public:
  static constexpr std::size_t kMaxDatagram = 2048;
  static constexpr std::uint8_t kRetransmissionLine = 2;

  NasdaqItchVenue(VenueId id, NasdaqItchVenueConfig cfg);
  ~NasdaqItchVenue() override;
  NasdaqItchVenue(const NasdaqItchVenue&) = delete;
  NasdaqItchVenue& operator=(const NasdaqItchVenue&) = delete;
  NasdaqItchVenue(NasdaqItchVenue&&) = delete;
  NasdaqItchVenue& operator=(NasdaqItchVenue&&) = delete;

  // ---- Venue ---------------------------------------------------------------------------------
  [[nodiscard]] VenueId id() const noexcept override { return id_; }
  [[nodiscard]] std::string_view name() const noexcept override { return cfg_.name; }
  [[nodiscard]] VenueCaps caps() const noexcept override;
  Result<void, std::string> load_reference_data(InstrumentTable& instruments) override;
  void attach(const SymbolTable& symbols,
              const InstrumentTable& instruments,
              EventSink& md_sink,
              EventSink& order_sink,
              MsgRing* outbound) override;
  // Opens the sources, the re-request socket and (with glimpse_url) the first GLIMPSE
  // connection. Throws std::runtime_error when a source cannot be opened.
  void connect(net::Reactor& reactor) override;
  void disconnect() override;
  void subscribe(std::span<const InstrumentId> instruments) override;
  void on_timer(std::int64_t now_ns) override;
  void on_wake() override;
  void send_now(std::span<const EventHeader* const> batch) override;
  void poll() noexcept override;
  void request_open_orders() override;
  bool cancel_all() override;
  [[nodiscard]] VenueStatus status() const noexcept override;

  // ---- introspection (tests) ----------------------------------------------------------------
  [[nodiscard]] FeedState feed_state() const noexcept { return feed_state_; }
  [[nodiscard]] const codecs::itch::ItchL2Bridge* bridge() const noexcept { return bridge_.get(); }
  [[nodiscard]] std::uint64_t next_sequence() const noexcept;
  [[nodiscard]] bool ouch_up() const noexcept { return ouch_up_; }
  [[nodiscard]] const NasdaqItchVenueConfig& config() const noexcept { return cfg_; }

 private:
  struct RxHandler {
    NasdaqItchVenue* v;
    void on_message(std::uint64_t seq,
                    std::span<const std::byte> msg,
                    const ItchRxMeta& meta) noexcept {
      v->on_message(seq, msg, meta);
    }
    void send_request(std::span<const std::byte> bytes) noexcept { v->send_request(bytes); }
    void on_end_of_session() noexcept { v->on_end_of_session(); }
    void on_gap_unrecoverable(std::uint64_t from, std::uint64_t count) noexcept {
      v->on_gap_unrecoverable(from, count);
    }
  };
  using Receiver = codecs::moldudp::Receiver<RxHandler, ItchRxMeta>;

  // Readiness of a datagram source or the re-request socket (adaptive mode).
  struct SourceIo final : net::IoHandler {
    NasdaqItchVenue* v = nullptr;
    void on_readable() override { v->drain_sources(); }
    void on_writable() override {}
    void on_error(int) override {}
  };
  struct RerequestIo final : net::IoHandler {
    NasdaqItchVenue* v = nullptr;
    void on_readable() override { v->drain_rerequest(); }
    void on_writable() override {}
    void on_error(int) override {}
  };
  struct GlimpseLink final : TcpLinkHandler {
    NasdaqItchVenue* v = nullptr;
    void on_link_up() noexcept override { v->on_glimpse_up(); }
    std::size_t on_link_data(std::span<const std::byte> b) noexcept override {
      return v->on_glimpse_data(b);
    }
    void on_link_down(int err) noexcept override { v->on_glimpse_down(err); }
  };
  struct OuchLink final : TcpLinkHandler {
    NasdaqItchVenue* v = nullptr;
    void on_link_up() noexcept override { v->on_ouch_up(); }
    std::size_t on_link_data(std::span<const std::byte> b) noexcept override {
      return v->on_ouch_data(b);
    }
    void on_link_down(int err) noexcept override { v->on_ouch_down(err); }
  };
  struct SnapshotHandler {
    NasdaqItchVenue* v;
    void on_snapshot_message(std::span<const std::byte> msg) noexcept {
      v->on_snapshot_message(msg);
    }
    void on_snapshot_end(std::uint64_t seq) noexcept { v->on_snapshot_end(seq); }
  };
  using Glimpse = codecs::itch::glimpse::GlimpseClient<TcpLink, SnapshotHandler>;
  using OuchSession = codecs::soupbin::ClientSession<TcpLink>;

  // t0_cycles of a receive batch -> sequence number of its first message (the order token).
  struct TokenSlot {
    std::uint64_t t0 = 0;
    std::uint64_t seq = 0;
  };
  static constexpr std::size_t kTokenSlots = 4096;

  // feed
  void open_sources();
  void drain_sources() noexcept;
  void drain_rerequest() noexcept;
  void on_datagram(std::span<const std::byte> payload, const net::RxMeta& m) noexcept;
  void after_packet() noexcept;
  void on_message(std::uint64_t seq,
                  std::span<const std::byte> msg,
                  const ItchRxMeta& meta) noexcept;
  void apply(std::uint64_t seq, std::span<const std::byte> msg, const ItchRxMeta& meta) noexcept;
  void send_request(std::span<const std::byte> bytes) noexcept;
  void on_end_of_session() noexcept;
  void on_gap_unrecoverable(std::uint64_t from, std::uint64_t count) noexcept;
  void start_recovery(std::string_view why) noexcept;
  void start_snapshot() noexcept;
  void finish_snapshot(std::uint64_t next_seq) noexcept;
  void go_live(std::uint64_t live_from) noexcept;
  void feed_lost(std::string_view why) noexcept;
  void service(std::int64_t now) noexcept;
  [[nodiscard]] static codecs::itch::DatagramStamp stamp_of(const ItchRxMeta& m) noexcept {
    return {m.t0_cycles, Timestamp{m.recv_ns}};
  }

  // GLIMPSE
  void on_glimpse_up() noexcept;
  std::size_t on_glimpse_data(std::span<const std::byte> bytes) noexcept;
  void on_glimpse_down(int err) noexcept;
  void on_snapshot_message(std::span<const std::byte> msg) noexcept;
  void on_snapshot_end(std::uint64_t seq) noexcept;

  // OUCH
  void open_ouch() noexcept;
  void on_ouch_up() noexcept;
  std::size_t on_ouch_data(std::span<const std::byte> bytes) noexcept;
  void on_ouch_down(int err) noexcept;
  void ouch_lost() noexcept;
  // Encodes and writes the orders `ring` holds (the outbound MsgRing or an OutboundBatch).
  template <class Ring>
  void write_orders(Ring& ring);
  void send_command(const OrderCommand& cmd) noexcept;
  // uncork() failed: the batch never left, so its orders are rejected (see BatchedOrders).
  void fail_batch() noexcept;
  void refuse(const OrderCommand& cmd, RejectReason reason, std::string_view why) noexcept;
  void emit_empty_reconcile() noexcept;
  [[nodiscard]] std::uint64_t token_for(Cycles t0) const noexcept;
  void note_token(Cycles t0, std::uint64_t seq) noexcept;

  void publish_status() noexcept;
  [[nodiscard]] static std::int64_t now_ns() noexcept { return net::Reactor::now_ns(); }

  VenueId id_;
  NasdaqItchVenueConfig cfg_;
  const SymbolTable* symbols_ = nullptr;
  const InstrumentTable* instruments_ = nullptr;
  EventSink* md_sink_ = nullptr;
  EventSink* order_sink_ = nullptr;
  MsgRing* outbound_ = nullptr;
  net::Reactor* reactor_ = nullptr;
  bool connected_ = false;
  std::vector<InstrumentId> subscribed_;

  // market data
  std::unique_ptr<codecs::itch::ItchL2Bridge> bridge_;
  std::unique_ptr<net::KernelDatagramSource> kernel_;
  std::unique_ptr<net::XdpDatagramSource> xdp_;
  std::unique_ptr<net::DpdkDatagramSource> dpdk_;
  std::vector<int> registered_fds_;
  SourceIo source_io_;
  RerequestIo rerequest_io_;
  net::UdpSocket rerequest_;
  std::unique_ptr<std::byte[]> rerequest_buf_;
  RxHandler rx_handler_{this};
  std::unique_ptr<Receiver> rx_;
  RecoveryBuffer recovery_;
  FeedState feed_state_ = FeedState::Down;
  std::uint64_t live_from_ = 0;      // messages below are older than the books
  std::uint64_t datagrams_ = 0;      // ItchRxMeta::datagram counter
  std::uint64_t open_datagram_ = 0;  // datagram the bridge has pending changes of
  bool datagram_open_ = false;
  std::uint64_t lost_end_ = 0;  // end of the last sequence range given up
  std::uint64_t book_errors_seen_ = 0;
  std::uint32_t overflow_streak_ = 0;  // consecutive snapshots ended by a full buffer
  bool mark_pending_ = false;          // mark_all_complete() failed (sink full)
  bool first_snapshot_done_ = false;
  std::uint64_t snapshots_ = 0;
  std::uint64_t resyncs_ = 0;
  std::uint64_t recovered_ = 0;
  std::uint64_t bytes_ = 0;
  LogLinearHistogram kernel_to_t0_;
  std::array<TokenSlot, kTokenSlots> tokens_{};

  // GLIMPSE
  GlimpseLink glimpse_link_;
  std::unique_ptr<TcpLink> glimpse_tcp_;
  SnapshotHandler snapshot_handler_{this};
  std::optional<Glimpse> glimpse_;
  net::SockAddr glimpse_addr_{};
  std::int64_t snapshot_retry_ns_ = 0;  // 0: none scheduled
  std::int64_t snapshot_started_ns_ = 0;

  // OUCH
  OuchLink ouch_link_;
  std::unique_ptr<TcpLink> ouch_tcp_;
  std::optional<OuchSession> ouch_session_;
  net::SockAddr ouch_addr_{};
  std::unique_ptr<codecs::ouch50::UserRefMap> ouch_ids_;
  std::unique_ptr<codecs::ouch50::OuchEncoder> ouch_encoder_;
  std::unique_ptr<codecs::ouch50::OuchDecoder> ouch_decoder_;
  bool ouch_up_ = false;
  std::int64_t ouch_retry_ns_ = 0;
  std::atomic<bool> cancel_all_done_{false};
  std::array<std::byte, 256> order_buf_{};
  SentWatermark sent_;
  BatchedOrders batch_;  // orders written into the corked OUCH link

  // timers (poll())
  std::int64_t next_service_ns_ = 0;
  std::int64_t next_session_timer_ns_ = 0;
  std::int64_t next_xdp_stats_ns_ = 0;

  bool venue_kill_sent_ = false;
  WireLatencyRecorder wire_;
  VenueStatus stats_{};
  Seqlocked<VenueStatus> published_{};
};

// Builds a NasdaqItchVenueConfig from a [venues.<name>] section (keys: docs/reference/
// configuration.md, "nasdaq_itch"). Throws std::invalid_argument for bad values.
NasdaqItchVenueConfig make_nasdaq_itch_config(const VenueSection& v, bool dry_run, bool busy_poll);

// "a.b.c.d:port" -> address; false for anything else.
[[nodiscard]] bool parse_ip_port(std::string_view text, net::SockAddr& out) noexcept;

}  // namespace fastmm::venues::nasdaq
