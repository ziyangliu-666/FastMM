// The strategy's side of a gateway attachment (live/gateway.hpp).
#include "fastmm/live/gateway.hpp"
#include "fastmm/store/reader.hpp"

#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <new>
#include <vector>

namespace fastmm::live {

namespace {

// How long the gateway may take to answer: it switches every venue's sinks on their network
// threads and starts the reconciliations before it replies.
constexpr int kReplyTimeoutMs = 10'000;

// Every venue's resume fits in one attach, so none is ever cut short.
static_assert(gw::kMaxKnownExecIds >= kMaxVenuesConfig * store::Recovery::kMaxKnownExecIds);

template <std::size_t N>
std::string from_field(const char (&f)[N]) {
  return std::string(f, ::strnlen(f, N));
}

template <std::size_t N>
void to_field(char (&f)[N], std::string_view s) {
  std::memset(f, 0, N);
  std::memcpy(f, s.data(), std::min(s.size(), N - 1));
}

}  // namespace

namespace gw {

int create_wake_page() noexcept {
  const int fd = ::memfd_create("fastmm-gw-wake", MFD_CLOEXEC);
  if (fd < 0) return -1;
  if (::ftruncate(fd, static_cast<off_t>(kWakePageBytes)) != 0) {
    const int e = errno;
    ::close(fd);
    errno = e;
    return -1;
  }
  WakePage* page = map_wake_page(fd);
  if (page == nullptr) {
    const int e = errno;
    ::close(fd);
    errno = e;
    return -1;
  }
  new (page) WakePage{};
  unmap_wake_page(page);
  return fd;
}

WakePage* map_wake_page(int fd) noexcept {
  void* p = ::mmap(nullptr, kWakePageBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  return p == MAP_FAILED ? nullptr : static_cast<WakePage*>(p);
}

void unmap_wake_page(WakePage* page) noexcept {
  if (page != nullptr) ::munmap(page, kWakePageBytes);
}

void wake_if_blocked(SleepFlag& flag, int eventfd) noexcept {
  if (!flag.take()) return;
  const std::uint64_t one = 1;
  // EAGAIN means the counter is saturated, which still wakes the reactor.
  [[maybe_unused]] const ssize_t n = ::write(eventfd, &one, sizeof one);
}

ssize_t send_with_fds(int sock, const void* data, std::size_t len, std::span<const int> fds) {
  iovec iov{const_cast<void*>(data), len};
  msghdr msg{};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  std::vector<std::byte> ctl;
  if (!fds.empty()) {
    ctl.resize(CMSG_SPACE(fds.size() * sizeof(int)));
    msg.msg_control = ctl.data();
    msg.msg_controllen = ctl.size();
    cmsghdr* c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(fds.size() * sizeof(int));
    std::memcpy(CMSG_DATA(c), fds.data(), fds.size() * sizeof(int));
  }
  return ::sendmsg(sock, &msg, MSG_NOSIGNAL);
}

ssize_t recv_with_fds(
    int sock, void* data, std::size_t len, int* fds, std::size_t max_fds, std::size_t* nfds) {
  *nfds = 0;
  iovec iov{data, len};
  msghdr msg{};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  std::vector<std::byte> ctl(CMSG_SPACE(std::max<std::size_t>(max_fds, 1) * sizeof(int)));
  msg.msg_control = ctl.data();
  msg.msg_controllen = ctl.size();
  const ssize_t n = ::recvmsg(sock, &msg, MSG_CMSG_CLOEXEC);
  if (n < 0) return n;
  for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c != nullptr; c = CMSG_NXTHDR(&msg, c)) {
    if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
    const std::size_t k = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
    for (std::size_t i = 0; i < k; ++i) {
      int fd = -1;
      std::memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof fd);
      if (*nfds < max_fds) {
        fds[(*nfds)++] = fd;
      } else {
        ::close(fd);
      }
    }
  }
  return n;
}

}  // namespace gw

