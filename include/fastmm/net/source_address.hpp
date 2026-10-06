#pragma once
// SourceAddress: where an outbound TCP connection leaves this host from. On a host with several
// addresses, each venue (each account of a pool) can go out from its own, and a venue that counts
// request weight per IP then counts each separately ([venues.<name>] source_ip / source_interface,
// docs/reference/configuration.md#source-address).
//
// The socket is bound to the address before connect(): to `ip` itself, or to the first address of
// `interface` in the family of the peer (getifaddrs). Binding chooses the source address, which is
// what the venue sees; the route out (the device) follows the host's routing table, as for any
// socket bound to that address, so no privilege is needed. The operating system must hold the
// address already: binding to one it does not have fails (EADDRNOTAVAIL), and check_source()
// says so at start-up.
//
// Empty: the kernel chooses, as without one. Control path (connects): getifaddrs allocates.
#include "fastmm/net/tcp_socket.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm::net {

struct SourceAddress {
  std::string ip;         // IPv4 or IPv6 literal
  std::string interface;  // network interface name (eth1); ip and interface are exclusive
  [[nodiscard]] bool empty() const noexcept { return ip.empty() && interface.empty(); }
  bool operator==(const SourceAddress&) const = default;
};

// What is wrong with the setting as written (both set, an ip that is not a literal, an interface
// name too long); empty when it is fine. Needs nothing from the host.
[[nodiscard]] std::string source_syntax_error(const SourceAddress& src);
// What is wrong with it on this host: the ip is not one of its addresses, or the interface does
// not exist or has no IPv4/IPv6 address. Empty when it is fine (and for an empty source).
[[nodiscard]] std::string check_source(const SourceAddress& src);
// The local address (port 0) to bind a socket of `family` (AF_INET, AF_INET6) to. Nullopt when the
// source is empty or has no address of that family.
[[nodiscard]] std::optional<SockAddr> source_bind_address(const SourceAddress& src, int family);
// Drops the peer addresses the source cannot reach (no local address of their family). Unchanged
// for an empty source.
void keep_reachable(const SourceAddress& src, std::vector<SockAddr>& peers);
// The address the source stands for, as text: the ip (normalised: "::1", not "0::1"), or the
// interface's first IPv4 address, else its first IPv6 one, else "if:<name>". Empty for an empty
// source. Rate limiters that count per IP group by it.
[[nodiscard]] std::string source_label(const SourceAddress& src);

}  // namespace fastmm::net
