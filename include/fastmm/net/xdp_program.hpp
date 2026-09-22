#pragma once
// The AF_XDP filter program (ADR-0015, section 3) as BPF bytecode built with bpf_asm.hpp.
//
//   Ethernet, at most one 802.1Q tag, IPv4 (version 4, IHL >= 5, no fragment), UDP with a full
//   header after the IPv4 options; anything else returns XDP_PASS.
//   key {dst_ip, dst_port, pad = 0} (network byte order, as on the wire) in `subs`:
//     miss -> XDP_PASS
//     hit  -> bpf_redirect_map(xsks, rx_queue_index, XDP_PASS)
//   A hit whose queue has no socket in `xsks` gets XDP_PASS back from the helper; the program
//   adds one to the per-CPU counter in `fallback[0]` and passes the packet to the kernel.
//   With a TcpMatch, TCP to its address (and port) and ARP for its address are redirected the
//   same way (UserTcp sharing the socket).
//
// Every packet access follows a data_end comparison that covers it, the IHL is masked to 4 bits
// and checked >= 5 before it offsets the packet pointer, and map lookups are null-checked, which
// is what the verifier needs to accept the program.
#include "fastmm/net/bpf_asm.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace fastmm::net::xdp {

// Hash map key; field bytes are copied from the packet.
struct Key {
  std::uint32_t dst_ip;    // network byte order
  std::uint16_t dst_port;  // network byte order
  std::uint16_t pad;       // 0
};
static_assert(sizeof(Key) == 8);

// struct xdp_md field offsets (include/uapi/linux/bpf.h).
inline constexpr std::int16_t kMdData = 0;
inline constexpr std::int16_t kMdDataEnd = 4;
inline constexpr std::int16_t kMdRxQueueIndex = 16;

// enum xdp_action.
inline constexpr std::int32_t kAborted = 0;
inline constexpr std::int32_t kDrop = 1;
inline constexpr std::int32_t kPass = 2;
inline constexpr std::int32_t kTx = 3;
inline constexpr std::int32_t kRedirect = 4;

// Helper function ids (enum bpf_func_id).
inline constexpr std::int32_t kHelperMapLookupElem = 1;
inline constexpr std::int32_t kHelperRedirectMap = 51;

// Stack slots below r10.
inline constexpr std::int16_t kStackKey = -8;
inline constexpr std::int16_t kStackCounterKey = -12;

inline constexpr std::size_t kProgramCapacity = 128;

// A 16-bit value in network byte order as a 16-bit load from the packet returns it.
[[nodiscard]] constexpr std::int32_t wire16(std::uint16_t v) noexcept {
  if constexpr (std::endian::native == std::endian::little)
    return static_cast<std::int32_t>(((v & 0xFFU) << 8U) | (v >> 8U));
  return v;
}

struct MapFds {
  int subs = -1;      // BPF_MAP_TYPE_HASH, Key -> u32
  int xsks = -1;      // BPF_MAP_TYPE_XSKMAP, rx queue -> XSK fd
  int fallback = -1;  // BPF_MAP_TYPE_PERCPU_ARRAY, one u64
};

struct Program {
  std::array<bpf::Insn, kProgramCapacity> insns{};
  std::size_t size = 0;
  bool ok = false;
};

// Frames for a user-space TCP on the same socket (UserTcp): TCP to `ip` (and `port`), and ARP whose
// target address is `ip`, are redirected like a subscribed datagram. ip 0 = none.
struct TcpMatch {
  std::uint32_t ip = 0;    // network byte order
  std::uint16_t port = 0;  // host byte order; 0 = any destination port
  bool arp = true;         // false when the kernel owns `ip` (it must see the ARP)
};

// The two 16-bit halves of a network-byte-order address as 16-bit packet loads return them.
[[nodiscard]] constexpr std::int32_t addr_half(std::uint32_t ip_be, bool second) noexcept {
  if constexpr (std::endian::native == std::endian::little)
    return static_cast<std::int32_t>(second ? ip_be >> 16U : ip_be & 0xFFFFU);
  return static_cast<std::int32_t>(second ? ip_be & 0xFFFFU : ip_be >> 16U);
}

