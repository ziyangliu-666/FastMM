// NasdaqItchVenue (see nasdaq_itch_venue.hpp).
#include "fastmm/venues/nasdaq/nasdaq_itch_venue.hpp"

#include "fastmm/codecs/itch/nasdaq_fields.hpp"
#include "fastmm/core/log.hpp"
#include "fastmm/venues/decimal.hpp"
#include "fastmm/venues/order_events.hpp"

#include <fmt/format.h>

#include <ifaddrs.h>
#include <netinet/in.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <stdexcept>

namespace fastmm::venues::nasdaq {

namespace {

namespace glimpse = codecs::itch::glimpse;
namespace ouch50 = codecs::ouch50;

// True when a kernel interface in this network namespace has the IPv4 address (network order).
bool host_has_address(std::uint32_t ip) noexcept {
  ifaddrs* list = nullptr;
  if (::getifaddrs(&list) != 0) return false;
  bool found = false;
  for (const ifaddrs* a = list; a != nullptr && !found; a = a->ifa_next) {
    if (a->ifa_addr == nullptr || a->ifa_addr->sa_family != AF_INET) continue;
    found = reinterpret_cast<const sockaddr_in*>(a->ifa_addr)->sin_addr.s_addr == ip;
  }
  ::freeifaddrs(list);
  return found;
}
namespace soupbin = codecs::soupbin;
namespace nasdaq_fields = codecs::nasdaq;

constexpr std::int64_t kServiceIntervalNs = 500'000;   // poll(): receiver and retry timers
constexpr std::int64_t kSessionTimerNs = 100'000'000;  // SoupBinTCP heartbeats, status
constexpr std::int64_t kRetryNs = 1'000'000'000;       // GLIMPSE / OUCH reconnects
constexpr std::int64_t kXdpStatsNs = 1'000'000'000;
constexpr std::int64_t kNsPerDay = 86'400'000'000'000;
constexpr std::size_t kLinkRxBytes = std::size_t{1} << 20;
constexpr std::size_t kLinkTxBytes = std::size_t{1} << 20;

Timestamp utc_midnight() noexcept {
  const std::int64_t now = wall_now().ns;
  return Timestamp{now - now % kNsPerDay};
}

template <class T>
std::string decimal(T v) {
  char buf[48];
  return std::string(buf, v.to_decimal(buf));
}

bool parse_u64(std::string_view s, std::uint64_t& out) noexcept {
  if (s.empty()) return false;
  const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
  return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}

std::string_view backend_name(RxBackend b) noexcept {
  switch (b) {
    case RxBackend::Kernel:
      return "kernel";
    case RxBackend::AfXdp:
      return "af_xdp";
    case RxBackend::Dpdk:
      return "dpdk";
  }
  return "?";
}

std::vector<std::string> split_args(std::string_view text) {
  std::vector<std::string> out;
  std::size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && text[i] == ' ') ++i;
    const std::size_t j = std::min(text.find(' ', i), text.size());
    if (j > i) out.emplace_back(text.substr(i, j - i));
    i = j;
  }
  return out;
}

}  // namespace

bool parse_ip_port(std::string_view text, net::SockAddr& out) noexcept {
  const std::size_t colon = text.rfind(':');
  if (colon == std::string_view::npos || colon == 0) return false;
  std::uint64_t port = 0;
  if (!parse_u64(text.substr(colon + 1), port) || port == 0 || port > 65535) return false;
  const auto a = net::SockAddr::from_ip(text.substr(0, colon), static_cast<std::uint16_t>(port));
  if (!a.has_value() || a->family() != AF_INET) return false;
  out = *a;
  return true;
}

// ---- construction, reference data, wiring ----------------------------------------------------

NasdaqItchVenue::NasdaqItchVenue(VenueId id, NasdaqItchVenueConfig cfg)
    : id_(id), cfg_(std::move(cfg)) {
  source_io_.v = this;
  rerequest_io_.v = this;
  glimpse_link_.v = this;
  ouch_link_.v = this;
  if (cfg_.dry_run) cfg_.order_entry = OrderEntry::None;
  if (!cfg_.glimpse_url.empty() && !parse_ip_port(cfg_.glimpse_url, glimpse_addr_))
    throw std::invalid_argument(cfg_.name + ": glimpse_url must be ip:port");
  if (cfg_.order_entry == OrderEntry::SimOuch && !parse_ip_port(cfg_.ouch_url, ouch_addr_))
    throw std::invalid_argument(cfg_.name + ": ouch_url must be ip:port");
  glimpse_tcp_ = std::make_unique<TcpLink>(glimpse_link_, kLinkRxBytes, kLinkTxBytes);
  if (cfg_.order_entry == OrderEntry::SimOuch) {
    if (cfg_.order_transport == OrderTransport::UserTcp) {
      UserTcpLinkConfig uc;
      uc.interface = cfg_.user_tcp_interface;
      if (!net::parse_ipv4(cfg_.user_tcp_ip, uc.local_ip))
        throw std::invalid_argument(cfg_.name + ": user_tcp_ip must be an IPv4 address");
      if (!cfg_.user_tcp_gateway.empty() && !net::parse_ipv4(cfg_.user_tcp_gateway, uc.gateway))
        throw std::invalid_argument(cfg_.name + ": user_tcp_gateway must be an IPv4 address");
      uc.local_port = cfg_.user_tcp_port;
      uc.register_fd = !cfg_.busy_poll;
      // af_xdp on the host's own address: the kernel keeps the ARP traffic.
      if (cfg_.rx_backend == RxBackend::AfXdp && host_has_address(uc.local_ip)) {
        if (uc.local_port == 0)
          throw std::invalid_argument(
              cfg_.name + ": user_tcp_ip is a local address: user_tcp_port must be set");
        uc.neighbour_mac = true;
      }
      if (cfg_.rx_backend == RxBackend::Dpdk && cfg_.user_tcp_port == 0 &&
          !cfg_.dpdk_exception_ip.empty() &&
          cfg_.dpdk_exception_ip.substr(0, cfg_.dpdk_exception_ip.find('/')) == cfg_.user_tcp_ip)
        throw std::invalid_argument(
            cfg_.name + ": user_tcp_ip is the exception interface's address: set user_tcp_port");
      auto link = std::make_unique<UserTcpLink>(ouch_link_, uc);
      user_tcp_ = link.get();
      ouch_tcp_ = std::move(link);
    } else {
      ouch_tcp_ = std::make_unique<TcpLink>(ouch_link_, kLinkRxBytes, kLinkTxBytes);
    }
    ouch_ids_ = std::make_unique<ouch50::UserRefMap>(1);
    ouch_encoder_ = std::make_unique<ouch50::OuchEncoder>(*ouch_ids_);
    ouch_decoder_ = std::make_unique<ouch50::OuchDecoder>(ouch_ids_.get(), id_);
    ouch_decoder_->set_midnight(utc_midnight());
  }
  stats_.feed.state = FeedState::Down;
  stats_.feed.backend = static_cast<std::uint8_t>(cfg_.rx_backend);
  published_.store(stats_);
}

NasdaqItchVenue::~NasdaqItchVenue() {
  // The links unregister from the reactor in their destructors; the sources first.
  if (reactor_ != nullptr) {
    for (const int fd : registered_fds_) reactor_->remove(fd);
    if (rerequest_.valid()) reactor_->remove(rerequest_.fd());
  }
}

VenueCaps NasdaqItchVenue::caps() const noexcept {
  VenueCaps c;
  const bool orders = cfg_.order_entry == OrderEntry::SimOuch;
  c.supports_replace = orders;
  c.supports_post_only = true;
  c.ws_order_entry = false;
  c.user_stream = orders;
  return c;
}

