#pragma once
// Configuration of fastmm-sim-itch (ADR-0015, section 6): the Nasdaq-style simulator that
// publishes TotalView-ITCH 5.0 over MoldUDP64 multicast, answers re-requests, serves GLIMPSE 5.0
// and accepts OUCH 5.0 orders. Read from [[instruments]] and [sim] / [sim.itch] / [sim.glimpse] /
// [sim.ouch] / [sim.generator] of a FastMM TOML file (configs/sim-itch.toml), or built in code by
// tests (SimItchConfig::defaults()). See docs/reference/sim-itch.md.
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/sim/market_generator.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fastmm {
class Config;
}

namespace fastmm::sim::itch {

struct SimItchSymbol {
  std::string symbol;                       // 1..8 characters
  std::uint16_t locate = 0;                 // ITCH Stock Locate (0: position + 1)
  Price tick = Price::from_raw(1'000'000);  // 0.01
  Qty lot = Qty::from_int(1);               // whole shares
  Price start_mid = Price::from_int(100);
};

struct SimItchLine {
  std::string group;  // IPv4 multicast group; empty disables the line
  std::uint16_t port = 0;
  double drop_rate = 0.0;  // probability that a data datagram is not sent on this line
};

struct SimItchConfig {
  // ---- market ----
  std::vector<SimItchSymbol> symbols;
  std::uint64_t seed = 7;
  MarketGeneratorParams generator;  // start_mid / tick / lot are taken per symbol
  bool generator_enabled = true;
  int seed_levels = 10;
  double speed = 1.0;  // generator time runs this many times faster than the wall clock

  // ---- multicast feed ----
  std::string session = "FMSIM00001";  // MoldUDP64 session (10 characters)
  SimItchLine line_a{"239.192.0.1", 31001, 0.0};
  SimItchLine line_b{"239.192.0.2", 31002, 0.0};
  std::string interface = "lo";  // IP_MULTICAST_IF: name, IPv4 address or "" (routing table)
  std::string source;            // local address the sending socket binds ("" any)
  int ttl = 1;
  bool loop = true;                   // IP_MULTICAST_LOOP (receivers on the same host)
  std::uint32_t max_datagram = 1472;  // MoldUDP64 packet size limit
  std::uint32_t max_messages = 0;     // messages per packet, 0 = as many as fit
  std::uint32_t burst = 32;           // datagrams per sendmmsg (and token-bucket depth)
  double packet_rate = 0.0;           // datagrams per second per line, 0 = unpaced
  std::uint32_t flush_us = 0;         // hold messages this long to fill packets, 0 = none
  std::uint64_t drop_seed = 1;        // line i draws from seed + i
  std::uint32_t heartbeat_ms = 1000;  // MoldUDP64 heartbeat after this long without data
  std::size_t history_messages = std::size_t{1} << 20;  // re-request ring
  std::size_t history_bytes = std::size_t{1} << 26;
  int sndbuf_bytes = 0;  // 0 = system default

  // ---- unicast servers ----
  std::string bind_host = "127.0.0.1";
  int rerequest_port = 31000;  // MoldUDP64 request server (UDP); 0 = ephemeral, < 0 = off
  int glimpse_port = 31010;    // GLIMPSE 5.0 over SoupBinTCP
  int ouch_port = 31020;       // OUCH 5.0 over SoupBinTCP
  std::string glimpse_username = "glimps";
  std::string glimpse_password = "glimpse";
  std::string glimpse_session = "GLIMPSE";
  std::size_t glimpse_max_messages = std::size_t{1} << 18;  // snapshot size limit per login
  std::string ouch_username = "fmouch";
  std::string ouch_password = "ouch";
  std::string ouch_session = "OUCH";
  std::size_t ouch_history_messages = std::size_t{1} << 18;

  // ---- runtime ----
  bool busy_poll = false;                         // never block in poll()
  std::size_t stamp_ring = std::size_t{1} << 20;  // send stamps kept for wire-to-wire lookups

  // Reads [[instruments]] (tick, lot) and the [sim] tables (see configs/sim-itch.toml). Throws
  // std::invalid_argument on bad values.
  static SimItchConfig from_config(const Config& cfg);
  static SimItchConfig load(const std::string& path);
  // Two symbols (FMAA, FMBB) at $100 with tick 0.01, lot 1 share and a generator of about
  // 200 order events per second per symbol.
  static SimItchConfig defaults();
  // Throws std::invalid_argument when a value is out of range.
  void validate() const;
};

}  // namespace fastmm::sim::itch
