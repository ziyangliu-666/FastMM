// Nasdaq TotalView-ITCH: the registry entry, the keys the connector owns and its factory. Market
// data needs no credentials (caps.credentials = false) and OUCH order entry logs in with
// ouch_username / ouch_password, so the session never asks this venue for an API key.
#include "fastmm/venues/nasdaq/nasdaq_itch_venue.hpp"
#include "fastmm/venues/registry.hpp"

namespace fastmm::venues {

namespace {

constexpr VenueKeySpec kNasdaqItchKeys[] = {
    {"rx_backend",
     KeyType::String,
     false,
     "multicast receive, kernel (UDP sockets) | af_xdp (needs CAP_NET_ADMIN, CAP_NET_RAW, CAP_BPF, "
     "CAP_IPC_LOCK) | dpdk (a -DFASTMM_WITH_DPDK=ON build, spin_mode = \"busy\") (default kernel)"},
    {"interface",
     KeyType::String,
     false,
     "interface of both lines, name or IPv4 address; af_xdp needs a name (default: routing table)"},
    {"line_a",
     KeyType::String,
     false,
     "line A, \"<multicast group or local unicast address>:<port>\" (required)"},
    {"line_b",
     KeyType::String,
     false,
     "line B, \"<multicast group or local unicast address>:<port>\"; absent = one line"},
    {"line_a_interface", KeyType::String, false, "interface of line A, overrides interface"},
    {"line_b_interface", KeyType::String, false, "interface of line B, overrides interface"},
    {"line_a_source",
     KeyType::String,
     false,
     "source address of line A: a source-specific join (default any source)"},
    {"line_b_source", KeyType::String, false, "source address of line B"},
    {"depth",
     KeyType::Int,
     false,
     "price levels per side sent to the engine, 1 to 256 (default 20)"},
    {"queues",
     KeyType::Any,
     false,
     "af_xdp: RX queues to bind on every line interface, [0, 1] or \"0,1\" (default 0)"},
    {"dpdk_eal_args",
     KeyType::String,
     false,
     "dpdk: rte_eal_init arguments, space-separated (e.g. \"--no-huge --no-pci --in-memory "
     "--vdev=net_af_packet0,iface=eth1\")"},
    {"dpdk_port",
     KeyType::String,
     false,
     "dpdk: ethdev name, e.g. net_af_packet0 or a PCI address (default: the first port)"},
    {"dpdk_exception_port",
     KeyType::String,
     false,
     "dpdk: ethdev name of a net_tap vdev that carries the kernel's traffic on the port (ARP, "
     "GLIMPSE, re-requests, IGMP, kernel TCP); default none"},
    {"dpdk_exception_ip",
     KeyType::String,
     false,
     "dpdk: \"a.b.c.d/len\" given to the exception port's interface"},
    {"dpdk_exception_interval_us",
     KeyType::Int,
     false,
     "dpdk: how often the exception port is read, microseconds; 0 = every poll (default 20)"},
    {"xdp_mode",
     KeyType::String,
     false,
     "af_xdp: auto | zerocopy | native_copy | generic (default auto: the first that works in that "
     "order)"},
    {"rcvbuf", KeyType::Int, false, "kernel: SO_RCVBUF, bytes; 0 = system default (default 0)"},
    {"batch",
     KeyType::Int,
     false,
     "datagrams per recvmmsg (kernel) or RX descriptors per poll (af_xdp), 1 to 1024 (default 32)"},
    {"rerequest",
     KeyType::String,
     false,
     "MoldUDP64 re-request server, \"<IPv4 address>:<port>\"; absent = gaps are unrecoverable"},
    {"glimpse_url",
     KeyType::String,
     false,
     "GLIMPSE 5.0 server, \"<IPv4 address>:<port>\"; absent = start at sequence 1 (before the "
     "directory spin)"},
    {"glimpse_username",
     KeyType::String,
     false,
     "GLIMPSE login, at most 6 characters (default glimps, the simulator's)"},
    {"glimpse_password",
     KeyType::String,
     false,
     "GLIMPSE password, at most 10 characters (default glimpse)"},
    {"reorder_packets", KeyType::Int, false, "packets held ahead of a gap (default 256)"},
    {"gap_timeout_ns",
     KeyType::Int,
     false,
     "a missing sequence awaited this long on every line is a gap, ns; size it from the A/B skew "
     "in the status (default 2000000)"},
    {"max_request_attempts",
     KeyType::Int,
     false,
     "re-requests of one gap before it is unrecoverable; 0 = no limit (default 4)"},
    {"request_timeout_ns",
     KeyType::Int,
     false,
     "re-send an unanswered re-request after this long, ns (default 250000000)"},
    {"recovery_buffer_packets",
     KeyType::Int,
     false,
     "datagrams buffered while a GLIMPSE snapshot is taken, allocated at start (default 65536)"},
    {"price_window_ticks",
     KeyType::Int,
     false,
     "L3 book price window per side, in 0.0001 steps, a multiple of 64; orders outside go to an "
     "overflow store (default 65536)"},
    {"max_orders",
     KeyType::Int,
     false,
     "resting orders per instrument the L3 book holds (default 262144)"},
    {"hw_timestamps",
     KeyType::Bool,
     false,
     "kernel: enable NIC receive timestamps on the line interfaces (SIOCSHWTSTAMP, CAP_NET_ADMIN) "
     "(default false)"},
    {"hw_clock",
     KeyType::String,
     false,
     "none | phc_synced: use the NIC timestamp as recv_ts (only when the PHC is synchronised to "
     "CLOCK_REALTIME) (default none)"},
    {"order_entry",
     KeyType::String,
     false,
     "none (every order is rejected) | sim_ouch (OUCH 5.0 to fastmm-sim-itch) (default none)"},
    {"ouch_url", KeyType::String, false, "sim_ouch: OUCH 5.0 server, \"<IPv4 address>:<port>\""},
    {"order_transport",
     KeyType::String,
     false,
     "sim_ouch: kernel (TCP socket, TCP_NODELAY; the only value) (default kernel)"},
    {"ouch_username",
     KeyType::String,
     false,
     "sim_ouch: login, at most 6 characters (default fmouch, the simulator's)"},
    {"ouch_password",
     KeyType::String,
     false,
     "sim_ouch: password, at most 10 characters (default ouch)"},
};

std::unique_ptr<Venue> make(VenueId id, const VenueSection& s, const VenueFactoryOptions& opts) {
  return std::make_unique<nasdaq::NasdaqItchVenue>(
      id, nasdaq::make_nasdaq_itch_config(s, opts.dry_run, opts.busy_poll));
}

}  // namespace

void register_nasdaq_itch_venue(VenueRegistry& r) {
  static_cast<void>(r.add(
      {.name = "nasdaq_itch",
       .summary = "Nasdaq TotalView-ITCH market data, with OUCH order entry to fastmm-sim-itch",
       .keys = kNasdaqItchKeys,
       .caps = {.credentials = false,
                .order_entry = true,
                .replace = true,
                .positions = false,
                .polls = true},
       .make = &make}));
}

}  // namespace fastmm::venues