Result<void, std::string> NasdaqItchVenue::load_reference_data(InstrumentTable& instruments) {
  std::size_t n = 0;
  for (const Instrument& inst : instruments) {
    if (inst.venue != id_) continue;
    const std::string_view sym = inst.symbol.view();
    if (sym.empty() || sym.size() > 8)
      return fail(fmt::format("{}: symbol '{}' is not 1 to 8 characters", cfg_.name, sym));
    std::uint64_t p4 = 0;
    if (!nasdaq_fields::price_to_price4(inst.tick, p4) || p4 == 0)
      return fail(fmt::format(
          "{}: {} tick {} is not a multiple of 0.0001", cfg_.name, sym, decimal(inst.tick)));
    std::uint64_t shares = 0;
    if (cfg_.order_entry == OrderEntry::SimOuch &&
        (!nasdaq_fields::qty_to_shares(inst.lot, shares) || shares == 0))
      return fail(
          fmt::format("{}: {} lot {} is not whole shares", cfg_.name, sym, decimal(inst.lot)));
    ++n;
  }
  // Open the sources here, on the main thread, so a missing capability or interface stops the
  // start (exit code 4) instead of the network thread.
  try {
    open_sources();
  } catch (const std::exception& e) {
    return fail(std::string(e.what()));
  }
  if (user_tcp_ != nullptr && cfg_.rx_backend == RxBackend::Kernel) {
    std::string err;
    if (user_tcp_->init(err) != 0)
      return fail(fmt::format(
          "{}: order_transport = user_tcp on {}: {}", cfg_.name, cfg_.user_tcp_interface, err));
  }
  FASTMM_LOG_INFO("{}: {} instruments from [[instruments]]; {} lines on {} backend",
                  cfg_.name,
                  n,
                  cfg_.lines[1].group.empty() ? 1 : 2,
                  backend_name(cfg_.rx_backend));
  return {};
}

void NasdaqItchVenue::attach(const SymbolTable& symbols,
                             const InstrumentTable& instruments,
                             EventSink& md_sink,
                             EventSink& order_sink,
                             MsgRing* outbound) {
  symbols_ = &symbols;
  instruments_ = &instruments;
  md_sink_ = &md_sink;
  order_sink_ = &order_sink;
  outbound_ = outbound;
  codecs::itch::ItchL2BridgeConfig bc;
  bc.venue = id_;
  bc.depth = cfg_.depth;
  bc.book.price_window_ticks = cfg_.price_window_ticks;
  bc.book.max_orders = cfg_.max_orders;
  bridge_ = std::make_unique<codecs::itch::ItchL2Bridge>(md_sink, bc);
}

void NasdaqItchVenue::subscribe(std::span<const InstrumentId> instruments) {
  for (const InstrumentId id : instruments) {
    if (symbols_ == nullptr || symbols_->venue_of(id) != id_) continue;
    if (std::find(subscribed_.begin(), subscribed_.end(), id) != subscribed_.end()) continue;
    const std::string_view sym = instruments_->get(id).symbol.view();
    if (bridge_ == nullptr || !bridge_->add_instrument(sym, id))
      throw std::invalid_argument(fmt::format("{}: cannot add instrument {}", cfg_.name, sym));
    if (ouch_encoder_) {
      static_cast<void>(ouch_encoder_->add_symbol(sym, id));
      static_cast<void>(ouch_decoder_->add_symbol(sym, id));
    }
    subscribed_.push_back(id);
  }
  stats_.books_total = static_cast<std::uint32_t>(subscribed_.size());
  publish_status();
}

void NasdaqItchVenue::open_sources() {
  if (kernel_ != nullptr || xdp_ != nullptr || dpdk_ != nullptr) return;
  const std::size_t lines = cfg_.lines[1].group.empty() ? 1 : 2;
  if (cfg_.rx_backend == RxBackend::Kernel) {
    net::KernelSourceConfig kc;
    for (std::size_t i = 0; i < lines; ++i) {
      const ItchLine& l = cfg_.lines[i];
      kc.subscriptions.push_back({l.interface, l.group, l.port, l.source});
    }
    kc.rcvbuf_bytes = cfg_.rcvbuf_bytes;
    kc.batch = cfg_.batch;
    kc.max_datagram = kMaxDatagram;
    kc.timestamps = true;
    kc.busy_poll = cfg_.busy_poll;
    if (cfg_.hw_timestamps) {
      for (std::size_t i = 0; i < lines; ++i) {
        const int r = net::enable_hw_timestamps(cfg_.lines[i].interface);
        if (r != 0) {
          FASTMM_LOG_WARN("{}: hw_timestamps on {}: {} (kernel timestamps only)",
                          cfg_.name,
                          cfg_.lines[i].interface,
                          std::string_view(std::strerror(-r)));
        }
      }
    }
    auto src = std::make_unique<net::KernelDatagramSource>();
    const net::KernelSourceOpen r = src->open(kc);
    if (!r.ok()) {
      throw std::runtime_error(fmt::format("{}: line {} ({}:{}): {} failed: {}",
                                           cfg_.name,
                                           r.line == 0 ? 'A' : 'B',
                                           cfg_.lines[r.line].group,
                                           cfg_.lines[r.line].port,
                                           r.step,
                                           std::string_view(std::strerror(-r.err))));
    }
    if (cfg_.busy_poll && !r.busy_poll_applied()) {
      FASTMM_LOG_WARN(
          "{}: busy-poll socket options refused (SO_BUSY_POLL {}, SO_PREFER_BUSY_POLL {}, "
          "SO_BUSY_POLL_BUDGET {}); polling from user space",
          cfg_.name,
          r.busy_poll,
          r.prefer_busy_poll,
          r.busy_poll_budget);
    }
    kernel_ = std::move(src);
    return;
  }
  if (cfg_.rx_backend == RxBackend::Dpdk) {
    net::DpdkConfig dc;
    dc.eal_args = split_args(cfg_.dpdk_eal_args);
    dc.port = cfg_.dpdk_port;
    dc.batch = std::min<std::uint32_t>(cfg_.batch, 256);
    for (std::size_t i = 0; i < lines; ++i) {
      const ItchLine& l = cfg_.lines[i];
      net::DpdkSubscription s;
      s.interface = l.interface;
      if (!net::parse_ipv4(l.group, s.group))
        throw std::runtime_error(fmt::format("{}: bad group {}", cfg_.name, l.group));
      s.port = l.port;
      if (!l.source.empty() && !net::parse_ipv4(l.source, s.source))
        throw std::runtime_error(fmt::format("{}: bad source {}", cfg_.name, l.source));
      dc.subscriptions.push_back(s);
    }
    dc.exception_port = cfg_.dpdk_exception_port;
    dc.exception_ip = cfg_.dpdk_exception_ip;
    dc.exception_interval_ns = cfg_.dpdk_exception_interval_ns;
    auto src = std::make_unique<net::DpdkDatagramSource>();
    if (src->open(dc) != 0)
      throw std::runtime_error(fmt::format("{}: dpdk: {}", cfg_.name, src->error()));
    FASTMM_LOG_INFO("{}: dpdk port {}{}{}",
                    cfg_.name,
                    src->port_name(),
                    src->exception_interface().empty() ? "" : ", kernel exception interface ",
                    src->exception_interface());
    if (user_tcp_ != nullptr) {
      user_tcp_->init_shared(src->frame_tx(), src->mac(), src->mtu());
      src->set_frame_sink(user_tcp_->frame_sink());
    }
    dpdk_ = std::move(src);
    return;
  }
  net::XdpConfig xc;
  for (std::size_t i = 0; i < lines; ++i) {
    const ItchLine& l = cfg_.lines[i];
    net::XdpSubscription s;
    s.interface = l.interface;
    if (!net::parse_ipv4(l.group, s.group))
      throw std::runtime_error(fmt::format("{}: bad group {}", cfg_.name, l.group));
    s.port = l.port;
    if (!l.source.empty() && !net::parse_ipv4(l.source, s.source))
      throw std::runtime_error(fmt::format("{}: bad source {}", cfg_.name, l.source));
    xc.subscriptions.push_back(s);
    const bool listed = std::any_of(xc.queues.begin(), xc.queues.end(), [&](const auto& q) {
      return q.interface == l.interface;
    });
    if (!listed && !cfg_.queues.empty()) xc.queues.push_back({l.interface, cfg_.queues});
  }
  xc.batch = cfg_.batch;
  xc.mode = cfg_.xdp_mode;
  xc.busy_poll = cfg_.busy_poll;
  if (user_tcp_ != nullptr) {
    xc.tcp_interface = cfg_.user_tcp_interface;
    if (!net::parse_ipv4(cfg_.user_tcp_ip, xc.tcp_ip))
      throw std::runtime_error(fmt::format("{}: bad user_tcp_ip", cfg_.name));
    xc.tcp_port = cfg_.user_tcp_port;
    xc.tcp_arp = !host_has_address(xc.tcp_ip);
  }
  auto src = std::make_unique<net::XdpDatagramSource>();
  if (src->open(xc) != 0)
    throw std::runtime_error(fmt::format("{}: af_xdp: {}", cfg_.name, src->error()));
  for (const std::string& w : src->warnings())
    FASTMM_LOG_WARN("{}: af_xdp: {}", cfg_.name, std::string_view(w));
  for (const net::XdpInterfaceStatus& i : src->interfaces()) {
    FASTMM_LOG_INFO("{}: af_xdp on {} in {} mode", cfg_.name, i.name, net::to_string(i.mode));
  }
  if (!src->interfaces().empty())
    stats_.feed.xdp_mode = static_cast<std::uint8_t>(src->interfaces()[0].mode);
  if (user_tcp_ != nullptr) {
    user_tcp_->init_shared(src->frame_tx(), src->mac(), src->mtu());
    src->set_frame_sink(user_tcp_->frame_sink());
  }
  xdp_ = std::move(src);
}

