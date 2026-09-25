// The strategy's side of a gateway attachment (live/gateway.hpp).
#include "fastmm/live/gateway.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <vector>

namespace fastmm::live {

namespace {

// How long the gateway may take to answer: it switches every venue's sinks on their network
// threads and starts the reconciliations before it replies.
constexpr int kReplyTimeoutMs = 10'000;

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

  const std::size_t known = std::min<std::size_t>(req.known_exec_ids.size(), gw::kMaxKnownExecIds);
  std::vector<std::byte> out(sizeof(gw::AttachRequest) + known * sizeof(gw::ExecId));
  gw::AttachRequest r{};
  r.hdr = gw::Header{gw::kMagic,
                     gw::kVersion,
                     gw::MsgType::AttachRequest,
                     static_cast<std::uint32_t>(out.size()),
                     0};
  to_field(r.engine, req.engine);
  r.pid = static_cast<std::uint32_t>(::getpid());
  r.flags = req.resume_executions ? gw::kResumeExecutions : 0U;
  r.exec_since_ms = req.exec_since_ms;
  r.known_count = static_cast<std::uint32_t>(known);
  std::memcpy(out.data(), &r, sizeof r);
  for (std::size_t i = 0; i < known; ++i) {
    gw::ExecId id{};
    to_field(id.id, req.known_exec_ids[i]);
    std::memcpy(out.data() + sizeof r + i * sizeof id, &id, sizeof id);
  }
  if (::send(c->fd_, out.data(), out.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(out.size()))
    return fail(std::string("send attach request: ") + std::strerror(errno));

  pollfd pfd{c->fd_, POLLIN, 0};
  const int pr = ::poll(&pfd, 1, kReplyTimeoutMs);
  if (pr == 0) return fail("the gateway did not answer the attach request");
  if (pr < 0) return fail(std::string("poll: ") + std::strerror(errno));
  std::vector<std::byte> in(gw::kMaxDatagram);
  const ssize_t n = ::recv(c->fd_, in.data(), in.size(), 0);
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
  c->attach_id_ = rep.attach_id;
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
