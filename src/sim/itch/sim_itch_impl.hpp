#pragma once
// SimItchServer internals (see include/fastmm/sim/itch/sim_itch_server.hpp).
#include "fastmm/codecs/itch/itch_encoder.hpp"
#include "fastmm/codecs/moldudp/moldudp64.hpp"
#include "fastmm/codecs/ouch/ouch50.hpp"
#include "fastmm/codecs/soupbin/soupbin_session.hpp"
#include "fastmm/core/rng.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/net/reactor.hpp"
#include "fastmm/net/tcp_socket.hpp"
#include "fastmm/net/udp_socket.hpp"
#include "fastmm/sim/itch/itch_publisher.hpp"
#include "fastmm/sim/itch/sim_itch_server.hpp"
#include "fastmm/sim/market_generator.hpp"
#include "fastmm/sim/matching_engine.hpp"

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace fastmm::sim::itch {

using Impl = SimItchServer::Impl;

// Send stamps of the last `capacity` data datagrams, in sequence order.
class StampRing {
 public:
  explicit StampRing(std::size_t capacity) : e_(capacity), mask_(capacity - 1) {}
  void push(std::uint64_t first, std::uint32_t count, std::uint64_t tsc) noexcept {
    e_[n_ & mask_] = Entry{first, count, tsc};
    ++n_;
  }
  // The stamp of the datagram that carried `seq`; 0 when it is not in the ring.
  [[nodiscard]] std::uint64_t find(std::uint64_t seq) const noexcept;

 private:
  struct Entry {
    std::uint64_t first = 0;
    std::uint32_t count = 0;
    std::uint64_t tsc = 0;
  };
  std::vector<Entry> e_;
  std::size_t mask_;
  std::size_t n_ = 0;  // entries pushed
};

// A SoupBinTCP connection: buffered non-blocking writes (the session's ByteWriter) and reads.
class TcpConn : public net::IoHandler {
 public:
  TcpConn(Impl& impl, net::TcpSocket sock);
  ~TcpConn() override = default;
  TcpConn(const TcpConn&) = delete;
  TcpConn& operator=(const TcpConn&) = delete;

  bool send(std::span<const std::byte> bytes) noexcept;
  void on_readable() override;
  void on_writable() override;
  void on_error(int err) override;

  virtual void on_timer(std::int64_t now_ns) noexcept = 0;
  // Closes the socket; the connection stays until Impl::reap() calls release() outside any
  // engine call and destroys it.
  void close() noexcept;
  virtual void release() noexcept {}
  // While corked, send() only queues; uncork() writes the queue.
  void cork() noexcept { corked_ = true; }
  void uncork() noexcept;
  [[nodiscard]] bool dead() const noexcept { return dead_; }
  [[nodiscard]] int fd() const noexcept { return sock_.fd(); }

 protected:
  // One SoupBinTCP packet from the client.
  virtual void on_packet(const codecs::FrameView& f) noexcept = 0;

  Impl& impl_;
  std::uint64_t rx_tsc_ = 0;  // rdtscp right after the read that returned the current bytes

 private:
  void process() noexcept;
  void flush() noexcept;

  net::TcpSocket sock_;
  std::vector<std::byte> in_;
  std::size_t in_used_ = 0;
  std::vector<std::byte> out_;
  std::size_t out_off_ = 0;
  bool writing_ = false;
  bool corked_ = false;
  bool dead_ = false;
};

// Client Unsequenced Data: GLIMPSE has none; OUCH hands it to its connection.
struct GlimpseInbound {
  void on_unsequenced(std::span<const std::byte>) noexcept {}
};
class OuchConn;
struct OuchInbound {
  OuchConn* conn = nullptr;
  void on_unsequenced(std::span<const std::byte> msg) noexcept;
};

class GlimpseConn final : public TcpConn {
 public:
  GlimpseConn(Impl& impl, net::TcpSocket sock, std::int64_t now_ns);
  void on_timer(std::int64_t now_ns) noexcept override;

 private:
  void on_packet(const codecs::FrameView& f) noexcept override;
  void send_snapshot() noexcept;

  GlimpseInbound inbound_;
  codecs::soupbin::ServerSession<TcpConn, GlimpseInbound> session_;
  bool sent_ = false;
};

class OuchConn final : public TcpConn {
 public:
  OuchConn(Impl& impl, net::TcpSocket sock, AccountId account, std::int64_t now_ns);
  void on_unsequenced(std::span<const std::byte> msg) noexcept;
  void on_timer(std::int64_t now_ns) noexcept override;

  // Engine effects on this connection's account (from Publisher).
  void order_ack(const SimOrder& o, std::uint64_t ref) noexcept;
  void order_reject(const NewOrder& n, RejectReason why) noexcept;
  void order_cancel(const SimOrder& o, CancelReason why) noexcept;
  void order_fill(
      const SimOrder& o, Price px, Qty qty, char liquidity, std::uint64_t match) noexcept;

 private:
  struct Order {
    std::size_t symbol = 0;
    ClientOrderId engine_id{};
    codecs::ouch50::EnterOrder enter{};
  };

  void on_packet(const codecs::FrameView& f) noexcept override;
  void release() noexcept override;
  void enter(std::span<const std::byte> msg) noexcept;
  void replace(std::span<const std::byte> msg) noexcept;
  void cancel(std::span<const std::byte> msg) noexcept;
  void reject(std::uint32_t urn, std::uint16_t reason, const char* cl_ord_id14) noexcept;
  void out(std::size_t n) noexcept;
  [[nodiscard]] std::uint32_t urn_of(ClientOrderId engine_id) const noexcept;
  void forget(std::uint32_t urn) noexcept;

  OuchInbound inbound_;
  codecs::soupbin::ServerSession<TcpConn, OuchInbound> session_;
  AccountId account_;
  std::unordered_map<std::uint32_t, Order> orders_;                 // UserRefNum -> order
  std::unordered_map<std::uint64_t, std::uint32_t> urn_by_engine_;  // engine id -> UserRefNum
  std::uint32_t last_urn_ = 0;
  std::uint64_t next_synthetic_ = 1;
  // context of the engine call in progress
  const codecs::ouch50::host::ReplaceView* replacing_ = nullptr;
  bool replaced_ = false;
  bool reducing_ = false;
  bool closing_ = false;
  std::array<std::byte, 128> buf_{};
};

class Listener final : public net::IoHandler {
 public:
  enum class Kind : std::uint8_t { Glimpse, Ouch };
  Listener(Impl& impl, Kind kind) : impl_(impl), kind_(kind) {}
  void on_readable() override;
  void on_writable() override {}
  void on_error(int) override {}

  net::TcpSocket sock;

 private:
  Impl& impl_;
  Kind kind_;
};

class RequestServer final : public net::IoHandler {
 public:
  explicit RequestServer(Impl& impl) : impl_(impl) {}
  void on_readable() override;
  void on_writable() override {}
  void on_error(int) override {}

  net::UdpSocket sock;

 private:
  Impl& impl_;
  std::array<std::byte, 2048> in_{};
  std::array<std::byte, 65536> out_{};
};

struct SimItchServer::Impl final : ItchOutput, ItchPublisher::Client {
  explicit Impl(SimItchConfig c);
  ~Impl() override;
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  bool open();
  bool open_feed();
  bool open_servers();
  void poll(int max_wait_ms);
  void start_day();
  void run_market(std::int64_t now_ns);
  void flush(std::int64_t now_ns, bool force);
  void send_packets(std::size_t k, std::int64_t now_ns);
  void send_control(std::span<const std::byte> datagram);
  void end_session();
  void on_timer(std::int64_t now_ns);
  void reap();
  [[nodiscard]] int wait_ms(int max_wait_ms, std::int64_t now_ns) const;

  // ---- ItchOutput (the publishers) ----
  std::uint64_t itch_timestamp() noexcept override { return itch_ts(); }
  void publish(std::span<const std::byte> msg) noexcept override;
  // ---- ItchPublisher::Client: effects on OUCH accounts, routed to their connection ----
  void on_ack(const SimOrder& o, std::uint64_t ref) noexcept override;
  void on_reject(const NewOrder& n, RejectReason why) noexcept override;
  void on_cancel(const SimOrder& o, CancelReason why) noexcept override;
  void on_fill(
      const SimOrder& o, Price px, Qty qty, char liquidity, std::uint64_t match) noexcept override;

  // ---- used by the connections ----
  void emit(std::size_t n) noexcept;  // publish enc_buf[0, n)
  [[nodiscard]] std::uint64_t itch_ts() const noexcept;
  [[nodiscard]] Timestamp sim_now() const noexcept;
  [[nodiscard]] std::size_t find_symbol(std::string_view symbol) const noexcept;  // npos
  void record_wire_to_wire(std::uint64_t seq, std::uint64_t rx_tsc) noexcept;
  void accept(Listener::Kind kind, net::TcpSocket sock);

  struct Symbol {
    SimItchSymbol cfg;
    std::uint16_t locate = 0;
    std::unique_ptr<ItchPublisher> pub;
    std::unique_ptr<MatchingEngine> engine;
    std::unique_ptr<MarketGenerator> gen;
  };

  SimItchConfig cfg;
  std::string error;
  net::Reactor reactor;
  TscClock clock;
  std::vector<Symbol> symbols;
  std::unordered_map<std::string, std::size_t> by_symbol;
  bool generator_on = true;
  bool started = false;
  bool ended = false;
  bool opened = false;
  std::int64_t steady_start_ns = 0;
  std::int64_t itch_ts_start = 0;  // ITCH timestamp (ns since UTC midnight) at steady_start_ns

  // feed
  codecs::moldudp::Transmitter tx;
  codecs::itch::ItchEncoder enc;
  std::array<std::byte, 128> enc_buf{};
  ItchNumbering numbering;
  net::UdpSocket mcast;
  struct LineOut {
    bool on = false;
    sockaddr_in addr{};
    Xoshiro256ss rng;
    double drop_rate = 0.0;
  };
  std::array<LineOut, 2> lines;
  std::vector<std::byte> pkt_buf;  // burst x max_datagram
  std::vector<std::size_t> pkt_len;
  std::vector<std::uint64_t> pkt_first;
  std::vector<std::uint32_t> pkt_count;
  std::vector<std::uint64_t> pkt_tsc;
  std::vector<std::size_t> pkt_index;  // mmsghdr slot -> packet
  std::vector<mmsghdr> msgs;
  std::vector<iovec> iovs;
  double tokens = 0.0;
  std::int64_t last_refill_ns = 0;
  std::int64_t first_unsent_ns = 0;  // publish time of the oldest unsent message
  std::int64_t last_send_ns = 0;
  StampRing stamps;
  LogLinearHistogram w2w;

  // servers
  RequestServer requests;
  Listener glimpse;
  Listener ouch;
  std::vector<std::unique_ptr<TcpConn>> conns;
  std::array<OuchConn*, kMaxAccounts> accounts{};
  std::int64_t next_timer_ns = 0;

  SimItchStats stats;
};

}  // namespace fastmm::sim::itch