void NasdaqItchVenue::connect(net::Reactor& reactor) {
  if (connected_) return;
  if (bridge_ == nullptr) throw std::logic_error("NasdaqItchVenue::connect before attach");
  reactor_ = &reactor;
  connected_ = true;
  recovery_.allocate(cfg_.recovery_buffer_packets, kMaxDatagram);
  codecs::moldudp::ReceiverConfig rc;
  rc.next_sequence = cfg_.glimpse_url.empty() ? 1 : 0;
  rc.can_request = !cfg_.rerequest.empty();
  rc.reorder_packets = cfg_.reorder_packets;
  rc.gap_timeout_ns = cfg_.gap_timeout_ns;
  rc.max_request_attempts = cfg_.max_request_attempts;
  rc.request_timeout_ns = cfg_.request_timeout_ns;
  rc.max_packet_bytes = kMaxDatagram;
  rx_ = std::make_unique<Receiver>(rx_handler_, rc);
  bridge_->set_midnight(utc_midnight());
  rerequest_buf_.reset(new std::byte[kMaxDatagram]);
  if (!cfg_.rerequest.empty()) {
    net::SockAddr to;
    if (!parse_ip_port(cfg_.rerequest, to)) {
      FASTMM_LOG_ERROR("{}: rerequest must be ip:port", cfg_.name);
    } else {
      rerequest_ = net::UdpSocket::open();
      if (!rerequest_.valid() || rerequest_.connect(to) != 0 ||
          !reactor.add(rerequest_.fd(), rerequest_io_, net::IoEvent::Read)) {
        FASTMM_LOG_ERROR("{}: cannot open the re-request socket to {}", cfg_.name, cfg_.rerequest);
        rerequest_.close();
      }
    }
  }
  try {
    open_sources();
  } catch (const std::exception& e) {
    FASTMM_LOG_ERROR("{}", std::string_view(e.what()));
    feed_lost("the multicast source cannot be opened");
    publish_status();
    return;
  }
  if (!cfg_.busy_poll) {
    if (kernel_) {
      for (std::size_t i = 0; i < kernel_->line_count(); ++i)
        registered_fds_.push_back(kernel_->fd(i));
    } else if (xdp_) {
      for (const int fd : xdp_->fds()) registered_fds_.push_back(fd);
    }
    for (const int fd : registered_fds_) {
      if (!reactor.add(fd, source_io_, net::IoEvent::Read))
        FASTMM_LOG_ERROR("{}: cannot register descriptor {} with the reactor", cfg_.name, fd);
    }
  }
  feed_state_ = FeedState::Snapshot;
  if (cfg_.glimpse_url.empty()) {
    FASTMM_LOG_WARN(
        "{}: no glimpse_url: the feed must start at sequence 1 (before the directory spin) and "
        "an unrecoverable gap stops it",
        cfg_.name);
  } else {
    start_snapshot();
  }
  if (cfg_.order_entry == OrderEntry::SimOuch) open_ouch();
  const std::int64_t now = now_ns();
  next_service_ns_ = now;
  next_session_timer_ns_ = now + kSessionTimerNs;
  FASTMM_LOG_INFO(
      "{}: joined {} line(s); order_entry={} order_transport={} busy_poll={} rerequest={} "
      "glimpse={}",
      cfg_.name,
      cfg_.lines[1].group.empty() ? 1 : 2,
      cfg_.order_entry == OrderEntry::SimOuch ? "sim_ouch" : "none",
      user_tcp_ != nullptr ? "user_tcp" : "kernel",
      cfg_.busy_poll,
      cfg_.rerequest.empty() ? std::string_view("none") : cfg_.rerequest,
      cfg_.glimpse_url.empty() ? std::string_view("none") : cfg_.glimpse_url);
  publish_status();
}

void NasdaqItchVenue::disconnect() {
  if (!connected_) return;
  connected_ = false;
  if (reactor_ != nullptr) {
    for (const int fd : registered_fds_) reactor_->remove(fd);
    registered_fds_.clear();
    if (rerequest_.valid()) reactor_->remove(rerequest_.fd());
  }
  rerequest_.close();
  glimpse_.reset();
  glimpse_tcp_->close();
  if (ouch_tcp_) {
    if (ouch_session_ && ouch_session_->state() == codecs::SessionState::Up)
      static_cast<void>(ouch_session_->logout());
    ouch_session_.reset();
    ouch_tcp_->close();
    ouch_up_ = false;
  }
  if (user_tcp_ != nullptr) {
    // Let the FIN go out and be acknowledged.
    for (int i = 0; i < 200; ++i) {
      if (user_tcp_->shared()) drain_sources();  // the link's frames arrive through the source
      user_tcp_->poll();
      if (i % 20 == 19) ::usleep(1000);
    }
    if (const net::UserTcpStats* t = user_tcp_->tcp_stats()) {
      FASTMM_LOG_INFO(
          "{}: user_tcp: {} segments out, {} in, {} retransmits ({} RTO, {} fast), {} out of "
          "order, {} bad frames, {} unknown, {} RSTs in, {} TX drops, srtt {} us",
          cfg_.name,
          t->segments_out,
          t->segments_in,
          t->retransmits,
          t->rto_expiries,
          t->fast_retransmits,
          t->out_of_order,
          t->bad_frames,
          t->unknown_segments,
          t->rst_in,
          t->tx_drops,
          t->srtt_ns / 1000);
    }
  }
  if (kernel_) kernel_->close();
  if (xdp_) {
    const int stats_rc = xdp_->refresh_stats();
    const net::XdpStats& x = xdp_->stats();
    std::uint64_t fallback = 0;
    for (const net::XdpInterfaceStatus& i : xdp_->interfaces()) fallback += i.fallback_packets;
    FASTMM_LOG_INFO(
        "{}: af_xdp: {} socket(s), {} datagrams, {} unmatched, {} bad frames, {} passed to the "
        "kernel (no socket on their RX queue){}",
        cfg_.name,
        xdp_->fds().size(),
        x.datagrams,
        x.unmatched,
        x.bad_frames,
        fallback,
        stats_rc < 0 ? "; statistics incomplete" : "");
    publish_status();  // the final XDP counters, before close() clears them
    if (user_tcp_ != nullptr)
      FASTMM_LOG_INFO("{}: af_xdp: {} frames to user_tcp, {} sent, {} TX drops, {} kicks",
                      cfg_.name,
                      x.to_sink,
                      x.tx_frames,
                      x.tx_drops,
                      x.tx_kicks);
    xdp_->close();
  }
  if (dpdk_) {
    static_cast<void>(dpdk_->refresh_stats());
    const net::DpdkStats& d = dpdk_->stats();
    FASTMM_LOG_INFO(
        "{}: dpdk: {} packets, {} missed, {} errors, {} no-mbuf, {} bad frames, {} other, {} "
        "unmatched, {} to user_tcp, {} to / {} from the kernel, {} sent, {} TX drops",
        cfg_.name,
        d.ipackets,
        d.imissed,
        d.ierrors,
        d.rx_nombuf,
        d.bad_frames,
        d.other,
        d.unmatched,
        d.to_sink,
        d.to_kernel,
        d.from_kernel,
        d.tx_frames,
        d.tx_drops);
    dpdk_->close();
  }
  publish_status();
}