[[nodiscard]] constexpr Program build_program(MapFds fds, TcpMatch tcp = {}) noexcept {
  using namespace bpf;
  enum : std::size_t { LabelL3, LabelFallback, LabelPass, LabelRedirect, LabelTcp, LabelArp };
  Assembler<kProgramCapacity> a;
  constexpr std::int16_t kEth = 14;

  a.emit(mov64_reg(R6, R1));  // r6 = ctx (callee-saved)
  a.emit(ldx(kW, R2, R6, kMdData));
  a.emit(ldx(kW, R3, R6, kMdDataEnd));
  // Ethernet header.
  a.emit(mov64_reg(R4, R2));
  a.emit(alu64_imm(kAdd, R4, kEth));
  a.jmp_reg(kJgt, R4, R3, LabelPass);
  a.emit(ldx(kH, R5, R2, 12));
  // One 802.1Q tag: r2 moves 4 bytes so the rest reads as untagged.
  a.jmp_imm(kJne, R5, wire16(0x8100), LabelL3);
  a.emit(mov64_reg(R4, R2));
  a.emit(alu64_imm(kAdd, R4, kEth + 4));
  a.jmp_reg(kJgt, R4, R3, LabelPass);
  a.emit(ldx(kH, R5, R2, 16));
  a.emit(alu64_imm(kAdd, R2, 4));
  a.bind(LabelL3);
  if (tcp.ip != 0 && tcp.arp) a.jmp_imm(kJeq, R5, wire16(0x0806), LabelArp);
  a.jmp_imm(kJne, R5, wire16(0x0800), LabelPass);
  // IPv4 fixed header (20 bytes).
  a.emit(mov64_reg(R4, R2));
  a.emit(alu64_imm(kAdd, R4, kEth + 20));
  a.jmp_reg(kJgt, R4, R3, LabelPass);
  a.emit(ldx(kB, R5, R2, kEth));  // version | IHL
  a.emit(mov64_reg(R4, R5));
  a.emit(alu64_imm(kAnd, R4, 0xF0));
  a.jmp_imm(kJne, R4, 0x40, LabelPass);
  a.emit(alu64_imm(kAnd, R5, 0x0F));
  a.jmp_imm(kJlt, R5, 5, LabelPass);
  a.emit(alu64_imm(kLsh, R5, 2));     // header length, 20..60
  a.emit(ldx(kH, R4, R2, kEth + 6));  // flags | fragment offset
  a.emit(alu64_imm(kAnd, R4, wire16(0x3FFF)));
  a.jmp_imm(kJne, R4, 0, LabelPass);  // MF or offset: fragment
  a.emit(ldx(kB, R4, R2, kEth + 9));
  if (tcp.ip != 0) a.jmp_imm(kJeq, R4, 6, LabelTcp);
  a.jmp_imm(kJne, R4, 17, LabelPass);
  a.emit(ldx(kW, R4, R2, kEth + 16));  // destination address
  a.emit(stx(kW, R10, kStackKey, R4));
  // UDP header after the options.
  a.emit(alu64_reg(kAdd, R2, R5));
  a.emit(mov64_reg(R4, R2));
  a.emit(alu64_imm(kAdd, R4, kEth + 8));
  a.jmp_reg(kJgt, R4, R3, LabelPass);
  a.emit(ldx(kH, R4, R2, kEth + 2));  // destination port
  a.emit(stx(kH, R10, kStackKey + 4, R4));
  a.emit(st_imm(kH, R10, kStackKey + 6, 0));
  // Subscribed?
  a.emit(ld_map_fd(R1, fds.subs));
  a.emit(mov64_reg(R2, R10));
  a.emit(alu64_imm(kAdd, R2, kStackKey));
  a.emit(call(kHelperMapLookupElem));
  a.jmp_imm(kJeq, R0, 0, LabelPass);
  if (tcp.ip != 0) {
    a.ja(LabelRedirect);
    // TCP to the user-space stack's address (and port); r5 is the IPv4 header length.
    a.bind(LabelTcp);
    a.emit(ldx(kH, R4, R2, kEth + 16));
    a.jmp_imm(kJne, R4, addr_half(tcp.ip, false), LabelPass);
    a.emit(ldx(kH, R4, R2, kEth + 18));
    a.jmp_imm(kJne, R4, addr_half(tcp.ip, true), LabelPass);
    if (tcp.port != 0) {
      a.emit(alu64_reg(kAdd, R2, R5));
      a.emit(mov64_reg(R4, R2));
      a.emit(alu64_imm(kAdd, R4, kEth + 4));
      a.jmp_reg(kJgt, R4, R3, LabelPass);
      a.emit(ldx(kH, R4, R2, kEth + 2));  // destination port
      a.jmp_imm(kJne, R4, wire16(tcp.port), LabelPass);
    }
    if (tcp.arp) {
      a.ja(LabelRedirect);
      // ARP whose target protocol address is the stack's.
      a.bind(LabelArp);
      a.emit(mov64_reg(R4, R2));
      a.emit(alu64_imm(kAdd, R4, kEth + 28));
      a.jmp_reg(kJgt, R4, R3, LabelPass);
      a.emit(ldx(kH, R4, R2, kEth + 24));
      a.jmp_imm(kJne, R4, addr_half(tcp.ip, false), LabelPass);
      a.emit(ldx(kH, R4, R2, kEth + 26));
      a.jmp_imm(kJne, R4, addr_half(tcp.ip, true), LabelPass);
    }
  }
  // Redirect to the queue's socket, XDP_PASS when it has none.
  a.bind(LabelRedirect);
  a.emit(ld_map_fd(R1, fds.xsks));
  a.emit(ldx(kW, R2, R6, kMdRxQueueIndex));
  a.emit(mov64_imm(R3, kPass));
  a.emit(call(kHelperRedirectMap));
  a.jmp_imm(kJne, R0, kRedirect, LabelFallback);
  a.emit(mov64_imm(R0, kRedirect));
  a.emit(exit_insn());
  // Count the fallback.
  a.bind(LabelFallback);
  a.emit(st_imm(kW, R10, kStackCounterKey, 0));
  a.emit(ld_map_fd(R1, fds.fallback));
  a.emit(mov64_reg(R2, R10));
  a.emit(alu64_imm(kAdd, R2, kStackCounterKey));
  a.emit(call(kHelperMapLookupElem));
  a.jmp_imm(kJeq, R0, 0, LabelPass);
  a.emit(ldx(kDw, R1, R0, 0));
  a.emit(alu64_imm(kAdd, R1, 1));
  a.emit(stx(kDw, R0, 0, R1));
  a.bind(LabelPass);
  a.emit(mov64_imm(R0, kPass));
  a.emit(exit_insn());

  Program p;
  p.ok = a.finish();
  p.size = a.size();
  p.insns = a.insns();
  return p;
}

}  // namespace fastmm::net::xdp
