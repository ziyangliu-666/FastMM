#include "fastmm/net/kernel_datagram_source.hpp"

#include <cerrno>
#include <cstring>
#include <ctime>
#include <utility>

#ifndef SCM_TIMESTAMPING
#define SCM_TIMESTAMPING SO_TIMESTAMPING  // NOLINT(readability-identifier-naming): Linux 2.6.30
#endif

namespace fastmm::net {

namespace {

// SCM_TIMESTAMPING payload (struct scm_timestamping): ts[0] software, ts[1] unused, ts[2] raw
// hardware.
constexpr std::size_t kTimestampingLen = 3 * sizeof(timespec);

// CMSG_* without the macros' C casts. The kernel aligns control messages to sizeof(size_t).
constexpr std::size_t cmsg_align(std::size_t len) noexcept {
  return (len + sizeof(std::size_t) - 1) & ~(sizeof(std::size_t) - 1);
}
constexpr std::size_t kCmsgHdrLen = cmsg_align(sizeof(cmsghdr));
constexpr std::size_t kCmsgSpace = kCmsgHdrLen + cmsg_align(kTimestampingLen);

// Next control message after `c` (the first one when `c` is null), or null.
const cmsghdr* next_cmsg(const msghdr& h, const cmsghdr* c) noexcept {
  const auto* begin = static_cast<const std::byte*>(h.msg_control);
  const std::byte* end = begin + h.msg_controllen;
  const std::byte* p =
      c == nullptr ? begin : reinterpret_cast<const std::byte*>(c) + cmsg_align(c->cmsg_len);
  if (static_cast<std::size_t>(end - p) < sizeof(cmsghdr)) return nullptr;
  const auto* next = reinterpret_cast<const cmsghdr*>(p);
  if (next->cmsg_len < sizeof(cmsghdr) || next->cmsg_len > static_cast<std::size_t>(end - p)) {
    return nullptr;
  }
  return next;
}

std::int64_t to_ns(const timespec& t) noexcept {
  return static_cast<std::int64_t>(t.tv_sec) * 1'000'000'000 + t.tv_nsec;
}

}  // namespace

KernelSourceOpen KernelDatagramSource::open(const KernelSourceConfig& cfg) {
  close();
  KernelSourceOpen res;
  auto fail = [&](std::size_t line, std::string_view step, int err) {
    close();
    res.err = err;
    res.line = line;
    res.step = step;
    return res;
  };
  const std::size_t n_lines = cfg.subscriptions.size();
  if (n_lines == 0 || n_lines > 256 || cfg.batch == 0 || cfg.batch > 1024 ||
      cfg.max_datagram == 0 || cfg.max_datagram > 65'535) {
    return fail(0, "config", -EINVAL);
  }

  lines_.reserve(n_lines);
  for (std::size_t i = 0; i < n_lines; ++i) {
    const DatagramSubscription& sub = cfg.subscriptions[i];
    McastInterface ifc;
    if (const int rc = McastInterface::resolve(sub.interface, ifc); rc != 0) {
      return fail(i, "interface", rc);
    }
    std::uint32_t group = 0;
    if (!parse_ipv4(sub.group, group) || !is_ipv4_multicast(group) || sub.port == 0) {
      return fail(i, "group", -EINVAL);
    }
    std::uint32_t source = 0;
    if (!sub.source.empty() && !parse_ipv4(sub.source, source)) return fail(i, "source", -EINVAL);

    UdpSocket s = UdpSocket::open();
    if (!s.valid()) return fail(i, "socket", -s.last_error());
    if (const int rc = s.set_reuseaddr(true); rc != 0) return fail(i, "SO_REUSEADDR", rc);
    if (const int rc = s.set_multicast_all(false); rc != 0) return fail(i, "IP_MULTICAST_ALL", rc);
    if (cfg.rcvbuf_bytes > 0) {
      if (const int rc = s.set_rcvbuf(cfg.rcvbuf_bytes); rc != 0) return fail(i, "SO_RCVBUF", rc);
    }
    if (cfg.timestamps) {
      if (const int rc = s.enable_rx_timestamps(); rc != 0) return fail(i, "SO_TIMESTAMPING", rc);
    }
    SockAddr bind_addr = SockAddr::any_v4(sub.port);
    reinterpret_cast<sockaddr_in*>(&bind_addr.storage)->sin_addr.s_addr = group;
    if (const int rc = s.bind(bind_addr); rc != 0) return fail(i, "bind", rc);
    const int join_rc =
        source == 0 ? s.join_group(group, ifc) : s.join_source_group(group, source, ifc);
    if (join_rc != 0) return fail(i, source == 0 ? "IP_ADD_MEMBERSHIP" : "source join", join_rc);
    if (cfg.busy_poll) {
      auto first_error = [](int& slot, int rc) noexcept {
        if (slot == 0) slot = rc;
      };
      first_error(res.busy_poll, s.set_busy_poll(cfg.busy_poll_usec));
      first_error(res.prefer_busy_poll, s.set_prefer_busy_poll(true));
      if (cfg.busy_poll_budget > 0) {
        first_error(res.busy_poll_budget, s.set_busy_poll_budget(cfg.busy_poll_budget));
      }
    }
    if (i == 0) res.rcvbuf = s.rcvbuf();
    lines_.push_back(Line{std::move(s), group, sub.port});
  }

  batch_ = cfg.batch;
  max_datagram_ = cfg.max_datagram;
  cmsg_space_ = kCmsgSpace;
  const std::size_t slots = n_lines * batch_;
  msgs_.assign(slots, mmsghdr{});
  iovs_.assign(slots, iovec{});
  names_.assign(slots, sockaddr_in{});
  cmsgs_.assign(slots * cmsg_space_, std::byte{0});
  payload_.assign(slots * max_datagram_, std::byte{0});
  lens_.assign(slots, 0);
  metas_.assign(slots, RxMeta{});
  for (std::size_t i = 0; i < slots; ++i) {
    iovs_[i].iov_base = payload_.data() + i * max_datagram_;
    iovs_[i].iov_len = max_datagram_;
    msghdr& h = msgs_[i].msg_hdr;
    h.msg_name = &names_[i];
    h.msg_namelen = sizeof(sockaddr_in);
    h.msg_iov = &iovs_[i];
    h.msg_iovlen = 1;
    h.msg_control = cmsgs_.data() + i * cmsg_space_;
    h.msg_controllen = cmsg_space_;
  }
  stats_ = DatagramSourceStats{};
  return res;
}

void KernelDatagramSource::close() noexcept {
  lines_.clear();  // closes the sockets, which leaves the groups
  batch_ = 0;
}

std::size_t KernelDatagramSource::receive(std::size_t line) noexcept {
  const std::size_t base = line * batch_;
  const int rc = lines_[line].sock.recv_batch(std::span<mmsghdr>(msgs_.data() + base, batch_));
  if (rc <= 0) {
    if (rc < 0) {
      ++stats_.errors;
      stats_.last_error = -rc;
    }
    return 0;
  }
  const Cycles t0 = rdtscp();
  const std::int64_t t0_wall = wall_now().ns;
  const auto n = static_cast<std::size_t>(rc);
  const Line& l = lines_[line];
  for (std::size_t i = base; i < base + n; ++i) {
    msghdr& h = msgs_[i].msg_hdr;
    RxMeta& m = metas_[i];
    m.t0_cycles = t0;
    m.t0_wall_ns = t0_wall;
    m.hw_ts_ns = 0;
    m.sw_ts_ns = 0;
    m.src_ip = names_[i].sin_addr.s_addr;
    m.dst_ip = l.group_be;
    m.dst_port = l.port;
    m.line = static_cast<std::uint8_t>(line);
    for (const cmsghdr* c = next_cmsg(h, nullptr); c != nullptr; c = next_cmsg(h, c)) {
      if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_TIMESTAMPING ||
          c->cmsg_len < kCmsgHdrLen + kTimestampingLen) {
        continue;
      }
      timespec ts[3];
      std::memcpy(ts, reinterpret_cast<const std::byte*>(c) + kCmsgHdrLen, kTimestampingLen);
      m.sw_ts_ns = to_ns(ts[0]);
      m.hw_ts_ns = to_ns(ts[2]);
    }
    if ((h.msg_flags & MSG_TRUNC) != 0) {
      lens_[i] = kDropped;
      ++stats_.truncated;
    } else {
      lens_[i] = msgs_[i].msg_len;
      ++stats_.datagrams;
      stats_.bytes += msgs_[i].msg_len;
    }
    // The kernel overwrites these on every call.
    h.msg_namelen = sizeof(sockaddr_in);
    h.msg_controllen = cmsg_space_;
    h.msg_flags = 0;
  }
  return n;
}

}  // namespace fastmm::net