std::uint64_t NasdaqItchVenue::next_sequence() const noexcept {
  return rx_ ? rx_->next_sequence() : 0;
}

// ---- receive path ---------------------------------------------------------------------------

void NasdaqItchVenue::drain_sources() noexcept {
  auto handler = [this](std::span<const std::byte> p, const net::RxMeta& m) noexcept {
    on_datagram(p, m);
  };
  if (kernel_) {
    while (kernel_->poll(handler) != 0) {
    }
  } else if (xdp_) {
    while (xdp_->poll(handler) != 0) {
    }
  } else if (dpdk_) {
    while (dpdk_->poll(handler) != 0) {
    }
  }
}

void NasdaqItchVenue::drain_rerequest() noexcept {
  if (!rerequest_.valid()) return;
  for (;;) {
    const net::IoResult r =
        rerequest_.recv_from(std::span<std::byte>(rerequest_buf_.get(), kMaxDatagram));
    if (!r.ok() || r.would_block()) return;
    net::RxMeta m{};
    m.t0_cycles = rdtscp();
    m.t0_wall_ns = wall_now().ns;
    m.line = kRetransmissionLine;
    on_datagram(std::span<const std::byte>(rerequest_buf_.get(), r.bytes), m);
  }
}

void NasdaqItchVenue::on_datagram(std::span<const std::byte> payload,
                                  const net::RxMeta& m) noexcept {
  bytes_ += payload.size();
  // Only while live: datagrams queued before connect() or during a snapshot are not a latency.
  if (m.sw_ts_ns != 0 && m.t0_wall_ns > m.sw_ts_ns && feed_state_ == FeedState::Live)
    kernel_to_t0_.record(static_cast<std::uint64_t>(m.t0_wall_ns - m.sw_ts_ns));
  ItchRxMeta meta;
  meta.t0_cycles = m.t0_cycles;
  meta.recv_ns = m.sw_ts_ns != 0 ? m.sw_ts_ns : m.t0_wall_ns;
  if (cfg_.hw_clock == HwClock::PhcSynced && m.hw_ts_ns != 0) meta.recv_ns = m.hw_ts_ns;
  meta.datagram = ++datagrams_;
  meta.line = m.line;
  if (feed_state_ == FeedState::Lost || feed_state_ == FeedState::Down) return;
  rx_->on_packet(m.line, payload, now_ns(), meta);
  after_packet();
}

void NasdaqItchVenue::after_packet() noexcept {
  if (datagram_open_) {
    bridge_->end_datagram();
    datagram_open_ = false;
  }
  if (feed_state_ == FeedState::Live && bridge_->stats().book_errors != book_errors_seen_) {
    book_errors_seen_ = bridge_->stats().book_errors;
    start_recovery("L3 book inconsistency");
  }
}

void NasdaqItchVenue::on_message(std::uint64_t seq,
                                 std::span<const std::byte> msg,
                                 const ItchRxMeta& meta) noexcept {
  if (meta.line == kRetransmissionLine) ++recovered_;
  switch (feed_state_) {
    case FeedState::Live:
      apply(seq, msg, meta);
      return;
    case FeedState::Snapshot:
      if (cfg_.glimpse_url.empty()) {
        // Start of stream: the books are complete (empty) before sequence 1.
        if (seq == 1) {
          go_live(1);
          apply(seq, msg, meta);
        }
        return;
      }
      // Waiting to retry GLIMPSE: the next snapshot starts later than these messages.
      if (snapshot_retry_ns_ != 0) return;
      if (recovery_.append(seq, msg, meta)) return;
      ++stats_.feed.recovery_overflows;
      if (++overflow_streak_ >= 2) {
        feed_lost("the recovery buffer overflowed twice in a row");
        return;
      }
      FASTMM_LOG_WARN("{}: recovery buffer full ({} datagrams) during a snapshot: restarting it",
                      cfg_.name,
                      recovery_.packets());
      start_snapshot();
      return;
    case FeedState::None:
    case FeedState::Down:
    case FeedState::Lost:
      return;
  }
}

void NasdaqItchVenue::apply(std::uint64_t seq,
                            std::span<const std::byte> msg,
                            const ItchRxMeta& meta) noexcept {
  if (seq < live_from_) return;
  if (datagram_open_ && meta.datagram != open_datagram_) bridge_->end_datagram();
  open_datagram_ = meta.datagram;
  datagram_open_ = true;
  if (ouch_encoder_) note_token(meta.t0_cycles, seq);
  static_cast<void>(bridge_->on_itch_message(seq, msg, stamp_of(meta)));
}

void NasdaqItchVenue::send_request(std::span<const std::byte> bytes) noexcept {
  if (rerequest_.valid()) static_cast<void>(rerequest_.send(bytes));
}

void NasdaqItchVenue::on_end_of_session() noexcept {
  FASTMM_LOG_INFO("{}: MoldUDP64 End of Session", cfg_.name);
}

void NasdaqItchVenue::on_gap_unrecoverable(std::uint64_t from, std::uint64_t count) noexcept {
  lost_end_ = std::max(lost_end_, from + count);
  FASTMM_LOG_WARN("{}: {} message(s) from sequence {} cannot be recovered", cfg_.name, count, from);
  if (feed_state_ == FeedState::Live) {
    start_recovery("unrecoverable gap");
  } else if (feed_state_ == FeedState::Snapshot && cfg_.glimpse_url.empty()) {
    feed_lost("the stream did not start at sequence 1 and there is no glimpse_url");
  }
}

void NasdaqItchVenue::start_recovery(std::string_view why) noexcept {
  if (datagram_open_) {
    bridge_->end_datagram();
    datagram_open_ = false;
  }
  if (cfg_.glimpse_url.empty()) {
    feed_lost(why);
    return;
  }
  ++resyncs_;
  FASTMM_LOG_WARN("{}: {}: rebuilding the books from GLIMPSE", cfg_.name, why);
  feed_state_ = FeedState::Snapshot;
  start_snapshot();
}

void NasdaqItchVenue::start_snapshot() noexcept {
  glimpse_.reset();
  glimpse_tcp_->close();
  bridge_->mark_incomplete();
  book_errors_seen_ = bridge_->stats().book_errors;
  recovery_.clear();
  lost_end_ = 0;
  snapshot_retry_ns_ = 0;
  snapshot_started_ns_ = now_ns();
  if (!glimpse_tcp_->open(*reactor_, glimpse_addr_)) {
    FASTMM_LOG_WARN("{}: cannot connect to GLIMPSE at {}; retrying", cfg_.name, cfg_.glimpse_url);
    snapshot_retry_ns_ = now_ns() + kRetryNs;
  }
}

