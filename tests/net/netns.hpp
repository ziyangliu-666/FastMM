#pragma once
// Moves the calling process into a new user and network namespace whose lo carries multicast:
// the equivalent of `unshare -Urn` followed by `ip link set lo up multicast on` and
// `ip route add 224.0.0.0/4 dev lo`. No privileges needed where unprivileged user namespaces are
// allowed. Shared by tests/net, tests/hotpath and bench/bench_udp.cpp; no doctest dependency.
#include <fcntl.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

namespace fastmm::net::test {

namespace detail {

inline bool write_file(const char* path, const std::string& text) {
  const int fd = ::open(path, O_WRONLY | O_CLOEXEC);
  if (fd < 0) return false;
  const bool ok = ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
  ::close(fd);
  return ok;
}

inline std::string failed(const char* step) {
  return std::string(step) + ": " + std::strerror(errno);
}

}  // namespace detail

// Returns "" on success, otherwise the step that failed and its errno. The process must be
// single-threaded (unshare(CLONE_NEWUSER) fails with EINVAL otherwise). Not reversible.
inline std::string enter_multicast_netns() {
  const uid_t uid = ::getuid();
  const gid_t gid = ::getgid();
  if (::unshare(CLONE_NEWUSER | CLONE_NEWNET) != 0) return detail::failed("unshare");
  if (!detail::write_file("/proc/self/setgroups", "deny")) return detail::failed("setgroups");
  if (!detail::write_file("/proc/self/uid_map", "0 " + std::to_string(uid) + " 1")) {
    return detail::failed("uid_map");
  }
  if (!detail::write_file("/proc/self/gid_map", "0 " + std::to_string(gid) + " 1")) {
    return detail::failed("gid_map");
  }
  const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return detail::failed("socket");
  std::string err;
  ifreq ifr{};
  std::strncpy(ifr.ifr_name, "lo", sizeof(ifr.ifr_name) - 1);
  if (::ioctl(fd, SIOCGIFFLAGS, &ifr) != 0) {
    err = detail::failed("SIOCGIFFLAGS lo");
  } else {
    ifr.ifr_flags = static_cast<short>(ifr.ifr_flags | IFF_UP | IFF_MULTICAST);
    if (::ioctl(fd, SIOCSIFFLAGS, &ifr) != 0) err = detail::failed("SIOCSIFFLAGS lo");
  }
  if (err.empty()) {
    rtentry rt{};
    auto* dst = reinterpret_cast<sockaddr_in*>(&rt.rt_dst);
    dst->sin_family = AF_INET;
    dst->sin_addr.s_addr = htonl(0xE0000000U);  // 224.0.0.0/4
    auto* mask = reinterpret_cast<sockaddr_in*>(&rt.rt_genmask);
    mask->sin_family = AF_INET;
    mask->sin_addr.s_addr = htonl(0xF0000000U);
    reinterpret_cast<sockaddr_in*>(&rt.rt_gateway)->sin_family = AF_INET;
    rt.rt_flags = RTF_UP;
    char dev[] = "lo";
    rt.rt_dev = dev;
    if (::ioctl(fd, SIOCADDRT, &rt) != 0) err = detail::failed("SIOCADDRT 224.0.0.0/4 dev lo");
  }
  ::close(fd);
  return err;
}

}  // namespace fastmm::net::test
