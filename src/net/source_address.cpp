#include "fastmm/net/source_address.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

#include <algorithm>
#include <cstring>

namespace fastmm::net {

namespace {

// getifaddrs() as a list, freed on scope exit.
class Interfaces {
 public:
  Interfaces() {
    if (::getifaddrs(&head_) != 0) head_ = nullptr;
  }
  ~Interfaces() {
    if (head_ != nullptr) ::freeifaddrs(head_);
  }
  Interfaces(const Interfaces&) = delete;
  Interfaces& operator=(const Interfaces&) = delete;
  [[nodiscard]] bool ok() const noexcept { return head_ != nullptr; }
  template <class F>
  void each(const F& f) const {
    for (const ifaddrs* a = head_; a != nullptr; a = a->ifa_next) {
      if (a->ifa_addr == nullptr) continue;
      const int fam = a->ifa_addr->sa_family;
      if (fam == AF_INET || fam == AF_INET6) f(*a);
    }
  }

 private:
  ifaddrs* head_ = nullptr;
};

SockAddr of(const sockaddr* sa) {
  SockAddr out;
  const socklen_t len = sa->sa_family == AF_INET6 ? sizeof(sockaddr_in6)
                                                  : static_cast<socklen_t>(sizeof(sockaddr_in));
  std::memcpy(&out.storage, sa, len);
  out.len = len;
  return out;
}

bool same_ip(const SockAddr& a, const sockaddr* b) {
  if (a.family() != b->sa_family) return false;
  if (a.family() == AF_INET) {
    return reinterpret_cast<const sockaddr_in*>(&a.storage)->sin_addr.s_addr ==
           reinterpret_cast<const sockaddr_in*>(b)->sin_addr.s_addr;
  }
  return std::memcmp(&reinterpret_cast<const sockaddr_in6*>(&a.storage)->sin6_addr,
                     &reinterpret_cast<const sockaddr_in6*>(b)->sin6_addr,
                     sizeof(in6_addr)) == 0;
}

// An IPv4 address inside a loopback interface's network (Linux delivers all of 127.0.0.0/8 to
// lo, though getifaddrs lists 127.0.0.1 alone).
bool on_loopback_net(const SockAddr& a, const ifaddrs& ifa) {
  if ((ifa.ifa_flags & IFF_LOOPBACK) == 0 || a.family() != AF_INET ||
      ifa.ifa_addr->sa_family != AF_INET || ifa.ifa_netmask == nullptr)
    return false;
  const std::uint32_t mask = reinterpret_cast<const sockaddr_in*>(ifa.ifa_netmask)->sin_addr.s_addr;
  const std::uint32_t net = reinterpret_cast<const sockaddr_in*>(ifa.ifa_addr)->sin_addr.s_addr;
  const std::uint32_t ip = reinterpret_cast<const sockaddr_in*>(&a.storage)->sin_addr.s_addr;
  return (ip & mask) == (net & mask);
}

std::string ip_text(const SockAddr& a) {
  char buf[INET6_ADDRSTRLEN] = {};
  const void* src =
      a.family() == AF_INET6
          ? static_cast<const void*>(&reinterpret_cast<const sockaddr_in6*>(&a.storage)->sin6_addr)
          : static_cast<const void*>(&reinterpret_cast<const sockaddr_in*>(&a.storage)->sin_addr);
  if (::inet_ntop(a.family(), src, buf, sizeof buf) == nullptr) return {};
  return buf;
}

// The interface's first address of `family`.
std::optional<SockAddr> interface_address(const std::string& name, int family) {
  const Interfaces ifs;
  std::optional<SockAddr> out;
  ifs.each([&](const ifaddrs& a) {
    if (!out && a.ifa_addr->sa_family == family && name == a.ifa_name) {
      out = of(a.ifa_addr);
      if (family == AF_INET6) reinterpret_cast<sockaddr_in6*>(&out->storage)->sin6_port = 0;
      if (family == AF_INET) reinterpret_cast<sockaddr_in*>(&out->storage)->sin_port = 0;
    }
  });
  return out;
}

}  // namespace

std::string source_syntax_error(const SourceAddress& src) {
  if (!src.ip.empty() && !src.interface.empty())
    return "source_ip and source_interface are exclusive: name one";
  if (!src.ip.empty() && !SockAddr::from_ip(src.ip, 0))
    return "source_ip '" + src.ip + "' is not an IPv4 or IPv6 address";
  if (!src.interface.empty() && src.interface.size() >= IFNAMSIZ)
    return "source_interface '" + src.interface + "' is longer than an interface name can be";
  return {};
}

std::string check_source(const SourceAddress& src) {
  if (src.empty()) return {};
  if (std::string e = source_syntax_error(src); !e.empty()) return e;
  const Interfaces ifs;
  if (!ifs.ok()) return "cannot list this host's addresses (getifaddrs failed)";
  if (!src.ip.empty()) {
    const std::optional<SockAddr> parsed = SockAddr::from_ip(src.ip, 0);
    if (!parsed) return "source_ip '" + src.ip + "' is not an IPv4 or IPv6 address";
    const SockAddr want = *parsed;
    bool found = false;
    ifs.each([&](const ifaddrs& a) {
      found = found || same_ip(want, a.ifa_addr) || on_loopback_net(want, a);
    });
    if (!found) return "source_ip " + src.ip + " is not an address of this host";
    return {};
  }
  bool exists = false;
  bool has_address = false;
  ifs.each([&](const ifaddrs& a) {
    if (src.interface != a.ifa_name) return;
    exists = true;
    has_address = true;
  });
  if (!exists && ::if_nametoindex(src.interface.c_str()) == 0)
    return "source_interface " + src.interface + " does not exist on this host";
  if (!has_address) return "source_interface " + src.interface + " has no IPv4 or IPv6 address";
  return {};
}

std::optional<SockAddr> source_bind_address(const SourceAddress& src, int family) {
  if (!src.ip.empty()) {
    auto a = SockAddr::from_ip(src.ip, 0);
    if (!a || a->family() != family) return std::nullopt;
    return a;
  }
  if (!src.interface.empty()) return interface_address(src.interface, family);
  return std::nullopt;
}

void keep_reachable(const SourceAddress& src, std::vector<SockAddr>& peers) {
  if (src.empty()) return;
  const bool v4 = source_bind_address(src, AF_INET).has_value();
  const bool v6 = source_bind_address(src, AF_INET6).has_value();
  std::erase_if(peers, [&](const SockAddr& p) {
    if (p.family() == AF_INET) return !v4;
    if (p.family() == AF_INET6) return !v6;
    return true;
  });
}

std::string source_label(const SourceAddress& src) {
  if (!src.ip.empty()) {
    const auto a = SockAddr::from_ip(src.ip, 0);
    return a ? ip_text(*a) : src.ip;
  }
  if (src.interface.empty()) return {};
  for (const int family : {AF_INET, AF_INET6}) {
    if (const auto a = interface_address(src.interface, family)) return ip_text(*a);
  }
  return "if:" + src.interface;
}

}  // namespace fastmm::net