void NasdaqItchVenue::go_live(std::uint64_t live_from) noexcept {
  live_from_ = live_from;
  feed_state_ = FeedState::Live;
  overflow_streak_ = 0;
  bridge_->set_stamp(codecs::itch::DatagramStamp{rdtscp(), wall_now()});
  mark_pending_ = !bridge_->mark_all_complete();
  book_errors_seen_ = bridge_->stats().book_errors;
  if (first_snapshot_done_ && !cfg_.glimpse_url.empty()) ++snapshots_;
  first_snapshot_done_ = true;
  FASTMM_LOG_INFO("{}: books live from sequence {}", cfg_.name, live_from);
}

void NasdaqItchVenue::finish_snapshot(std::uint64_t next_seq) noexcept {
  if (feed_state_ != FeedState::Snapshot) return;
  const auto retry = [&](std::string_view why, bool later) {
    FASTMM_LOG_WARN("{}: snapshot to {} not usable ({}); taking another", cfg_.name, next_seq, why);
    if (later) {
      bridge_->mark_incomplete();
      recovery_.clear();
      snapshot_retry_ns_ = now_ns() + kRetryNs;
    } else {
      start_snapshot();
    }
  };
  if (lost_end_ > next_seq) return retry("messages after End of Snapshot were lost", false);
  if (bridge_->stats().book_errors != book_errors_seen_)
    return retry("the snapshot is inconsistent", true);
  std::uint64_t expected = next_seq;
  bool hole = false;
  recovery_.for_each(
      [&](std::uint64_t seq, std::span<const std::byte> msg, const ItchRxMeta& meta) {
        if (hole || seq < next_seq) return;
        if (seq != expected) {
          hole = true;
          return;
        }
        static_cast<void>(bridge_->on_itch_message(seq, msg, stamp_of(meta)));
        ++expected;
      },
      [&] { bridge_->end_datagram(); });
  const std::uint32_t buffered = recovery_.packets();
  recovery_.clear();
  if (hole) return retry("the buffered messages have a hole", false);
  if (bridge_->stats().book_errors != book_errors_seen_)
    return retry("buffered messages do not apply to it", true);
  const std::uint64_t rxn = rx_->next_sequence();
  if (rxn == 0 || rxn > expected) {
    // Nothing received yet, or the buffer starts after End of Snapshot's sequence: resume there
    // and let the receiver request what is missing.
    if (rxn > expected && cfg_.rerequest.empty())
      return retry("the buffer does not reach back to it and there is no rerequest", false);
    rx_->reset(expected);
  }
  FASTMM_LOG_INFO("{}: GLIMPSE snapshot to sequence {} applied with {} buffered datagram(s)",
                  cfg_.name,
                  next_seq,
                  buffered);
  go_live(expected);
}

void NasdaqItchVenue::feed_lost(std::string_view why) noexcept {
  if (feed_state_ == FeedState::Lost) return;
  feed_state_ = FeedState::Lost;
  FASTMM_LOG_ERROR("{}: market data lost ({}): the venue stops", cfg_.name, why);
  glimpse_.reset();
  glimpse_tcp_->close();
  snapshot_retry_ns_ = 0;
  recovery_.clear();
  if (bridge_) bridge_->mark_incomplete();
  if (!venue_kill_sent_ && order_sink_ != nullptr) {
    venue_kill_sent_ = true;
    emit_venue_kill(*order_sink_, id_, KillReason::FeedLost);
  }
}

// ---- GLIMPSE ----------------------------------------------------------------------------------

void NasdaqItchVenue::on_glimpse_up() noexcept {
  glimpse::GlimpseConfig gc;
  gc.session.username = std::string_view(cfg_.glimpse_username);
  gc.session.password = std::string_view(cfg_.glimpse_password);
  glimpse_.emplace(*glimpse_tcp_, snapshot_handler_, gc);
  if (!glimpse_->login(now_ns())) {
    FASTMM_LOG_WARN("{}: GLIMPSE login could not be sent; retrying", cfg_.name);
    glimpse_.reset();
    glimpse_tcp_->close();
    snapshot_retry_ns_ = now_ns() + kRetryNs;
  }
}

std::size_t NasdaqItchVenue::on_glimpse_data(std::span<const std::byte> bytes) noexcept {
  if (!glimpse_) return bytes.size();
  const std::size_t used = glimpse_->on_bytes(bytes);
  if (glimpse_->complete()) {
    const std::uint64_t end = glimpse_->end_sequence();
    glimpse_.reset();
    glimpse_tcp_->close();
    finish_snapshot(end);
  } else if (glimpse_->state() == glimpse::GlimpseState::Idle) {
    FASTMM_LOG_WARN("{}: GLIMPSE session closed (reason {}); retrying",
                    cfg_.name,
                    static_cast<int>(glimpse_->close_reason()));
    glimpse_.reset();
    glimpse_tcp_->close();
    bridge_->mark_incomplete();
    snapshot_retry_ns_ = now_ns() + kRetryNs;
  }
  return used;
}

void NasdaqItchVenue::on_glimpse_down(int err) noexcept {
  glimpse_.reset();
  if (feed_state_ != FeedState::Snapshot || cfg_.glimpse_url.empty()) return;
  FASTMM_LOG_WARN("{}: GLIMPSE connection to {} lost ({}); retrying",
                  cfg_.name,
                  cfg_.glimpse_url,
                  std::string_view(err != 0 ? std::strerror(err) : "closed"));
  bridge_->mark_incomplete();
  snapshot_retry_ns_ = now_ns() + kRetryNs;
}

void NasdaqItchVenue::on_snapshot_message(std::span<const std::byte> msg) noexcept {
  static_cast<void>(bridge_->on_itch_message(0, msg, codecs::itch::DatagramStamp{}));
}

void NasdaqItchVenue::on_snapshot_end(std::uint64_t) noexcept {
  // Handled in on_glimpse_data() once GlimpseClient::on_bytes() has returned.
}

// ---- timers -----------------------------------------------------------------------------------

void NasdaqItchVenue::poll() noexcept {
  if (!connected_) return;
  if (cfg_.busy_poll) drain_sources();
  if (user_tcp_ != nullptr) user_tcp_->poll();
  const std::int64_t now = now_ns();
  if (now < next_service_ns_) return;
  next_service_ns_ = now + kServiceIntervalNs;
  service(now);
}

void NasdaqItchVenue::service(std::int64_t now) noexcept {
  if (rx_ && rx_->next_sequence() != 0 && rx_->next_sequence() < rx_->highest_known()) {
    rx_->on_timer(now);
    after_packet();
  }
  if (mark_pending_ && feed_state_ == FeedState::Live)
    mark_pending_ = !bridge_->mark_all_complete();
  if (snapshot_retry_ns_ != 0 && now >= snapshot_retry_ns_ && feed_state_ == FeedState::Snapshot)
    start_snapshot();
  if (ouch_retry_ns_ != 0 && now >= ouch_retry_ns_) open_ouch();
  if (now >= next_session_timer_ns_) {
    next_session_timer_ns_ = now + kSessionTimerNs;
    if (glimpse_) {
      glimpse_->on_timer(now);
      if (glimpse_->state() == glimpse::GlimpseState::Idle && !glimpse_->complete()) {
        FASTMM_LOG_WARN("{}: GLIMPSE session timed out; retrying", cfg_.name);
        glimpse_.reset();
        glimpse_tcp_->close();
        bridge_->mark_incomplete();
        snapshot_retry_ns_ = now + kRetryNs;
      }
    }
    if (ouch_session_) {
      ouch_session_->on_timer(now);
      if (ouch_session_->state() == codecs::SessionState::Down) {
        FASTMM_LOG_WARN("{}: OUCH session down (reason {})",
                        cfg_.name,
                        static_cast<int>(ouch_session_->close_reason()));
        ouch_tcp_->close();
        ouch_lost();
        if (!cancel_all_done_.load()) ouch_retry_ns_ = now + kRetryNs;
      }
    }
    publish_status();
  }
  if ((xdp_ || dpdk_) && now >= next_xdp_stats_ns_) {
    next_xdp_stats_ns_ = now + kXdpStatsNs;
    if (xdp_) static_cast<void>(xdp_->refresh_stats());
    if (dpdk_) static_cast<void>(dpdk_->refresh_stats());
  }
}