std::unique_ptr<GatewayClient> GatewayClient::attach(const std::string& path,
                                                     const GatewayAttachRequest& req,
                                                     std::string* error) {
  const auto fail = [&](std::string why) -> std::unique_ptr<GatewayClient> {
    if (error != nullptr) *error = std::move(why);
    return nullptr;
  };
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof addr.sun_path) return fail("gateway socket path too long: " + path);
  std::memcpy(addr.sun_path, path.data(), path.size());
  std::unique_ptr<GatewayClient> c(new GatewayClient());
  c->fd_ = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (c->fd_ < 0) return fail(std::string("socket: ") + std::strerror(errno));
  if (::connect(c->fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0)
    return fail("connect " + path + ": " + std::strerror(errno));

  // The ids are never cut short: one the gateway did not hear of would be booked twice. The store
  // keeps each venue's to what fits (Recovery::kMaxKnownExecIds, moving the start later).
  if (req.resume.size() > kMaxVenuesConfig) return fail("too many venues to resume");
  std::size_t known = 0;
  for (const GatewayAttachRequest::Resume& v : req.resume) known += v.known_exec_ids.size();
  if (known > gw::kMaxKnownExecIds)
    return fail("the store lists " + std::to_string(known) + " trade ids to skip, more than the " +
                std::to_string(gw::kMaxKnownExecIds) + " an attach carries");
  if (req.instruments.size() > kMaxInstruments) return fail("too many instruments to claim");
  const std::size_t claims = req.instruments.size();
  if (req.positions.size() > kMaxInstruments) return fail("too many positions to report");
  const std::size_t positions = req.positions.size();
  const std::size_t resumes = req.resume.size();
  std::vector<std::byte> out(sizeof(gw::AttachRequest) + known * sizeof(gw::ExecId) +
                             claims * sizeof(gw::InstrumentClaim) +
                             positions * sizeof(gw::PositionSeed) +
                             resumes * sizeof(gw::VenueResume));
  gw::AttachRequest r{};
  r.hdr = gw::Header{gw::kMagic,
                     gw::kVersion,
                     gw::MsgType::AttachRequest,
                     static_cast<std::uint32_t>(out.size()),
                     0};
  to_field(r.engine, req.engine);
  r.pid = static_cast<std::uint32_t>(::getpid());
  r.flags = (resumes > 0 ? gw::kResumeExecutions : 0U) | (req.blocks ? gw::kStrategyBlocks : 0U);
  r.resume_count = static_cast<std::uint32_t>(resumes);
  r.known_count = static_cast<std::uint32_t>(known);
  r.claim_count = static_cast<std::uint32_t>(claims);
  r.position_count = static_cast<std::uint32_t>(positions);
  std::memcpy(out.data(), &r, sizeof r);
  std::size_t next_id = 0;
  for (const GatewayAttachRequest::Resume& v : req.resume) {
    for (const std::string& s : v.known_exec_ids) {
      gw::ExecId id{};
      to_field(id.id, s);
      std::memcpy(out.data() + sizeof r + next_id++ * sizeof id, &id, sizeof id);
    }
  }
  std::byte* claim_at = out.data() + sizeof r + known * sizeof(gw::ExecId);
  for (std::size_t i = 0; i < claims; ++i) {
    gw::InstrumentClaim cl{};
    to_field(cl.venue, req.instruments[i].first);
    to_field(cl.symbol, req.instruments[i].second);
    std::memcpy(claim_at + i * sizeof cl, &cl, sizeof cl);
  }
  std::byte* position_at = claim_at + claims * sizeof(gw::InstrumentClaim);
  for (std::size_t i = 0; i < positions; ++i) {
    gw::PositionSeed ps{};
    to_field(ps.venue, req.positions[i].venue);
    to_field(ps.symbol, req.positions[i].symbol);
    ps.qty = req.positions[i].qty.raw;
    ps.avg_px = req.positions[i].avg_px.raw;
    std::memcpy(position_at + i * sizeof ps, &ps, sizeof ps);
  }
  std::byte* resume_at = position_at + positions * sizeof(gw::PositionSeed);
  std::size_t first = 0;
  for (std::size_t i = 0; i < resumes; ++i) {
    gw::VenueResume vr{};
    to_field(vr.venue, req.resume[i].venue);
    vr.since_ms = req.resume[i].since_ms;
    vr.first_known = static_cast<std::uint32_t>(first);
    vr.known_count = static_cast<std::uint32_t>(req.resume[i].known_exec_ids.size());
    first += vr.known_count;
    std::memcpy(resume_at + i * sizeof vr, &vr, sizeof vr);
  }
  if (::send(c->fd_, out.data(), out.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(out.size()))
    return fail(std::string("send attach request: ") + std::strerror(errno));

  pollfd pfd{c->fd_, POLLIN, 0};
  const int pr = ::poll(&pfd, 1, kReplyTimeoutMs);
  if (pr == 0) return fail("the gateway did not answer the attach request");
  if (pr < 0) return fail(std::string("poll: ") + std::strerror(errno));
  std::vector<std::byte> in(gw::kMaxDatagram);
  int fds[10];
  std::size_t nfds = 0;
  const ssize_t n = gw::recv_with_fds(c->fd_, in.data(), in.size(), fds, 10, &nfds);
  // Owned by the client from here on, so every failure below closes them.
  if (nfds >= 2) {
    c->page_ = gw::map_wake_page(fds[0]);
    c->net_page_ = gw::map_wake_page(fds[1]);
    ::close(fds[0]);
    ::close(fds[1]);
    if (c->page_ == nullptr || c->net_page_ == nullptr) {
      for (std::size_t i = 2; i < nfds; ++i) ::close(fds[i]);
      return fail(std::string("cannot map the gateway's wake pages: ") + std::strerror(errno));
    }
    for (std::size_t i = 2; i < nfds; ++i) c->wake_fds_[c->wake_count_++] = fds[i];
  } else {
    for (std::size_t i = 0; i < nfds; ++i) ::close(fds[i]);
  }
  if (n <= 0) return fail("the gateway closed the connection without answering");
  if (static_cast<std::size_t>(n) < sizeof(gw::AttachReply)) return fail("short attach reply");
  gw::AttachReply rep{};
  std::memcpy(&rep, in.data(), sizeof rep);
  if (rep.hdr.magic != gw::kMagic || rep.hdr.type != gw::MsgType::AttachReply)
    return fail("not a gateway reply");
  if (rep.hdr.version != gw::kVersion)
    return fail("gateway protocol version " + std::to_string(rep.hdr.version) + ", this is " +
                std::to_string(gw::kVersion));
  if (rep.status != 0) return fail("the gateway refused: " + from_field(rep.error));
  if (rep.instrument_bytes != sizeof(Instrument))
    return fail("the gateway's Instrument is " + std::to_string(rep.instrument_bytes) +
                " bytes, this build's " + std::to_string(sizeof(Instrument)) +
                ": run the same FastMM build on both sides");
  const std::size_t need = sizeof rep + rep.venue_count * sizeof(gw::VenueInfo) +
                           static_cast<std::size_t>(rep.instrument_count) * sizeof(Instrument);
  if (static_cast<std::size_t>(n) != need || rep.hdr.bytes != need)
    return fail("attach reply of " + std::to_string(n) + " bytes, expected " +
                std::to_string(need));
  if (rep.venue_count > 8 || rep.instrument_count > kMaxInstruments)
    return fail("attach reply names too many venues or instruments");
  if (c->page_ == nullptr || c->wake_count_ != rep.venue_count)
    return fail("the attach reply carries " + std::to_string(nfds) + " descriptor(s), expected " +
                std::to_string(rep.venue_count + 2));
  if (rep.session_epoch == 0) return fail("the gateway gave no session epoch");
  c->attach_id_ = rep.attach_id;
  c->epoch_ = rep.session_epoch;
  c->gateway_blocks_ = (rep.flags & gw::kGatewayBlocks) != 0;
  const std::byte* p = in.data() + sizeof rep;
  for (std::uint32_t i = 0; i < rep.venue_count; ++i, p += sizeof(gw::VenueInfo)) {
    gw::VenueInfo vi{};
    std::memcpy(&vi, p, sizeof vi);
    GatewayVenue v;
    v.id = VenueId{vi.id};
    v.name = from_field(vi.name);
    v.replace = (vi.flags & gw::kVenueReplace) != 0;
    v.executions = (vi.flags & gw::kVenueExecutions) != 0;
    if (vi.id != i) return fail("the gateway's venue ids are not dense");
    const auto open = [&](const char(&f)[128], std::unique_ptr<ShmRing>& ring) {
      auto opened = ShmRing::open(from_field(f));
      if (!opened) {
        if (error != nullptr) *error = opened.error();
        return false;
      }
      ring = std::make_unique<ShmRing>(std::move(*opened));
      return true;
    };
    if (!open(vi.md_path, v.md) || !open(vi.order_path, v.order) ||
        !open(vi.outbound_path, v.outbound))
      return nullptr;
    c->venues_.push_back(std::move(v));
  }
  for (std::uint32_t i = 0; i < rep.instrument_count; ++i, p += sizeof(Instrument)) {
    Instrument inst{};
    std::memcpy(static_cast<void*>(&inst), p, sizeof inst);
    auto id = c->instruments_.add(inst);
    if (!id || id->value != i) return fail("the gateway's instrument table does not load here");
  }
  return c;
}

GatewayClient::~GatewayClient() {
  close();
  for (std::size_t i = 0; i < wake_count_; ++i) ::close(wake_fds_[i]);
  gw::unmap_wake_page(page_);
  gw::unmap_wake_page(net_page_);
}

// The engine published into the outbound ring first (a release store); take() is a locked
// exchange, so either the network thread's recheck after setting its flag sees the message or this
// sees the flag (the protocol of wake_venue in live/venue_slot.cpp).
void GatewayClient::wake_venue(void* ctx, VenueId v) noexcept {
  auto* c = static_cast<GatewayClient*>(ctx);
  if (v.value < c->wake_count_)
    gw::wake_if_blocked(c->net_page_->net[v.value].flag, c->wake_fds_[v.value]);
}

bool GatewayClient::connected() noexcept {
  if (fd_ < 0) return false;
  pollfd pfd{fd_, POLLIN | POLLRDHUP, 0};
  if (::poll(&pfd, 1, 0) <= 0) return true;
  // The gateway sends nothing after its reply: readable means closed.
  return (pfd.revents & (POLLIN | POLLRDHUP | POLLHUP | POLLERR)) == 0;
}

void GatewayClient::close() noexcept {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

}  // namespace fastmm::live