void NasdaqItchVenue::on_timer(std::int64_t) {
  publish_status();
}

// ---- order entry -----------------------------------------------------------------------------

void NasdaqItchVenue::open_ouch() noexcept {
  ouch_retry_ns_ = 0;
  if (cancel_all_done_.load() || !ouch_tcp_) return;
  ouch_session_.reset();
  if (!ouch_tcp_->open(*reactor_, ouch_addr_)) {
    FASTMM_LOG_WARN("{}: cannot connect to OUCH at {}; retrying", cfg_.name, cfg_.ouch_url);
    ouch_retry_ns_ = now_ns() + kRetryNs;
  }
}

void NasdaqItchVenue::on_ouch_up() noexcept {
  soupbin::ClientConfig cc;
  cc.username = std::string_view(cfg_.ouch_username);
  cc.password = std::string_view(cfg_.ouch_password);
  cc.sequence = 1;
  ouch_session_.emplace(*ouch_tcp_, cc);
  if (!ouch_session_->login(now_ns())) {
    ouch_session_.reset();
    ouch_tcp_->close();
    ouch_retry_ns_ = now_ns() + kRetryNs;
  }
}

std::size_t NasdaqItchVenue::on_ouch_data(std::span<const std::byte> bytes) noexcept {
  if (!ouch_session_) return bytes.size();
  const soupbin::SoupBinFramer framer;
  std::size_t used = 0;
  const std::int64_t rx = wall_now().ns;
  while (used < bytes.size()) {
    const codecs::FrameView f = framer.next(bytes.subspan(used));
    if (!f.complete()) break;
    used += f.consumed;
    const bool session_packet = ouch_session_->on_frame(f);
    if (!ouch_up_ && ouch_session_->state() == codecs::SessionState::Up) {
      ouch_up_ = true;
      FASTMM_LOG_INFO("{}: OUCH session up at {}", cfg_.name, cfg_.ouch_url);
      emit_connection_state(*order_sink_, id_, 1, ConnState::Live);
    }
    if (!session_packet && static_cast<char>(f.kind) == 'S') {
      static_cast<void>(ouch_decoder_->decode(f, rx, *order_sink_));
      ++stats_.order_events;
    }
    if (ouch_session_->state() == codecs::SessionState::Down) {
      FASTMM_LOG_WARN("{}: OUCH session closed (reason {})",
                      cfg_.name,
                      static_cast<int>(ouch_session_->close_reason()));
      ouch_tcp_->close();
      ouch_lost();
      if (!cancel_all_done_.load()) ouch_retry_ns_ = now_ns() + kRetryNs;
      return bytes.size();
    }
  }
  return used;
}

void NasdaqItchVenue::on_ouch_down(int err) noexcept {
  if (cancel_all_done_.load()) {
    FASTMM_LOG_INFO("{}: OUCH connection closed by cancel_all", cfg_.name);
  } else {
    FASTMM_LOG_WARN("{}: OUCH connection to {} lost ({})",
                    cfg_.name,
                    cfg_.ouch_url,
                    std::string_view(err != 0 ? std::strerror(err) : "closed"));
  }
  ouch_lost();
  if (!cancel_all_done_.load() && connected_) ouch_retry_ns_ = now_ns() + kRetryNs;
}

// The simulator cancels an account's orders when its connection closes.
void NasdaqItchVenue::ouch_lost() noexcept {
  ouch_session_.reset();
  if (!ouch_up_) return;
  ouch_up_ = false;
  emit_connection_state(*order_sink_, id_, 1, ConnState::Disconnected);
  emit_empty_reconcile();
}

void NasdaqItchVenue::emit_empty_reconcile() noexcept {
  ReconcileMsg begin{};
  init_header(begin, EventType::Reconcile, InstrumentId::invalid(), id_);
  begin.kind = ReconcileMsg::Kind::Begin;
  SentWatermark::stamp(begin, sent_.value());
  begin.hdr.recv_ts = wall_now();
  static_cast<void>(order_sink_->push(begin.hdr));
  ReconcileMsg end{};
  init_header(end, EventType::Reconcile, InstrumentId::invalid(), id_);
  end.kind = ReconcileMsg::Kind::End;
  end.hdr.recv_ts = wall_now();
  static_cast<void>(order_sink_->push(end.hdr));
}

void NasdaqItchVenue::request_open_orders() {
  // Only the simulator takes orders, and it keeps none across connections: with the session down
  // the venue holds no orders; with it up there is no query (no Account Query in the simulator).
  if (order_sink_ != nullptr && !ouch_up_) emit_empty_reconcile();
}

bool NasdaqItchVenue::cancel_all() {
  if (cfg_.order_entry != OrderEntry::SimOuch || !ouch_tcp_) return true;
  cancel_all_done_.store(true);
  // The simulator cancels every order of the connection when it closes.
  static_cast<void>(ouch_tcp_->shutdown_from_any_thread());
  return true;
}

// All orders the ring holds go out in one write (SoupBinTCP header and OUCH message of each).
template <class Ring>
void NasdaqItchVenue::write_orders(Ring& ring) {
  ByteLink* const link = ouch_tcp_.get();
  drain_outbound_coalesced(
      ring,
      wire_,
      [link] {
        if (link != nullptr) link->cork();
      },
      [this](const EventHeader& h) {
        if (const auto cmd = OrderCommand::from(h)) {
          sent_.note(*cmd);
          send_command(*cmd);
        }
      },
      [link] { return link != nullptr && link->uncork(); });
}

void NasdaqItchVenue::on_wake() {
  if (outbound_ != nullptr) write_orders(*outbound_);
}

void NasdaqItchVenue::send_now(std::span<const EventHeader* const> batch) {
  OutboundBatch b(batch);
  write_orders(b);
}

void NasdaqItchVenue::refuse(const OrderCommand& cmd,
                             RejectReason reason,
                             std::string_view why) noexcept {
  if (cmd.kind == OrderCommandKind::Cancel) {
    emit_cancel_reject(*order_sink_, id_, cmd.instrument, cmd.cl_ord_id, reason, 0, why);
  } else {
    emit_order_reject(*order_sink_, id_, cmd.instrument, cmd.cl_ord_id, reason, 0, why);
  }
  ++stats_.order_events;
}

void NasdaqItchVenue::send_command(const OrderCommand& cmd) noexcept {
  if (cfg_.order_entry == OrderEntry::None)
    return refuse(cmd, RejectReason::VenueReject, "order_entry = none");
  if (!ouch_up_ || !ouch_session_) {
    ++stats_.order_send_failures;
    return refuse(cmd,
                  cmd.kind == OrderCommandKind::Cancel ? RejectReason::VenueUnknownOrder
                                                       : RejectReason::VenueReject,
                  "OUCH session down");
  }
  const Cycles before_encode = rdtscp();
  const std::size_t n = ouch_encoder_->encode(cmd, order_buf_);
  if (n == 0) {
    return refuse(
        cmd,
        cmd.kind == OrderCommandKind::New ? RejectReason::VenueReject : RejectReason::UnknownOrder,
        "OUCH encoding failed");
  }
  if (cmd.kind != OrderCommandKind::Cancel) {
    // The simulator times Enter and Replace Orders whose ClOrdID names a sequence number.
    const std::uint64_t token = token_for(cmd.t0_cycles());
    const std::size_t at = cmd.kind == OrderCommandKind::New
                               ? offsetof(ouch50::EnterOrder, cl_ord_id)
                               : offsetof(ouch50::ReplaceOrder, cl_ord_id);
    if (token != 0)
      static_cast<void>(
          ouch50::put_seq_token(reinterpret_cast<char*>(order_buf_.data()) + at, token));
  }
  const Cycles after_encode = rdtscp();
  if (!ouch_session_->send(std::span<const std::byte>(order_buf_.data(), n))) {
    ++stats_.order_send_failures;
    return refuse(cmd, RejectReason::VenueReject, "OUCH send failed");
  }
  wire_.record(cmd.t0_cycles(), before_encode, after_encode, rdtscp());
  switch (cmd.kind) {
    case OrderCommandKind::New:
      ++stats_.orders_sent;
      break;
    case OrderCommandKind::Cancel:
      ++stats_.cancels_sent;
      break;
    case OrderCommandKind::Replace:
      ++stats_.replaces_sent;
      break;
  }
}

namespace {
[[nodiscard]] std::size_t token_slot(std::uint64_t t0) noexcept {
  return static_cast<std::size_t>((t0 * 0x9E37'79B9'7F4A'7C15ULL) >> 52);  // 12 bits
}
}  // namespace

void NasdaqItchVenue::note_token(Cycles t0, std::uint64_t seq) noexcept {
  if (t0.v == 0) return;
  TokenSlot& s = tokens_[token_slot(t0.v)];
  if (s.t0 == t0.v) return;  // the batch's first message wins
  s.t0 = t0.v;
  s.seq = seq;
}

std::uint64_t NasdaqItchVenue::token_for(Cycles t0) const noexcept {
  if (t0.v == 0) return 0;
  const TokenSlot& s = tokens_[token_slot(t0.v)];
  return s.t0 == t0.v ? s.seq : 0;
}

// ---- status ----------------------------------------------------------------------------------

void NasdaqItchVenue::publish_status() noexcept {
  VenueFeedStatus& f = stats_.feed;
  f.state = feed_state_;
  f.bytes = bytes_;
  f.recovered = recovered_;
  f.snapshot_recoveries = snapshots_;
  if (rx_) {
    const codecs::moldudp::ReceiverStats& r = rx_->stats();
    f.packets = r.packets;
    for (std::size_t l = 0; l < 2; ++l) {
      const codecs::moldudp::ReceiverLineStats& ls = r.lines[l];
      f.line_packets[l] = ls.packets;
      f.line_duplicates[l] = ls.duplicate_packets;
      f.line_skew_max_ns[l] = ls.skew_max_ns;
      f.line_skew_mean_ns[l] =
          ls.skew_samples == 0 ? 0 : ls.skew_sum_ns / static_cast<std::int64_t>(ls.skew_samples);
    }
    f.gaps = r.gaps;
    f.unrecovered = r.lost_messages;
    f.reorder_high_water = r.reorder_high_water;
    f.requests = r.requests;
    f.malformed = r.malformed + (kernel_ ? kernel_->stats().truncated : 0);
  }
  // After close() the source has no statistics left: keep the last ones.
  if (xdp_ && !xdp_->interfaces().empty()) {
    f.malformed = (rx_ ? rx_->stats().malformed : 0) + xdp_->stats().bad_frames;
    f.xdp_rx_dropped = 0;
    f.xdp_rx_invalid_descs = 0;
    f.xdp_rx_ring_full = 0;
    f.xdp_fill_ring_empty = 0;
    for (const net::XdpSocketStats& s : xdp_->socket_stats()) {
      f.xdp_rx_dropped += s.rx_dropped;
      f.xdp_rx_invalid_descs += s.rx_invalid_descs;
      f.xdp_rx_ring_full += s.rx_ring_full;
      f.xdp_fill_ring_empty += s.rx_fill_ring_empty_descs;
    }
    f.xdp_fallback = 0;
    for (const net::XdpInterfaceStatus& i : xdp_->interfaces())
      f.xdp_fallback += i.fallback_packets;
  }
  if (dpdk_) {
    f.malformed = (rx_ ? rx_->stats().malformed : 0) + dpdk_->stats().bad_frames;
    f.xdp_rx_dropped = dpdk_->stats().imissed + dpdk_->stats().rx_nombuf;
  }
  const TscCalibration cal = tsc_calibration();
  f.kernel_to_t0.count = kernel_to_t0_.count();
  f.kernel_to_t0.p50_ns = kernel_to_t0_.percentile(0.50);
  f.kernel_to_t0.p99_ns = kernel_to_t0_.percentile(0.99);
  f.kernel_to_t0.p999_ns = kernel_to_t0_.percentile(0.999);
  f.kernel_to_t0.max_ns = kernel_to_t0_.max();
  if (bridge_) {
    const codecs::itch::ItchL2BridgeStats& b = bridge_->stats();
    f.book_errors = b.book_errors;
    stats_.md_messages = b.messages;
    stats_.md_dropped = b.overflow;
    std::uint32_t synced = 0;
    for (const InstrumentId id : subscribed_) {
      if (bridge_->complete(id)) ++synced;
    }
    stats_.books_synced = synced;
  }
  stats_.md = feed_state_ == FeedState::Live       ? ChannelState::Live
              : feed_state_ == FeedState::Snapshot ? ChannelState::Connecting
                                                   : ChannelState::Down;
  stats_.order = ouch_up_ ? ChannelState::Live : ChannelState::Down;
  stats_.user = stats_.order;
  stats_.resyncs = resyncs_;
  wire_.summarize(cal, stats_.wire_tick_to_trade, stats_.order_encode, stats_.order_send);
  published_.store(stats_);
}

VenueStatus NasdaqItchVenue::status() const noexcept {
  VenueStatus s;
  for (int i = 0; i < 100; ++i) {
    if (published_.try_load(s)) return s;
  }
  return s;
}

// ---- config -----------------------------------------------------------------------------------

NasdaqItchVenueConfig make_nasdaq_itch_config(const VenueSection& v, bool dry_run, bool busy_poll) {
  NasdaqItchVenueConfig c;
  c.name = v.name;
  c.dry_run = dry_run;
  c.busy_poll = busy_poll;
  const auto bad = [&](std::string_view key, std::string_view why) {
    return std::invalid_argument(fmt::format("venues.{}.{}: {}", v.name, key, why));
  };
  auto extra = [&](const char* key) -> std::string {
    const auto it = v.extra.find(key);
    return it == v.extra.end() ? std::string{} : it->second;
  };
  auto extra_u64 = [&](const char* key, std::uint64_t def, std::uint64_t lo, std::uint64_t hi) {
    const std::string s = extra(key);
    if (s.empty()) return def;
    std::uint64_t n = 0;
    if (!parse_u64(s, n) || n < lo || n > hi)
      throw bad(key, fmt::format("expected an integer from {} to {}", lo, hi));
    return n;
  };
  auto extra_bool = [&](const char* key, bool def) {
    const std::string s = extra(key);
    if (s.empty()) return def;
    if (s == "true") return true;
    if (s == "false") return false;
    throw bad(key, "expected true or false");
  };
  const std::string backend = extra("rx_backend");
  if (backend.empty() || backend == "kernel") {
    c.rx_backend = RxBackend::Kernel;
  } else if (backend == "af_xdp") {
    c.rx_backend = RxBackend::AfXdp;
  } else if (backend == "dpdk") {
    c.rx_backend = RxBackend::Dpdk;
    if (!busy_poll)
      throw bad("rx_backend",
                "dpdk has no descriptor to wait on: needs [engine] spin_mode = \"busy\"");
  } else {
    throw bad("rx_backend", "expected kernel, af_xdp or dpdk");
  }
  c.dpdk_eal_args = extra("dpdk_eal_args");
  c.dpdk_port = extra("dpdk_port");
  c.dpdk_exception_port = extra("dpdk_exception_port");
  c.dpdk_exception_ip = extra("dpdk_exception_ip");
  c.dpdk_exception_interval_ns = static_cast<std::int64_t>(
      extra_u64("dpdk_exception_interval_us",
                static_cast<std::uint64_t>(c.dpdk_exception_interval_ns / 1000),
                0,
                1'000'000) *
      1000);
  const std::string interface = extra("interface");
  const char* line_keys[2] = {"line_a", "line_b"};
  for (std::size_t i = 0; i < 2; ++i) {
    const std::string key = line_keys[i];
    const std::string spec = extra(key.c_str());
    ItchLine& l = c.lines[i];
    if (spec.empty()) {
      if (i == 0) throw bad(key, "required (\"group:port\")");
      continue;
    }
    const std::size_t colon = spec.rfind(':');
    std::uint64_t port = 0;
    std::uint32_t group_be = 0;
    if (colon == std::string::npos || !parse_u64(std::string_view(spec).substr(colon + 1), port) ||
        port == 0 || port > 65535 || !net::parse_ipv4(spec.substr(0, colon), group_be) ||
        group_be == 0)
      throw bad(key, "expected \"<IPv4 multicast group or local address>:<port>\"");
    l.group = spec.substr(0, colon);
    l.port = static_cast<std::uint16_t>(port);
    l.interface = extra((key + "_interface").c_str());
    if (l.interface.empty()) l.interface = interface;
    l.source = extra((key + "_source").c_str());
    if (c.rx_backend == RxBackend::AfXdp && l.interface.empty())
      throw bad(key + "_interface", "af_xdp needs an interface name (or interface)");
  }
  std::string queues;  // "0,1" or the stringified array "[0, 1]"
  for (const char ch : extra("queues")) {
    if (ch != '[' && ch != ']' && ch != ' ') queues += ch;
  }
  for (std::size_t pos = 0; pos < queues.size();) {
    const std::size_t comma = std::min(queues.find(',', pos), queues.size());
    std::uint64_t q = 0;
    if (!parse_u64(std::string_view(queues).substr(pos, comma - pos), q) || q > 4095)
      throw bad("queues", "expected a comma-separated list of RX queue numbers");
    c.queues.push_back(static_cast<std::uint32_t>(q));
    pos = comma + 1;
  }
  const std::string mode = extra("xdp_mode");
  if (mode.empty() || mode == "auto") {
    c.xdp_mode = net::XdpMode::Auto;
  } else if (mode == "zerocopy") {
    c.xdp_mode = net::XdpMode::ZeroCopy;
  } else if (mode == "native_copy") {
    c.xdp_mode = net::XdpMode::NativeCopy;
  } else if (mode == "generic") {
    c.xdp_mode = net::XdpMode::Generic;
  } else {
    throw bad("xdp_mode", "expected auto, zerocopy, native_copy or generic");
  }
  c.rcvbuf_bytes = static_cast<int>(extra_u64("rcvbuf", 0, 0, 1U << 30));
  c.batch = static_cast<std::uint32_t>(extra_u64("batch", c.batch, 1, 1024));
  c.rerequest = extra("rerequest");
  net::SockAddr probe;
  if (!c.rerequest.empty() && !parse_ip_port(c.rerequest, probe))
    throw bad("rerequest", "expected \"<IPv4 address>:<port>\"");
  c.glimpse_url = extra("glimpse_url");
  if (!c.glimpse_url.empty() && !parse_ip_port(c.glimpse_url, probe))
    throw bad("glimpse_url", "expected \"<IPv4 address>:<port>\"");
  if (const std::string u = extra("glimpse_username"); !u.empty()) c.glimpse_username = u;
  if (const std::string p = extra("glimpse_password"); !p.empty()) c.glimpse_password = p;
  if (c.glimpse_username.size() > 6) throw bad("glimpse_username", "at most 6 characters");
  if (c.glimpse_password.size() > 10) throw bad("glimpse_password", "at most 10 characters");
  c.reorder_packets =
      static_cast<std::uint32_t>(extra_u64("reorder_packets", c.reorder_packets, 0, 1U << 20));
  c.gap_timeout_ns = static_cast<std::int64_t>(
      extra_u64("gap_timeout_ns", static_cast<std::uint64_t>(c.gap_timeout_ns), 0, 10'000'000'000));
  c.max_request_attempts = static_cast<std::uint32_t>(
      extra_u64("max_request_attempts", c.max_request_attempts, 0, 1'000'000));
  c.request_timeout_ns =
      static_cast<std::int64_t>(extra_u64("request_timeout_ns",
                                          static_cast<std::uint64_t>(c.request_timeout_ns),
                                          1'000'000,
                                          60'000'000'000));
  c.recovery_buffer_packets = static_cast<std::uint32_t>(
      extra_u64("recovery_buffer_packets", c.recovery_buffer_packets, 16, 1U << 22));
  c.depth = static_cast<std::uint32_t>(
      extra_u64("depth", c.depth, 1, codecs::itch::ItchL2Bridge::kMaxDepth));
  c.price_window_ticks = extra_u64("price_window_ticks", c.price_window_ticks, 64, 1U << 24);
  if (c.price_window_ticks % 64 != 0) throw bad("price_window_ticks", "must be a multiple of 64");
  c.max_orders = extra_u64("max_orders", c.max_orders, 16, 1U << 26);
  c.hw_timestamps = extra_bool("hw_timestamps", false);
  const std::string clock = extra("hw_clock");
  if (clock.empty() || clock == "none") {
    c.hw_clock = HwClock::None;
  } else if (clock == "phc_synced") {
    c.hw_clock = HwClock::PhcSynced;
  } else {
    throw bad("hw_clock", "expected none or phc_synced");
  }
  const std::string oe = extra("order_entry");
  if (oe.empty() || oe == "none") {
    c.order_entry = OrderEntry::None;
  } else if (oe == "sim_ouch") {
    c.order_entry = OrderEntry::SimOuch;
  } else {
    throw bad("order_entry", "expected none or sim_ouch");
  }
  c.ouch_url = extra("ouch_url");
  if (c.order_entry == OrderEntry::SimOuch && !parse_ip_port(c.ouch_url, probe))
    throw bad("ouch_url", "sim_ouch needs \"<IPv4 address>:<port>\"");
  if (const std::string u = extra("ouch_username"); !u.empty()) c.ouch_username = u;
  if (const std::string p = extra("ouch_password"); !p.empty()) c.ouch_password = p;
  if (c.ouch_username.size() > 6) throw bad("ouch_username", "at most 6 characters");
  if (c.ouch_password.size() > 10) throw bad("ouch_password", "at most 10 characters");
  const std::string transport = extra("order_transport");
  if (transport.empty() || transport == "kernel") {
    c.order_transport = OrderTransport::Kernel;
  } else if (transport == "user_tcp") {
    c.order_transport = OrderTransport::UserTcp;
    c.user_tcp_interface = extra("user_tcp_interface");
    if (c.user_tcp_interface.empty()) c.user_tcp_interface = interface;
    if (c.user_tcp_interface.empty() && c.rx_backend != RxBackend::Dpdk)
      throw bad("user_tcp_interface", "user_tcp needs an interface name (or interface)");
    c.user_tcp_port = static_cast<std::uint16_t>(extra_u64("user_tcp_port", 0, 0, 65535));
    c.user_tcp_ip = extra("user_tcp_ip");
    std::uint32_t probe_ip = 0;
    if (!net::parse_ipv4(c.user_tcp_ip, probe_ip))
      throw bad("user_tcp_ip", "user_tcp needs its own IPv4 address");
    c.user_tcp_gateway = extra("user_tcp_gateway");
    if (!c.user_tcp_gateway.empty() && !net::parse_ipv4(c.user_tcp_gateway, probe_ip))
      throw bad("user_tcp_gateway", "expected an IPv4 address");
  } else {
    throw bad("order_transport", "expected kernel or user_tcp");
  }
  if (dry_run) c.order_entry = OrderEntry::None;
  return c;
}

}  // namespace fastmm::venues::nasdaq
